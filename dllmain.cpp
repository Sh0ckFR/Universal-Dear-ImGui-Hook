#include "stdafx.h"
#include <cstring>

// Pointers to original LoadLibrary functions
using LoadLibraryA_t = HMODULE(WINAPI*)(LPCSTR);
using LoadLibraryW_t = HMODULE(WINAPI*)(LPCWSTR);
static LoadLibraryA_t oLoadLibraryA = nullptr;
static LoadLibraryW_t oLoadLibraryW = nullptr;

// Helper: check loaded module name and initialize hooks if needed
static void InitForModule(const char* name)
{
    if (!name || globals::activeBackend != globals::Backend::None)
        return;

    const char* base = strrchr(name, '\\');
    base = base ? base + 1 : name;

    if (_stricmp(base, "d3d9.dll") == 0) {
        DebugLog("[DllMain] LoadLibrary detected d3d9.dll, initializing DX9 hooks.\n");
        d3d9hook::Init();
        globals::activeBackend = globals::Backend::DX9;
    }
    else if (_stricmp(base, "d3d10.dll") == 0) {
        DebugLog("[DllMain] LoadLibrary detected d3d10.dll, initializing DX10 hooks.\n");
        hooks_dx10::Init();
        globals::activeBackend = globals::Backend::DX10;
    }
    else if (_stricmp(base, "d3d11.dll") == 0) {
        DebugLog("[DllMain] LoadLibrary detected d3d11.dll, initializing DX11 hooks.\n");
        hooks_dx11::Init();
        globals::activeBackend = globals::Backend::DX11;
    }
    else if (_stricmp(base, "d3d12.dll") == 0 || _stricmp(base, "dxgi.dll") == 0) {
        DebugLog("[DllMain] LoadLibrary detected DX12/dxgi module, initializing DX12 hooks.\n");
        hooks::Init();
        globals::activeBackend = globals::Backend::DX12;
    }
    else if (_stricmp(base, "vulkan-1.dll") == 0) {
        DebugLog("[DllMain] LoadLibrary detected vulkan-1.dll, initializing Vulkan hooks.\n");
        hooks_vk::Init();
        globals::activeBackend = globals::Backend::Vulkan;
    }
}

// Hooked LoadLibraryA
static HMODULE WINAPI hookLoadLibraryA(LPCSTR lpLibFileName)
{
    HMODULE mod = oLoadLibraryA(lpLibFileName);
    if (mod)
        InitForModule(lpLibFileName);
    return mod;
}

// Hooked LoadLibraryW
static HMODULE WINAPI hookLoadLibraryW(LPCWSTR lpLibFileName)
{
    HMODULE mod = oLoadLibraryW(lpLibFileName);
    if (mod && lpLibFileName)
    {
        char name[MAX_PATH];
        WideCharToMultiByte(CP_ACP, 0, lpLibFileName, -1, name, MAX_PATH, nullptr, nullptr);
        InitForModule(name);
    }
    return mod;
}

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
        d3d9hook::Init();
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

    // Hook LoadLibraryA/W to catch backends loaded after injection
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    if (k32) {
        LPVOID addrA = GetProcAddress(k32, "LoadLibraryA");
        LPVOID addrW = GetProcAddress(k32, "LoadLibraryW");
        if (addrA) {
            MH_CreateHook(addrA, reinterpret_cast<LPVOID>(hookLoadLibraryA), reinterpret_cast<LPVOID*>(&oLoadLibraryA));
            MH_EnableHook(addrA);
            DebugLog("[DllMain] Hooked LoadLibraryA@%p\n", addrA);
        }
        if (addrW) {
            MH_CreateHook(addrW, reinterpret_cast<LPVOID>(hookLoadLibraryW), reinterpret_cast<LPVOID*>(&oLoadLibraryW));
            MH_EnableHook(addrW);
            DebugLog("[DllMain] Hooked LoadLibraryW@%p\n", addrW);
        }
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
            d3d9hook::release();
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
