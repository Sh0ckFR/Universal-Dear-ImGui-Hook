#include "uih.h"

#include <string>

void DrawMenu();

// Hotkeys come from UIH_CONFIG, or from Universal-ImGui-Hook.ini next to the DLL.
static std::wstring KeyFilePath(HMODULE module) {
    std::wstring path(32768, L'\0');
    DWORD length = GetEnvironmentVariableW(L"UIH_CONFIG", path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) {
        length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
        path.resize(length);
        return path.substr(0, path.find_last_of(L"\\/") + 1) + L"Universal-ImGui-Hook.ini";
    }
    path.resize(length);
    return path;
}

BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        uih::Config config;
        config.module = module;
        config.render = DrawMenu;
        config.debugLog = false;
        uih::LoadKeyFile(config, KeyFilePath(module).c_str());
        uih::Start(config);
    }
    return TRUE;
}
