#include "internal.h"

#include <d3d9.h>

#include "imgui_impl_dx9.h"

namespace uih::detail {
namespace {

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using PresentExFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9Ex*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);
using SwapChainPresentFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DSwapChain9*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);
using ResetFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using ResetExFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9Ex*, D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*);
using Direct3DCreate9ExFn = HRESULT(WINAPI*)(UINT, IDirect3D9Ex**);

constexpr size_t kResetIndex = 16;
constexpr size_t kPresentIndex = 17;
constexpr size_t kPresentExIndex = 121;
constexpr size_t kResetExIndex = 132;
constexpr size_t kSwapChainPresentIndex = 3;

// Devices created by Direct3DCreate9 and Direct3DCreate9Ex may not share their
// methods, so each kind gets its own hooks.
enum DeviceKind { kPlain, kEx, kKinds };

HMODULE g_module = nullptr;
PresentFn g_present[kKinds] = {};
ResetFn g_reset[kKinds] = {};
SwapChainPresentFn g_swapChainPresent[kKinds] = {};
PresentExFn g_presentEx = nullptr;
ResetExFn g_resetEx = nullptr;

IDirect3DDevice9* g_device = nullptr;
// Device::Present may go through SwapChain::Present: only the outer call renders.
thread_local int g_presentDepth = 0;

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
    Teardown();
    HWND window = DeviceWindow(device);
    if (!window || !ClaimBackend(Backend::D3D9) || !BindWindow(window))
        return false;
    ImGui_ImplDX9_Init(device);
    device->AddRef();
    g_device = device;
    return true;
}

// Draws on the back buffer of the implicit swap chain, outside of the
// application's scenes, whatever render target is bound.
void Render(IDirect3DDevice9* device) {
    if (device != g_device && !Init(device))
        return;

    // Before any reference to the back buffer, which an exception in the frame would leak.
    ImGui_ImplDX9_NewFrame();
    ImDrawData* drawData = Frame();

    IDirect3DSurface9* backBuffer = nullptr;
    if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)))
        return;
    IDirect3DSurface9* previous = nullptr;
    device->GetRenderTarget(0, &previous);
    D3DVIEWPORT9 viewport{};
    device->GetViewport(&viewport);
    device->SetRenderTarget(0, backBuffer);
    // sRGB writes would encode the overlay's colors, already sRGB, a second time.
    DWORD srgbWrite = FALSE;
    device->GetRenderState(D3DRS_SRGBWRITEENABLE, &srgbWrite);
    device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    if (SUCCEEDED(device->BeginScene())) {
        ImGui_ImplDX9_RenderDrawData(drawData);
        device->EndScene();
    }
    device->SetRenderState(D3DRS_SRGBWRITEENABLE, srgbWrite);
    device->SetRenderTarget(0, previous);
    device->SetViewport(&viewport);
    Release(previous);
    backBuffer->Release();
}

void OnPresent(IDirect3DDevice9* device) {
    if (g_presentDepth++ == 0 && ShouldRender(Backend::D3D9))
        DrawOverlay(SwapchainKey(device), DeviceWindow(device), [&] { Render(device); });
}

void AfterPresent() {
    if (--g_presentDepth == 0)
        ServiceTeardown(Backend::D3D9, Teardown);
}

template <DeviceKind Kind>
HRESULT STDMETHODCALLTYPE HookPresent(IDirect3DDevice9* device, const RECT* source, const RECT* dest, HWND window, const RGNDATA* dirty) {
    ScopedCall call;
    ForeignPresentScope presentScope;
    OnPresent(device);
    HRESULT hr = g_present[Kind](device, source, dest, window, dirty);
    AfterPresent();
    return hr;
}

HRESULT STDMETHODCALLTYPE HookPresentEx(IDirect3DDevice9Ex* device, const RECT* source, const RECT* dest, HWND window, const RGNDATA* dirty, DWORD flags) {
    ScopedCall call;
    ForeignPresentScope presentScope;
    OnPresent(device);
    HRESULT hr = g_presentEx(device, source, dest, window, dirty, flags);
    AfterPresent();
    return hr;
}

template <DeviceKind Kind>
HRESULT STDMETHODCALLTYPE HookSwapChainPresent(IDirect3DSwapChain9* swapChain, const RECT* source, const RECT* dest, HWND window, const RGNDATA* dirty, DWORD flags) {
    ScopedCall call;
    ForeignPresentScope presentScope;
    // Additional swap chains present other windows: only the implicit one gets the overlay.
    IDirect3DDevice9* device = nullptr;
    IDirect3DSwapChain9* implicit = nullptr;
    if (SUCCEEDED(swapChain->GetDevice(&device))) {
        device->GetSwapChain(0, &implicit);
        if (implicit == swapChain)
            OnPresent(device);
        else
            ++g_presentDepth;
        Release(implicit);
    } else {
        ++g_presentDepth;
    }
    HRESULT hr = g_swapChainPresent[Kind](swapChain, source, dest, window, dirty, flags);
    AfterPresent();
    Release(device);
    return hr;
}

void BeforeReset(IDirect3DDevice9* device) {
    if (device == g_device)
        ImGui_ImplDX9_InvalidateDeviceObjects();
}

void AfterReset(IDirect3DDevice9* device, HRESULT hr) {
    if (device == g_device && SUCCEEDED(hr))
        ImGui_ImplDX9_CreateDeviceObjects();
}

template <DeviceKind Kind>
HRESULT STDMETHODCALLTYPE HookReset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* params) {
    ScopedCall call;
    BeforeReset(device);
    HRESULT hr = g_reset[Kind](device, params);
    AfterReset(device, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookResetEx(IDirect3DDevice9Ex* device, D3DPRESENT_PARAMETERS* params, D3DDISPLAYMODEEX* mode) {
    ScopedCall call;
    BeforeReset(device);
    HRESULT hr = g_resetEx(device, params, mode);
    AfterReset(device, hr);
    return hr;
}

struct DeviceMethods {
    void* present = nullptr;
    void* reset = nullptr;
    void* swapChainPresent = nullptr;
};

DeviceMethods Methods(IDirect3DDevice9* device) {
    DeviceMethods methods;
    methods.present = VirtualMethod(device, kPresentIndex);
    methods.reset = VirtualMethod(device, kResetIndex);
    IDirect3DSwapChain9* swapChain = nullptr;
    if (SUCCEEDED(device->GetSwapChain(0, &swapChain))) {
        methods.swapChainPresent = VirtualMethod(swapChain, kSwapChainPresentIndex);
        swapChain->Release();
    }
    return methods;
}

// Methods already hooked through the other kind of device are left alone.
template <DeviceKind Kind>
bool HookDevice(const DeviceMethods& methods, const DeviceMethods& hooked) {
    bool present = methods.present == hooked.present ||
                   CreateHook("IDirect3DDevice9::Present", methods.present, reinterpret_cast<void*>(HookPresent<Kind>),
                              reinterpret_cast<void**>(&g_present[Kind]));
    if (methods.reset != hooked.reset)
        CreateHook("IDirect3DDevice9::Reset", methods.reset, reinterpret_cast<void*>(HookReset<Kind>), reinterpret_cast<void**>(&g_reset[Kind]));
    if (methods.swapChainPresent && methods.swapChainPresent != hooked.swapChainPresent)
        CreateHook("IDirect3DSwapChain9::Present", methods.swapChainPresent, reinterpret_cast<void*>(HookSwapChainPresent<Kind>),
                   reinterpret_cast<void**>(&g_swapChainPresent[Kind]));
    return present;
}

D3DPRESENT_PARAMETERS DummyParameters(HWND window) {
    D3DPRESENT_PARAMETERS present{};
    present.Windowed = TRUE;
    present.SwapEffect = D3DSWAPEFFECT_DISCARD;
    present.hDeviceWindow = window;
    return present;
}

constexpr DWORD kDummyFlags = D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_NOWINDOWCHANGES;

bool Install() {
    g_module = LoadLibraryW(L"d3d9.dll");
    auto create = GetProc<decltype(&Direct3DCreate9)>(g_module, "Direct3DCreate9");
    auto createEx = GetProc<Direct3DCreate9ExFn>(g_module, "Direct3DCreate9Ex");
    if (!create)
        return false;

    HWND window = CreateHiddenWindow();
    DeviceMethods plain;
    bool hooked = false;

    if (IDirect3D9* d3d = create(D3D_SDK_VERSION)) {
        D3DPRESENT_PARAMETERS present = DummyParameters(window);
        IDirect3DDevice9* device = nullptr;
        HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, kDummyFlags, &present, &device);
        if (SUCCEEDED(hr)) {
            DeviceMethods methods = Methods(device);
            if (HookDevice<kPlain>(methods, DeviceMethods{})) {
                plain = methods;
                hooked = true;
            }
            device->Release();
        } else {
            Log("D3D9 dummy device creation failed: 0x%08lX", hr);
        }
        d3d->Release();
    }

    IDirect3D9Ex* d3dEx = nullptr;
    if (createEx && SUCCEEDED(createEx(D3D_SDK_VERSION, &d3dEx))) {
        D3DPRESENT_PARAMETERS present = DummyParameters(window);
        IDirect3DDevice9Ex* device = nullptr;
        HRESULT hr = d3dEx->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, kDummyFlags, &present, nullptr, &device);
        if (SUCCEEDED(hr)) {
            hooked = HookDevice<kEx>(Methods(device), plain) || hooked;
            CreateHook("IDirect3DDevice9Ex::PresentEx", VirtualMethod(device, kPresentExIndex),
                       reinterpret_cast<void*>(HookPresentEx), reinterpret_cast<void**>(&g_presentEx));
            CreateHook("IDirect3DDevice9Ex::ResetEx", VirtualMethod(device, kResetExIndex),
                       reinterpret_cast<void*>(HookResetEx), reinterpret_cast<void**>(&g_resetEx));
            device->Release();
        } else {
            Log("D3D9Ex dummy device creation failed: 0x%08lX", hr);
        }
        d3dEx->Release();
    }

    DestroyWindow(window);
    return hooked;
}

void Uninstall() {
    for (int kind = 0; kind < kKinds; ++kind) {
        g_present[kind] = nullptr;
        g_reset[kind] = nullptr;
        g_swapChainPresent[kind] = nullptr;
    }
    g_presentEx = nullptr;
    g_resetEx = nullptr;
    if (g_module) {
        FreeLibrary(g_module);
        g_module = nullptr;
    }
}

} // namespace

const Renderer kD3D9Renderer = {"D3D9", L"d3d9.dll", Install, Uninstall, Teardown};

} // namespace uih::detail
