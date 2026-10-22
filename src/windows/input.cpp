#include "internal.h"

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

HWND g_window = nullptr;
WNDPROC g_originalProc = nullptr;
bool g_unicode = false;

thread_local int g_ownCalls = 0;
// Cursor position the application sees while the menu is open.
POINT g_frozenCursor{};
std::atomic<bool> g_cursorFrozen{false};

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
    if (IsMenuOpen())
        return TRUE;
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

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    ScopedCall call;
    WNDPROC original = nullptr;
    bool unicode = false;
    bool handled = false;
    {
        // The thread that presents may be drawing the overlay or destroying its context.
        InputLock lock;
        original = g_originalProc;
        unicode = g_unicode;
        if (ImGui::GetCurrentContext() && hwnd == g_window) {
            handled = true;
            Guarded([&] {
                OwnCalls own;
                ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam);
            });
        }
    }
    if (handled && IsMenuOpen() && IsInputMessage(msg)) {
        // The system frees raw input data in DefWindowProc.
        if (msg == WM_INPUT)
            return unicode ? DefWindowProcW(hwnd, msg, wParam, lParam) : DefWindowProcA(hwnd, msg, wParam, lParam);
        return 0;
    }
    if (unicode)
        return CallWindowProcW(original, hwnd, msg, wParam, lParam);
    return CallWindowProcA(original, hwnd, msg, wParam, lParam);
}

WNDPROC SetProc(HWND hwnd, WNDPROC proc) {
    LONG_PTR value = reinterpret_cast<LONG_PTR>(proc);
    LONG_PTR previous = g_unicode ? SetWindowLongPtrW(hwnd, GWLP_WNDPROC, value)
                                  : SetWindowLongPtrA(hwnd, GWLP_WNDPROC, value);
    return reinterpret_cast<WNDPROC>(previous);
}

WNDPROC CurrentProc(HWND hwnd) {
    LONG_PTR value = g_unicode ? GetWindowLongPtrW(hwnd, GWLP_WNDPROC) : GetWindowLongPtrA(hwnd, GWLP_WNDPROC);
    return reinterpret_cast<WNDPROC>(value);
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
    g_window = window;
    g_unicode = IsWindowUnicode(window) != FALSE;
    g_originalProc = SetProc(window, WndProc);
    if (!g_originalProc) {
        Log("SetWindowLongPtr failed on %p: %lu", window, GetLastError());
        g_window = nullptr;
    }
}

void Detach() {
    if (!g_window)
        return;
    if (IsWindow(g_window)) {
        if (CurrentProc(g_window) == WndProc)
            SetProc(g_window, g_originalProc);
        else
            Log("Window %p was re-subclassed by someone else, leaving its procedure in place", g_window);
    }
    // g_originalProc stays valid for a message that is still being dispatched.
    g_window = nullptr;
}

void OnMenuToggled(bool open) {
    if (open) {
        // The real functions stand in while the hooks are being installed.
        ClipCursorFn clipCursor = g_clipCursor ? g_clipCursor : ClipCursor;
        clipCursor(nullptr);
        GetCursorPosFn getCursorPos = g_getCursorPos ? g_getCursorPos : GetCursorPos;
        g_cursorFrozen = getCursorPos(&g_frozenCursor) != FALSE;
    } else if (g_cursorFrozen.exchange(false)) {
        // The application finds the cursor where it left it.
        SetCursorPosFn setCursorPos = g_setCursorPos ? g_setCursorPos : SetCursorPos;
        setCursorPos(g_frozenCursor.x, g_frozenCursor.y);
    }
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
