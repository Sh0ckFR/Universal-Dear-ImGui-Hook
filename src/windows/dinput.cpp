// DirectInput keyboards and mice read while the menu is open see nothing but releases.
#include "internal.h"

#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

#include <cstring>

namespace uih::detail {
namespace {

using DirectInput8CreateFn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, void**, IUnknown*);
using GetDeviceStateFn = HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice8W*, DWORD, void*);
using GetDeviceDataFn = HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice8W*, DWORD, DIDEVICEOBJECTDATA*, DWORD*, DWORD);

constexpr GUID kDirectInput8A = {0xBF798030, 0x483A, 0x4DA2, {0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00}};
constexpr GUID kDirectInput8W = {0xBF798031, 0x483A, 0x4DA2, {0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00}};
constexpr GUID kSysKeyboard = {0x6F1D2B61, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
constexpr size_t kGetDeviceStateIndex = 9;
constexpr size_t kGetDeviceDataIndex = 10;

// The ANSI and Unicode interfaces are separate objects: each gets its hooks.
enum Charset { kAnsi, kUnicode, kCharsets };

HMODULE g_module = nullptr;
GetDeviceStateFn g_getDeviceState[kCharsets] = {};
GetDeviceDataFn g_getDeviceData[kCharsets] = {};

enum class DeviceType { Keyboard, Mouse, Other };

// Both interfaces share the layout of GetCapabilities.
DeviceType TypeOf(IDirectInputDevice8W* device) {
    DIDEVCAPS caps{};
    caps.dwSize = sizeof(caps);
    if (FAILED(device->GetCapabilities(&caps)))
        return DeviceType::Other;
    switch (GET_DIDEVICE_TYPE(caps.dwDevType)) {
    case DI8DEVTYPE_KEYBOARD: return DeviceType::Keyboard;
    case DI8DEVTYPE_MOUSE: return DeviceType::Mouse;
    default: return DeviceType::Other;
    }
}

bool IsRelease(DeviceType type, const DIDEVICEOBJECTDATA& event) {
    if (event.dwData & 0x80)
        return false;
    if (type == DeviceType::Keyboard)
        return true;
    return event.dwOfs >= DIMOFS_BUTTON0 && event.dwOfs <= DIMOFS_BUTTON7;
}

template <Charset C>
HRESULT STDMETHODCALLTYPE HookGetDeviceState(IDirectInputDevice8W* device, DWORD size, void* data) {
    ScopedCall call;
    HRESULT hr = g_getDeviceState[C](device, size, data);
    if (SUCCEEDED(hr) && data && input::BlockInput() && TypeOf(device) != DeviceType::Other)
        memset(data, 0, size);
    return hr;
}

template <Charset C>
HRESULT STDMETHODCALLTYPE HookGetDeviceData(IDirectInputDevice8W* device, DWORD objectSize, DIDEVICEOBJECTDATA* data, DWORD* count, DWORD flags) {
    ScopedCall call;
    HRESULT hr = g_getDeviceData[C](device, objectSize, data, count, flags);
    if (FAILED(hr) || !data || !count || !input::BlockInput())
        return hr;
    DeviceType type = TypeOf(device);
    if (type == DeviceType::Other)
        return hr;
    // Events are objectSize bytes apart, which may be less than sizeof(DIDEVICEOBJECTDATA).
    auto* bytes = reinterpret_cast<BYTE*>(data);
    DWORD kept = 0;
    for (DWORD i = 0; i < *count; ++i) {
        DIDEVICEOBJECTDATA event{};
        memcpy(&event, bytes + i * objectSize, objectSize < sizeof(event) ? objectSize : sizeof(event));
        if (IsRelease(type, event)) {
            if (kept != i)
                memmove(bytes + kept * objectSize, bytes + i * objectSize, objectSize);
            ++kept;
        }
    }
    *count = kept;
    return hr;
}

// Hooks the keyboard device of one interface; `hooked` holds the methods hooked so far.
template <Charset C>
void HookInterface(DirectInput8CreateFn create, const GUID& iid, void* hooked[2]) {
    IDirectInput8W* directInput = nullptr;
    if (FAILED(create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, iid, reinterpret_cast<void**>(&directInput), nullptr)))
        return;
    IDirectInputDevice8W* device = nullptr;
    if (SUCCEEDED(directInput->CreateDevice(kSysKeyboard, &device, nullptr))) {
        void* getDeviceState = VirtualMethod(device, kGetDeviceStateIndex);
        void* getDeviceData = VirtualMethod(device, kGetDeviceDataIndex);
        if (getDeviceState != hooked[0] &&
            CreateHook("IDirectInputDevice8::GetDeviceState", getDeviceState, reinterpret_cast<void*>(HookGetDeviceState<C>),
                       reinterpret_cast<void**>(&g_getDeviceState[C])))
            hooked[0] = getDeviceState;
        if (getDeviceData != hooked[1] &&
            CreateHook("IDirectInputDevice8::GetDeviceData", getDeviceData, reinterpret_cast<void*>(HookGetDeviceData<C>),
                       reinterpret_cast<void**>(&g_getDeviceData[C])))
            hooked[1] = getDeviceData;
        device->Release();
    }
    directInput->Release();
}

bool Install() {
    g_module = LoadLibraryW(L"dinput8.dll");
    auto create = GetProc<DirectInput8CreateFn>(g_module, "DirectInput8Create");
    if (!create)
        return false;
    void* hooked[2] = {};
    HookInterface<kUnicode>(create, kDirectInput8W, hooked);
    HookInterface<kAnsi>(create, kDirectInput8A, hooked);
    return hooked[0] != nullptr;
}

void Uninstall() {
    for (int charset = 0; charset < kCharsets; ++charset) {
        g_getDeviceState[charset] = nullptr;
        g_getDeviceData[charset] = nullptr;
    }
    if (g_module) {
        FreeLibrary(g_module);
        g_module = nullptr;
    }
}

void NoTeardown() {}

} // namespace

const Renderer kDirectInputHooks = {"DirectInput", L"dinput8.dll", Install, Uninstall, NoTeardown};

} // namespace uih::detail
