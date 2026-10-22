#pragma once

#include "uih.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <type_traits>

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
// has claimed the process yet.
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
// Runs `teardown` when Stop() asked the active backend to release its resources
// on the rendering thread.
void ServiceTeardown(Backend backend, void (*teardown)());
// Runs `release` once no thread draws the overlay.
void ReleaseOverlay(void (*release)());
// False when the rendering thread did not answer and the caller has to release the resources.
bool TeardownOnRenderThread(unsigned timeoutMs);

// False when the ImGui context already exists.
bool CreateContext();
// Creates the ImGui context on first use and attaches the platform backend
// and input hooks to `window`.
bool BindWindow(NativeWindow window);
// Platform NewFrame, key polling, user callback and ImGui::Render().
ImDrawData* Frame();
// Dear ImGui produces sRGB vertex colors. Render targets that store linear
// values (sRGB and floating-point formats) need them converted.
void LinearizeColors(ImDrawData* drawData);
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
void Unload();
// While the hook is stopped: true when the menu key or buttons of `config`
// are held and the application has the focus.
bool MenuInputDown(const Config& config);
// True when `window` or the window it belongs to has the focus.
bool HasFocus(NativeWindow window);
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
