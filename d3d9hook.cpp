#include "stdafx.h"

namespace d3d9hook {
    EndSceneFn oEndScene = nullptr;
    ResetFn    oReset    = nullptr;

    static bool gInitialized = false;

    HRESULT __stdcall hookEndScene(IDirect3DDevice9* device) {
        if (!gInitialized) {
            D3DDEVICE_CREATION_PARAMETERS params{};
            if (SUCCEEDED(device->GetCreationParameters(&params))) {
                ImGui::CreateContext();
                ImGuiIO& io = ImGui::GetIO(); (void)io;
                io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
                ImGui::StyleColorsDark();
                ImGui_ImplWin32_Init(params.hFocusWindow);
                ImGui_ImplDX9_Init(device);
                inputhook::Init(params.hFocusWindow);
                gInitialized = true;
                DebugLog("[d3d9hook] ImGui initialized on EndScene.\\n");
            }
        }

        if (GetAsyncKeyState(globals::openMenuKey) & 1) {
            menu::isOpen = !menu::isOpen;
            DebugLog("[d3d9hook] Toggle menu: %d\\n", menu::isOpen);
        }

        if (gInitialized) {
            ImGui_ImplDX9_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            if (menu::isOpen) {
                menu::Init();
            }
            ImGui::EndFrame();
            ImGui::Render();
            ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
        }

        return oEndScene(device);
    }

    HRESULT __stdcall hookReset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* params) {
        if (gInitialized) {
            ImGui_ImplDX9_InvalidateDeviceObjects();
        }
        HRESULT hr = oReset(device, params);
        if (gInitialized) {
            ImGui_ImplDX9_CreateDeviceObjects();
        }
        return hr;
    }

    void Init() {
        DebugLog("[d3d9hook] Init starting\\n");
        IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
        if (!d3d) {
            DebugLog("[d3d9hook] Direct3DCreate9 failed\\n");
            return;
        }

        WNDCLASSEX wc{ sizeof(WNDCLASSEX), CS_CLASSDC, DefWindowProc, 0L, 0L,
            GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr,
            L"DummyD3D9", nullptr };
        RegisterClassEx(&wc);
        HWND hwnd = CreateWindow(wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW,
            0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);

        D3DPRESENT_PARAMETERS d3dpp{};
        d3dpp.Windowed = TRUE;
        d3dpp.SwapEffect = D3DSWAPEFFECT_DISCARD;
        d3dpp.hDeviceWindow = hwnd;

        IDirect3DDevice9* device = nullptr;
        HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING, &d3dpp, &device);
        if (SUCCEEDED(hr)) {
            void** vtbl = *reinterpret_cast<void***>(device);
            MH_CreateHook(vtbl[42], reinterpret_cast<void*>(hookEndScene), reinterpret_cast<void**>(&oEndScene));
            MH_CreateHook(vtbl[16], reinterpret_cast<void*>(hookReset), reinterpret_cast<void**>(&oReset));
            MH_EnableHook(vtbl[42]);
            MH_EnableHook(vtbl[16]);
            DebugLog("[d3d9hook] Hooks placed EndScene@%p Reset@%p\\n", vtbl[42], vtbl[16]);
            device->Release();
        } else {
            DebugLog("[d3d9hook] CreateDevice failed: 0x%08X\\n", hr);
        }

        DestroyWindow(hwnd);
        UnregisterClass(wc.lpszClassName, wc.hInstance);
        d3d->Release();
    }

    void release() {
        DebugLog("[d3d9hook] Releasing resources\\n");
        if (globals::mainWindow) {
            inputhook::Remove(globals::mainWindow);
        }
        if (gInitialized) {
            ImGui_ImplDX9_Shutdown();
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
            gInitialized = false;
        }
        MH_DisableHook(MH_ALL_HOOKS);
        MH_RemoveHook(MH_ALL_HOOKS);
        MH_Uninitialize();
    }
}
