#include "internal.h"

#include <cstdio>
#include <thread>

namespace uih::detail {
namespace {

NativeWindow g_window = 0;
std::atomic<bool> g_unloadRequested{false};

} // namespace

bool BindWindow(NativeWindow window) {
    if (g_window == window && ImGui::GetCurrentContext())
        return true;

    if (!CreateContext())
        input::Detach();

    g_window = window;
    ImGui::GetIO().BackendPlatformName = "uih_x11";
    input::Attach(window);
    Log("Bound to window 0x%x", window);
    return true;
}

void UnbindWindow() {
    if (!ImGui::GetCurrentContext())
        return;
    input::Detach();
    ImGui::DestroyContext();
    g_window = 0;
}

namespace platform {

void WriteLog(const char* line) { fputs(line, stderr); }

bool Start() {
    g_unloadRequested = false;
    return true;
}

void Stop() {
    if (!TeardownOnRenderThread(1000)) {
        Log("Rendering thread did not respond, releasing resources from Stop()");
        TeardownVulkan();
    }
}

void NewFrame() { input::NewFrame(); }

bool IsKeyDown(int key) { return input::IsKeyDown(key); }

bool IsFocused() { return input::IsFocused(); }

void Unload() {
    if (g_unloadRequested.exchange(true))
        return;
    Log("Unload requested");
    std::thread(uih::Stop).detach();
}

} // namespace platform

} // namespace uih::detail
