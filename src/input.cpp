#include "internal.h"

#include "imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace uih::detail::input {
namespace {

using SetCursorPosFn = BOOL(WINAPI*)(int, int);
using ClipCursorFn = BOOL(WINAPI*)(const RECT*);

SetCursorPosFn g_setCursorPos = nullptr;
ClipCursorFn g_clipCursor = nullptr;

HWND g_window = nullptr;
WNDPROC g_originalProc = nullptr;
bool g_unicode = false;

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

bool IsInputMessage(UINT msg) {
    return (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) ||
           (msg >= WM_KEYFIRST && msg <= WM_KEYLAST) ||
           msg == WM_INPUT || msg == WM_MOUSEHOVER || msg == WM_MOUSELEAVE;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    ScopedCall call;
    if (ImGui::GetCurrentContext() && hwnd == g_window) {
        ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam);
        if (IsMenuOpen() && IsInputMessage(msg))
            return 0;
    }
    if (g_unicode)
        return CallWindowProcW(g_originalProc, hwnd, msg, wParam, lParam);
    return CallWindowProcA(g_originalProc, hwnd, msg, wParam, lParam);
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
    HWND best;
    long bestArea;
};

BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM param) {
    auto* search = reinterpret_cast<WindowSearch*>(param);
    DWORD process = 0;
    GetWindowThreadProcessId(hwnd, &process);
    if (process != search->process || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER))
        return TRUE;
    RECT rect{};
    GetClientRect(hwnd, &rect);
    long area = (rect.right - rect.left) * (rect.bottom - rect.top);
    if (area > search->bestArea) {
        search->best = hwnd;
        search->bestArea = area;
    }
    return TRUE;
}

} // namespace

void InstallCursorHooks() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32)
        return;
    if (void* target = GetProc<void*>(user32, "SetCursorPos"))
        CreateHook("SetCursorPos", target, reinterpret_cast<void*>(HookSetCursorPos), reinterpret_cast<void**>(&g_setCursorPos));
    if (void* target = GetProc<void*>(user32, "ClipCursor"))
        CreateHook("ClipCursor", target, reinterpret_cast<void*>(HookClipCursor), reinterpret_cast<void**>(&g_clipCursor));
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
    g_window = nullptr;
    g_originalProc = nullptr;
}

void ReleaseCursor() {
    if (g_clipCursor)
        g_clipCursor(nullptr);
}

HWND FindProcessWindow() {
    WindowSearch search{GetCurrentProcessId(), nullptr, -1};
    HWND foreground = GetForegroundWindow();
    DWORD process = 0;
    if (foreground && GetWindowThreadProcessId(foreground, &process) && process == search.process)
        return foreground;
    EnumWindows(EnumWindowsProc, reinterpret_cast<LPARAM>(&search));
    return search.best;
}

} // namespace uih::detail::input
