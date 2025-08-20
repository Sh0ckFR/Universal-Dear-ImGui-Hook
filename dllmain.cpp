#include "stdafx.h"

// Thread entry: initialize MinHook and start hook setup
static DWORD WINAPI onAttach(LPVOID lpParameter)
{
    DebugLog("[DllMain] onAttach starting.\n");

    // Initialize MinHook
    {
        MH_STATUS mhStatus = MH_Initialize();
        if (mhStatus != MH_OK) {
            DebugLog("[DllMain] MinHook initialization failed: %s\n",
                MH_StatusToString(mhStatus));
            return 1;
        }
        DebugLog("[DllMain] MinHook initialized.\n");
    }

    // Detect loaded rendering backends and initialize hooks accordingly
    HMODULE mod = nullptr;
    if ((mod = GetModuleHandleA("d3d9.dll"))) {
        DebugLog("[DllMain] Detected d3d9.dll (%p). Initializing DX9 hooks.\n", mod);
        hooks_dx9::Init();
        globals::activeBackend = globals::Backend::DX9;
    }
    else if ((mod = GetModuleHandleA("d3d10.dll"))) {
        DebugLog("[DllMain] Detected d3d10.dll (%p). Initializing DX10 hooks.\n", mod);
        hooks_dx10::Init();
        globals::activeBackend = globals::Backend::DX10;
    }
    else if ((mod = GetModuleHandleA("d3d11.dll"))) {
        DebugLog("[DllMain] Detected d3d11.dll (%p). Initializing DX11 hooks.\n", mod);
        hooks_dx11::Init();
        globals::activeBackend = globals::Backend::DX11;
    }
    else if ((mod = GetModuleHandleA("d3d12.dll")) || (mod = GetModuleHandleA("dxgi.dll"))) {
        DebugLog("[DllMain] Detected DX12/dxgi module (%p). Initializing DX12 hooks.\n", mod);
        hooks::Init();
        globals::activeBackend = globals::Backend::DX12;
    }
    else if ((mod = GetModuleHandleA("vulkan-1.dll"))) {
        DebugLog("[DllMain] Detected vulkan-1.dll (%p). Initializing Vulkan hooks.\n", mod);
        hooks_vk::Init();
        globals::activeBackend = globals::Backend::Vulkan;
    }
    else {
        DebugLog("[DllMain] No supported rendering backend detected.\n");
    }

    DebugLog("[DllMain] Hook initialization completed.\n");
    return 0;
}

BOOL WINAPI DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
        DebugLog("[DllMain] DLL_PROCESS_ATTACH: hModule=%p\n", hModule);
        globals::mainModule = hModule;
        // Create a thread for hook setup to avoid blocking loading
        {
            HANDLE thread = CreateThread(
                nullptr, 0,
                onAttach,
                nullptr,
                0,
                nullptr
            );
            if (thread) CloseHandle(thread);
            else DebugLog("[DllMain] Failed to create hook thread: %d\n", GetLastError());
        }
        break;

    case DLL_PROCESS_DETACH:
        DebugLog("[DllMain] DLL_PROCESS_DETACH. Releasing hooks and uninitializing MinHook.\n");
        switch (globals::activeBackend) {
        case globals::Backend::DX9:
            hooks_dx9::release();
            break;
        case globals::Backend::DX10:
            hooks_dx10::release();
            break;
        case globals::Backend::DX11:
            hooks_dx11::release();
            break;
        case globals::Backend::DX12:
            d3d12hook::release();
            break;
        case globals::Backend::Vulkan:
            hooks_vk::release();
            break;
        default:
            break;
        }
        break;
    }
    return TRUE;
}
