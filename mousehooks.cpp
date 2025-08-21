#include "stdafx.h"

using SetCursorPos_t = BOOL (WINAPI*)(int,int);
static SetCursorPos_t oSetCursorPos = nullptr;
BOOL WINAPI hookSetCursorPos(int x,int y) {
    return menu::isOpen ? TRUE : oSetCursorPos(x,y);
}

using ClipCursor_t = BOOL (WINAPI*)(const RECT*);
static ClipCursor_t oClipCursor = nullptr;
BOOL WINAPI hookClipCursor(const RECT* rect) {
    return menu::isOpen ? TRUE : oClipCursor(rect);
}

namespace mousehooks {
    void Init() {
        HMODULE user32 = GetModuleHandleA("user32.dll");
        if (!user32)
            return;

        if (auto addr = GetProcAddress(user32, "SetCursorPos")) {
            if (MH_CreateHook(addr, reinterpret_cast<LPVOID>(hookSetCursorPos), reinterpret_cast<LPVOID*>(&oSetCursorPos)) == MH_OK) {
                MH_EnableHook(addr);
                DebugLog("[mousehooks] Hooked SetCursorPos@%p\n", addr);
            }
        }

        if (auto addr = GetProcAddress(user32, "ClipCursor")) {
            if (MH_CreateHook(addr, reinterpret_cast<LPVOID>(hookClipCursor), reinterpret_cast<LPVOID*>(&oClipCursor)) == MH_OK) {
                MH_EnableHook(addr);
                DebugLog("[mousehooks] Hooked ClipCursor@%p\n", addr);
            }
        }
    }

    void Remove() {
        HMODULE user32 = GetModuleHandleA("user32.dll");
        if (!user32)
            return;

        if (auto addr = GetProcAddress(user32, "SetCursorPos")) {
            MH_DisableHook(addr);
            MH_RemoveHook(addr);
        }

        if (auto addr = GetProcAddress(user32, "ClipCursor")) {
            MH_DisableHook(addr);
            MH_RemoveHook(addr);
        }
    }
}

