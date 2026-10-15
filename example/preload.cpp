#include <dlfcn.h>

#include <cstdlib>
#include <string>

#include "uih.h"

void DrawMenu();

// Hotkeys come from UIH_CONFIG, or from Universal-ImGui-Hook.ini next to the library.
static std::string KeyFilePath() {
    if (const char* path = getenv("UIH_CONFIG"))
        return path;
    Dl_info info{};
    std::string library = dladdr(reinterpret_cast<void*>(&KeyFilePath), &info) && info.dli_fname ? info.dli_fname : "";
    return library.substr(0, library.rfind('/') + 1) + "Universal-ImGui-Hook.ini";
}

__attribute__((constructor)) static void Load() {
    uih::Config config;
    config.render = DrawMenu;
    config.debugLog = getenv("UIH_DEBUG") != nullptr;
    uih::LoadKeyFile(config, KeyFilePath().c_str());
    uih::Start(config);
}
