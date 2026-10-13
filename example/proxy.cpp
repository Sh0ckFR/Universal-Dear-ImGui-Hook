// Lets the DLL replace dinput8.dll next to the target executable: the export
// the target imports is forwarded to the system library.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string>

namespace {

using DirectInput8CreateFn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, void**, void*);

void* SystemProc(const wchar_t* dll, const char* name) {
    std::wstring path(MAX_PATH, L'\0');
    UINT length = GetSystemDirectoryW(&path[0], MAX_PATH);
    if (!length || length >= MAX_PATH)
        return nullptr;
    path.resize(length);
    path += L'\\';
    path += dll;
    HMODULE module = LoadLibraryW(path.c_str());
    return module ? reinterpret_cast<void*>(GetProcAddress(module, name)) : nullptr;
}

// The target keeps calling into this module, so the unload key must not free it.
void PinModule() {
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       reinterpret_cast<LPCWSTR>(&PinModule), &module);
}

} // namespace

extern "C" HRESULT WINAPI DirectInput8Create(HINSTANCE instance, DWORD version, REFIID iid, void** out, void* outer) {
    static const DirectInput8CreateFn original = [] {
        PinModule();
        return reinterpret_cast<DirectInput8CreateFn>(SystemProc(L"dinput8.dll", "DirectInput8Create"));
    }();
    return original ? original(instance, version, iid, out, outer) : E_FAIL;
}
