// C interface of the library, for C, and for languages that call C: Rust,
// Zig, Odin, Python's ctypes, C#. The same functions as include/uih.h, in
// the same library: link it as you would from C++. Draw your menu from the
// render callback with Dear ImGui's C bindings, such as cimgui.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum uih_backend {
    UIH_BACKEND_NONE,
    UIH_BACKEND_D3D9,
    UIH_BACKEND_D3D10,
    UIH_BACKEND_D3D11,
    UIH_BACKEND_D3D12,
    UIH_BACKEND_VULKAN,
    UIH_BACKEND_OPENGL,
} uih_backend;

// The buttons of uih_config.menu_buttons, as XInput numbers them.
enum {
    UIH_GAMEPAD_DPAD_UP = 0x0001,
    UIH_GAMEPAD_DPAD_DOWN = 0x0002,
    UIH_GAMEPAD_DPAD_LEFT = 0x0004,
    UIH_GAMEPAD_DPAD_RIGHT = 0x0008,
    UIH_GAMEPAD_START = 0x0010,
    UIH_GAMEPAD_BACK = 0x0020,
    UIH_GAMEPAD_L3 = 0x0040,
    UIH_GAMEPAD_R3 = 0x0080,
    UIH_GAMEPAD_L1 = 0x0100,
    UIH_GAMEPAD_R1 = 0x0200,
    UIH_GAMEPAD_A = 0x1000,
    UIH_GAMEPAD_B = 0x2000,
    UIH_GAMEPAD_X = 0x4000,
    UIH_GAMEPAD_Y = 0x8000,
};

// The settings of uih::Config, with the same meaning. Fill it with
// uih_config_init(), then change what you need.
typedef struct uih_config {
    // HMODULE of the DLL, for the unload key (Windows); unused on Linux.
    void* module;
    uih_backend backend;
    // Virtual-key code (Windows) or X11 keysym (Linux); 0 disables the key.
    int menu_key;
    int unload_key;
    unsigned menu_buttons;
    float scale;
    bool debug_log;
    bool remote_control;
    const char* remote_pipe;
    bool remote_memory;
    bool ai_advanced_features;
    // Called every frame while the overlay draws, and once when Dear ImGui is ready.
    void (*render)(void);
    void (*setup)(void);
} uih_config;

// Receives the pixels of uih_capture_frame(): opaque RGBA, top row first,
// valid during the call only.
typedef void (*uih_frame_callback)(const uint8_t* rgba, uint32_t width, uint32_t height, void* context);

void uih_config_init(uih_config* config);
void uih_start(const uih_config* config);
void uih_stop(void);
uih_backend uih_active_backend(void);
bool uih_is_menu_open(void);
void uih_set_menu_open(bool open);
const char* uih_backend_name(uih_backend backend);
// An image for ImGui::Image(), as an ImTextureData*; NULL when it cannot be made.
void* uih_create_texture(const void* rgba, int width, int height);
void uih_destroy_texture(void* texture);
// Copies the game's next frame without the overlay and hands it to `receive`;
// false on timeout or with an unsupported back buffer.
bool uih_capture_frame(uih_frame_callback receive, void* context, uint32_t timeout_ms);
bool uih_ai_advanced_features_enabled(void);
void uih_draw_ai_advanced_features(bool* open);

#ifdef __cplusplus
}
#endif
