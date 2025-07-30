#include "stdafx.h"

using Microsoft::WRL::ComPtr;

namespace hooks {
    // Dummy objects pour extraire les v-tables
    static ComPtr<IDXGISwapChain3>       pSwapChain = nullptr;
    static ComPtr<ID3D12Device>          pDevice = nullptr;
    static ComPtr<ID3D12CommandQueue>    pCommandQueue = nullptr;
    static HWND                          hDummyWindow = nullptr;
    static const wchar_t* dummyClassName = L"DummyWndClass";

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

        if (FAILED(CreateDeviceAndSwapChain())) {
            DebugLog("[hooks] Failed to create dummy device/swapchain.\n");
            return;
        }

        MH_STATUS mh;

        // --- Hook Present on SwapChain (vTable index 8) ---
        auto scVTable = *reinterpret_cast<uintptr_t**>(pSwapChain.Get());
        mh = MH_CreateHook(
            reinterpret_cast<LPVOID>(scVTable[8]),
            reinterpret_cast<LPVOID>(d3d12hook::hookPresentD3D12),
            reinterpret_cast<LPVOID*>(&d3d12hook::oPresentD3D12)
        );
        if (mh != MH_OK)
            DebugLog("[hooks] MH_CreateHook Present failed: %s\n", MH_StatusToString(mh));

        // --- Hook ResizeBuffers (index 13) ---
        mh = MH_CreateHook(
            reinterpret_cast<LPVOID>(scVTable[13]),
            reinterpret_cast<LPVOID>(d3d12hook::hookResizeBuffersD3D12),
            reinterpret_cast<LPVOID*>(&d3d12hook::oResizeBuffersD3D12)
        );
        if (mh != MH_OK)
            DebugLog("[hooks] MH_CreateHook ResizeBuffers failed: %s\n", MH_StatusToString(mh));

        // --- Hook ExecuteCommandLists (index 10) ---
        auto cqVTable = *reinterpret_cast<uintptr_t**>(pCommandQueue.Get());
        mh = MH_CreateHook(
            reinterpret_cast<LPVOID>(cqVTable[10]),
            reinterpret_cast<LPVOID>(d3d12hook::hookExecuteCommandListsD3D12),
            reinterpret_cast<LPVOID*>(&d3d12hook::oExecuteCommandListsD3D12)
        );
        if (mh != MH_OK)
            DebugLog("[hooks] MH_CreateHook ExecuteCommandLists failed: %s\n", MH_StatusToString(mh));

        // --- Hook Signal (index 14) ---
        mh = MH_CreateHook(
            reinterpret_cast<LPVOID>(cqVTable[14]),
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
                "[hooks] Hooks enabled. Present@%p, Resize@%p, Exec@%p, Signal@%p\n",
                reinterpret_cast<LPVOID>(scVTable[8]),
                reinterpret_cast<LPVOID>(scVTable[13]),
                reinterpret_cast<LPVOID>(cqVTable[10]),
                reinterpret_cast<LPVOID>(cqVTable[14])
            );
    }
}
