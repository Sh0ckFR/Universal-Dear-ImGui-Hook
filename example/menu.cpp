#include "imgui.h"
#include "uih.h"

void DrawMenu() {
    if (!uih::IsMenuOpen())
        return;

    static bool styled = false;
    if (!styled) {
        ImVec4* colors = ImGui::GetStyle().Colors;
        colors[ImGuiCol_WindowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.8f);
        colors[ImGuiCol_Header] = ImVec4(0.2f, 0.2f, 0.2f, 0.8f);
        colors[ImGuiCol_HeaderHovered] = ImVec4(0.3f, 0.3f, 0.3f, 0.8f);
        colors[ImGuiCol_Button] = ImVec4(0.26f, 0.59f, 0.98f, 0.4f);
        colors[ImGuiCol_ButtonHovered] = ImVec4(0.26f, 0.59f, 0.98f, 1.0f);
        colors[ImGuiCol_ButtonActive] = ImVec4(0.06f, 0.53f, 0.98f, 1.0f);
        styled = true;
    }

    static float slider = 1.0f;
    static bool checkbox = false;
    bool open = true;
    ImGui::SetNextWindowSize(ImVec2(450, 600), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(25, 25), ImGuiCond_FirstUseEver);
    ImGui::Begin("ImGui Menu", &open, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);
    ImGui::Text("Backend: %s", uih::BackendName(uih::ActiveBackend()));
    if (ImGui::CollapsingHeader("MENU")) {
        if (ImGui::TreeNode("SUB MENU")) {
            ImGui::Text("Text Test");
            ImGui::Button("Button Test");
            ImGui::Checkbox("Checkbox Test", &checkbox);
            ImGui::SliderFloat("Slider Test", &slider, 1.0f, 100.0f);
            ImGui::TreePop();
        }
    }
    ImGui::End();
    if (!open)
        uih::SetMenuOpen(false);
}
