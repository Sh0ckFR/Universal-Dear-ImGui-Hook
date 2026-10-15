#include "uih.h"

void ApplySettings(uih::Config& config);
void DrawMenu();

BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        uih::Config config;
        ApplySettings(config);
        config.module = module;
        config.render = DrawMenu;
        config.debugLog = false;
        uih::Start(config);
    }
    return TRUE;
}
