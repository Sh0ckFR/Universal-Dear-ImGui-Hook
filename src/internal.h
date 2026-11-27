#pragma once

#include "uih.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <type_traits>
#include <vector>

#include "imgui.h"

namespace uih::detail {

#ifdef _WIN32
using NativeWindow = HWND;
#else
// X11 window, 0 when unknown.
using NativeWindow = uint32_t;
#endif

void Log(const char* fmt, ...);

// Descriptors the renderer backends reserve for textures: those of
// CreateTexture() and the font atlas, which may hold several while it grows.
constexpr int kTextureDescriptors = kMaxTextures + 8;

// Every hook keeps one of these alive while it runs so Stop() can wait for
// in-flight calls before the hooks are removed.
class ScopedCall {
public:
    ScopedCall();
    ~ScopedCall();
    ScopedCall(const ScopedCall&) = delete;
    ScopedCall& operator=(const ScopedCall&) = delete;
};
bool WaitForIdleHooks(unsigned timeoutMs);

// Drivers may implement presentation of one API on top of another (Vulkan on
// a DXGI swap chain). Hooks of the outer API mark the thread so the inner API
// ignores the nested present.
class ForeignPresentScope {
public:
    ForeignPresentScope();
    ~ForeignPresentScope();
    ForeignPresentScope(const ForeignPresentScope&) = delete;
    ForeignPresentScope& operator=(const ForeignPresentScope&) = delete;
};
bool InsideForeignPresent();

// Dear ImGui is not thread-safe. The window procedure feeds its input on the
// thread of the window while the overlay draws on the thread that presents:
// both hold this lock while they use the input or create or destroy the context.
class InputLock {
public:
    InputLock();
    ~InputLock();
    InputLock(const InputLock&) = delete;
    InputLock& operator=(const InputLock&) = delete;

private:
    int depth_;
};
// Input locks held by this thread.
int InputLockDepth();
// Releases the input locks this thread took beyond `depth`.
void ReleaseInputLocks(int depth);

// Frame orchestration ----------------------------------------------------

const Config& GetConfig();
// True when `backend` is allowed by the configuration.
bool WantBackend(Backend backend);
// True when hooks are running and `backend` is either active or no backend
// has claimed the process yet. The active backend is released when it
// stopped presenting while `backend` does.
bool ShouldRender(Backend backend);
// Several windows or swap chains of the application can present, from one
// thread or more. The overlay is drawn on one of them, its target, by one
// thread at a time. It moves to another one when the target stops
// presenting, when the other one's window has the focus and the target's
// does not, or when the application replaces the target's swap chain on the
// same window. True when `swapchain` is the target: the caller draws the
// overlay, then calls EndOverlayFrame().
bool BeginOverlayFrame(uint64_t swapchain, NativeWindow window);
void EndOverlayFrame();
// Marks `backend` as the active one. Fails when another backend already owns the process.
bool ClaimBackend(Backend backend);
// The application destroyed the device or context the overlay drew with:
// another backend may claim the process, as when a game renders its splash
// screen with one API and the game with another.
void ReleaseBackend(Backend backend);
// Runs `teardown` when Stop() asked the active backend to release its resources
// on the rendering thread.
void ServiceTeardown(Backend backend, void (*teardown)());
// Runs `release` once no thread draws the overlay.
void ReleaseOverlay(void (*release)());
// Same, but a thread that draws the overlay or presents after it releases it
// itself once its present returns. False when it still draws or presents after
// `timeoutMs`: it releases the overlay later.
bool ReleaseOverlayWithin(unsigned timeoutMs, void (*release)());
// False when the rendering thread did not answer and the caller has to release the resources.
bool TeardownOnRenderThread(unsigned timeoutMs);

// False when the ImGui context already exists.
bool CreateContext();
// Creates the ImGui context on first use and attaches the platform backend
// and input hooks to `window`.
bool BindWindow(NativeWindow window);
// Window the overlay draws on, none before its first frame.
NativeWindow BoundWindow();
// Platform NewFrame, key polling, user callback and ImGui::Render().
ImDrawData* Frame();
// True while CaptureFrame() waits: the backend copies the frame of the game,
// before it draws the overlay, and hands it to CompleteCapture().
bool CaptureWanted();
// Empty `rgba` when the backend could not copy the frame.
void CompleteCapture(std::vector<uint8_t>&& rgba, uint32_t width, uint32_t height);
// Layout of the 32-bit texels of a copied frame, from the lowest bits.
enum class TexelLayout { Unsupported, RGBA8, BGRA8, RGB10A2, BGR10A2 };
// Opaque RGBA pixels, top row first, from rows of texels `pitch` bytes apart.
// Empty for an unsupported layout.
std::vector<uint8_t> ToRgba(const void* texels, size_t pitch, uint32_t width, uint32_t height, TexelLayout layout, bool bottomUp = false);
// Dear ImGui produces sRGB vertex colors. Render targets that store linear
// values (sRGB and floating-point formats) need them converted.
void LinearizeColors(ImDrawData* drawData);
// HDR10 swap chains store BT.2020 colors encoded with the PQ curve (SMPTE
// ST 2084): sRGB vertex colors are converted to it, white being `whiteNits`.
void EncodePqColors(ImDrawData* drawData, float whiteNits);
// Detaches the platform backend and destroys the ImGui context.
void UnbindWindow();
// Detaches the renderer backend without a call to the graphics API, when its
// objects belong to a context that is not current: they go with that context.
// The few bytes of the backend's state are left behind.
void AbandonRenderer();
// For a library that cannot leave the process: stops the hook, then starts
// it again with the menu open when the menu key or buttons are pressed in
// the application. Runs on a thread of its own.
void StopUntilMenuInput();
bool StoppingToResume();
// Destroys the ImGui context, after the renderer backend released its textures.
void DestroyContext();
// Called on the thread where work run by Guarded() raised an exception.
void OnFault(unsigned long code);

// Implemented once per platform -------------------------------------------

namespace platform {
void WriteLog(const char* line);
bool Start();
void Stop();
void NewFrame();
bool IsKeyDown(int key);
bool IsFocused();
// DPI scale of the bound window, 1 when unknown.
float WindowScale();
// Brightness of white in SDR content on the display of the bound window, in
// nits: the overlay is drawn as bright on an HDR10 swap chain.
float SdrWhiteNits();
void Unload();
// While the hook is stopped: true when the menu key or buttons of `config`
// are held and the application has the focus.
bool MenuInputDown(const Config& config);
// True when `window` or the window it belongs to has the focus.
bool HasFocus(NativeWindow window);
// Releases the resources of the backends from a thread that does not render
// with them. False when the platform cannot.
bool TeardownBackends();
// Runs `work`; false when it raised an exception, which OnFault() handled.
bool RunGuarded(void (*work)(void*), void* context);
} // namespace platform

namespace input {
void Attach(NativeWindow window);
void Detach();
// Called when the menu opens or closes: releases the cursor and hides or
// restores the input the application sees.
void OnMenuToggled(bool open);
// Best-effort lookup of the main window of this process, or on Windows of
// its window whose client area is `width` x `height` when there is one.
NativeWindow FindProcessWindow(uint32_t width = 0, uint32_t height = 0);
} // namespace input

// Remote control for Config::remoteControl --------------------------------

namespace remote {
// Answers one request of the protocol (src/remote.cpp). The pixels of a
// screenshot go to `payload`, sent after the answer.
std::string Handle(const std::string& line, std::vector<uint8_t>& payload);
// The last message of the assistant, drawn in the frame of the overlay.
void Draw();
void ClearText();
// The message of the assistant at the top of the screen, for `seconds`.
void ShowMessage(const std::string& text, double seconds);

// Implemented once per platform, in remote.cpp:
// serves the pipe or socket named `name` on a thread of its own.
void Start(const char* name);
// Ends the connection and the command in progress, and waits for the thread.
void Stop();
// Virtual-key code or keysym of a key named in lower case, 0 when unknown.
int KeyCode(const std::string& name);
// Holds `keys` for `ms` milliseconds, or until Stop(), and answers the request.
std::string HoldKeys(const std::vector<int>& keys, unsigned ms);
// Clicks `button` (1 left, 2 middle, 3 right) at x, y in the client area of the
// game window, held for `ms` milliseconds or until Stop(), and answers the request.
std::string ClickAt(int x, int y, int button, unsigned ms);
// Client area of the game window, and whether it has the keyboard focus.
bool GameWindow(uint32_t& width, uint32_t& height, bool& focused);
// True once Stop() was called, for long commands.
bool StopRequested();
// Config::aiAdvancedFeatures: types `text`, code points with '\n' for Enter
// and '\t' for Tab, and answers the request.
std::string TypeText(const std::vector<uint32_t>& text);
// Moves the pointer to x, y in the client area of the game window.
std::string MoveTo(int x, int y);
// Presses `button` at x0, y0, moves to x1, y1 over `ms` milliseconds, and releases it there.
std::string DragTo(int x0, int y0, int x1, int y1, int button, unsigned ms);

// Memory commands, src/memory.cpp: false when `command` is not one of them.
bool HandleMemory(const std::string& command, const std::string& rest, std::vector<uint8_t>& payload, std::string& answer);
// Implemented once per platform, for the memory commands:
struct MemoryRegion {
    uintptr_t base;
    size_t size;
    bool writable;
};
// Readable memory of the process, without device memory.
std::vector<MemoryRegion> MemoryRegions();
struct LoadedModule {
    std::string name;
    uintptr_t base;
    size_t size;
    // The executable of the process.
    bool main;
};
std::vector<LoadedModule> LoadedModules();
// The memory that the modules map, sorted: the segments of each on Linux,
// as the span of a module can hold other memory; the loader of glibc 2.28
// leaves the gap between its code and its data free for mmap().
struct AddressRange {
    uintptr_t base;
    size_t size;
};
std::vector<AddressRange> ModuleMemory();
// Copies memory of the process without faulting; false when part of it is not readable.
bool ReadMemory(uintptr_t address, void* out, size_t size);
// Writes memory of the process, code included.
bool WriteMemory(uintptr_t address, const void* data, size_t size);
// Memory of its own, zeroed, in a mapping apart from the heap; never released.
void* AllocateScanMemory(size_t size);

// Shared by the memory commands and the advanced features, src/memory.cpp:
enum class ValueType { I8, I16, I32, I64, F32, F64 };
bool ParseValueType(std::string name, ValueType& type);
const char* ValueTypeName(ValueType type);
size_t ValueSize(ValueType type);
std::string FormatValue(const uint8_t* bytes, ValueType type);
// The little-endian bytes of `text` read as a number of `type`; false when it is not one.
bool EncodeValue(const std::string& text, ValueType type, uint8_t* bytes);
// The modules sorted by address, for LocateAddress().
std::vector<LoadedModule> SortedModules();
// Whether `address` is in one of the sorted `ranges`, such as ModuleMemory().
bool InRanges(const std::vector<AddressRange>& ranges, uintptr_t address);
// module+offset of `address`, or "-" outside the modules.
std::string LocateAddress(uintptr_t address, const std::vector<LoadedModule>& sortedModules);
// An address: hexadecimal, module+offset, module-offset, and [expression] for
// the pointer stored there, such as [[game.exe+1a2b]+10]+8. `steps` gets one
// line per pointer read. False with the reason in `error`.
bool ResolveAddress(const std::string& text, const std::vector<LoadedModule>& modules, uintptr_t& address, std::string* error = nullptr,
                    std::vector<std::string>* steps = nullptr);
struct PointerHit {
    // Where the pointer is stored, and how far before the target it points.
    uintptr_t location;
    uintptr_t delta;
    // Stored in a module: found again at the same module+offset after a restart.
    bool inModule;
};
// Writable memory holding a pointer to `target`, or to up to `maxOffset`
// bytes before it, those in a module first; at most `limit`. False when
// `cancelled` returned true.
bool ScanPointers(uintptr_t target, size_t maxOffset, size_t limit, bool (*cancelled)(), std::vector<PointerHit>& hits);
// Addresses in `module` where `pattern` is, -1 for any byte; at most `limit`.
// False when the scan's memory cannot be allocated.
bool FindPattern(const std::vector<int>& pattern, const LoadedModule& module, size_t limit, std::vector<uintptr_t>& found);

// The code commands, src/code.cpp, with Config::remoteMemory and
// Config::aiAdvancedFeatures: the disassembler, the patches of the code and
// the signatures. False when `command` is not one of them.
bool HandleCode(const std::string& command, const std::string& rest, std::vector<uint8_t>& payload, std::string& answer);
// "<address> <instruction>" of the instruction that ends at `next`, as the
// instruction watch reports them; empty when it cannot be decoded.
std::string InstructionBefore(uintptr_t next);
struct CodePatch {
    int id;
    uintptr_t address;
    std::vector<uint8_t> original;
    std::vector<uint8_t> patched;
};
std::vector<CodePatch> CodePatches();
// Puts the original bytes back and forgets the patch. Empty when it did,
// "changed" when the game changed the bytes since and they were left as they
// are, the reason otherwise.
std::string RestorePatch(int id);
// Puts back every patch whose bytes are still there, once the overlay is gone.
void RestorePatches();
// Writes `size` bytes of code at `address`, an aligned 8 bytes at once when
// they fit in them, so that a thread running the code sees either all the old
// bytes or all the new ones. The memory must be writable; does not allocate.
void StoreCode(uintptr_t address, const void* data, size_t size);
// Implemented once per platform, in code.cpp:
// true when all of address..address+size is mapped executable.
bool IsCode(uintptr_t address, size_t size);
// Why patching there would break the hook, such as a MinHook hook or
// trampoline of the overlay; empty otherwise.
std::string ProtectedCode(uintptr_t address, size_t size);
// Writes code: makes the pages writable, then puts their protection back,
// and flushes the instruction cache. On Windows, the other threads are
// suspended meanwhile, and the write waits until none of them is inside
// `from`..address+size, past its first byte. Empty when it wrote, the reason otherwise.
std::string WriteCode(uintptr_t address, const void* data, size_t size, uintptr_t from);
// "name" or "name+0x12" of the exported or dynamic symbol `address` is in, empty when none is known.
std::string SymbolAt(uintptr_t address);
// True when the code of the function at `function` tests the bits `mask` of
// the 32-bit value `displacement` bytes into one of its arguments, as
// `if (value->flags & mask)` compiles: a test of the memory, or of a register
// loaded from it, within the first `length` bytes, after the jumps at its
// start. Decodes x86, x64 and ARM64 code; false on other processors.
bool CodeTestsBits(uintptr_t function, uint32_t displacement, uint32_t mask, size_t length);

// Objects of the game's engine, src/engine.cpp: false when `command` is not engine.
bool HandleEngine(const std::string& command, const std::string& rest, std::vector<uint8_t>& payload, std::string& answer);
// Runs the engine call waiting for the game's main thread, if any: called by
// the window procedure on Windows and by the X event hooks on Linux.
void RunGameThreadCalls();
// Runs `work` on the game's main thread and returns its answer, or an error
// after 10 seconds. `work` gets everything it uses by value: it may still run
// after the caller gave up on it.
std::string RunOnGameThread(std::function<std::string()> work);
// The most classes `engine classes` lists on this thread, 1000 by default;
// the Engine tab of the AI Advanced Features window lists them all.
size_t& EngineListLimit();
// Calls `visit` with the readable memory of [base, base + size), region by
// region, in pieces of at most 1 MB that overlap by `overlap` bytes, which
// may be fewer; stops when `visit` returns false. Modules have unreadable
// gaps between their segments on some systems, such as Linux on ARM64.
void ForEachReadable(uintptr_t base, size_t size, size_t overlap, const std::function<bool(uintptr_t, const uint8_t*, size_t)>& visit);
// Unreal Engine games, src/unreal.cpp: runs the engine command `sub` when
// the game is one, false otherwise.
bool HandleUnreal(const std::string& sub, const std::string& arguments, std::vector<uint8_t>& payload, std::string& answer);
// Implemented once per platform, for the engine commands:
// a module loaded in the process by its file name, such as mono-2.0-bdwgc.dll; null when it is not.
void* FindModule(const char* name);
// An exported function of a module FindModule() found.
void* ModuleSymbol(void* module, const char* name);
// Makes the game's main thread call RunGameThreadCalls() soon; false when it cannot.
bool WakeGameThread();

// Advanced features, Config::aiAdvancedFeatures, src/advanced.cpp:
// switches of the "AI Advanced Features" window, on until the player turns them off.
bool InputAllowed();
bool MemoryAllowed();
// Shows a command of the assistant and its answer in the window.
void RecordActivity(const std::string& line, const std::string& answer);
// The watch list and the instruction watch: false when `command` is not one of their commands.
bool HandleAdvanced(const std::string& command, const std::string& rest, std::vector<uint8_t>& payload, std::string& answer);
// Every frame of the overlay: writes the frozen values, watches new threads.
void Tick();
// The tools of the game, uih::RegisterTool(), src/tools.cpp: false when
// `command` is not tools or tool.
bool HandleTools(const std::string& command, const std::string& rest, std::vector<uint8_t>& payload, std::string& answer);
// Every frame of the overlay: runs the tool the assistant called, if any.
void RunTools();
// The elements of the JSON array `text`, strings decoded and other values as
// written; false when it is not one array.
bool ParseJsonArray(const std::string& text, std::vector<std::string>& values, std::vector<bool>* strings = nullptr);
std::string JsonQuote(const std::string& text);
// Once the overlay is gone: ends the instruction watch and the scan of the window.
void StopAdvanced();
// The Engine tab of the AI Advanced Features window, src/explorer.cpp: the
// classes, objects and functions of a Unity or Unreal Engine game, and the
// SDK. Its requests run on a thread of their own, stopped by StopExplorer().
void DrawExplorer();
void StopExplorer();
// Implemented once per platform: the instructions that write `size` bytes at
// `address`, or that access them. Replaces the previous watch; empty when it
// started, the reason otherwise.
std::string WatchInstructions(uintptr_t address, unsigned size, bool access);
void UnwatchInstructions();
// Also removes what WatchInstructions() installed, before the library leaves the process.
void ShutdownInstructionWatch();
struct InstructionHit {
    // The instruction after the one that hit, where the processor stops.
    uintptr_t next;
    uint64_t count;
};
std::vector<InstructionHit> InstructionHits();
// Watches the threads started since; called every frame.
void RefreshInstructionWatch();
} // namespace remote

// Speed of the game's clocks, src/timing.cpp ---------------------------------

namespace timing {
// The clocks the platforms hook: their readings are scaled one by one.
enum Clock {
    kQpc,           // QueryPerformanceCounter(), in its ticks
    kTickMs,        // GetTickCount64() and GetTickCount(), in milliseconds
    kTimeGetTime,   // timeGetTime(), in milliseconds
    kMonotonic,     // clock_gettime(CLOCK_MONOTONIC), in nanoseconds
    kMonotonicRaw,  // CLOCK_MONOTONIC_RAW
    kMonotonicCoarse,
    kBoottime,
    kClockCount
};
// True once the speed changed, outside RealTime: the hooks then scale the
// readings of the game's code.
bool Scaling();
// The game's reading of `clock` for its real reading `real`.
int64_t GameTime(Clock clock, int64_t real);
double Speed();
// 0 pauses the game, 1 is real time.
void SetSpeed(double speed);
// Back to real time, once the overlay is gone.
void Reset();
std::string Command(const std::string& rest);

// The clocks read by the overlay on this thread keep real time.
class RealTime {
public:
    RealTime();
    ~RealTime();
    RealTime(const RealTime&) = delete;
    RealTime& operator=(const RealTime&) = delete;
};

// For an input command of the assistant: a paused game runs during it, then
// pauses again.
class RunWhilePaused {
public:
    RunWhilePaused();
    ~RunWhilePaused();
    RunWhilePaused(const RunWhilePaused&) = delete;
    RunWhilePaused& operator=(const RunWhilePaused&) = delete;

private:
    uint64_t epoch_ = 0;
};

// Implemented once per platform, in clocks.cpp:
// the real reading of `clock`; false when the platform has no such clock.
bool RealClock(Clock clock, int64_t& real);
// True when the clock functions of the game are hooked.
bool ClocksHooked();
} // namespace timing

// Runs work of the overlay on a thread of the application. On Windows with
// MSVC, an exception in it, from a renderer backend or the render callback,
// removes the overlay instead of crashing the application, and the menu key
// or buttons bring it back. False when it faulted.
template <typename Work>
bool Guarded(Work&& work) {
    int depth = InputLockDepth();
    bool ok = platform::RunGuarded([](void* context) { (*static_cast<std::remove_reference_t<Work>*>(context))(); }, &work);
    // The exception may have skipped the destructors of the locks `work` took.
    if (!ok)
        ReleaseInputLocks(depth);
    return ok;
}

// While true on a thread, a fault there is the game's code failing a call
// the assistant made: OnFault() leaves the overlay running.
void SetInGameCall(bool inCall);

// Runs a call of the game's own code, such as an engine call, guarded like
// the overlay's work: false when it faulted, and the overlay stays.
template <typename Work>
bool GuardedCall(Work&& work) {
    SetInGameCall(true);
    bool ok = Guarded(work);
    SetInGameCall(false);
    return ok;
}

// Draws the overlay with `render` when `swapchain` is its target.
template <typename Render>
void DrawOverlay(uint64_t swapchain, NativeWindow window, Render&& render) {
    if (!BeginOverlayFrame(swapchain, window))
        return;
    Guarded(render);
    EndOverlayFrame();
}

inline uint64_t SwapchainKey(const void* swapchain) { return reinterpret_cast<uintptr_t>(swapchain); }

} // namespace uih::detail

#ifdef _WIN32
#include "windows/platform.h"
#else
#include "linux/platform.h"
#endif
