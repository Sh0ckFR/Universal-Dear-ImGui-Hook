#pragma once

namespace globals {
        extern HMODULE mainModule;
        extern HWND mainWindow;
        extern int uninjectKey;
        extern int openMenuKey;

        // Rendering backend currently in use
        enum class Backend {
                None,
                DX9,
                DX10,
                DX11,
                DX12,
                Vulkan
        };
        extern Backend activeBackend;
        // Preferred backend to hook. None means auto with fallback order
        extern Backend preferredBackend;
        extern bool enableDebugLog;
        void SetDebugLogging(bool enable);
}

namespace hooks {
        extern void Init();
}

namespace inputhook {
        extern void Init(HWND hWindow);
        extern void Remove(HWND hWindow);
        static LRESULT APIENTRY WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
}

namespace mousehooks {
        void Init();
        void Remove();
}

namespace d3d12hook {
        typedef HRESULT(STDMETHODCALLTYPE* PresentD3D12)(
                IDXGISwapChain3 * pSwapChain, UINT SyncInterval, UINT Flags);
        typedef HRESULT(STDMETHODCALLTYPE* Present1Fn)(
                IDXGISwapChain3 * pSwapChain, UINT SyncInterval, UINT Flags, const DXGI_PRESENT_PARAMETERS* pParams);
        extern PresentD3D12 oPresentD3D12;
        extern Present1Fn   oPresent1D3D12;

	typedef void(STDMETHODCALLTYPE* ExecuteCommandListsFn)(
		ID3D12CommandQueue * _this, UINT NumCommandLists, ID3D12CommandList* const* ppCommandLists);
        extern ExecuteCommandListsFn oExecuteCommandListsD3D12;

        typedef HRESULT(STDMETHODCALLTYPE* ResizeBuffersFn)(
                IDXGISwapChain3* pSwapChain,
                UINT BufferCount,
                UINT Width,
                UINT Height,
                DXGI_FORMAT NewFormat,
                UINT SwapChainFlags);
        extern ResizeBuffersFn oResizeBuffersD3D12;

        extern HRESULT STDMETHODCALLTYPE hookResizeBuffersD3D12(
                IDXGISwapChain3* pSwapChain,
                UINT BufferCount,
                UINT Width,
                UINT Height,
                DXGI_FORMAT NewFormat,
                UINT SwapChainFlags);

        extern long __fastcall hookPresentD3D12(IDXGISwapChain3* pSwapChain, UINT SyncInterval, UINT Flags);
        extern long __fastcall hookPresent1D3D12(IDXGISwapChain3* pSwapChain, UINT SyncInterval, UINT Flags, const DXGI_PRESENT_PARAMETERS* pParams);
        extern void STDMETHODCALLTYPE hookExecuteCommandListsD3D12(
                ID3D12CommandQueue* _this,
                UINT                          NumCommandLists,
                ID3D12CommandList* const* ppCommandLists);

        extern void release();
        bool IsInitialized();
}

// Forward declarations for other rendering backends
namespace d3d9hook {
    using EndSceneFn = HRESULT(__stdcall*)(IDirect3DDevice9*);
    using ResetFn = HRESULT(__stdcall*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);

    extern EndSceneFn oEndScene;
    extern ResetFn    oReset;

    HRESULT __stdcall hookEndScene(IDirect3DDevice9* device);
    HRESULT __stdcall hookReset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* params);

    void Init();
    void release();
    bool IsInitialized();
}

namespace hooks_dx10 {
    using PresentFn = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT);
    using Present1Fn = HRESULT(__stdcall*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
    using ResizeBuffersFn = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

    extern PresentFn       oPresentD3D10;
    extern Present1Fn      oPresent1D3D10;
    extern ResizeBuffersFn oResizeBuffersD3D10;

    HRESULT __stdcall hookPresentD3D10(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags);
    HRESULT __stdcall hookPresent1D3D10(IDXGISwapChain1* pSwapChain, UINT SyncInterval, UINT Flags, const DXGI_PRESENT_PARAMETERS* pPresentParameters);
    HRESULT __stdcall hookResizeBuffersD3D10(
        IDXGISwapChain* pSwapChain,
        UINT BufferCount,
        UINT Width,
        UINT Height,
        DXGI_FORMAT NewFormat,
        UINT SwapChainFlags);

    void Init();
    void release();
    bool IsInitialized();
}

namespace hooks_dx11 {
    using PresentFn   = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT);
    using Present1Fn  = HRESULT(__stdcall*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
    using ResizeBuffersFn = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

    extern PresentFn       oPresentD3D11;
    extern Present1Fn      oPresent1D3D11;
    extern ResizeBuffersFn oResizeBuffersD3D11;

    HRESULT __stdcall hookPresentD3D11(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags);
    HRESULT __stdcall hookPresent1D3D11(IDXGISwapChain1* pSwapChain, UINT SyncInterval, UINT Flags, const DXGI_PRESENT_PARAMETERS* pPresentParameters);
    HRESULT __stdcall hookResizeBuffersD3D11(
        IDXGISwapChain* pSwapChain,
        UINT BufferCount,
        UINT Width,
        UINT Height,
        DXGI_FORMAT NewFormat,
        UINT SwapChainFlags);

    void Init();
    void release();
    bool IsInitialized();
}

namespace hooks_vk {
    extern PFN_vkCreateInstance        oCreateInstance;
    extern PFN_vkCreateDevice          oCreateDevice;
    extern PFN_vkQueuePresentKHR       oQueuePresentKHR;

    VkResult VKAPI_PTR hook_vkCreateInstance(const VkInstanceCreateInfo* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkInstance* pInstance);
    VkResult VKAPI_PTR hook_vkCreateDevice(VkPhysicalDevice physicalDevice,
        const VkDeviceCreateInfo* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkDevice* pDevice);
    VkResult VKAPI_PTR hook_vkQueuePresentKHR(VkQueue queue,
        const VkPresentInfoKHR* pPresentInfo);

    void Init();
    void release();
    bool IsInitialized();
}

namespace menu {
        extern bool isOpen;
        extern void Init();
}

// Helper to unload the DLL and remove all hooks
void Uninject();
