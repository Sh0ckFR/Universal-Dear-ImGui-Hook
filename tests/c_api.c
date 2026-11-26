/* The C interface, compiled as C: uih_host --c-api starts the hook through
 * it, and checks its defaults and names first. */
#include "uih_c.h"

#include <string.h>

/* NULL when the defaults and the names are those of include/uih.h. */
const char* uih_test_check_c_api(void) {
    uih_config config;
    memset(&config, 0xAB, sizeof(config));
    uih_config_init(&config);
    if (config.backend != UIH_BACKEND_NONE || config.scale != 0.0f || config.remote_control || config.remote_memory ||
        config.ai_advanced_features || config.debug_log || config.render || config.setup)
        return "uih_config_init() does not set the defaults of uih::Config";
    if (config.menu_buttons != (UIH_GAMEPAD_L3 | UIH_GAMEPAD_R3) || !config.remote_pipe || strcmp(config.remote_pipe, "universal-imgui-hook") != 0)
        return "uih_config_init() does not set the menu buttons or the pipe of uih::Config";
#ifdef _WIN32
    if (config.menu_key != 0x2D || config.unload_key != 0x7A)
        return "uih_config_init() does not set INSERT and F11";
#endif
    if (strcmp(uih_backend_name(UIH_BACKEND_D3D11), "D3D11") != 0 || strcmp(uih_backend_name(UIH_BACKEND_VULKAN), "Vulkan") != 0)
        return "uih_backend_name() does not name the backends";
    if (uih_capture_frame(NULL, NULL, 0))
        return "uih_capture_frame() accepts no callback";
    return NULL;
}

/* Starts the hook as a C program does. */
void uih_test_start_c_api(void (*render)(void), void (*setup)(void), float scale) {
    uih_config config;
    uih_config_init(&config);
    config.render = render;
    config.setup = setup;
    config.scale = scale;
    config.debug_log = true;
    uih_start(&config);
}
