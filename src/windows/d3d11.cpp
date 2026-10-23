#include "internal.h"

#include <d3d11.h>
#include <dxgi.h>

#include "imgui_impl_dx11.h"

namespace uih::detail {
namespace {

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
IDXGISwapChain* g_swapChain = nullptr;

bool Probe(IDXGISwapChain* swapChain) {
    ID3D11Device* device = nullptr;
    if (FAILED(swapChain->GetDevice(IID_PPV_ARGS(&device))))
        return false;
    device->Release();
    return true;
}

void Teardown() {
    if (!g_device)
        return;
    ImGui_ImplDX11_Shutdown();
    UnbindWindow();
    Release(g_context);
    Release(g_device);
    g_swapChain = nullptr;
}

bool Init(IDXGISwapChain* swapChain) {
    DXGI_SWAP_CHAIN_DESC desc{};
    ID3D11Device* device = nullptr;
    if (FAILED(swapChain->GetDesc(&desc)) || FAILED(swapChain->GetDevice(IID_PPV_ARGS(&device))))
        return false;
    if (g_device && device != g_device)
        Teardown();
    if (!BindWindow(desc.OutputWindow)) {
        device->Release();
        return false;
    }
    if (!g_device) {
        g_device = device;
        g_device->GetImmediateContext(&g_context);
        ImGui_ImplDX11_Init(g_device, g_context);
    } else {
        device->Release();
    }
    g_swapChain = swapChain;
    return true;
}

ID3D11RenderTargetView* CreateTarget(bool& linear) {
    ID3D11Texture2D* buffer = nullptr;
    if (FAILED(g_swapChain->GetBuffer(0, IID_PPV_ARGS(&buffer))))
        return nullptr;
    D3D11_TEXTURE2D_DESC textureDesc{};
    buffer->GetDesc(&textureDesc);
    D3D11_RENDER_TARGET_VIEW_DESC viewDesc{};
    viewDesc.Format = RenderTargetFormat(textureDesc.Format);
    linear = StoresLinear(viewDesc.Format);
    viewDesc.ViewDimension = textureDesc.SampleDesc.Count > 1 ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;
    ID3D11RenderTargetView* target = nullptr;
    HRESULT hr = g_device->CreateRenderTargetView(buffer, &viewDesc, &target);
    buffer->Release();
    if (FAILED(hr))
        Log("D3D11 CreateRenderTargetView failed: 0x%08lX", hr);
    return target;
}

void Render(IDXGISwapChain* swapChain) {
    if (swapChain != g_swapChain && !Init(swapChain))
        return;
    // Before any reference to the back buffer, which an exception in the frame would leak.
    ImGui_ImplDX11_NewFrame();
    ImDrawData* drawData = Frame();
    bool linear = false;
    ID3D11RenderTargetView* target = CreateTarget(linear);
    if (!target)
        return;
    if (linear)
        LinearizeColors(drawData);

    ID3D11RenderTargetView* previous[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* depth = nullptr;
    g_context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, previous, &depth);
    g_context->OMSetRenderTargets(1, &target, nullptr);
    // A fault in the backend, as in a draw callback of the application, must
    // not leave the back buffer bound or referenced.
    Guarded([&] { ImGui_ImplDX11_RenderDrawData(drawData); });
    g_context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, previous, depth);
    for (ID3D11RenderTargetView* view : previous)
        Release(view);
    Release(depth);
    target->Release();
}

void PreResize(IDXGISwapChain*) {}

} // namespace

const DxgiPresenter kD3D11Presenter = {Backend::D3D11, Probe, Render, PreResize, Teardown};

} // namespace uih::detail
