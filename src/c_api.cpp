// The C interface of include/uih_c.h, on top of include/uih.h. No exception
// leaves these functions: C callers could not handle it.
#include "uih_c.h"

#include "uih.h"

#include <cstring>
#include <stdexcept>
#include <string>

static_assert(static_cast<int>(uih::Backend::OpenGL) == UIH_BACKEND_OPENGL, "the backends of uih.h and uih_c.h must match");
static_assert(uih::GamepadY == UIH_GAMEPAD_Y && uih::GamepadL3 == UIH_GAMEPAD_L3, "the buttons of uih.h and uih_c.h must match");

extern "C" {

void uih_config_init(uih_config* config) {
    if (!config)
        return;
    const uih::Config defaults;
    *config = {};
#ifdef _WIN32
    config->module = defaults.module;
#endif
    config->backend = static_cast<uih_backend>(defaults.backend);
    config->menu_key = defaults.menuKey;
    config->unload_key = defaults.unloadKey;
    config->menu_buttons = defaults.menuButtons;
    config->scale = defaults.scale;
    config->debug_log = defaults.debugLog;
    config->remote_control = defaults.remoteControl;
    config->remote_pipe = defaults.remotePipe;
    config->remote_memory = defaults.remoteMemory;
    config->ai_advanced_features = defaults.aiAdvancedFeatures;
    config->render = defaults.render;
    config->setup = defaults.setup;
}

void uih_start(const uih_config* c) {
    if (!c)
        return;
    uih::Config config;
#ifdef _WIN32
    config.module = static_cast<HMODULE>(c->module);
#endif
    config.backend = static_cast<uih::Backend>(c->backend);
    config.menuKey = c->menu_key;
    config.unloadKey = c->unload_key;
    config.menuButtons = c->menu_buttons;
    config.scale = c->scale;
    config.debugLog = c->debug_log;
    config.remoteControl = c->remote_control;
    if (c->remote_pipe)
        config.remotePipe = c->remote_pipe;
    config.remoteMemory = c->remote_memory;
    config.aiAdvancedFeatures = c->ai_advanced_features;
    config.render = c->render;
    config.setup = c->setup;
    try {
        uih::Start(config);
    } catch (...) {
    }
}

void uih_stop(void) {
    try {
        uih::Stop();
    } catch (...) {
    }
}

uih_backend uih_active_backend(void) { return static_cast<uih_backend>(uih::ActiveBackend()); }

bool uih_is_menu_open(void) { return uih::IsMenuOpen(); }

void uih_set_menu_open(bool open) { uih::SetMenuOpen(open); }

const char* uih_backend_name(uih_backend backend) { return uih::BackendName(static_cast<uih::Backend>(backend)); }

void* uih_create_texture(const void* rgba, int width, int height) {
    try {
        return uih::CreateTexture(rgba, width, height);
    } catch (...) {
        return nullptr;
    }
}

void uih_destroy_texture(void* texture) {
    try {
        uih::DestroyTexture(static_cast<ImTextureData*>(texture));
    } catch (...) {
    }
}

bool uih_capture_frame(uih_frame_callback receive, void* context, uint32_t timeout_ms) {
    if (!receive)
        return false;
    try {
        std::vector<uint8_t> pixels;
        uint32_t width = 0;
        uint32_t height = 0;
        if (!uih::CaptureFrame(pixels, width, height, timeout_ms))
            return false;
        receive(pixels.data(), width, height, context);
        return true;
    } catch (...) {
        return false;
    }
}

bool uih_ai_advanced_features_enabled(void) { return uih::AIAdvancedFeaturesEnabled(); }

void uih_draw_ai_advanced_features(bool* open) {
    try {
        uih::DrawAIAdvancedFeatures(open);
    } catch (...) {
    }
}

} // extern "C"

struct uih_tool_result {
    std::string text;
    std::string error;
    bool failed = false;
};

extern "C" {

void uih_tool_answer(uih_tool_result* result, const char* text) {
    if (!result)
        return;
    try {
        result->text = text ? text : "";
        result->failed = false;
    } catch (...) {
    }
}

void uih_tool_fail(uih_tool_result* result, const char* message) {
    if (!result)
        return;
    try {
        result->error = message && *message ? message : "the tool failed";
        result->failed = true;
    } catch (...) {
    }
}

bool uih_register_tool(const char* name, const char* description, const char* parameters, uih_tool_callback call, void* context) {
    if (!call)
        return false;
    try {
        return uih::RegisterTool(name, description, parameters, [call, context](const std::string& arguments) {
            uih_tool_result result;
            call(arguments.c_str(), &result, context);
            if (result.failed)
                throw std::runtime_error(result.error);
            return result.text;
        });
    } catch (...) {
        return false;
    }
}

void uih_unregister_tool(const char* name) {
    try {
        uih::UnregisterTool(name);
    } catch (...) {
    }
}

size_t uih_tool_argument(const char* arguments, const char* key, char* buffer, size_t size) {
    std::string value;
    try {
        if (arguments && key)
            value = uih::ToolArgument(arguments, key);
    } catch (...) {
        value.clear();
    }
    if (buffer && size > 0) {
        size_t copied = value.size() < size - 1 ? value.size() : size - 1;
        memcpy(buffer, value.data(), copied);
        buffer[copied] = '\0';
    }
    return value.size();
}

} // extern "C"
