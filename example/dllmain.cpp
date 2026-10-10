#include "uih.h"

void DrawMenu();

BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        uih::Config config;
        config.module = module;
        config.render = DrawMenu;
        config.debugLog = false;
        uih::Start(config);
    }
    return TRUE;
}
