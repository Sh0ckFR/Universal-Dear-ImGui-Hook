#include "internal.h"

#include <winternl.h>

#include <cstdarg>
#include <cstdio>
#include <cwchar>

#include "imgui_impl_win32.h"

namespace uih {
namespace detail {
namespace {

enum class State { Stopped, Running, Stopping };

struct RendererSlot {
    const Renderer* renderer;
    bool attempted = false;
    bool installed = false;
};

Config g_config;
std::mutex g_lifecycle;
std::atomic<State> g_state{State::Stopped};
std::atomic<Backend> g_activeBackend{Backend::None};
std::atomic<bool> g_menuOpen{false};
std::atomic<int> g_inFlight{0};

std::atomic<bool> g_teardownPending{false};
HANDLE g_teardownDone = nullptr;

HANDLE g_worker = nullptr;
HANDLE g_stopEvent = nullptr;
HANDLE g_wakeEvent = nullptr;
void* g_dllNotificationCookie = nullptr;
std::atomic<bool> g_unloadRequested{false};

RendererSlot g_renderers[] = {
    {&kVulkanRenderer},
    {&kD3D9Renderer},
    {&kDxgiRenderer},
    {&kD3D12QueueRenderer},
};

HWND g_window = nullptr;
bool g_menuKeyDown = false;
bool g_unloadKeyDown = false;

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
    return WantBackend(Backend::Vulkan);
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

    input::InstallCursorHooks();
    InstallLoadedRenderers();

    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto registerFn = GetProc<LdrRegisterDllNotificationFn>(ntdll, "LdrRegisterDllNotification");
    auto unregisterFn = GetProc<LdrUnregisterDllNotificationFn>(ntdll, "LdrUnregisterDllNotification");
    if (registerFn && unregisterFn && registerFn(0, OnDllNotification, nullptr, &g_dllNotificationCookie) != 0)
        g_dllNotificationCookie = nullptr;

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
    Stop();
    FreeLibraryAndExitThread(static_cast<HMODULE>(module), 0);
}

void RequestUnload() {
    if (!g_config.module || g_unloadRequested.exchange(true))
        return;
    Log("Unload requested");
    HANDLE thread = CreateThread(nullptr, 0, UnloadThread, g_config.module, 0, nullptr);
    if (thread)
        CloseHandle(thread);
}

bool KeyPressed(int key, bool& wasDown, bool focused) {
    bool down = (GetAsyncKeyState(key) & 0x8000) != 0;
    bool pressed = down && !wasDown && focused;
    wasDown = down;
    return pressed;
}

void PollKeys() {
    HWND foreground = GetForegroundWindow();
    bool focused = foreground && (foreground == g_window || IsChild(g_window, foreground));
    if (KeyPressed(g_config.menuKey, g_menuKeyDown, focused))
        SetMenuOpen(!g_menuOpen);
    if (KeyPressed(g_config.unloadKey, g_unloadKeyDown, focused))
        RequestUnload();
}

bool WaitForIdleHooks(DWORD timeoutMs) {
    ULONGLONG deadline = GetTickCount64() + timeoutMs;
    while (g_inFlight > 0) {
        if (GetTickCount64() >= deadline)
            return false;
        Sleep(1);
    }
    return true;
}

} // namespace

void Log(const char* fmt, ...) {
    if (!g_config.debugLog)
        return;
    char buffer[1024];
    int offset = snprintf(buffer, sizeof(buffer), "[uih] ");
    va_list args;
    va_start(args, fmt);
    offset += vsnprintf(buffer + offset, sizeof(buffer) - offset - 1, fmt, args);
    va_end(args);
    if (offset < 0 || offset >= static_cast<int>(sizeof(buffer)) - 1)
        offset = static_cast<int>(sizeof(buffer)) - 2;
    buffer[offset] = '\n';
    buffer[offset + 1] = '\0';
    OutputDebugStringA(buffer);
}

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

ScopedCall::ScopedCall() { ++g_inFlight; }
ScopedCall::~ScopedCall() { --g_inFlight; }

thread_local int g_foreignPresentDepth = 0;

ForeignPresentScope::ForeignPresentScope() { ++g_foreignPresentDepth; }
ForeignPresentScope::~ForeignPresentScope() { --g_foreignPresentDepth; }

bool InsideForeignPresent() { return g_foreignPresentDepth > 0; }

const Config& GetConfig() { return g_config; }

bool WantBackend(Backend backend) {
    return g_config.backend == Backend::None || g_config.backend == backend;
}

bool ShouldRender(Backend backend) {
    if (g_state != State::Running || !WantBackend(backend))
        return false;
    Backend active = g_activeBackend;
    return active == Backend::None || active == backend;
}

bool ClaimBackend(Backend backend) {
    Backend expected = Backend::None;
    if (g_activeBackend.compare_exchange_strong(expected, backend) || expected == backend) {
        Log("%s backend active", BackendName(backend));
        return true;
    }
    return false;
}

void ServiceTeardown(Backend backend, void (*teardown)()) {
    if (g_activeBackend != backend || !g_teardownPending.exchange(false))
        return;
    teardown();
    SetEvent(g_teardownDone);
}

bool BindWindow(HWND window) {
    if (!window || !IsWindow(window)) {
        Log("Cannot bind to invalid window %p", window);
        return false;
    }
    if (g_window == window && ImGui::GetCurrentContext())
        return true;

    if (!ImGui::GetCurrentContext()) {
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        ImGui::StyleColorsDark();
    } else {
        input::Detach();
        ImGui_ImplWin32_Shutdown();
    }

    g_window = window;
    ImGui_ImplWin32_Init(window);
    input::Attach(window);
    Log("Bound to window %p", window);
    return true;
}

ImDrawData* Frame() {
    ImGui_ImplWin32_NewFrame();
    PollKeys();
    ImGui::GetIO().MouseDrawCursor = g_menuOpen;
    ImGui::NewFrame();
    if (g_config.render)
        g_config.render();
    ImGui::Render();
    return ImGui::GetDrawData();
}

void UnbindWindow() {
    if (!ImGui::GetCurrentContext())
        return;
    input::Detach();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    g_window = nullptr;
}

} // namespace detail

using namespace detail;

void Start(const Config& config) {
    std::lock_guard<std::mutex> lock(g_lifecycle);
    if (g_state != State::Stopped)
        return;

    g_config = config;
    g_activeBackend = Backend::None;
    g_menuOpen = false;
    g_unloadRequested = false;
    g_teardownPending = false;
    for (RendererSlot& slot : g_renderers)
        slot.attempted = slot.installed = false;

    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_teardownDone = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_state = State::Running;
    g_worker = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    if (!g_worker) {
        Log("CreateThread failed: %lu", GetLastError());
        g_state = State::Stopped;
    }
}

void Stop() {
    std::lock_guard<std::mutex> lock(g_lifecycle);
    if (g_state != State::Running)
        return;
    g_state = State::Stopping;
    Log("Stopping");

    SetEvent(g_stopEvent);
    WaitForSingleObject(g_worker, INFINITE);
    CloseHandle(g_worker);
    g_worker = nullptr;

    // Release GPU resources on the thread that renders. Fall back to this
    // thread when the application stopped presenting.
    Backend active = g_activeBackend;
    if (active != Backend::None) {
        g_teardownPending = true;
        if (WaitForSingleObject(g_teardownDone, 1000) != WAIT_OBJECT_0 && g_teardownPending.exchange(false)) {
            Log("Rendering thread did not respond, releasing resources from Stop()");
            for (RendererSlot& slot : g_renderers)
                if (slot.installed)
                    slot.renderer->Teardown();
        }
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
    CloseHandle(g_teardownDone);
    g_stopEvent = g_wakeEvent = g_teardownDone = nullptr;
    g_activeBackend = Backend::None;
    g_state = State::Stopped;
    Log("Stopped");
}

Backend ActiveBackend() { return g_activeBackend; }

bool IsMenuOpen() { return g_menuOpen; }

void SetMenuOpen(bool open) {
    if (g_menuOpen.exchange(open) == open)
        return;
    Log("Menu %s", open ? "opened" : "closed");
    if (open)
        input::ReleaseCursor();
}

const char* BackendName(Backend backend) {
    switch (backend) {
    case Backend::D3D9: return "D3D9";
    case Backend::D3D10: return "D3D10";
    case Backend::D3D11: return "D3D11";
    case Backend::D3D12: return "D3D12";
    case Backend::Vulkan: return "Vulkan";
    default: return "None";
    }
}

} // namespace uih
