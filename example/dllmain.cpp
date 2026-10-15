#include "uih.h"

#include <cwchar>

void ApplySettings(uih::Config& config);
void DrawMenu();

// Loaded as dinput8.dll, the DLL stays in the game: the unload key removes
// the overlay, and the menu key brings it back.
static bool LoadedAsProxy(HMODULE module) {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(module, path, MAX_PATH);
    const wchar_t* name = wcsrchr(path, L'\\');
    return _wcsicmp(name ? name + 1 : path, L"dinput8.dll") == 0;
}

BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        uih::Config config;
        ApplySettings(config);
        config.module = LoadedAsProxy(module) ? nullptr : module;
        config.render = DrawMenu;
        config.debugLog = false;
        uih::Start(config);
    }
    return TRUE;
}
