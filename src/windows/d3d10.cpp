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

// The frame of the game, before the overlay is drawn on it.
void Capture() {
    std::vector<uint8_t> rgba;
    ID3D10Texture2D* buffer = nullptr;
    if (FAILED(g_swapChain->GetBuffer(0, IID_PPV_ARGS(&buffer)))) {
        CompleteCapture({}, 0, 0);
        return;
    }
    D3D10_TEXTURE2D_DESC desc{};
    buffer->GetDesc(&desc);
    DXGI_FORMAT format = RenderTargetFormat(desc.Format);
    TexelLayout layout = DxgiTexelLayout(format);
    ID3D10Texture2D* source = buffer;
    ID3D10Texture2D* resolved = nullptr;
    ID3D10Texture2D* staging = nullptr;
    D3D10_TEXTURE2D_DESC copyDesc = desc;
    copyDesc.Format = format;
    copyDesc.MipLevels = 1;
    copyDesc.ArraySize = 1;
    copyDesc.SampleDesc = {1, 0};
    copyDesc.MiscFlags = 0;
    bool ok = layout != TexelLayout::Unsupported;
    if (ok && desc.SampleDesc.Count > 1) {
        copyDesc.Usage = D3D10_USAGE_DEFAULT;
        copyDesc.BindFlags = 0;
        copyDesc.CPUAccessFlags = 0;
        ok = SUCCEEDED(g_device->CreateTexture2D(&copyDesc, nullptr, &resolved));
        if (ok) {
            g_device->ResolveSubresource(resolved, 0, buffer, 0, format);
            source = resolved;
        }
    }
    copyDesc.Usage = D3D10_USAGE_STAGING;
    copyDesc.BindFlags = 0;
    copyDesc.CPUAccessFlags = D3D10_CPU_ACCESS_READ;
    D3D10_MAPPED_TEXTURE2D mapped{};
    if (ok && SUCCEEDED(g_device->CreateTexture2D(&copyDesc, nullptr, &staging))) {
        g_device->CopySubresourceRegion(staging, 0, 0, 0, 0, source, 0, nullptr);
        if (SUCCEEDED(staging->Map(0, D3D10_MAP_READ, 0, &mapped))) {
            rgba = ToRgba(mapped.pData, mapped.RowPitch, desc.Width, desc.Height, layout);
            staging->Unmap(0);
        }
    }
    Release(staging);
    Release(resolved);
    buffer->Release();
    CompleteCapture(std::move(rgba), desc.Width, desc.Height);
}

void Render(IDXGISwapChain* swapChain) {
    if (swapChain != g_swapChain && !Init(swapChain))
        return;
    if (CaptureWanted())
        Capture();
    // Before any reference to the back buffer, which an exception in the frame would leak.
    ImGui_ImplDX10_NewFrame();
    ImDrawData* drawData = Frame();
    bool linear = false;
    ID3D10RenderTargetView* target = CreateTarget(linear);
    if (!target)
        return;
    if (linear)
        LinearizeColors(drawData);

    ID3D10RenderTargetView* previous[D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D10DepthStencilView* depth = nullptr;
    g_device->OMGetRenderTargets(D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT, previous, &depth);
    g_device->OMSetRenderTargets(1, &target, nullptr);
    // A fault in the backend, as in a draw callback of the application, must
    // not leave the back buffer bound or referenced.
    Guarded([&] { ImGui_ImplDX10_RenderDrawData(drawData); });
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
