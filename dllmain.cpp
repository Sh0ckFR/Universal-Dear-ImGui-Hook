#include "stdafx.h"

namespace mousehooks { void Init(); void Remove(); }

// Utility helpers for backend initialization checks
using IsInitFn = bool (*)();

static bool WaitForInitialization(IsInitFn fn, int attempts = 50, int sleepMs = 100)
{
    for (int i = 0; i < attempts; ++i)
    {
        if (fn())
            return true;
        Sleep(sleepMs);
    }
    return false;
}

static bool TryInitBackend(globals::Backend backend)
{
    switch (backend)
    {
    case globals::Backend::Vulkan:
        if (GetModuleHandleA("vulkan-1.dll"))
        {
            DebugLog("[DllMain] Attempting Vulkan initialization.\n");
            hooks_vk::Init();
            if (WaitForInitialization(hooks_vk::IsInitialized))
            {
                DebugLog("[DllMain] Vulkan initialization succeeded.\n");
                globals::activeBackend = globals::Backend::Vulkan;
                return true;
            }
            DebugLog("[DllMain] Vulkan initialization pending, hooks remain active.\n");
        }
        break;
    case globals::Backend::DX12:
        if (GetModuleHandleA("d3d12.dll") || GetModuleHandleA("dxgi.dll"))
        {
            DebugLog("[DllMain] Attempting DX12 initialization.\n");
            hooks::Init();
            if (WaitForInitialization(d3d12hook::IsInitialized))
            {
                DebugLog("[DllMain] DX12 initialization succeeded.\n");
                globals::activeBackend = globals::Backend::DX12;
                return true;
            }
            DebugLog("[DllMain] DX12 initialization failed, falling back.\n");
            d3d12hook::release();
        }
        break;
    case globals::Backend::DX11:
        if (GetModuleHandleA("d3d11.dll"))
        {
            DebugLog("[DllMain] Attempting DX11 initialization.\n");
            hooks_dx11::Init();
            if (WaitForInitialization(hooks_dx11::IsInitialized))
            {
                DebugLog("[DllMain] DX11 initialization succeeded.\n");
                globals::activeBackend = globals::Backend::DX11;
                return true;
            }
            DebugLog("[DllMain] DX11 initialization failed, falling back.\n");
            hooks_dx11::release();
        }
        break;
    case globals::Backend::DX10:
        if (GetModuleHandleA("d3d10.dll"))
        {
            DebugLog("[DllMain] Attempting DX10 initialization.\n");
            hooks_dx10::Init();
            if (WaitForInitialization(hooks_dx10::IsInitialized))
            {
                DebugLog("[DllMain] DX10 initialization succeeded.\n");
                globals::activeBackend = globals::Backend::DX10;
                return true;
            }
            DebugLog("[DllMain] DX10 initialization failed, falling back.\n");
            hooks_dx10::release();
        }
        break;
    case globals::Backend::DX9:
        if (GetModuleHandleA("d3d9.dll"))
        {
            DebugLog("[DllMain] Attempting DX9 initialization.\n");
            d3d9hook::Init();
            if (WaitForInitialization(d3d9hook::IsInitialized))
            {
                DebugLog("[DllMain] DX9 initialization succeeded.\n");
                globals::activeBackend = globals::Backend::DX9;
                return true;
            }
            DebugLog("[DllMain] DX9 initialization failed, falling back.\n");
            d3d9hook::release();
        }
        break;
    default:
        break;
    }
    return false;
}

static bool TryInitializeFrom(globals::Backend start)
{
    const globals::Backend order[] = {
        globals::Backend::Vulkan,
        globals::Backend::DX12,
        globals::Backend::DX11,
        globals::Backend::DX10,
        globals::Backend::DX9
    };
    int idx = 0;
    for (; idx < 5; ++idx)
    {
        if (order[idx] == start)
            break;
    }
    for (; idx < 5; ++idx)
    {
        if (TryInitBackend(order[idx]))
            return true;
    }
    DebugLog("[DllMain] All backend initialization attempts failed.\n");
    return false;
}

// Pointers to original LoadLibrary functions
using LoadLibraryA_t = HMODULE(WINAPI*)(LPCSTR);
using LoadLibraryW_t = HMODULE(WINAPI*)(LPCWSTR);
static LoadLibraryA_t oLoadLibraryA = nullptr;
static LoadLibraryW_t oLoadLibraryW = nullptr;

// Helper: check loaded module name and initialize hooks if needed
static int GetBackendPriority(globals::Backend backend)
{
    switch (backend)
    {
    case globals::Backend::Vulkan: return 5;
    case globals::Backend::DX12:   return 4;
    case globals::Backend::DX11:   return 3;
    case globals::Backend::DX10:   return 2;
    case globals::Backend::DX9:    return 1;
    default:                       return 0;
    }
}

static void InitForModule(const char* name)
{
    if (!name)
        return;

    const char* base = strrchr(name, '\\');
    base = base ? base + 1 : name;

    globals::Backend detected = globals::Backend::None;
    if (_stricmp(base, "vulkan-1.dll") == 0) {
        detected = globals::Backend::Vulkan;
    }
    else if (_stricmp(base, "d3d12.dll") == 0 || _stricmp(base, "dxgi.dll") == 0) {
        detected = globals::Backend::DX12;
    }
    else if (_stricmp(base, "d3d11.dll") == 0) {
        detected = globals::Backend::DX11;
    }
    else if (_stricmp(base, "d3d10.dll") == 0) {
        detected = globals::Backend::DX10;
    }
    else if (_stricmp(base, "d3d9.dll") == 0) {
        detected = globals::Backend::DX9;
    }
    else {
        return;
    }

    if (globals::preferredBackend != globals::Backend::None && detected != globals::preferredBackend)
        return;

    if (GetBackendPriority(detected) <= GetBackendPriority(globals::activeBackend))
        return;

    switch (globals::activeBackend)
    {
    case globals::Backend::DX9:    d3d9hook::release(); break;
    case globals::Backend::DX10:   hooks_dx10::release(); break;
    case globals::Backend::DX11:   hooks_dx11::release(); break;
    case globals::Backend::DX12:   d3d12hook::release(); break;
    case globals::Backend::Vulkan: hooks_vk::release(); break;
    default: break;
    }

    globals::activeBackend = globals::Backend::None;
    if (globals::preferredBackend != globals::Backend::None)
        TryInitBackend(globals::preferredBackend);
    else
        TryInitializeFrom(detected);
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

// Thread routine that performs cleanup and unloads the DLL
static DWORD WINAPI UninjectThread(LPVOID)
{
    DebugLog("[DllMain] Uninject thread starting.\n");

    switch (globals::activeBackend)
    {
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

    mousehooks::Remove();

    // Disable and remove all hooks, then uninitialize MinHook
    MH_DisableHook(MH_ALL_HOOKS);
    MH_RemoveHook(MH_ALL_HOOKS);
    MH_Uninitialize();

    DebugLog("[DllMain] Unloading module and exiting thread.\n");
    FreeLibraryAndExitThread(globals::mainModule, 0);
    return 0; // not reached
}

// Public helper to begin uninjecting the DLL
void Uninject()
{
    HANDLE hThread = CreateThread(nullptr, 0, UninjectThread, nullptr, 0, nullptr);
    if (hThread)
        CloseHandle(hThread);
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
    if (globals::preferredBackend != globals::Backend::None)
        TryInitBackend(globals::preferredBackend);
    else
        TryInitializeFrom(globals::Backend::Vulkan);

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

    mousehooks::Init();

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
        mousehooks::Remove();
        MH_DisableHook(MH_ALL_HOOKS);
        MH_RemoveHook(MH_ALL_HOOKS);
        MH_Uninitialize();
        break;
    }
    return TRUE;
}
