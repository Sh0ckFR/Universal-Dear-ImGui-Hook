// The hook preloaded by the input scenario of host_linux.cpp: with the menu
// open, draws a probe that turns from red to green once the wheel turned down.
#include "imgui.h"
#include "uih.h"

#include "host.h"

static float g_wheel = 0.0f;

static void Render() {
    if (!uih::IsMenuOpen())
        return;
    g_wheel += ImGui::GetIO().MouseWheel;
    ImU32 color = g_wheel < 0.0f ? IM_COL32(0, 255, 0, 255) : IM_COL32(255, 0, 0, 255);
    ImVec2 center(static_cast<float>(kInputProbe[0]), static_cast<float>(kInputProbe[1]));
    ImGui::GetForegroundDrawList()->AddRectFilled(ImVec2(center.x - 50, center.y - 50), ImVec2(center.x + 50, center.y + 50), color);
}

__attribute__((constructor)) static void Load() {
    uih::Config config;
    config.render = Render;
    config.debugLog = true;
    uih::Start(config);
}
