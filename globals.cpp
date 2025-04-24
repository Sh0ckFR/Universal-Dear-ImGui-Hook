#include "stdafx.h"

namespace globals {
    // Handle to our DLL module
    HMODULE mainModule = nullptr;
    // Main game window handle
    HWND mainWindow = nullptr;
    // Key to uninject and exit (F12 by default)
    int uninjectKey = VK_F12;
    // Key to open/close the ImGui menu (INSERT by default)
    int openMenuKey = VK_INSERT;
}

// Log initial global values for debugging
static void LogGlobals() {
    DebugLog("[Globals] mainModule=%p, mainWindow=%p, uninjectKey=0x%X, openMenuKey=0x%X\n",
        globals::mainModule, globals::mainWindow, globals::uninjectKey, globals::openMenuKey);
}

// Ensure we log when the DLL is loaded
struct GlobalsLogger {
    GlobalsLogger() {
        LogGlobals();
    }
};

static GlobalsLogger _globalsLogger;
