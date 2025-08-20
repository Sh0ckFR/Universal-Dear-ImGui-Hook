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
    // Currently active rendering backend
    Backend activeBackend = Backend::None;
}

// Log initial global values for debugging
static void LogGlobals() {
    DebugLog("[Globals] mainModule=%p, mainWindow=%p, uninjectKey=0x%X, openMenuKey=0x%X, backend=%d\n",
        globals::mainModule, globals::mainWindow, globals::uninjectKey, globals::openMenuKey,
        static_cast<int>(globals::activeBackend));
}

// Ensure we log when the DLL is loaded
struct GlobalsLogger {
    GlobalsLogger() {
        LogGlobals();
    }
};

static GlobalsLogger _globalsLogger;
