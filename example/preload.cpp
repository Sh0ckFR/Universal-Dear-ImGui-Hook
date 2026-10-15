#include <cstdlib>

#include "uih.h"

void ApplySettings(uih::Config& config);
void DrawMenu();

__attribute__((constructor)) static void Load() {
    uih::Config config;
    ApplySettings(config);
    config.render = DrawMenu;
    config.debugLog = getenv("UIH_DEBUG") != nullptr;
    uih::Start(config);
}
