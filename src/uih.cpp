#include "internal.h"

#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <thread>

namespace uih {
namespace detail {
namespace {

enum class State { Stopped, Running, Stopping };

Config g_config;
std::mutex g_lifecycle;
std::atomic<State> g_state{State::Stopped};
std::atomic<Backend> g_activeBackend{Backend::None};
std::atomic<bool> g_menuOpen{false};
std::atomic<int> g_inFlight{0};

std::atomic<bool> g_teardownPending{false};
std::mutex g_teardownMutex;
std::condition_variable g_teardownSignal;
bool g_teardownDone = false;

bool g_menuKeyDown = false;
bool g_unloadKeyDown = false;

bool KeyPressed(int key, bool& wasDown, bool focused) {
    bool down = platform::IsKeyDown(key);
    bool pressed = down && !wasDown && focused;
    wasDown = down;
    return pressed;
}

void PollKeys() {
    bool focused = platform::IsFocused();
    if (KeyPressed(g_config.menuKey, g_menuKeyDown, focused))
        SetMenuOpen(!g_menuOpen);
    if (KeyPressed(g_config.unloadKey, g_unloadKeyDown, focused))
        platform::Unload();
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
    platform::WriteLog(buffer);
}

ScopedCall::ScopedCall() { ++g_inFlight; }
ScopedCall::~ScopedCall() { --g_inFlight; }

bool WaitForIdleHooks(unsigned timeoutMs) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (g_inFlight > 0) {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

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
    {
        std::lock_guard<std::mutex> lock(g_teardownMutex);
        g_teardownDone = true;
    }
    g_teardownSignal.notify_all();
}

bool TeardownOnRenderThread(unsigned timeoutMs) {
    if (g_activeBackend == Backend::None)
        return true;
    std::unique_lock<std::mutex> lock(g_teardownMutex);
    g_teardownDone = false;
    g_teardownPending = true;
    if (g_teardownSignal.wait_for(lock, std::chrono::milliseconds(timeoutMs), [] { return g_teardownDone; }))
        return true;
    return !g_teardownPending.exchange(false);
}

bool CreateContext() {
    if (ImGui::GetCurrentContext())
        return false;
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    return true;
}

ImDrawData* Frame() {
    platform::NewFrame();
    PollKeys();
    ImGui::GetIO().MouseDrawCursor = g_menuOpen;
    ImGui::NewFrame();
    if (g_config.render)
        g_config.render();
    ImGui::Render();
    return ImGui::GetDrawData();
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
    g_teardownPending = false;
    g_state = State::Running;
    if (!platform::Start())
        g_state = State::Stopped;
}

void Stop() {
    std::lock_guard<std::mutex> lock(g_lifecycle);
    if (g_state != State::Running)
        return;
    g_state = State::Stopping;
    Log("Stopping");

    platform::Stop();

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
    input::OnMenuToggled(open);
}

const char* BackendName(Backend backend) {
    switch (backend) {
    case Backend::D3D9: return "D3D9";
    case Backend::D3D10: return "D3D10";
    case Backend::D3D11: return "D3D11";
    case Backend::D3D12: return "D3D12";
    case Backend::Vulkan: return "Vulkan";
    case Backend::OpenGL: return "OpenGL";
    default: return "None";
    }
}

} // namespace uih
