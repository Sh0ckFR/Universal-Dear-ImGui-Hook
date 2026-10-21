#pragma once

#include <dxgiformat.h>

#include "MinHook.h"

struct IDXGISwapChain;

namespace uih::detail {

template <typename T>
void Release(T*& object) {
    if (object) {
        object->Release();
        object = nullptr;
    }
}

template <typename Fn>
Fn GetProc(HMODULE module, const char* name) {
    return reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(module, name)));
}

// Hooks ------------------------------------------------------------------

bool CreateHook(const char* name, void* target, void* detour, void** original);
// Resolves a virtual method from an object's vtable.
inline void* VirtualMethod(void* object, size_t index) {
    return (*reinterpret_cast<void***>(object))[index];
}

// Backends --------------------------------------------------------------

struct Renderer {
    const char* name;
    const wchar_t* module;
    bool (*Install)();
    void (*Uninstall)();
    void (*Teardown)();
};

extern const Renderer kD3D9Renderer;
extern const Renderer kDxgiRenderer;
extern const Renderer kD3D12QueueRenderer;
extern const Renderer kVulkanRenderer;
extern const Renderer kOpenGLRenderer;

// Input libraries hooked to hide the input from the application while the menu is open.
extern const Renderer kDirectInputHooks;
extern const Renderer kXInput14Hooks;
extern const Renderer kXInput13Hooks;
extern const Renderer kXInput910Hooks;

// DXGI swap chain presenters implemented by the D3D10/11/12 backends.
struct DxgiPresenter {
    Backend backend;
    bool (*Probe)(IDXGISwapChain* swapChain);
    void (*Render)(IDXGISwapChain* swapChain);
    void (*PreResize)(IDXGISwapChain* swapChain);
    void (*Teardown)();
};

extern const DxgiPresenter kD3D10Presenter;
extern const DxgiPresenter kD3D11Presenter;
extern const DxgiPresenter kD3D12Presenter;

// Drops the references the D3D12 hooks keep to queues of removed devices.
void ReleaseRemovedQueues();

// Format usable for a render target view on a swap chain buffer.
DXGI_FORMAT RenderTargetFormat(DXGI_FORMAT format);
// True for render target formats that store linear values.
bool StoresLinear(DXGI_FORMAT format);
// Hidden window for dummy swap chains.
HWND CreateHiddenWindow();

namespace input {
void InstallHooks();
// Forgets the trampolines, freed with the hooks.
void UninstallHooks();

// Marks the calls made by the hook itself: they see the real input state.
class OwnCalls {
public:
    OwnCalls();
    ~OwnCalls();
    OwnCalls(const OwnCalls&) = delete;
    OwnCalls& operator=(const OwnCalls&) = delete;
};
// True when the menu is open and the caller is the application.
bool BlockInput();
SHORT RealAsyncKeyState(int key);
} // namespace input

} // namespace uih::detail
