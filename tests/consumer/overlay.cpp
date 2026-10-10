#include "imgui.h"
#include "uih.h"

static void Render() {
    if (!uih::IsMenuOpen())
        return;
    ImGui::Begin("My overlay");
    ImGui::Text("Backend: %s", uih::BackendName(uih::ActiveBackend()));
    ImGui::End();
}

BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        uih::Config config;
        config.module = module;
        config.render = Render;
        uih::Start(config);
    }
    return TRUE;
}
