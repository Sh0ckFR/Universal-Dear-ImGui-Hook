#pragma once

#include "uih.h"

#include <dxgiformat.h>

#include <atomic>
#include <cstdint>
#include <mutex>

#include "MinHook.h"
#include "imgui.h"

struct IDXGISwapChain;

namespace uih::detail {

void Log(const char* fmt, ...);

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

// Every hook keeps one of these alive while it runs so Stop() can wait for
// in-flight calls before the hooks are removed.
class ScopedCall {
public:
    ScopedCall();
    ~ScopedCall();
    ScopedCall(const ScopedCall&) = delete;
    ScopedCall& operator=(const ScopedCall&) = delete;
};

// Drivers may implement presentation of one API on top of another (Vulkan on
// a DXGI swap chain). Hooks of the outer API mark the thread so the inner API
// ignores the nested present.
class ForeignPresentScope {
public:
    ForeignPresentScope();
    ~ForeignPresentScope();
    ForeignPresentScope(const ForeignPresentScope&) = delete;
    ForeignPresentScope& operator=(const ForeignPresentScope&) = delete;
};
bool InsideForeignPresent();

// Frame orchestration ----------------------------------------------------

const Config& GetConfig();
// True when `backend` is allowed by the configuration.
bool WantBackend(Backend backend);
// True when hooks are running and `backend` is either active or no backend
// has claimed the process yet.
bool ShouldRender(Backend backend);
// Marks `backend` as the active one. Fails when another backend already owns the process.
bool ClaimBackend(Backend backend);
// Runs `teardown` when Stop() asked the active backend to release its resources
// on the rendering thread.
void ServiceTeardown(Backend backend, void (*teardown)());

// Creates the ImGui context on first use and attaches the platform backend
// and input hooks to `window`.
bool BindWindow(HWND window);
// Platform NewFrame, key polling, user callback and ImGui::Render().
ImDrawData* Frame();
// Detaches the platform backend and destroys the ImGui context.
void UnbindWindow();

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

// Format usable for a render target view on a swap chain buffer.
DXGI_FORMAT RenderTargetFormat(DXGI_FORMAT format);
// Hidden window for dummy swap chains.
HWND CreateHiddenWindow();

// Input -----------------------------------------------------------------

namespace input {
void InstallCursorHooks();
void Attach(HWND window);
void Detach();
// Undoes cursor clipping so the menu can be used.
void ReleaseCursor();
// Best-effort lookup of the main window of this process.
HWND FindProcessWindow();
} // namespace input

} // namespace uih::detail
