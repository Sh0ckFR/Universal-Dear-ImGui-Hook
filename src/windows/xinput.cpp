// XInput controllers read by the application while the menu is open report no input.
#include "internal.h"

#include <xinput.h>

namespace uih::detail {
namespace {

using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);

// XInputGetStateEx, exported by ordinal only, adds the guide button.
constexpr WORD kGetStateExOrdinal = 100;
// Marks blocked states so applications that skip unchanged packets read them.
constexpr DWORD kBlockedPacket = 0x80000000;

constexpr const wchar_t* kModules[] = {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"};

template <int N>
struct XInputHooks {
    static inline HMODULE handle = nullptr;
    static inline XInputGetStateFn getState = nullptr;
    static inline XInputGetStateFn getStateEx = nullptr;

    static DWORD Filter(DWORD result, XINPUT_STATE* state) {
        if (result == ERROR_SUCCESS && state && input::BlockInput()) {
            state->Gamepad = XINPUT_GAMEPAD{};
            state->dwPacketNumber ^= kBlockedPacket;
        }
        return result;
    }

    static DWORD WINAPI HookGetState(DWORD user, XINPUT_STATE* state) {
        ScopedCall call;
        return Filter(getState(user, state), state);
    }

    static DWORD WINAPI HookGetStateEx(DWORD user, XINPUT_STATE* state) {
        ScopedCall call;
        return Filter(getStateEx(user, state), state);
    }

    static bool Install() {
        handle = LoadLibraryW(kModules[N]);
        if (!handle)
            return false;
        bool hooked = false;
        if (void* target = GetProc<void*>(handle, "XInputGetState"))
            hooked = CreateHook("XInputGetState", target, reinterpret_cast<void*>(HookGetState), reinterpret_cast<void**>(&getState));
        if (void* target = GetProc<void*>(handle, MAKEINTRESOURCEA(kGetStateExOrdinal)))
            CreateHook("XInputGetStateEx", target, reinterpret_cast<void*>(HookGetStateEx), reinterpret_cast<void**>(&getStateEx));
        return hooked;
    }

    static void Uninstall() {
        getState = nullptr;
        getStateEx = nullptr;
        if (handle) {
            FreeLibrary(handle);
            handle = nullptr;
        }
    }

    static void Teardown() {}
};

} // namespace

const Renderer kXInput14Hooks = {"XInput 1.4", kModules[0], XInputHooks<0>::Install, XInputHooks<0>::Uninstall, XInputHooks<0>::Teardown};
const Renderer kXInput13Hooks = {"XInput 1.3", kModules[1], XInputHooks<1>::Install, XInputHooks<1>::Uninstall, XInputHooks<1>::Teardown};
const Renderer kXInput910Hooks = {"XInput 9.1.0", kModules[2], XInputHooks<2>::Install, XInputHooks<2>::Uninstall, XInputHooks<2>::Teardown};

} // namespace uih::detail
