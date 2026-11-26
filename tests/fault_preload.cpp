// The hook preloaded by the fault scenario of host_linux.cpp: once the menu
// is open, the render callback faults once as UIH_TEST_FAULT says, "access"
// (null pointer write), "exception" (C++ exception) or "draw" (null pointer
// write in a draw callback, while the backend draws).
#include <cstdlib>
#include <cstring>
#include <stdexcept>

#include "imgui.h"
#include "uih.h"

static const char* g_fault = nullptr;

__attribute__((no_sanitize("address", "undefined"))) static void WriteNull() {
    volatile int* volatile address = nullptr;
    *address = 1;
}

static void FaultWhileDrawing(const ImDrawList*, const ImDrawCmd*) { WriteNull(); }

static void Render() {
    if (!uih::IsMenuOpen())
        return;
    ImGui::GetForegroundDrawList()->AddRectFilled(ImVec2(300, 200), ImVec2(400, 300), IM_COL32(255, 0, 0, 255));
    const char* fault = g_fault;
    if (!fault)
        return;
    g_fault = nullptr;
    if (strcmp(fault, "exception") == 0)
        throw std::runtime_error("menu");
    if (strcmp(fault, "draw") == 0)
        ImGui::GetForegroundDrawList()->AddCallback(FaultWhileDrawing, nullptr);
    else
        WriteNull();
}

__attribute__((constructor)) static void Load() {
    g_fault = getenv("UIH_TEST_FAULT");
    uih::Config config;
    config.render = Render;
    config.debugLog = true;
    uih::Start(config);
}
