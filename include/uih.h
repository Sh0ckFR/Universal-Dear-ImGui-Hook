#pragma once

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

struct ImTextureData;

namespace uih {

enum class Backend {
    None,
    D3D9,
    D3D10,
    D3D11,
    D3D12,
    Vulkan,
    OpenGL,
};

struct Config {
#ifdef _WIN32
    // Module to unload when `unloadKey` is pressed. Leave null to disable unloading.
    HMODULE module = nullptr;
#endif
    // Backend to hook. `None` hooks every graphics API loaded in the process and
    // keeps the first one that renders a frame.
    Backend backend = Backend::None;
#ifdef _WIN32
    // Virtual-key codes.
    int menuKey = VK_INSERT;
    int unloadKey = VK_F11;
#else
    // X11 keysyms: XK_Insert, XK_F11.
    int menuKey = 0xff63;
    int unloadKey = 0xffc8;
#endif
    // Size of the interface, 1.5 for 150%. 0 follows the DPI of the target window.
    float scale = 0.0f;
    bool debugLog = false;
    // Called every frame between ImGui::NewFrame() and ImGui::Render().
    void (*render)() = nullptr;
    // Called when the ImGui context is created, before its first frame: load
    // fonts and set the style here. A new context is created when the game
    // switches to another device or window, so this may run more than once.
    void (*setup)() = nullptr;
};

// Installs the hooks. Safe to call from DllMain or from a library constructor.
void Start(const Config& config);
// Removes the overlay and releases all resources. Blocks until it is done.
// Must not be called from DllMain or from inside a hook.
void Stop();

Backend ActiveBackend();
bool IsMenuOpen();
void SetMenuOpen(bool open);

const char* BackendName(Backend backend);

// Creates an image from `width` * `height` RGBA pixels, 8 bits per channel,
// rows from top to bottom. Draw it with ImGui::Image(texture->GetTexRef(), size).
// The pixels are copied and uploaded by the active backend on the next frame,
// and again after a backend switch. Callable from any thread, before or
// after Start().
ImTextureData* CreateTexture(const void* rgba, int width, int height);
// Releases a texture on the next frame. Do not draw it afterwards.
void DestroyTexture(ImTextureData* texture);

} // namespace uih
