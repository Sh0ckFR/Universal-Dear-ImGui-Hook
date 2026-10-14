#include "internal.h"

#include <winternl.h>

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
    FreeLibraryAndExitThread(static_cast<HMODULE>(module), 0);
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
    // thread when the application stopped presenting.
    if (!TeardownOnRenderThread(1000)) {
        Log("Rendering thread did not respond, releasing resources from Stop()");
        for (RendererSlot& slot : g_renderers)
            if (slot.installed)
                slot.renderer->Teardown();
    }

    MH_DisableHook(MH_ALL_HOOKS);
    if (!WaitForIdleHooks(2000))
        Log("Hooks still running after timeout");
    Sleep(100);
    MH_RemoveHook(MH_ALL_HOOKS);
    MH_Uninitialize();

    for (RendererSlot& slot : g_renderers)
        if (slot.attempted)
            slot.renderer->Uninstall();

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

void Unload() {
    HMODULE module = GetConfig().module;
    if (!module || g_unloadRequested.exchange(true))
        return;
    Log("Unload requested");
    HANDLE thread = CreateThread(nullptr, 0, UnloadThread, module, 0, nullptr);
    if (thread)
        CloseHandle(thread);
}

} // namespace platform

} // namespace uih::detail
