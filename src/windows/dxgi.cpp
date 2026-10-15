#include "internal.h"

#include <d3d11.h>
#include <dxgi1_4.h>

namespace uih::detail {
namespace {

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using ResizeBuffers1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT, const UINT*, IUnknown* const*);
using D3D11CreateDeviceAndSwapChainFn = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
                                                         const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**, D3D_FEATURE_LEVEL*,
                                                         ID3D11DeviceContext**);

constexpr size_t kPresentIndex = 8;
constexpr size_t kResizeBuffersIndex = 13;
constexpr size_t kPresent1Index = 22;
constexpr size_t kResizeBuffers1Index = 39;

HMODULE g_module = nullptr;
PresentFn g_present = nullptr;
Present1Fn g_present1 = nullptr;
ResizeBuffersFn g_resizeBuffers = nullptr;
ResizeBuffers1Fn g_resizeBuffers1 = nullptr;

// D3D10 devices also expose ID3D11Device, so D3D10 is probed before D3D11.
const DxgiPresenter* const kPresenters[] = {&kD3D12Presenter, &kD3D10Presenter, &kD3D11Presenter};
const DxgiPresenter* g_presenter = nullptr;

void Teardown() {
    if (g_presenter)
        g_presenter->Teardown();
}

void OnPresent(IDXGISwapChain* swapChain, UINT flags) {
    if ((flags & DXGI_PRESENT_TEST) || InsideForeignPresent())
        return;
    if (!g_presenter) {
        for (const DxgiPresenter* presenter : kPresenters) {
            if (ShouldRender(presenter->backend) && presenter->Probe(swapChain) && ClaimBackend(presenter->backend)) {
                g_presenter = presenter;
                break;
            }
        }
        if (!g_presenter)
            return;
    }
    if (ShouldRender(g_presenter->backend))
        g_presenter->Render(swapChain);
}

HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) {
    ScopedCall call;
    Guarded([&] { OnPresent(swapChain, flags); });
    if (g_presenter)
        ServiceTeardown(g_presenter->backend, Teardown);
    return g_present(swapChain, syncInterval, flags);
}

HRESULT STDMETHODCALLTYPE HookPresent1(IDXGISwapChain1* swapChain, UINT syncInterval, UINT flags, const DXGI_PRESENT_PARAMETERS* params) {
    ScopedCall call;
    Guarded([&] { OnPresent(swapChain, flags); });
    if (g_presenter)
        ServiceTeardown(g_presenter->backend, Teardown);
    return g_present1(swapChain, syncInterval, flags, params);
}

HRESULT STDMETHODCALLTYPE HookResizeBuffers(IDXGISwapChain* swapChain, UINT bufferCount, UINT width, UINT height, DXGI_FORMAT format, UINT flags) {
    ScopedCall call;
    if (g_presenter)
        g_presenter->PreResize(swapChain);
    return g_resizeBuffers(swapChain, bufferCount, width, height, format, flags);
}

HRESULT STDMETHODCALLTYPE HookResizeBuffers1(IDXGISwapChain3* swapChain, UINT bufferCount, UINT width, UINT height, DXGI_FORMAT format, UINT flags,
                                             const UINT* nodeMasks, IUnknown* const* queues) {
    ScopedCall call;
    if (g_presenter)
        g_presenter->PreResize(swapChain);
    return g_resizeBuffers1(swapChain, bufferCount, width, height, format, flags, nodeMasks, queues);
}

bool Install() {
    g_module = LoadLibraryW(L"d3d11.dll");
    auto create = GetProc<D3D11CreateDeviceAndSwapChainFn>(g_module, "D3D11CreateDeviceAndSwapChain");
    if (!create)
        return false;

    HWND window = CreateHiddenWindow();
    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferCount = 2;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = window;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    const D3D_DRIVER_TYPE drivers[] = {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP};
    IDXGISwapChain* swapChain = nullptr;
    ID3D11Device* device = nullptr;
    HRESULT hr = E_FAIL;
    for (D3D_DRIVER_TYPE driver : drivers) {
        hr = create(nullptr, driver, nullptr, 0, levels, 3, D3D11_SDK_VERSION, &desc, &swapChain, &device, nullptr, nullptr);
        if (SUCCEEDED(hr))
            break;
    }

    bool hooked = false;
    if (SUCCEEDED(hr)) {
        hooked = CreateHook("IDXGISwapChain::Present", VirtualMethod(swapChain, kPresentIndex),
                            reinterpret_cast<void*>(HookPresent), reinterpret_cast<void**>(&g_present));
        CreateHook("IDXGISwapChain::ResizeBuffers", VirtualMethod(swapChain, kResizeBuffersIndex),
                   reinterpret_cast<void*>(HookResizeBuffers), reinterpret_cast<void**>(&g_resizeBuffers));

        IDXGISwapChain1* swapChain1 = nullptr;
        if (SUCCEEDED(swapChain->QueryInterface(IID_PPV_ARGS(&swapChain1)))) {
            CreateHook("IDXGISwapChain1::Present1", VirtualMethod(swapChain1, kPresent1Index),
                       reinterpret_cast<void*>(HookPresent1), reinterpret_cast<void**>(&g_present1));
            swapChain1->Release();
        }
        IDXGISwapChain3* swapChain3 = nullptr;
        if (SUCCEEDED(swapChain->QueryInterface(IID_PPV_ARGS(&swapChain3)))) {
            CreateHook("IDXGISwapChain3::ResizeBuffers1", VirtualMethod(swapChain3, kResizeBuffers1Index),
                       reinterpret_cast<void*>(HookResizeBuffers1), reinterpret_cast<void**>(&g_resizeBuffers1));
            swapChain3->Release();
        }
        swapChain->Release();
        device->Release();
    } else {
        Log("D3D11 dummy swap chain creation failed: 0x%08lX", hr);
    }

    DestroyWindow(window);
    return hooked;
}

void Uninstall() {
    g_presenter = nullptr;
    g_present = nullptr;
    g_present1 = nullptr;
    g_resizeBuffers = nullptr;
    g_resizeBuffers1 = nullptr;
    if (g_module) {
        FreeLibrary(g_module);
        g_module = nullptr;
    }
}

} // namespace

const Renderer kDxgiRenderer = {"DXGI", L"dxgi.dll", Install, Uninstall, Teardown};

DXGI_FORMAT RenderTargetFormat(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return format;
    }
}

bool StoresLinear(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
        return true;
    default:
        return false;
    }
}

HWND CreateHiddenWindow() {
    return CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 64, 64, nullptr, nullptr, nullptr, nullptr);
}

} // namespace uih::detail
