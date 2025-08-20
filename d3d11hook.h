#pragma once

#include <d3d11.h>
#include <dxgi.h>

namespace hooks_dx11 {
    using PresentFn = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT);
    using ResizeBuffersFn = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

    extern PresentFn       oPresentD3D11;
    extern ResizeBuffersFn oResizeBuffersD3D11;

    HRESULT __stdcall hookPresentD3D11(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags);
    HRESULT __stdcall hookResizeBuffersD3D11(
        IDXGISwapChain* pSwapChain,
        UINT BufferCount,
        UINT Width,
        UINT Height,
        DXGI_FORMAT NewFormat,
        UINT SwapChainFlags);

    void Init();
    void release();
}
