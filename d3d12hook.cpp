#include "stdafx.h"

namespace d3d12hook {
    PresentD3D12            oPresentD3D12 = nullptr;
    ExecuteCommandListsFn   oExecuteCommandListsD3D12 = nullptr;
    SignalFn                oSignalD3D12 = nullptr;

    static ID3D12Device* gDevice = nullptr;
    static ID3D12CommandQueue* gCommandQueue = nullptr;
    static ID3D12DescriptorHeap* gHeapRTV = nullptr;
    static ID3D12DescriptorHeap* gHeapSRV = nullptr;
    static ID3D12GraphicsCommandList* gCommandList = nullptr;
    static ID3D12Fence* gFence = nullptr;
    static UINT64                  gFenceValue = 0;
    static uintx_t                 gBufferCount = 0;

    struct FrameContext {
        ID3D12CommandAllocator* allocator;
        ID3D12Resource* renderTarget;
        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle;
    };
    static FrameContext* gFrameContexts = nullptr;
    static bool                   gInitialized = false;
    static bool                   gShutdown = false;

    // Utility to log HRESULTs
    inline void LogHRESULT(const char* label, HRESULT hr) {
        DebugLog("[d3d12hook] %s: hr=0x%08X\n", label, hr);
    }

    long __fastcall hookPresentD3D12(IDXGISwapChain3* pSwapChain, UINT SyncInterval, UINT Flags) {
        if (GetAsyncKeyState(globals::openMenuKey) & 1) {
            menu::isOpen = !menu::isOpen;
            DebugLog("[d3d12hook] Toggle menu: isOpen=%d\n", menu::isOpen);
        }

        if (!gInitialized) {
            DebugLog("[d3d12hook] Initializing ImGui on first Present.\n");
            if (FAILED(pSwapChain->GetDevice(__uuidof(ID3D12Device), (void**)&gDevice))) {
                LogHRESULT("GetDevice", E_FAIL);
                return oPresentD3D12(pSwapChain, SyncInterval, Flags);
            }

            // Swap Chain description
            DXGI_SWAP_CHAIN_DESC desc = {};
            pSwapChain->GetDesc(&desc);
            gBufferCount = desc.BufferCount;
            DebugLog("[d3d12hook] BufferCount=%u\n", gBufferCount);

            // Create descriptor heaps
            D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
            heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            heapDesc.NumDescriptors = gBufferCount;
            if (FAILED(gDevice->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&gHeapRTV)))) {
                LogHRESULT("CreateDescriptorHeap RTV", E_FAIL);
                return oPresentD3D12(pSwapChain, SyncInterval, Flags);
            }

            heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            if (FAILED(gDevice->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&gHeapSRV)))) {
                LogHRESULT("CreateDescriptorHeap SRV", E_FAIL);
                return oPresentD3D12(pSwapChain, SyncInterval, Flags);
            }

            // Allocate frame contexts
            gFrameContexts = new FrameContext[gBufferCount];
            ZeroMemory(gFrameContexts, sizeof(FrameContext) * gBufferCount);

            // Create command allocator for each frame
            for (UINT i = 0; i < gBufferCount; ++i) {
                if (FAILED(gDevice->CreateCommandAllocator(
                        D3D12_COMMAND_LIST_TYPE_DIRECT,
                        IID_PPV_ARGS(&gFrameContexts[i].allocator)))) {
                    LogHRESULT("CreateCommandAllocator", E_FAIL);
                    return oPresentD3D12(pSwapChain, SyncInterval, Flags);
                }
            }

            // Create RTVs for each back buffer
            UINT rtvSize = gDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            auto rtvHandle = gHeapRTV->GetCPUDescriptorHandleForHeapStart();
            for (UINT i = 0; i < gBufferCount; ++i) {
                ID3D12Resource* back;
                pSwapChain->GetBuffer(i, IID_PPV_ARGS(&back));
                gDevice->CreateRenderTargetView(back, nullptr, rtvHandle);
                gFrameContexts[i].renderTarget = back;
                gFrameContexts[i].rtvHandle = rtvHandle;
                rtvHandle.ptr += rtvSize;
            }

            // ImGui setup
            ImGui::CreateContext();
            ImGuiIO& io = ImGui::GetIO(); (void)io;
            io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
            ImGui::StyleColorsDark();
            ImGui_ImplWin32_Init(desc.OutputWindow);
            ImGui_ImplDX12_Init(gDevice, gBufferCount,
                desc.BufferDesc.Format,
                gHeapSRV,
                gHeapSRV->GetCPUDescriptorHandleForHeapStart(),
                gHeapSRV->GetGPUDescriptorHandleForHeapStart());
            DebugLog("[d3d12hook] ImGui initialized.\n");

            inputhook::Init(desc.OutputWindow);

            // Hook CommandQueue and Fence are already captured by minhook
            gInitialized = true;
        }

        if (!gShutdown) {
            // Render ImGui
            ImGui_ImplDX12_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();

            if (menu::isOpen) menu::Init();

            UINT frameIdx = pSwapChain->GetCurrentBackBufferIndex();
            FrameContext& ctx = gFrameContexts[frameIdx];

            // Reset allocator and command list using frame-specific allocator
            ctx.allocator->Reset();

            if (!gCommandList) {
                gDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                    ctx.allocator, nullptr, IID_PPV_ARGS(&gCommandList));
                gCommandList->Close();
            }
            gCommandList->Reset(ctx.allocator, nullptr);

            // Transition to render target
            D3D12_RESOURCE_BARRIER barrier = {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = ctx.renderTarget;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            gCommandList->ResourceBarrier(1, &barrier);

            gCommandList->OMSetRenderTargets(1, &ctx.rtvHandle, FALSE, nullptr);
            ID3D12DescriptorHeap* heaps[] = { gHeapSRV };
            gCommandList->SetDescriptorHeaps(1, heaps);

            ImGui::Render();
            ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), gCommandList);

            // Transition back to present
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
            gCommandList->ResourceBarrier(1, &barrier);
            gCommandList->Close();

            // Execute
            if (!gCommandQueue) {
                DebugLog("[d3d12hook] CommandQueue not set, skipping ExecuteCommandLists.\n");
            }
            else {
                gCommandQueue->ExecuteCommandLists(1, reinterpret_cast<ID3D12CommandList* const*>(&gCommandList));
            }
        }

        return oPresentD3D12(pSwapChain, SyncInterval, Flags);
    }

    void STDMETHODCALLTYPE hookExecuteCommandListsD3D12(
        ID3D12CommandQueue * _this,
        UINT                          NumCommandLists,
        ID3D12CommandList* const* ppCommandLists) {
        if (!gCommandQueue) {
            gCommandQueue = _this;
            DebugLog("[d3d12hook] Captured CommandQueue=%p\n", _this);
        }
        oExecuteCommandListsD3D12(_this, NumCommandLists, ppCommandLists);
    }

    HRESULT STDMETHODCALLTYPE hookSignalD3D12(
        ID3D12CommandQueue* _this,
        ID3D12Fence*        pFence,
        UINT64              Value)
    {
        if (gCommandQueue && _this == gCommandQueue)
        {
            if (pFence && pFence != gFence)
            {
                if (gFence)
                    gFence->Release();
                gFence = pFence;
                gFence->AddRef();
            }

            gFenceValue = Value;
            DebugLog("[d3d12hook] Captured Fence=%p, Value=%llu\n", pFence, Value);
        }

        return oSignalD3D12(_this, pFence, Value);
    }

    void release() {
        DebugLog("[d3d12hook] Releasing resources.\n");
        gShutdown = true;
        if (gCommandList) gCommandList->Release();
        if (gHeapRTV) gHeapRTV->Release();
        if (gHeapSRV) gHeapSRV->Release();

        for (UINT i = 0; i < gBufferCount; ++i) {
            if (gFrameContexts[i].renderTarget) gFrameContexts[i].renderTarget->Release();
        }
        if (gFence)
        {
            gFence->Release();
            gFence = nullptr;
        }

        if (gDevice) gDevice->Release();
        delete[] gFrameContexts;

        // Unhook
        MH_DisableHook(MH_ALL_HOOKS);
        DebugLog("[d3d12hook] Hooks disabled.\n");
    }
}
