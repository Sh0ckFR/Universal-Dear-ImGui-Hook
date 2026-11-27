// The hook preloaded by the input scenarios of host_linux.cpp and
// host_wayland.cpp: with the menu open, draws a probe that turns from red to
// green once the wheel turned down, and one that turns green once the menu got
// a left click on it.
#include "imgui.h"
#include "uih.h"

#include "host.h"

static float g_wheel = 0.0f;
static bool g_clicked = false;

static void Render() {
    if (!uih::IsMenuOpen())
        return;
    g_wheel += ImGui::GetIO().MouseWheel;
    ImU32 color = g_wheel < 0.0f ? IM_COL32(0, 255, 0, 255) : IM_COL32(255, 0, 0, 255);
    ImVec2 center(static_cast<float>(kInputProbe[0]), static_cast<float>(kInputProbe[1]));
    ImGui::GetForegroundDrawList()->AddRectFilled(ImVec2(center.x - 50, center.y - 50), ImVec2(center.x + 50, center.y + 50), color);
    ImVec2 target(static_cast<float>(kClickProbe[0]), static_cast<float>(kClickProbe[1]));
    ImVec2 min(target.x - 50, target.y - 50);
    ImVec2 max(target.x + 50, target.y + 50);
    g_clicked = g_clicked || (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsMouseHoveringRect(min, max, false));
    ImGui::GetForegroundDrawList()->AddRectFilled(min, max, g_clicked ? IM_COL32(0, 255, 0, 255) : IM_COL32(255, 0, 0, 255));
}

__attribute__((constructor)) static void Load() {
    uih::Config config;
    config.render = Render;
    config.debugLog = true;
    uih::Start(config);
}
