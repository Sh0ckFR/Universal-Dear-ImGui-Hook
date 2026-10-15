// The example menu: a tour of the common Dear ImGui widgets. "Show the Dear
// ImGui demo" opens ImGui's own demo window, which covers every function.
#include "imgui.h"
#include "uih.h"

#include <cmath>
#include <cstdio>

ImTextureData* ExampleImage(ImVec2& size);

static void Style() {
    static bool styled = false;
    if (styled)
        return;
    ImVec4* colors = ImGui::GetStyle().Colors;
    colors[ImGuiCol_WindowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.8f);
    colors[ImGuiCol_Header] = ImVec4(0.2f, 0.2f, 0.2f, 0.8f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.3f, 0.3f, 0.3f, 0.8f);
    colors[ImGuiCol_Button] = ImVec4(0.26f, 0.59f, 0.98f, 0.4f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.26f, 0.59f, 0.98f, 1.0f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.06f, 0.53f, 0.98f, 1.0f);
    styled = true;
}

static void Basics() {
    static float slider = 1.0f;
    static bool checkbox = false;
    ImGui::Text("Text Test");
    ImGui::Button("Button Test");
    ImGui::Checkbox("Checkbox Test", &checkbox);
    ImGui::SliderFloat("Slider Test", &slider, 1.0f, 100.0f);

    // Images: any RGBA pixels, here imgui.png embedded in the library.
    ImVec2 size;
    if (ImTextureData* image = ExampleImage(size)) {
        float width = ImGui::GetContentRegionAvail().x;
        ImGui::Image(image->GetTexRef(), ImVec2(width, width * size.y / size.x));
    }
}

static void Buttons() {
    static int clicks = 0;
    static int radio = 0;
    if (ImGui::Button("Click me"))
        ++clicks;
    ImGui::SameLine();
    ImGui::Text("clicked %d times", clicks);
    if (ImGui::SmallButton("Small"))
        ++clicks;
    ImGui::SameLine();
    if (ImGui::ArrowButton("left", ImGuiDir_Left))
        --clicks;
    ImGui::SameLine();
    if (ImGui::ArrowButton("right", ImGuiDir_Right))
        ++clicks;
    ImGui::RadioButton("One", &radio, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Two", &radio, 1);
    ImGui::SameLine();
    ImGui::RadioButton("Three", &radio, 2);
    ImGui::Button("Hover me");
    ImGui::SetItemTooltip("A tooltip");
    ImGui::ProgressBar(std::fmod(static_cast<float>(ImGui::GetTime()) * 0.2f, 1.0f), ImVec2(-1.0f, 0.0f));
}

static void Choices() {
    static const char* const kItems[] = {"Apple", "Banana", "Cherry", "Kiwi", "Mango", "Orange"};
    static int combo = 0;
    static int list = 1;
    static bool selected[3] = {};
    ImGui::Combo("Combo", &combo, kItems, IM_ARRAYSIZE(kItems));
    ImGui::ListBox("List box", &list, kItems, IM_ARRAYSIZE(kItems), 4);
    for (int i = 0; i < 3; ++i) {
        char label[32];
        snprintf(label, sizeof(label), "Selectable %d", i + 1);
        ImGui::Selectable(label, &selected[i]);
    }
}

static void Inputs() {
    static char text[128] = "Hello";
    static char notes[512] = "Several\nlines";
    static int integer = 42;
    static float number = 0.5f;
    static float drag = 10.0f;
    static int range[2] = {20, 80};
    static float angle = 0.0f;
    static float color[4] = {0.26f, 0.59f, 0.98f, 1.0f};
    ImGui::InputText("Text", text, sizeof(text));
    ImGui::InputTextWithHint("Hint", "type here", text, sizeof(text));
    ImGui::InputTextMultiline("Multiline", notes, sizeof(notes), ImVec2(0.0f, ImGui::GetTextLineHeight() * 4));
    ImGui::InputInt("Integer", &integer);
    ImGui::InputFloat("Float", &number, 0.1f, 1.0f, "%.2f");
    ImGui::DragFloat("Drag", &drag, 0.5f, 0.0f, 100.0f);
    ImGui::DragInt2("Range", range, 1.0f, 0, 100);
    ImGui::SliderAngle("Angle", &angle);
    ImGui::ColorEdit4("Color", color);
}

static void Layout() {
    ImGui::SeparatorText("Tabs");
    if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem("First")) {
            ImGui::TextWrapped("Tabs hold their own content. This text wraps at the edge of the window.");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Second")) {
            ImGui::BulletText("Bullet");
            ImGui::Indent();
            ImGui::BulletText("Indented bullet");
            ImGui::Unindent();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::SeparatorText("Tree");
    if (ImGui::TreeNode("Parent")) {
        if (ImGui::TreeNode("Child"))
            ImGui::TreePop();
        ImGui::TreePop();
    }
    ImGui::SeparatorText("Scrolling child");
    ImGui::BeginChild("child", ImVec2(0.0f, 80.0f), ImGuiChildFlags_Borders);
    for (int i = 0; i < 20; ++i)
        ImGui::Text("Line %d", i + 1);
    ImGui::EndChild();
}

static void Data() {
    static float values[64] = {};
    static int offset = 0;
    values[offset] = std::sin(static_cast<float>(ImGui::GetTime()) * 3.0f);
    offset = (offset + 1) % IM_ARRAYSIZE(values);
    ImGui::PlotLines("Lines", values, IM_ARRAYSIZE(values), offset, nullptr, -1.0f, 1.0f, ImVec2(0.0f, 60.0f));
    static const float kHistogram[] = {0.2f, 0.6f, 0.4f, 0.9f, 0.5f, 0.3f, 0.7f};
    ImGui::PlotHistogram("Histogram", kHistogram, IM_ARRAYSIZE(kHistogram), 0, nullptr, 0.0f, 1.0f, ImVec2(0.0f, 60.0f));

    if (ImGui::BeginTable("table", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("Name");
        ImGui::TableSetupColumn("Type");
        ImGui::TableSetupColumn("Value");
        ImGui::TableHeadersRow();
        static const char* const kRows[][3] = {{"Health", "int", "100"}, {"Speed", "float", "1.5"}, {"Name", "string", "Player"}};
        for (const auto& row : kRows) {
            ImGui::TableNextRow();
            for (int column = 0; column < 3; ++column) {
                ImGui::TableSetColumnIndex(column);
                ImGui::TextUnformatted(row[column]);
            }
        }
        ImGui::EndTable();
    }
}

static void Popups() {
    if (ImGui::Button("Open a modal"))
        ImGui::OpenPopup("Modal");
    if (ImGui::BeginPopupModal("Modal", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("A modal window blocks the rest of the menu.");
        if (ImGui::Button("Close"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    ImGui::Button("Right-click me");
    if (ImGui::BeginPopupContextItem("context")) {
        ImGui::MenuItem("Copy");
        ImGui::MenuItem("Paste");
        ImGui::EndPopup();
    }
}

static void Drawing() {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetCursorScreenPos();
    float time = static_cast<float>(ImGui::GetTime());
    draw->AddRectFilled(origin, ImVec2(origin.x + 60, origin.y + 60), IM_COL32(66, 150, 250, 255), 6.0f);
    draw->AddCircle(ImVec2(origin.x + 110, origin.y + 30), 28.0f, IM_COL32(255, 200, 0, 255), 0, 3.0f);
    draw->AddLine(ImVec2(origin.x + 150, origin.y + 30 + 25 * std::sin(time)), ImVec2(origin.x + 250, origin.y + 30 - 25 * std::sin(time)),
                  IM_COL32(255, 80, 80, 255), 3.0f);
    draw->AddText(ImVec2(origin.x + 260, origin.y + 22), IM_COL32_WHITE, "Draw lists");
    ImGui::Dummy(ImVec2(0.0f, 64.0f));
}

void DrawMenu() {
    if (!uih::IsMenuOpen())
        return;
    Style();

    static bool demo = false;
    bool open = true;
    ImGui::SetNextWindowSize(ImVec2(480, 640), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(25, 25), ImGuiCond_FirstUseEver);
    ImGui::Begin("ImGui Menu", &open, ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoCollapse);
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("Menu")) {
            ImGui::MenuItem("Show the Dear ImGui demo", nullptr, &demo);
            if (ImGui::MenuItem("Close"))
                open = false;
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }
    ImGui::Text("Backend: %s", uih::BackendName(uih::ActiveBackend()));
    ImGui::SetNextItemOpen(true, ImGuiCond_FirstUseEver);
    if (ImGui::CollapsingHeader("MENU")) {
        ImGui::SetNextItemOpen(true, ImGuiCond_FirstUseEver);
        if (ImGui::TreeNode("SUB MENU")) {
            Basics();
            ImGui::TreePop();
        }
    }
    if (ImGui::CollapsingHeader("Buttons"))
        Buttons();
    if (ImGui::CollapsingHeader("Combo, list box, selectables"))
        Choices();
    if (ImGui::CollapsingHeader("Inputs, sliders, colors"))
        Inputs();
    if (ImGui::CollapsingHeader("Tabs, trees, layout"))
        Layout();
    if (ImGui::CollapsingHeader("Plots and tables"))
        Data();
    if (ImGui::CollapsingHeader("Popups"))
        Popups();
    if (ImGui::CollapsingHeader("Drawing"))
        Drawing();
    if (ImGui::Button("Show the Dear ImGui demo"))
        demo = true;
    ImGui::End();

    if (demo)
        ImGui::ShowDemoWindow(&demo);
    if (!open)
        uih::SetMenuOpen(false);
}
