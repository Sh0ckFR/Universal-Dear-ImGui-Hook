#include "stdafx.h"

namespace d3d12hook {
    PresentD3D12            oPresentD3D12 = nullptr;
    Present1Fn              oPresent1D3D12 = nullptr;
    ExecuteCommandListsFn   oExecuteCommandListsD3D12 = nullptr;
    SignalFn                oSignalD3D12 = nullptr;
    ResizeBuffersFn         oResizeBuffersD3D12 = nullptr;

    static ID3D12Device* gDevice = nullptr;
    static ID3D12CommandQueue* gCommandQueue = nullptr;
    static ID3D12DescriptorHeap* gHeapRTV = nullptr;
    static ID3D12DescriptorHeap* gHeapSRV = nullptr;
    static ID3D12GraphicsCommandList* gCommandList = nullptr;
    static ID3D12Fence* gFence = nullptr;
    static HANDLE                   gFenceEvent = nullptr;
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

        if (GetAsyncKeyState(globals::uninjectKey) & 1) {
            Uninject();
            return oPresentD3D12(pSwapChain, SyncInterval, Flags);
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

            if (!gFenceEvent) {
                gFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
                if (!gFenceEvent) {
                    DebugLog("[d3d12hook] Failed to create fence event: %lu\n", GetLastError());
                }
            }

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

            // Wait for the GPU to finish with the previous frame
            if (!gFence || !gFenceEvent) {
                // Missing synchronization objects, skip waiting
            } else if (gFence->GetCompletedValue() < gFenceValue) {
                HRESULT hr = gFence->SetEventOnCompletion(gFenceValue, gFenceEvent);
                if (SUCCEEDED(hr)) {
                    const DWORD waitTimeoutMs = 1000; // 1 second timeout
                    DWORD waitRes = WaitForSingleObject(gFenceEvent, waitTimeoutMs);
                    if (waitRes == WAIT_TIMEOUT) {
                        DebugLog("[d3d12hook] WaitForSingleObject timeout\n");
                    } else if (waitRes != WAIT_OBJECT_0) {
                        DebugLog("[d3d12hook] WaitForSingleObject failed: %lu\n", GetLastError());
                    }
                } else {
                    LogHRESULT("SetEventOnCompletion", hr);
                }
            }

            // Reset allocator and command list using frame-specific allocator
            HRESULT hr = ctx.allocator->Reset();
            if (FAILED(hr)) {
                LogHRESULT("CommandAllocator->Reset", hr);
                return oPresentD3D12(pSwapChain, SyncInterval, Flags);
            }

            if (!gCommandList) {
                hr = gDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                    ctx.allocator, nullptr, IID_PPV_ARGS(&gCommandList));
                if (FAILED(hr)) {
                    LogHRESULT("CreateCommandList", hr);
                    return oPresentD3D12(pSwapChain, SyncInterval, Flags);
                }
                gCommandList->Close();
            }
            hr = gCommandList->Reset(ctx.allocator, nullptr);
            if (FAILED(hr)) {
                LogHRESULT("CommandList->Reset", hr);
                return oPresentD3D12(pSwapChain, SyncInterval, Flags);
            }

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
                oExecuteCommandListsD3D12(gCommandQueue, 1, reinterpret_cast<ID3D12CommandList* const*>(&gCommandList));
                if (gFence) {
                    HRESULT hr = gCommandQueue->Signal(gFence, ++gFenceValue);
                    if (FAILED(hr)) {
                        LogHRESULT("Signal", hr);
                    }
                }
            }
        }

        return oPresentD3D12(pSwapChain, SyncInterval, Flags);
    }

    long __fastcall hookPresent1D3D12(IDXGISwapChain3* pSwapChain, UINT SyncInterval, UINT Flags, const DXGI_PRESENT_PARAMETERS* pParams) {
        if (GetAsyncKeyState(globals::openMenuKey) & 1) {
            menu::isOpen = !menu::isOpen;
            DebugLog("[d3d12hook] Toggle menu: isOpen=%d\n", menu::isOpen);
        }

        if (GetAsyncKeyState(globals::uninjectKey) & 1) {
            Uninject();
            return oPresent1D3D12(pSwapChain, SyncInterval, Flags, pParams);
        }

        if (!gInitialized) {
            DebugLog("[d3d12hook] Initializing ImGui on first Present1.\n");
            if (FAILED(pSwapChain->GetDevice(__uuidof(ID3D12Device), (void**)&gDevice))) {
                LogHRESULT("GetDevice", E_FAIL);
                return oPresent1D3D12(pSwapChain, SyncInterval, Flags, pParams);
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
                return oPresent1D3D12(pSwapChain, SyncInterval, Flags, pParams);
            }

            heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            if (FAILED(gDevice->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&gHeapSRV)))) {
                LogHRESULT("CreateDescriptorHeap SRV", E_FAIL);
                return oPresent1D3D12(pSwapChain, SyncInterval, Flags, pParams);
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
                    return oPresent1D3D12(pSwapChain, SyncInterval, Flags, pParams);
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

            if (!gFenceEvent) {
                gFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
                if (!gFenceEvent) {
                    DebugLog("[d3d12hook] Failed to create fence event: %lu\n", GetLastError());
                }
            }

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

            // Wait for the GPU to finish with the previous frame
            if (!gFence || !gFenceEvent) {
                // Missing synchronization objects, skip waiting
            } else if (gFence->GetCompletedValue() < gFenceValue) {
                HRESULT hr = gFence->SetEventOnCompletion(gFenceValue, gFenceEvent);
                if (SUCCEEDED(hr)) {
                    const DWORD waitTimeoutMs = 1000; // 1 second timeout
                    DWORD waitRes = WaitForSingleObject(gFenceEvent, waitTimeoutMs);
                    if (waitRes == WAIT_TIMEOUT) {
                        DebugLog("[d3d12hook] WaitForSingleObject timeout\n");
                    } else if (waitRes != WAIT_OBJECT_0) {
                        DebugLog("[d3d12hook] WaitForSingleObject failed: %lu\n", GetLastError());
                    }
                } else {
                    LogHRESULT("SetEventOnCompletion", hr);
                }
            }

            // Reset allocator and command list using frame-specific allocator
            HRESULT hr = ctx.allocator->Reset();
            if (FAILED(hr)) {
                LogHRESULT("CommandAllocator->Reset", hr);
                return oPresent1D3D12(pSwapChain, SyncInterval, Flags, pParams);
            }

            if (!gCommandList) {
                hr = gDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                    ctx.allocator, nullptr, IID_PPV_ARGS(&gCommandList));
                if (FAILED(hr)) {
                    LogHRESULT("CreateCommandList", hr);
                    return oPresent1D3D12(pSwapChain, SyncInterval, Flags, pParams);
                }
                gCommandList->Close();
            }
            hr = gCommandList->Reset(ctx.allocator, nullptr);
            if (FAILED(hr)) {
                LogHRESULT("CommandList->Reset", hr);
                return oPresent1D3D12(pSwapChain, SyncInterval, Flags, pParams);
            }

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
                oExecuteCommandListsD3D12(gCommandQueue, 1, reinterpret_cast<ID3D12CommandList* const*>(&gCommandList));
                if (gFence) {
                    HRESULT hr = gCommandQueue->Signal(gFence, ++gFenceValue);
                    if (FAILED(hr)) {
                        LogHRESULT("Signal", hr);
                    }
                }
            }
        }

        return oPresent1D3D12(pSwapChain, SyncInterval, Flags, pParams);
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

    HRESULT STDMETHODCALLTYPE hookResizeBuffersD3D12(
        IDXGISwapChain3* pSwapChain,
        UINT BufferCount,
        UINT Width,
        UINT Height,
        DXGI_FORMAT NewFormat,
        UINT SwapChainFlags)
    {
        DebugLog("[d3d12hook] ResizeBuffers called: %ux%u Buffers=%u\n",
            Width, Height, BufferCount);

        if (gInitialized)
        {
            DebugLog("[d3d12hook] Releasing resources for resize\n");

            ImGui_ImplDX12_Shutdown();
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
            inputhook::Remove(globals::mainWindow);

            if (gCommandList)
            {
                gCommandList->Release();
                gCommandList = nullptr;
            }
            if (gHeapRTV)
            {
                gHeapRTV->Release();
                gHeapRTV = nullptr;
            }
            if (gHeapSRV)
            {
                gHeapSRV->Release();
                gHeapSRV = nullptr;
            }

            for (UINT i = 0; i < gBufferCount; ++i)
            {
                if (gFrameContexts[i].renderTarget)
                {
                    gFrameContexts[i].renderTarget->Release();
                    gFrameContexts[i].renderTarget = nullptr;
                }
                if (gFrameContexts[i].allocator)
                {
                    gFrameContexts[i].allocator->Release();
                    gFrameContexts[i].allocator = nullptr;
                }
            }

            delete[] gFrameContexts;
            gFrameContexts = nullptr;
            gBufferCount = 0;

            gInitialized = false;
        }

        return oResizeBuffersD3D12(
            pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);
    }

    void release() {
        DebugLog("[d3d12hook] Releasing resources and hooks.\n");
        gShutdown = true;
        if (globals::mainWindow) {
            inputhook::Remove(globals::mainWindow);
        }

        // Shutdown ImGui before releasing any D3D resources
        ImGui_ImplDX12_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();

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

        if (gFenceEvent)
        {
            CloseHandle(gFenceEvent);
            gFenceEvent = nullptr;
        }

        if (gDevice) gDevice->Release();
        delete[] gFrameContexts;

        // Unhook
        MH_STATUS mh = MH_DisableHook(MH_ALL_HOOKS);
        if (mh != MH_OK)
            DebugLog("[d3d12hook] MH_DisableHook failed: %s\n", MH_StatusToString(mh));
        else
            DebugLog("[d3d12hook] Hooks disabled.\n");

        mh = MH_RemoveHook(MH_ALL_HOOKS);
        if (mh != MH_OK)
            DebugLog("[d3d12hook] MH_RemoveHook failed: %s\n", MH_StatusToString(mh));
        else
            DebugLog("[d3d12hook] Hooks removed.\n");

        // Uninitialize MinHook
        MH_Uninitialize();
        DebugLog("[DllMain] MinHook uninitialized.\n");
    }
}
