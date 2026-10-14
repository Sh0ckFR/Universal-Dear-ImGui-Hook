#include "internal.h"

#include <d3d9.h>

#include "imgui_impl_dx9.h"

namespace uih::detail {
namespace {

using EndSceneFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*);
using ResetFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using Direct3DCreate9Fn = IDirect3D9*(WINAPI*)(UINT);

constexpr size_t kResetIndex = 16;
constexpr size_t kEndSceneIndex = 42;

HMODULE g_module = nullptr;
EndSceneFn g_endScene = nullptr;
ResetFn g_reset = nullptr;
IDirect3DDevice9* g_device = nullptr;

HWND DeviceWindow(IDirect3DDevice9* device) {
    HWND window = nullptr;
    IDirect3DSwapChain9* swapChain = nullptr;
    if (SUCCEEDED(device->GetSwapChain(0, &swapChain))) {
        D3DPRESENT_PARAMETERS present{};
        if (SUCCEEDED(swapChain->GetPresentParameters(&present)))
            window = present.hDeviceWindow;
        swapChain->Release();
    }
    if (window)
        return window;
    D3DDEVICE_CREATION_PARAMETERS params{};
    if (SUCCEEDED(device->GetCreationParameters(&params)))
        window = params.hFocusWindow;
    return window;
}

void Teardown() {
    if (!g_device)
        return;
    ImGui_ImplDX9_Shutdown();
    UnbindWindow();
    Release(g_device);
}

bool Init(IDirect3DDevice9* device) {
    HWND window = DeviceWindow(device);
    if (!window || !ClaimBackend(Backend::D3D9) || !BindWindow(window))
        return false;
    ImGui_ImplDX9_Init(device);
    device->AddRef();
    g_device = device;
    return true;
}

void Render(IDirect3DDevice9* device) {
    if (!g_device && !Init(device))
        return;
    if (device != g_device)
        return;
    ImGui_ImplDX9_NewFrame();
    ImGui_ImplDX9_RenderDrawData(Frame());
}

HRESULT STDMETHODCALLTYPE HookEndScene(IDirect3DDevice9* device) {
    ScopedCall call;
    ForeignPresentScope presentScope;
    if (ShouldRender(Backend::D3D9))
        Render(device);
    ServiceTeardown(Backend::D3D9, Teardown);
    return g_endScene(device);
}

HRESULT STDMETHODCALLTYPE HookReset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* params) {
    ScopedCall call;
    bool ours = device == g_device;
    if (ours)
        ImGui_ImplDX9_InvalidateDeviceObjects();
    HRESULT hr = g_reset(device, params);
    if (ours && SUCCEEDED(hr))
        ImGui_ImplDX9_CreateDeviceObjects();
    return hr;
}

bool Install() {
    g_module = LoadLibraryW(L"d3d9.dll");
    auto create = GetProc<Direct3DCreate9Fn>(g_module, "Direct3DCreate9");
    if (!create)
        return false;

    IDirect3D9* d3d = create(D3D_SDK_VERSION);
    if (!d3d)
        return false;

    HWND window = CreateHiddenWindow();
    D3DPRESENT_PARAMETERS present{};
    present.Windowed = TRUE;
    present.SwapEffect = D3DSWAPEFFECT_DISCARD;
    present.hDeviceWindow = window;

    IDirect3DDevice9* device = nullptr;
    HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
                                   D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_NOWINDOWCHANGES,
                                   &present, &device);
    bool hooked = false;
    if (SUCCEEDED(hr)) {
        hooked = CreateHook("IDirect3DDevice9::EndScene", VirtualMethod(device, kEndSceneIndex),
                            reinterpret_cast<void*>(HookEndScene), reinterpret_cast<void**>(&g_endScene));
        CreateHook("IDirect3DDevice9::Reset", VirtualMethod(device, kResetIndex),
                   reinterpret_cast<void*>(HookReset), reinterpret_cast<void**>(&g_reset));
        device->Release();
    } else {
        Log("D3D9 dummy device creation failed: 0x%08lX", hr);
    }

    DestroyWindow(window);
    d3d->Release();
    return hooked;
}

void Uninstall() {
    g_endScene = nullptr;
    g_reset = nullptr;
    if (g_module) {
        FreeLibrary(g_module);
        g_module = nullptr;
    }
}

} // namespace

const Renderer kD3D9Renderer = {"D3D9", L"d3d9.dll", Install, Uninstall, Teardown};

} // namespace uih::detail
