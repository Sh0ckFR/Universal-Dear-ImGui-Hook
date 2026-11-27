#pragma once

#include <dxgi.h>

#include "MinHook.h"

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
// timeGetTime(), hooked once winmm.dll loads for the speed of the game's clocks.
extern const Renderer kClockHooks;

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

// True while the application presents with Vulkan.
bool VulkanPresenting();

// Drops the references the D3D12 hooks keep to queues of removed devices.
void ReleaseRemovedQueues();

// Format usable for a render target view on a swap chain buffer.
DXGI_FORMAT RenderTargetFormat(DXGI_FORMAT format);
// True for render target formats that store linear values.
bool StoresLinear(DXGI_FORMAT format);
// Converts the vertex colors for the back buffer of `swapChain`, of `format`:
// to linear values, or to PQ when the game set the swap chain to HDR10.
void EncodeColors(IDXGISwapChain* swapChain, DXGI_FORMAT format, ImDrawData* drawData);
// Layout of the texels of a render target format, for CaptureFrame().
TexelLayout DxgiTexelLayout(DXGI_FORMAT format);
// The window of the process of its size for a swap chain made for composition, which has none.
HWND SwapChainWindow(IDXGISwapChain* swapChain, const DXGI_SWAP_CHAIN_DESC& desc);
// Hidden window for dummy swap chains.
HWND CreateHiddenWindow();

// The clocks the game reads, src/windows/clocks.cpp, with Config::aiAdvancedFeatures.
namespace clocks {
void InstallHooks();
void UninstallHooks();
} // namespace clocks

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
