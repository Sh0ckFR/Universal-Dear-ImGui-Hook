#include "stdafx.h"

namespace hooks_dx10 {
    using Microsoft::WRL::ComPtr;

    PresentFn       oPresentD3D10 = nullptr;
    Present1Fn      oPresent1D3D10 = nullptr;
    ResizeBuffersFn oResizeBuffersD3D10 = nullptr;

    static ID3D10Device*           gDevice = nullptr;
    static IDXGISwapChain*         gSwapChain = nullptr;
    static ID3D10RenderTargetView* gRTV = nullptr;
    static bool                    gInitialized = false;

    static void CreateRenderTarget()
    {
        ID3D10Texture2D* pBackBuffer = nullptr;
        if (gSwapChain && SUCCEEDED(gSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer))))
        {
            gDevice->CreateRenderTargetView(pBackBuffer, nullptr, &gRTV);
            pBackBuffer->Release();
        }
    }

    static void CleanupRenderTarget()
    {
        if (gRTV)
        {
            gRTV->Release();
            gRTV = nullptr;
        }
    }

    static void RenderFrame(IDXGISwapChain* pSwapChain)
    {
        if (!gInitialized)
        {
            gSwapChain = pSwapChain;
            if (SUCCEEDED(pSwapChain->GetDevice(__uuidof(ID3D10Device), (void**)&gDevice)))
            {
                DXGI_SWAP_CHAIN_DESC desc{};
                pSwapChain->GetDesc(&desc);

                ImGui::CreateContext();
                ImGuiIO& io = ImGui::GetIO(); (void)io;
                io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
                ImGui::StyleColorsDark();
                ImGui_ImplWin32_Init(desc.OutputWindow);
                ImGui_ImplDX10_Init(gDevice);
                inputhook::Init(desc.OutputWindow);
                CreateRenderTarget();
                gInitialized = true;
                DebugLog("[d3d10hook] ImGui initialized.\n");
            }
        }

        if (GetAsyncKeyState(globals::openMenuKey) & 1)
        {
            menu::isOpen = !menu::isOpen;
            DebugLog("[d3d10hook] Toggle menu: %d\n", menu::isOpen);
        }

        if (gInitialized)
        {
            ImGui_ImplDX10_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            if (menu::isOpen)
                menu::Init();
            ImGui::EndFrame();
            ImGui::Render();
            gDevice->OMSetRenderTargets(1, &gRTV, nullptr);
            ImGui_ImplDX10_RenderDrawData(ImGui::GetDrawData());
        }
    }

    HRESULT __stdcall hookPresentD3D10(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags)
    {
        RenderFrame(pSwapChain);
        return oPresentD3D10(pSwapChain, SyncInterval, Flags);
    }

    HRESULT __stdcall hookPresent1D3D10(IDXGISwapChain1* pSwapChain, UINT SyncInterval, UINT Flags, const DXGI_PRESENT_PARAMETERS* pPresentParameters)
    {
        RenderFrame(pSwapChain);
        return oPresent1D3D10(pSwapChain, SyncInterval, Flags, pPresentParameters);
    }

    HRESULT __stdcall hookResizeBuffersD3D10(
        IDXGISwapChain* pSwapChain,
        UINT BufferCount,
        UINT Width,
        UINT Height,
        DXGI_FORMAT NewFormat,
        UINT SwapChainFlags)
    {
        if (gInitialized)
        {
            ImGui_ImplDX10_InvalidateDeviceObjects();
            CleanupRenderTarget();
        }

        HRESULT hr = oResizeBuffersD3D10(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);

        if (gInitialized)
        {
            CreateRenderTarget();
            ImGui_ImplDX10_CreateDeviceObjects();
        }

        return hr;
    }

    void Init()
    {
        DebugLog("[d3d10hook] Init starting\n");

        WNDCLASSEXW wc = {
            sizeof(WNDCLASSEXW), CS_CLASSDC, DefWindowProcW,
            0L, 0L, GetModuleHandleW(nullptr), nullptr, nullptr, nullptr, nullptr,
            L"DummyDX10", nullptr
        };
        RegisterClassExW(&wc);
        HWND hwnd = CreateWindowW(wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW,
            0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);

        DXGI_SWAP_CHAIN_DESC sd{};
        sd.BufferCount = 2;
        sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow = hwnd;
        sd.SampleDesc.Count = 1;
        sd.Windowed = TRUE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

        ID3D10Device* device = nullptr;
        IDXGISwapChain* swapChain = nullptr;
        HRESULT hr = D3D10CreateDeviceAndSwapChain(
            nullptr, D3D10_DRIVER_TYPE_HARDWARE, nullptr, 0,
            D3D10_SDK_VERSION, &sd, &swapChain, &device);

        if (SUCCEEDED(hr))
        {
            void** vtbl = *reinterpret_cast<void***>(swapChain);
            MH_CreateHook(vtbl[8], hookPresentD3D10, reinterpret_cast<void**>(&oPresentD3D10));
            MH_CreateHook(vtbl[13], hookResizeBuffersD3D10, reinterpret_cast<void**>(&oResizeBuffersD3D10));

            IDXGISwapChain1* swapChain1 = nullptr;
            void* present1Addr = nullptr;
            if (SUCCEEDED(swapChain->QueryInterface(__uuidof(IDXGISwapChain1), (void**)&swapChain1)))
            {
                void** vtbl1 = *reinterpret_cast<void***>(swapChain1);
                present1Addr = vtbl1[22];
                MH_CreateHook(vtbl1[22], hookPresent1D3D10, reinterpret_cast<void**>(&oPresent1D3D10));
                MH_EnableHook(vtbl1[22]);
                swapChain1->Release();
            }

            MH_EnableHook(vtbl[8]);
            MH_EnableHook(vtbl[13]);
            DebugLog("[d3d10hook] Hooks placed Present@%p Present1@%p ResizeBuffers@%p\n", vtbl[8], present1Addr, vtbl[13]);
            swapChain->Release();
            device->Release();
        }
        else
        {
            DebugLog("[d3d10hook] D3D10CreateDeviceAndSwapChain failed: 0x%08X\n", hr);
        }

        DestroyWindow(hwnd);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
    }

    void release()
    {
        DebugLog("[d3d10hook] Releasing resources\n");

        if (globals::mainWindow)
            inputhook::Remove(globals::mainWindow);

        if (gInitialized)
        {
            ImGui_ImplDX10_Shutdown();
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
            CleanupRenderTarget();
            if (gDevice)
            {
                gDevice->Release();
                gDevice = nullptr;
            }
            gSwapChain = nullptr;
            gInitialized = false;
        }

        MH_DisableHook(MH_ALL_HOOKS);
        MH_RemoveHook(MH_ALL_HOOKS);
        MH_Uninitialize();
    }
}
