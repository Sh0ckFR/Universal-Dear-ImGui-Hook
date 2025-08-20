#include "stdafx.h"

using Microsoft::WRL::ComPtr;

namespace hooks {
    // VTable indices derived from the official DirectX interface order.
    // These values are stable across Windows versions and SDKs.
    constexpr size_t kPresentIndex  = 8;            // IDXGISwapChain::Present
    constexpr size_t kPresent1Index = 22;           // IDXGISwapChain1::Present1
    constexpr size_t kResizeBuffersIndex = 13;     // IDXGISwapChain::ResizeBuffers
    constexpr size_t kExecuteCommandListsIndex = 10; // ID3D12CommandQueue::ExecuteCommandLists
    constexpr size_t kSignalIndex = 14;            // ID3D12CommandQueue::Signal
    // Dummy objects pour extraire les v-tables
    static ComPtr<IDXGISwapChain3>       pSwapChain = nullptr;
    static ComPtr<ID3D12Device>          pDevice = nullptr;
    static ComPtr<ID3D12CommandQueue>    pCommandQueue = nullptr;
    static HWND                          hDummyWindow = nullptr;
    static const wchar_t* dummyClassName = L"DummyWndClass";

    static void CleanupDummyObjects()
    {
        if (hDummyWindow)
        {
            DestroyWindow(hDummyWindow);
            hDummyWindow = nullptr;
        }

        UnregisterClassW(dummyClassName, GetModuleHandle(nullptr));

        pSwapChain.Reset();
        pDevice.Reset();
        pCommandQueue.Reset();
    }

    // Create hidden Window + device + DX12 swapchain
    static HRESULT CreateDeviceAndSwapChain() {
        // 1) Register dummy window
        WNDCLASSEXW wc = {
            sizeof(WNDCLASSEXW),
            CS_CLASSDC,
            DefWindowProcW,
            0, 0,
            GetModuleHandleW(nullptr),
            nullptr, nullptr, nullptr, nullptr,
            dummyClassName,
            nullptr
        };
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            DebugLog("[hooks] RegisterClassExW failed: %u\n", GetLastError());
            return E_FAIL;
        }

        // 2) Create hidden window
        hDummyWindow = CreateWindowExW(
            0, dummyClassName, L"Dummy",
            WS_OVERLAPPEDWINDOW,
            0, 0, 1, 1,
            nullptr, nullptr, wc.hInstance, nullptr
        );
        if (!hDummyWindow) {
            DebugLog("[hooks] CreateWindowExW failed: %u\n", GetLastError());
            return E_FAIL;
        }

        // 3) Factory DXGI
        ComPtr<IDXGIFactory4> factory;
        HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        if (FAILED(hr)) {
            DebugLog("[hooks] CreateDXGIFactory1 failed: 0x%08X\n", hr);
            return hr;
        }

        // 4) Device D3D12
        hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&pDevice));
        if (FAILED(hr)) {
            DebugLog("[hooks] D3D12CreateDevice failed: 0x%08X\n", hr);
            return hr;
        }

        // 5) Command Queue
        D3D12_COMMAND_QUEUE_DESC cqDesc = {};
        cqDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        cqDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        hr = pDevice->CreateCommandQueue(&cqDesc, IID_PPV_ARGS(&pCommandQueue));
        if (FAILED(hr)) {
            DebugLog("[hooks] CreateCommandQueue failed: 0x%08X\n", hr);
            return hr;
        }

        // 6) SwapChainDesc1
        DXGI_SWAP_CHAIN_DESC1 scDesc = {};
        scDesc.BufferCount = 2;
        scDesc.Width = 1;
        scDesc.Height = 1;
        scDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        scDesc.SampleDesc.Count = 1;

        ComPtr<IDXGISwapChain1> swapChain1;
        hr = factory->CreateSwapChainForHwnd(
            pCommandQueue.Get(),
            hDummyWindow,
            &scDesc,
            nullptr, nullptr,
            &swapChain1
        );
        if (FAILED(hr)) {
            DebugLog("[hooks] CreateSwapChainForHwnd failed: 0x%08X\n", hr);
            return hr;
        }

        // 7) Query IDXGISwapChain3
        hr = swapChain1.As(&pSwapChain);
        if (FAILED(hr)) {
            DebugLog("[hooks] QueryInterface IDXGISwapChain3 failed: 0x%08X\n", hr);
            return hr;
        }

        return S_OK;
    }

    void Init() {
        DebugLog("[hooks] Init starting\n");
        DebugLog(
            "[hooks] VTable indices - Present:%zu Present1:%zu ResizeBuffers:%zu ExecuteCmdLists:%zu Signal:%zu\n",
            kPresentIndex, kPresent1Index, kResizeBuffersIndex, kExecuteCommandListsIndex, kSignalIndex);

        struct CleanupGuard {
            ~CleanupGuard() { CleanupDummyObjects(); }
        } cleanup;

        if (FAILED(CreateDeviceAndSwapChain())) {
            DebugLog("[hooks] Failed to create dummy device/swapchain.\n");
            return;
        }

        MH_STATUS mh;

        // --- Hook Present on SwapChain ---
        auto scVTable = *reinterpret_cast<void***>(pSwapChain.Get());
        mh = MH_CreateHook(
            reinterpret_cast<LPVOID>(scVTable[kPresentIndex]),
            reinterpret_cast<LPVOID>(d3d12hook::hookPresentD3D12),
            reinterpret_cast<LPVOID*>(&d3d12hook::oPresentD3D12)
        );
        if (mh != MH_OK)
            DebugLog("[hooks] MH_CreateHook Present failed: %s\n", MH_StatusToString(mh));

        mh = MH_CreateHook(
            reinterpret_cast<LPVOID>(scVTable[kPresent1Index]),
            reinterpret_cast<LPVOID>(d3d12hook::hookPresent1D3D12),
            reinterpret_cast<LPVOID*>(&d3d12hook::oPresent1D3D12)
        );
        if (mh != MH_OK)
            DebugLog("[hooks] MH_CreateHook Present1 failed: %s\n", MH_StatusToString(mh));

        // --- Hook ResizeBuffers ---
        mh = MH_CreateHook(
            reinterpret_cast<LPVOID>(scVTable[kResizeBuffersIndex]),
            reinterpret_cast<LPVOID>(d3d12hook::hookResizeBuffersD3D12),
            reinterpret_cast<LPVOID*>(&d3d12hook::oResizeBuffersD3D12)
        );
        if (mh != MH_OK)
            DebugLog("[hooks] MH_CreateHook ResizeBuffers failed: %s\n", MH_StatusToString(mh));

        // --- Hook ExecuteCommandLists ---
        auto cqVTable = *reinterpret_cast<void***>(pCommandQueue.Get());
        mh = MH_CreateHook(
            reinterpret_cast<LPVOID>(cqVTable[kExecuteCommandListsIndex]),
            reinterpret_cast<LPVOID>(d3d12hook::hookExecuteCommandListsD3D12),
            reinterpret_cast<LPVOID*>(&d3d12hook::oExecuteCommandListsD3D12)
        );
        if (mh != MH_OK)
            DebugLog("[hooks] MH_CreateHook ExecuteCommandLists failed: %s\n", MH_StatusToString(mh));

        // --- Hook Signal ---
        mh = MH_CreateHook(
            reinterpret_cast<LPVOID>(cqVTable[kSignalIndex]),
            reinterpret_cast<LPVOID>(d3d12hook::hookSignalD3D12),
            reinterpret_cast<LPVOID*>(&d3d12hook::oSignalD3D12)
        );
        if (mh != MH_OK)
            DebugLog("[hooks] MH_CreateHook Signal failed: %s\n", MH_StatusToString(mh));

        // --- Enable all hooks ---
        mh = MH_EnableHook(MH_ALL_HOOKS);
        if (mh != MH_OK)
            DebugLog("[hooks] MH_EnableHook failed: %s\n", MH_StatusToString(mh));
        else
            DebugLog(
                "[hooks] Hooks enabled. Present@%p (idx=%zu), Present1@%p (idx=%zu), Resize@%p (idx=%zu), Exec@%p (idx=%zu), Signal@%p (idx=%zu)\n",
                reinterpret_cast<LPVOID>(scVTable[kPresentIndex]), kPresentIndex,
                reinterpret_cast<LPVOID>(scVTable[kPresent1Index]), kPresent1Index,
                reinterpret_cast<LPVOID>(scVTable[kResizeBuffersIndex]), kResizeBuffersIndex,
                reinterpret_cast<LPVOID>(cqVTable[kExecuteCommandListsIndex]), kExecuteCommandListsIndex,
                reinterpret_cast<LPVOID>(cqVTable[kSignalIndex]), kSignalIndex
            );
    }
}
