#include <cstdlib>

#include "uih.h"

void DrawMenu();

__attribute__((constructor)) static void Load() {
    uih::Config config;
    config.render = DrawMenu;
    config.debugLog = getenv("UIH_DEBUG") != nullptr;
    uih::Start(config);
}
