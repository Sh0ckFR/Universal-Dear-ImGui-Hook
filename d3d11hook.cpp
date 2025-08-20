#include "stdafx.h"
#include <d3d11.h>
#include "imgui/backends/imgui_impl_dx11.h"
#include "d3d11hook.h"

#pragma comment(lib, "d3d11.lib")

namespace hooks_dx11 {
    using Microsoft::WRL::ComPtr;

    PresentFn       oPresentD3D11 = nullptr;
    ResizeBuffersFn oResizeBuffersD3D11 = nullptr;

    static ID3D11Device*            gDevice = nullptr;
    static ID3D11DeviceContext*     gContext = nullptr;
    static IDXGISwapChain*          gSwapChain = nullptr;
    static ID3D11RenderTargetView*  gRTV = nullptr;
    static bool                     gInitialized = false;

    static void CreateRenderTarget()
    {
        ID3D11Texture2D* pBackBuffer = nullptr;
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

    HRESULT __stdcall hookPresentD3D11(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags)
    {
        if (!gInitialized)
        {
            gSwapChain = pSwapChain;
            if (SUCCEEDED(pSwapChain->GetDevice(__uuidof(ID3D11Device), (void**)&gDevice)))
            {
                gDevice->GetImmediateContext(&gContext);

                DXGI_SWAP_CHAIN_DESC desc{};
                pSwapChain->GetDesc(&desc);

                ImGui::CreateContext();
                ImGuiIO& io = ImGui::GetIO(); (void)io;
                io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
                ImGui::StyleColorsDark();
                ImGui_ImplWin32_Init(desc.OutputWindow);
                ImGui_ImplDX11_Init(gDevice, gContext);
                inputhook::Init(desc.OutputWindow);
                CreateRenderTarget();
                gInitialized = true;
                DebugLog("[d3d11hook] ImGui initialized.\n");
            }
        }

        if (GetAsyncKeyState(globals::openMenuKey) & 1)
        {
            menu::isOpen = !menu::isOpen;
            DebugLog("[d3d11hook] Toggle menu: %d\n", menu::isOpen);
        }

        if (gInitialized)
        {
            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            if (menu::isOpen)
                menu::Init();
            ImGui::EndFrame();
            ImGui::Render();
            gContext->OMSetRenderTargets(1, &gRTV, nullptr);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        }

        return oPresentD3D11(pSwapChain, SyncInterval, Flags);
    }

    HRESULT __stdcall hookResizeBuffersD3D11(
        IDXGISwapChain* pSwapChain,
        UINT BufferCount,
        UINT Width,
        UINT Height,
        DXGI_FORMAT NewFormat,
        UINT SwapChainFlags)
    {
        if (gInitialized)
        {
            ImGui_ImplDX11_InvalidateDeviceObjects();
            CleanupRenderTarget();
        }

        HRESULT hr = oResizeBuffersD3D11(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);

        if (gInitialized)
        {
            CreateRenderTarget();
            ImGui_ImplDX11_CreateDeviceObjects();
        }

        return hr;
    }

    void Init()
    {
        DebugLog("[d3d11hook] Init starting\n");

        WNDCLASSEXW wc = {
            sizeof(WNDCLASSEXW), CS_CLASSDC, DefWindowProcW,
            0L, 0L, GetModuleHandleW(nullptr), nullptr, nullptr, nullptr, nullptr,
            L"DummyDX11", nullptr
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

        D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
        ID3D11Device* device = nullptr;
        ID3D11DeviceContext* context = nullptr;
        IDXGISwapChain* swapChain = nullptr;

        HRESULT hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &featureLevel, 1,
            D3D11_SDK_VERSION, &sd, &swapChain, &device, nullptr, &context);

        if (SUCCEEDED(hr))
        {
            void** vtbl = *reinterpret_cast<void***>(swapChain);
            MH_CreateHook(vtbl[8], hookPresentD3D11, reinterpret_cast<void**>(&oPresentD3D11));
            MH_CreateHook(vtbl[13], hookResizeBuffersD3D11, reinterpret_cast<void**>(&oResizeBuffersD3D11));
            MH_EnableHook(vtbl[8]);
            MH_EnableHook(vtbl[13]);
            DebugLog("[d3d11hook] Hooks placed Present@%p ResizeBuffers@%p\n", vtbl[8], vtbl[13]);
            swapChain->Release();
            device->Release();
            context->Release();
        }
        else
        {
            DebugLog("[d3d11hook] D3D11CreateDeviceAndSwapChain failed: 0x%08X\n", hr);
        }

        DestroyWindow(hwnd);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
    }

    void release()
    {
        DebugLog("[d3d11hook] Releasing resources\n");

        if (globals::mainWindow)
            inputhook::Remove(globals::mainWindow);

        if (gInitialized)
        {
            ImGui_ImplDX11_Shutdown();
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
            CleanupRenderTarget();
            if (gContext)
            {
                gContext->Release();
                gContext = nullptr;
            }
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
