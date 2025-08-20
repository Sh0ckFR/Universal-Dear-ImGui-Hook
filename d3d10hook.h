#pragma once

#include <d3d10.h>
#include <dxgi.h>

namespace hooks_dx10 {
    using PresentFn = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT);
    using ResizeBuffersFn = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

    extern PresentFn       oPresentD3D10;
    extern ResizeBuffersFn oResizeBuffersD3D10;

    HRESULT __stdcall hookPresentD3D10(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags);
    HRESULT __stdcall hookResizeBuffersD3D10(
        IDXGISwapChain* pSwapChain,
        UINT BufferCount,
        UINT Width,
        UINT Height,
        DXGI_FORMAT NewFormat,
        UINT SwapChainFlags);

    void Init();
    void release();
}
