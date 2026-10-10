#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

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
    // Module to unload when `unloadKey` is pressed. Leave null to disable unloading.
    HMODULE module = nullptr;
    // Backend to hook. `None` hooks every graphics API loaded in the process and
    // keeps the first one that renders a frame.
    Backend backend = Backend::None;
    int menuKey = VK_INSERT;
    int unloadKey = VK_F11;
    bool debugLog = false;
    // Called every frame between ImGui::NewFrame() and ImGui::Render().
    void (*render)() = nullptr;
};

// Installs the hooks from a worker thread. Safe to call from DllMain.
void Start(const Config& config);
// Removes every hook and releases all resources. Blocks until no hook is running.
// Must not be called from DllMain or from inside a hook.
void Stop();

Backend ActiveBackend();
bool IsMenuOpen();
void SetMenuOpen(bool open);

const char* BackendName(Backend backend);

} // namespace uih
