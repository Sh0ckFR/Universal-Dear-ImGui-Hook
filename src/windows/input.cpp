#include "internal.h"

#include <cstring>
#include <vector>

#include "imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace uih::detail::input {
namespace {

using SetCursorPosFn = BOOL(WINAPI*)(int, int);
using ClipCursorFn = BOOL(WINAPI*)(const RECT*);
using GetCursorPosFn = BOOL(WINAPI*)(POINT*);
using GetAsyncKeyStateFn = SHORT(WINAPI*)(int);
using GetKeyStateFn = SHORT(WINAPI*)(int);
using GetKeyboardStateFn = BOOL(WINAPI*)(BYTE*);
using GetRawInputDataFn = UINT(WINAPI*)(HRAWINPUT, UINT, void*, UINT*, UINT);
using GetRawInputBufferFn = UINT(WINAPI*)(RAWINPUT*, UINT*, UINT);

SetCursorPosFn g_setCursorPos = nullptr;
ClipCursorFn g_clipCursor = nullptr;
GetCursorPosFn g_getCursorPos = nullptr;
GetAsyncKeyStateFn g_getAsyncKeyState = nullptr;
GetKeyStateFn g_getKeyState = nullptr;
GetKeyboardStateFn g_getKeyboardState = nullptr;
GetRawInputDataFn g_getRawInputData = nullptr;
GetRawInputBufferFn g_getRawInputBuffer = nullptr;

// Windows whose procedure was replaced by WndProc, guarded by the input
// lock. An entry outlives the subclassing: a message that is still being
// dispatched needs its original procedure.
struct Subclass {
    HWND window;
    WNDPROC original;
    bool unicode;
    // WndProc is still in the chain of procedures of the window.
    bool chained;
    // The procedure installed on the window, which jumps to WndProc (CreateStub()).
    WNDPROC stub;
    // The stub calls the original procedure: the application subclassed the
    // window after it, so it stays in the chain.
    bool forwarded;
};
std::vector<Subclass> g_subclasses;
// Window whose input feeds the overlay.
HWND g_window = nullptr;

thread_local int g_ownCalls = 0;
// Cursor position the application sees while the menu is open.
POINT g_frozenCursor{};
std::atomic<bool> g_cursorFrozen{false};
// Rectangle the application confines the cursor to, applied again when the menu closes.
std::mutex g_clipMutex;
bool g_clipped = false;
RECT g_clip{};

constexpr USHORT kMouseButtonReleases = RI_MOUSE_LEFT_BUTTON_UP | RI_MOUSE_RIGHT_BUTTON_UP | RI_MOUSE_MIDDLE_BUTTON_UP |
                                        RI_MOUSE_BUTTON_4_UP | RI_MOUSE_BUTTON_5_UP;

// Removes movement and presses, keeps releases so no key stays down in the application.
void Neutralize(RAWINPUT* input) {
    if (input->header.dwType == RIM_TYPEMOUSE) {
        RAWMOUSE& mouse = input->data.mouse;
        mouse.usFlags = MOUSE_MOVE_RELATIVE;
        mouse.lLastX = 0;
        mouse.lLastY = 0;
        mouse.usButtonFlags &= kMouseButtonReleases;
        mouse.usButtonData = 0;
        mouse.ulRawButtons = 0;
    } else if (input->header.dwType == RIM_TYPEKEYBOARD && !(input->data.keyboard.Flags & RI_KEY_BREAK)) {
        input->data.keyboard.VKey = 0xFF;
        input->data.keyboard.MakeCode = 0;
    }
}

BOOL WINAPI HookSetCursorPos(int x, int y) {
    ScopedCall call;
    if (IsMenuOpen())
        return TRUE;
    return g_setCursorPos(x, y);
}

BOOL WINAPI HookClipCursor(const RECT* rect) {
    ScopedCall call;
    std::lock_guard<std::mutex> lock(g_clipMutex);
    if (IsMenuOpen()) {
        g_clipped = rect != nullptr;
        if (rect)
            g_clip = *rect;
        return TRUE;
    }
    return g_clipCursor(rect);
}

BOOL WINAPI HookGetCursorPos(POINT* point) {
    ScopedCall call;
    if (point && g_cursorFrozen && BlockInput()) {
        *point = g_frozenCursor;
        return TRUE;
    }
    return g_getCursorPos(point);
}

SHORT WINAPI HookGetAsyncKeyState(int key) {
    ScopedCall call;
    if (BlockInput())
        return 0;
    return g_getAsyncKeyState(key);
}

SHORT WINAPI HookGetKeyState(int key) {
    ScopedCall call;
    SHORT state = g_getKeyState(key);
    // Toggle states, such as Caps Lock, are kept.
    return BlockInput() ? static_cast<SHORT>(state & 1) : state;
}

BOOL WINAPI HookGetKeyboardState(BYTE* state) {
    ScopedCall call;
    BOOL result = g_getKeyboardState(state);
    if (result && state && BlockInput())
        for (int key = 0; key < 256; ++key)
            state[key] &= 1;
    return result;
}

UINT WINAPI HookGetRawInputData(HRAWINPUT handle, UINT command, void* data, UINT* size, UINT headerSize) {
    ScopedCall call;
    UINT result = g_getRawInputData(handle, command, data, size, headerSize);
    if (command == RID_INPUT && data && result != static_cast<UINT>(-1) && result >= sizeof(RAWINPUTHEADER) && BlockInput())
        Neutralize(static_cast<RAWINPUT*>(data));
    return result;
}

#ifdef _WIN64
// 32-bit processes on 64-bit Windows receive 64-bit structures from GetRawInputBuffer,
// so the buffer is only rewritten in 64-bit builds.
UINT WINAPI HookGetRawInputBuffer(RAWINPUT* data, UINT* size, UINT headerSize) {
    ScopedCall call;
    UINT count = g_getRawInputBuffer(data, size, headerSize);
    if (data && count != static_cast<UINT>(-1) && BlockInput()) {
        RAWINPUT* input = data;
        for (UINT i = 0; i < count; ++i) {
            Neutralize(input);
            // NEXTRAWINPUTBLOCK: entries are 8-byte aligned.
            input = reinterpret_cast<RAWINPUT*>(reinterpret_cast<BYTE*>(input) + ((input->header.dwSize + 7) & ~7u));
        }
    }
    return count;
}
#endif

bool IsInputMessage(UINT msg) {
    return (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) ||
           (msg >= WM_KEYFIRST && msg <= WM_KEYLAST) ||
           msg == WM_INPUT || msg == WM_MOUSEHOVER || msg == WM_MOUSELEAVE;
}

// Input the application does not see while the menu is open. Releases reach
// it, so that no key or button stays down in it, and so does keyboard and
// mouse raw input, which the GetRawInputData hook rids of presses and motion.
bool HiddenFromApplication(UINT msg, LPARAM lParam) {
    switch (msg) {
    case WM_KEYUP:
    case WM_SYSKEYUP:
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP:
    case WM_XBUTTONUP:
        return false;
    case WM_INPUT: {
        RAWINPUTHEADER header{};
        UINT size = sizeof(header);
        return !g_getRawInputData ||
               g_getRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_HEADER, &header, &size, sizeof(header)) == static_cast<UINT>(-1) ||
               header.dwType == RIM_TYPEHID;
    }
    default:
        return IsInputMessage(msg);
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    ScopedCall call;
    // An engine call of the assistant, on the main thread of the game.
    remote::RunGameThreadCalls();
    if (msg != 0 && msg == remote::GameThreadMessage())
        return 0;
    WNDPROC original = nullptr;
    bool unicode = false;
    bool handled = false;
    {
        // The thread that presents may be drawing the overlay or destroying its context.
        InputLock lock;
        for (const Subclass& entry : g_subclasses) {
            if (entry.window == hwnd) {
                original = entry.original;
                unicode = entry.unicode;
            }
        }
        if (ImGui::GetCurrentContext() && hwnd == g_window) {
            handled = true;
            Guarded([&] {
                OwnCalls own;
                ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam);
            });
        }
    }
    if (handled && IsMenuOpen() && HiddenFromApplication(msg, lParam)) {
        // The system frees raw input data in DefWindowProc, which also turns
        // Alt+F4 into a request to close the window.
        if (msg == WM_INPUT || (msg == WM_SYSKEYDOWN && wParam == VK_F4))
            return unicode ? DefWindowProcW(hwnd, msg, wParam, lParam) : DefWindowProcA(hwnd, msg, wParam, lParam);
        return 0;
    }
    if (unicode)
        return CallWindowProcW(original, hwnd, msg, wParam, lParam);
    return CallWindowProcA(original, hwnd, msg, wParam, lParam);
}

WNDPROC SetProc(HWND hwnd, bool unicode, WNDPROC proc) {
    LONG_PTR value = reinterpret_cast<LONG_PTR>(proc);
    LONG_PTR previous = unicode ? SetWindowLongPtrW(hwnd, GWLP_WNDPROC, value)
                                : SetWindowLongPtrA(hwnd, GWLP_WNDPROC, value);
    return reinterpret_cast<WNDPROC>(previous);
}

WNDPROC CurrentProc(HWND hwnd, bool unicode) {
    LONG_PTR value = unicode ? GetWindowLongPtrW(hwnd, GWLP_WNDPROC) : GetWindowLongPtrA(hwnd, GWLP_WNDPROC);
    return reinterpret_cast<WNDPROC>(value);
}

Subclass* FindSubclass(HWND window) {
    for (Subclass& entry : g_subclasses)
        if (entry.window == window)
            return &entry;
    return nullptr;
}

// The window procedure installed on a window is a stub outside the library,
// which jumps to the address in its first 8 bytes: WndProc, until Detach().
// When the application subclassed the window after it, its procedure keeps
// calling the stub, which then calls the window's original procedure: the
// library can be unloaded. A stub is never freed, since a message may be
// running it; one page per window the overlay was bound to.
constexpr size_t kStubSize = 128;
constexpr size_t kStubEntry = 16;
constexpr size_t kStubForward = 64;

WNDPROC CreateStub() {
    auto* page = static_cast<uint8_t*>(VirtualAlloc(nullptr, kStubSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!page)
        return nullptr;
    void* target = reinterpret_cast<void*>(&WndProc);
    memcpy(page, &target, sizeof(target));
    uint8_t* code = page + kStubEntry;
    void* slot = page;
#ifdef _WIN64
    // mov rax, slot; jmp [rax]
    code[0] = 0x48;
    code[1] = 0xB8;
    memcpy(code + 2, &slot, 8);
    code[10] = 0xFF;
    code[11] = 0x20;
#else
    // jmp [slot]
    code[0] = 0xFF;
    code[1] = 0x25;
    memcpy(code + 2, &slot, 4);
#endif
    FlushInstructionCache(GetCurrentProcess(), page, kStubSize);
    return reinterpret_cast<WNDPROC>(code);
}

// Points the stub at code that calls CallWindowProc(original, ...), for once WndProc is gone.
void ForwardStub(WNDPROC stub, WNDPROC original, bool unicode) {
    uint8_t* page = reinterpret_cast<uint8_t*>(stub) - kStubEntry;
    uint8_t* code = page + kStubForward;
    void* call = reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"), unicode ? "CallWindowProcW" : "CallWindowProcA"));
    void* previous = reinterpret_cast<void*>(original);
#ifdef _WIN64
    // The arguments move one register, or the stack, to the right:
    // sub rsp, 38h; mov [rsp+20h], r9; mov r9, r8; mov r8d, edx; mov rdx, rcx;
    // mov rcx, original; mov rax, CallWindowProc; call rax; add rsp, 38h; ret
    const uint8_t head[] = {0x48, 0x83, 0xEC, 0x38, 0x4C, 0x89, 0x4C, 0x24, 0x20, 0x4D, 0x89, 0xC1, 0x41, 0x89, 0xD0, 0x48, 0x89, 0xCA, 0x48, 0xB9};
    const uint8_t middle[] = {0x48, 0xB8};
    const uint8_t tail[] = {0xFF, 0xD0, 0x48, 0x83, 0xC4, 0x38, 0xC3};
    size_t at = 0;
    memcpy(code + at, head, sizeof(head));
    at += sizeof(head);
    memcpy(code + at, &previous, 8);
    at += 8;
    memcpy(code + at, middle, sizeof(middle));
    at += sizeof(middle);
    memcpy(code + at, &call, 8);
    at += 8;
    memcpy(code + at, tail, sizeof(tail));
#else
    // The original procedure goes in as the first argument, under the return
    // address; CallWindowProc removes it with the others:
    // pop eax; push original; push eax; mov eax, CallWindowProc; jmp eax
    code[0] = 0x58;
    code[1] = 0x68;
    memcpy(code + 2, &previous, 4);
    code[6] = 0x50;
    code[7] = 0xB8;
    memcpy(code + 8, &call, 4);
    code[12] = 0xFF;
    code[13] = 0xE0;
#endif
    FlushInstructionCache(GetCurrentProcess(), code, kStubSize - kStubForward);
    void* forward = code;
    InterlockedExchangePointer(reinterpret_cast<void* volatile*>(page), forward);
}

// Points a forwarded stub at WndProc again, when the overlay comes back.
void RestoreStub(WNDPROC stub) {
    void* target = reinterpret_cast<void*>(&WndProc);
    InterlockedExchangePointer(reinterpret_cast<void* volatile*>(reinterpret_cast<uint8_t*>(stub) - kStubEntry), target);
}

struct WindowSearch {
    DWORD process;
    long width;
    long height;
    HWND best;
    long bestArea;
    HWND sized;
};

BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM param) {
    auto* search = reinterpret_cast<WindowSearch*>(param);
    DWORD process = 0;
    GetWindowThreadProcessId(hwnd, &process);
    if (process != search->process || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER))
        return TRUE;
    RECT rect{};
    GetClientRect(hwnd, &rect);
    if (search->width && rect.right == search->width && rect.bottom == search->height && !search->sized)
        search->sized = hwnd;
    long area = (rect.right - rect.left) * (rect.bottom - rect.top);
    if (area > search->bestArea) {
        search->best = hwnd;
        search->bestArea = area;
    }
    return TRUE;
}

template <typename Fn>
void Hook(HMODULE module, const char* name, Fn detour, Fn* original) {
    if (void* target = GetProc<void*>(module, name))
        CreateHook(name, target, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(original));
}

} // namespace

OwnCalls::OwnCalls() { ++g_ownCalls; }
OwnCalls::~OwnCalls() { --g_ownCalls; }

bool BlockInput() { return g_ownCalls == 0 && IsMenuOpen(); }

SHORT RealAsyncKeyState(int key) {
    return g_getAsyncKeyState ? g_getAsyncKeyState(key) : GetAsyncKeyState(key);
}

void UninstallHooks() {
    g_setCursorPos = nullptr;
    g_clipCursor = nullptr;
    g_getCursorPos = nullptr;
    g_getAsyncKeyState = nullptr;
    g_getKeyState = nullptr;
    g_getKeyboardState = nullptr;
    g_getRawInputData = nullptr;
    g_getRawInputBuffer = nullptr;
}

void InstallHooks() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32)
        return;
    Hook(user32, "SetCursorPos", HookSetCursorPos, &g_setCursorPos);
    Hook(user32, "ClipCursor", HookClipCursor, &g_clipCursor);
    Hook(user32, "GetCursorPos", HookGetCursorPos, &g_getCursorPos);
    Hook(user32, "GetAsyncKeyState", HookGetAsyncKeyState, &g_getAsyncKeyState);
    Hook(user32, "GetKeyState", HookGetKeyState, &g_getKeyState);
    Hook(user32, "GetKeyboardState", HookGetKeyboardState, &g_getKeyboardState);
    Hook(user32, "GetRawInputData", HookGetRawInputData, &g_getRawInputData);
#ifdef _WIN64
    Hook(user32, "GetRawInputBuffer", HookGetRawInputBuffer, &g_getRawInputBuffer);
#endif
}

void Attach(HWND window) {
    if (g_window)
        Detach();
    Subclass* entry = FindSubclass(window);
    // Subclassing the window again would make WndProc call itself through
    // the procedure installed after it.
    if (entry && entry->chained) {
        g_window = window;
        return;
    }
    // Still in the chain, below the application's procedure.
    if (entry && entry->forwarded) {
        RestoreStub(entry->stub);
        entry->chained = true;
        entry->forwarded = false;
        g_window = window;
        return;
    }
    bool unicode = IsWindowUnicode(window) != FALSE;
    WNDPROC stub = CreateStub();
    WNDPROC original = stub ? SetProc(window, unicode, stub) : nullptr;
    if (!original) {
        Log("SetWindowLongPtr failed on %p: %lu", window, GetLastError());
        return;
    }
    if (entry)
        *entry = {window, original, unicode, true, stub, false};
    else
        g_subclasses.push_back({window, original, unicode, true, stub, false});
    g_window = window;
}

void Detach() {
    if (!g_window)
        return;
    Subclass* entry = FindSubclass(g_window);
    if (!IsWindow(g_window)) {
        entry->chained = false;
    } else if (CurrentProc(g_window, entry->unicode) == entry->stub) {
        SetProc(g_window, entry->unicode, entry->original);
        entry->chained = false;
    } else {
        Log("Window %p was subclassed again by the application: its procedure now calls the original one directly", g_window);
        ForwardStub(entry->stub, entry->original, entry->unicode);
        entry->chained = false;
        entry->forwarded = true;
    }
    g_window = nullptr;
}

void OnMenuToggled(bool open) {
    // The real functions stand in while the hooks are being installed.
    ClipCursorFn clipCursor = g_clipCursor ? g_clipCursor : ClipCursor;
    if (open) {
        {
            // An unconfined cursor reports the whole virtual screen.
            std::lock_guard<std::mutex> lock(g_clipMutex);
            RECT desktop{GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN)};
            desktop.right = desktop.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
            desktop.bottom = desktop.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
            g_clipped = GetClipCursor(&g_clip) && !EqualRect(&g_clip, &desktop);
        }
        clipCursor(nullptr);
        GetCursorPosFn getCursorPos = g_getCursorPos ? g_getCursorPos : GetCursorPos;
        g_cursorFrozen = getCursorPos(&g_frozenCursor) != FALSE;
        return;
    }
    if (g_cursorFrozen.exchange(false)) {
        // The application finds the cursor where it left it.
        SetCursorPosFn setCursorPos = g_setCursorPos ? g_setCursorPos : SetCursorPos;
        setCursorPos(g_frozenCursor.x, g_frozenCursor.y);
    }
    std::lock_guard<std::mutex> lock(g_clipMutex);
    if (g_clipped)
        clipCursor(&g_clip);
}

HWND FindProcessWindow(uint32_t width, uint32_t height) {
    WindowSearch search{GetCurrentProcessId(), static_cast<long>(width), static_cast<long>(height), nullptr, -1, nullptr};
    HWND foreground = GetForegroundWindow();
    DWORD process = 0;
    bool ownForeground = foreground && GetWindowThreadProcessId(foreground, &process) && process == search.process;
    RECT client{};
    if (ownForeground && (!width || (GetClientRect(foreground, &client) && client.right == search.width && client.bottom == search.height)))
        return foreground;
    EnumWindows(EnumWindowsProc, reinterpret_cast<LPARAM>(&search));
    if (search.sized)
        return search.sized;
    return ownForeground ? foreground : search.best;
}

} // namespace uih::detail::input
