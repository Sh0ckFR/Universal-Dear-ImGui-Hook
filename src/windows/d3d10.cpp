#include "internal.h"

#include <d3d10_1.h>
#include <dxgi.h>

#include "imgui_impl_dx10.h"

namespace uih::detail {
namespace {

ID3D10Device* g_device = nullptr;
IDXGISwapChain* g_swapChain = nullptr;

bool Probe(IDXGISwapChain* swapChain) {
    ID3D10Device* device = nullptr;
    if (FAILED(swapChain->GetDevice(IID_PPV_ARGS(&device))))
        return false;
    device->Release();
    return true;
}

void Teardown() {
    if (!g_device)
        return;
    ImGui_ImplDX10_Shutdown();
    UnbindWindow();
    Release(g_device);
    g_swapChain = nullptr;
}

bool Init(IDXGISwapChain* swapChain) {
    DXGI_SWAP_CHAIN_DESC desc{};
    ID3D10Device* device = nullptr;
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
        ImGui_ImplDX10_Init(g_device);
    } else {
        device->Release();
    }
    g_swapChain = swapChain;
    return true;
}

ID3D10RenderTargetView* CreateTarget(bool& linear) {
    ID3D10Texture2D* buffer = nullptr;
    if (FAILED(g_swapChain->GetBuffer(0, IID_PPV_ARGS(&buffer))))
        return nullptr;
    D3D10_TEXTURE2D_DESC textureDesc{};
    buffer->GetDesc(&textureDesc);
    D3D10_RENDER_TARGET_VIEW_DESC viewDesc{};
    viewDesc.Format = RenderTargetFormat(textureDesc.Format);
    linear = StoresLinear(viewDesc.Format);
    viewDesc.ViewDimension = textureDesc.SampleDesc.Count > 1 ? D3D10_RTV_DIMENSION_TEXTURE2DMS : D3D10_RTV_DIMENSION_TEXTURE2D;
    ID3D10RenderTargetView* target = nullptr;
    HRESULT hr = g_device->CreateRenderTargetView(buffer, &viewDesc, &target);
    buffer->Release();
    if (FAILED(hr))
        Log("D3D10 CreateRenderTargetView failed: 0x%08lX", hr);
    return target;
}

void Render(IDXGISwapChain* swapChain) {
    if (swapChain != g_swapChain && !Init(swapChain))
        return;
    bool linear = false;
    ID3D10RenderTargetView* target = CreateTarget(linear);
    if (!target)
        return;

    ImGui_ImplDX10_NewFrame();
    ImDrawData* drawData = Frame();
    if (linear)
        LinearizeColors(drawData);

    ID3D10RenderTargetView* previous[D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D10DepthStencilView* depth = nullptr;
    g_device->OMGetRenderTargets(D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT, previous, &depth);
    g_device->OMSetRenderTargets(1, &target, nullptr);
    ImGui_ImplDX10_RenderDrawData(drawData);
    g_device->OMSetRenderTargets(D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT, previous, depth);
    for (ID3D10RenderTargetView* view : previous)
        Release(view);
    Release(depth);
    target->Release();
}

void PreResize(IDXGISwapChain*) {}

} // namespace

const DxgiPresenter kD3D10Presenter = {Backend::D3D10, Probe, Render, PreResize, Teardown};

} // namespace uih::detail
