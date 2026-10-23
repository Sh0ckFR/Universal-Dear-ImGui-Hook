#include "internal.h"

#include <winternl.h>

#include <malloc.h>

#include <cwchar>

#include "imgui_impl_win32.h"

namespace uih::detail {
namespace {

struct RendererSlot {
    const Renderer* renderer;
    bool attempted = false;
    bool installed = false;
};

HANDLE g_worker = nullptr;
HANDLE g_stopEvent = nullptr;
HANDLE g_wakeEvent = nullptr;
void* g_dllNotificationCookie = nullptr;
std::atomic<bool> g_unloadRequested{false};
// False when Stop() left a thread inside a hook: the module must stay loaded.
std::atomic<bool> g_hooksIdle{true};

RendererSlot g_renderers[] = {
    {&kVulkanRenderer},
    {&kOpenGLRenderer},
    {&kD3D9Renderer},
    {&kDxgiRenderer},
    {&kD3D12QueueRenderer},
    {&kDirectInputHooks},
    {&kXInput14Hooks},
    {&kXInput13Hooks},
    {&kXInput910Hooks},
};

HWND g_window = nullptr;

// ntdll DLL load notifications -------------------------------------------

constexpr ULONG kDllLoaded = 1;

struct LdrDllLoadedNotificationData {
    ULONG Flags;
    const UNICODE_STRING* FullDllName;
    const UNICODE_STRING* BaseDllName;
    void* DllBase;
    ULONG SizeOfImage;
};

using LdrDllNotificationFunction = void(CALLBACK*)(ULONG reason, const void* data, void* context);
using LdrRegisterDllNotificationFn = NTSTATUS(NTAPI*)(ULONG flags, LdrDllNotificationFunction fn, void* context, void** cookie);
using LdrUnregisterDllNotificationFn = NTSTATUS(NTAPI*)(void* cookie);

bool RendererWanted(const Renderer* renderer) {
    if (renderer == &kDxgiRenderer)
        return WantBackend(Backend::D3D10) || WantBackend(Backend::D3D11) || WantBackend(Backend::D3D12);
    if (renderer == &kD3D9Renderer)
        return WantBackend(Backend::D3D9);
    if (renderer == &kD3D12QueueRenderer)
        return WantBackend(Backend::D3D12);
    if (renderer == &kOpenGLRenderer)
        return WantBackend(Backend::OpenGL);
    if (renderer == &kVulkanRenderer)
        return WantBackend(Backend::Vulkan);
    return true;
}

bool IsWatchedModule(const UNICODE_STRING* name) {
    if (!name || !name->Buffer)
        return false;
    for (const RendererSlot& slot : g_renderers) {
        const wchar_t* module = slot.renderer->module;
        size_t length = wcslen(module);
        if (name->Length == length * sizeof(wchar_t) && _wcsnicmp(name->Buffer, module, length) == 0)
            return true;
    }
    return false;
}

void CALLBACK OnDllNotification(ULONG reason, const void* data, void*) {
    if (reason != kDllLoaded)
        return;
    auto* loaded = static_cast<const LdrDllLoadedNotificationData*>(data);
    if (IsWatchedModule(loaded->BaseDllName))
        SetEvent(g_wakeEvent);
}

void InstallLoadedRenderers() {
    for (RendererSlot& slot : g_renderers) {
        if (slot.attempted || !RendererWanted(slot.renderer) || !GetModuleHandleW(slot.renderer->module))
            continue;
        slot.attempted = true;
        slot.installed = slot.renderer->Install();
        Log("%s hooks %s", slot.renderer->name, slot.installed ? "installed" : "unavailable");
    }
}

DWORD WINAPI Worker(void*) {
    MH_STATUS status = MH_Initialize();
    if (status != MH_OK) {
        Log("MH_Initialize failed: %s", MH_StatusToString(status));
        return 1;
    }

    input::InstallHooks();

    // Registered before the first pass: a module loaded meanwhile wakes the loop below.
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto registerFn = GetProc<LdrRegisterDllNotificationFn>(ntdll, "LdrRegisterDllNotification");
    auto unregisterFn = GetProc<LdrUnregisterDllNotificationFn>(ntdll, "LdrUnregisterDllNotification");
    if (registerFn && unregisterFn && registerFn(0, OnDllNotification, nullptr, &g_dllNotificationCookie) != 0)
        g_dllNotificationCookie = nullptr;

    InstallLoadedRenderers();

    HANDLE events[] = {g_stopEvent, g_wakeEvent};
    while (WaitForMultipleObjects(2, events, FALSE, INFINITE) == WAIT_OBJECT_0 + 1)
        InstallLoadedRenderers();

    if (g_dllNotificationCookie) {
        unregisterFn(g_dllNotificationCookie);
        g_dllNotificationCookie = nullptr;
    }
    return 0;
}

DWORD WINAPI UnloadThread(void* module) {
    uih::Stop();
    if (!g_hooksIdle) {
        Log("A thread is still inside a hook, the library stays loaded");
        return 0;
    }
    FreeLibraryAndExitThread(static_cast<HMODULE>(module), 0);
}

DWORD WINAPI RemoveThread(void*) {
    StopUntilMenuInput();
    return 0;
}

// Once no thread runs the hooks: MinHook frees the trampolines they call
// the application through, and Uninstall() clears the pointers to them.
void RemoveHooks() {
    Sleep(100);
    MH_RemoveHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    input::UninstallHooks();
    for (RendererSlot& slot : g_renderers)
        if (slot.attempted)
            slot.renderer->Uninstall();
}

void TeardownRenderers() {
    for (RendererSlot& slot : g_renderers)
        if (slot.installed)
            Guarded([&] { slot.renderer->Teardown(); });
}

bool ApplicationFocused() {
    DWORD process = 0;
    HWND foreground = GetForegroundWindow();
    return foreground && GetWindowThreadProcessId(foreground, &process) && process == GetCurrentProcessId();
}

// The first XInput controller, read directly: the hooks are removed.
WORD ControllerButtons() {
    using XInputGetStateFn = DWORD(WINAPI*)(DWORD, void*);
    static const auto getState = [] {
        HMODULE xinput = LoadLibraryW(L"xinput1_4.dll");
        return xinput ? GetProc<XInputGetStateFn>(xinput, "XInputGetState") : nullptr;
    }();
    // XINPUT_STATE: packet number, then the buttons.
    struct {
        DWORD packet;
        WORD buttons;
        BYTE rest[10];
    } state{};
    return getState && getState(0, &state) == ERROR_SUCCESS ? state.buttons : 0;
}

} // namespace

bool CreateHook(const char* name, void* target, void* detour, void** original) {
    MH_STATUS status = MH_CreateHook(target, detour, original);
    if (status == MH_OK)
        status = MH_EnableHook(target);
    if (status != MH_OK) {
        Log("Hooking %s at %p failed: %s", name, target, MH_StatusToString(status));
        return false;
    }
    Log("Hooked %s at %p", name, target);
    return true;
}

bool BindWindow(HWND window) {
    if (!window || !IsWindow(window)) {
        Log("Cannot bind to invalid window %p", window);
        return false;
    }
    if (g_window == window && ImGui::GetCurrentContext())
        return true;

    InputLock lock;
    if (!CreateContext()) {
        input::Detach();
        ImGui_ImplWin32_Shutdown();
    }

    g_window = window;
    ImGui_ImplWin32_Init(window);
    input::Attach(window);
    Log("Bound to window %p", window);
    return true;
}

void UnbindWindow() {
    InputLock lock;
    if (!ImGui::GetCurrentContext())
        return;
    input::Detach();
    ImGui_ImplWin32_Shutdown();
    DestroyContext();
    g_window = nullptr;
}

namespace platform {

void WriteLog(const char* line) { OutputDebugStringA(line); }

bool Start() {
    if (!g_hooksIdle) {
        if (!WaitForIdleHooks(0)) {
            Log("A thread is still inside a hook, the hook cannot start yet");
            return false;
        }
        RemoveHooks();
        g_hooksIdle = true;
    }
    g_unloadRequested = false;
    for (RendererSlot& slot : g_renderers)
        slot.attempted = slot.installed = false;

    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_worker = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    if (!g_worker) {
        Log("CreateThread failed: %lu", GetLastError());
        return false;
    }
    return true;
}

void Stop() {
    SetEvent(g_stopEvent);
    WaitForSingleObject(g_worker, INFINITE);
    CloseHandle(g_worker);
    g_worker = nullptr;

    // Release GPU resources on the thread that renders. Fall back to this
    // thread when the application stopped presenting, but not while a frame
    // of the overlay still uses them: that thread releases them afterwards.
    if (!TeardownOnRenderThread(1000)) {
        Log("Rendering thread did not respond, releasing resources from Stop()");
        if (!ReleaseOverlayWithin(2000, TeardownRenderers))
            Log("The overlay is still drawing, it is released when the frame ends");
    }

    MH_DisableHook(MH_ALL_HOOKS);
    g_hooksIdle = WaitForIdleHooks(2000);
    if (g_hooksIdle)
        RemoveHooks();
    else
        Log("A thread is still inside a hook, the hooks are removed on the next start");

    CloseHandle(g_stopEvent);
    CloseHandle(g_wakeEvent);
    g_stopEvent = g_wakeEvent = nullptr;
}

void NewFrame() {
    input::OwnCalls own;
    ImGui_ImplWin32_NewFrame();
}

bool IsKeyDown(int key) { return (input::RealAsyncKeyState(key) & 0x8000) != 0; }

float WindowScale() {
    using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
    static auto getDpiForWindow = GetProc<GetDpiForWindowFn>(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    UINT dpi = getDpiForWindow && g_window ? getDpiForWindow(g_window) : 0;
    return dpi ? static_cast<float>(dpi) / USER_DEFAULT_SCREEN_DPI : 1.0f;
}

bool IsFocused() {
    HWND foreground = GetForegroundWindow();
    return foreground && (foreground == g_window || IsChild(g_window, foreground));
}

// Without a module to free, as when the DLL stands in for dinput8.dll, the
// overlay is removed and comes back with the menu input.
void Unload() {
    if (g_unloadRequested.exchange(true))
        return;
    HMODULE module = GetConfig().module;
    Log("%s requested", module ? "Unload" : "Removal");
    HANDLE thread = CreateThread(nullptr, 0, module ? UnloadThread : RemoveThread, module, 0, nullptr);
    if (thread)
        CloseHandle(thread);
}

bool HasFocus(HWND window) {
    HWND foreground = GetForegroundWindow();
    return window && foreground && (foreground == window || foreground == GetAncestor(window, GA_ROOT));
}

bool RunGuarded(void (*work)(void*), void* context) {
#ifdef _MSC_VER
    DWORD code = 0;
    __try {
        work(context);
        return true;
    } __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
    }
    if (code == EXCEPTION_STACK_OVERFLOW)
        _resetstkoflw();
    OnFault(code);
    return false;
#else
    // MinGW-w64 has no structured exception handling.
    work(context);
    return true;
#endif
}

bool MenuInputDown(const Config& config) {
    if (!ApplicationFocused())
        return false;
    if (config.menuKey && (GetAsyncKeyState(config.menuKey) & 0x8000))
        return true;
    return config.menuButtons && (ControllerButtons() & config.menuButtons) == config.menuButtons;
}

} // namespace platform

} // namespace uih::detail
