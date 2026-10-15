// Stand-in for xinput1_4.dll: one controller whose buttons the test host sets.
// The functions are exported under their XInput names by fake_xinput.def.
#include <windows.h>
#include <xinput.h>

namespace {

XINPUT_STATE g_state{};

} // namespace

extern "C" {

DWORD WINAPI FakeXInputGetState(DWORD user, XINPUT_STATE* state) {
    if (user != 0)
        return ERROR_DEVICE_NOT_CONNECTED;
    *state = g_state;
    return ERROR_SUCCESS;
}

DWORD WINAPI FakeXInputGetCapabilities(DWORD user, DWORD, XINPUT_CAPABILITIES* capabilities) {
    if (user != 0)
        return ERROR_DEVICE_NOT_CONNECTED;
    *capabilities = XINPUT_CAPABILITIES{};
    capabilities->Type = XINPUT_DEVTYPE_GAMEPAD;
    capabilities->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
    return ERROR_SUCCESS;
}

void WINAPI FakeXInputSetButtons(WORD buttons) {
    g_state.Gamepad.wButtons = buttons;
    ++g_state.dwPacketNumber;
}

} // extern "C"
