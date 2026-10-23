// Settings of the example library, compiled into the DLL and the .so.
// Edit them and rebuild to change the hotkeys or the hooked API.
#include "uih.h"

void ApplySettings(uih::Config& config) {
#ifdef _WIN32
    // Virtual-key codes, listed at
    // https://learn.microsoft.com/windows/win32/inputdev/virtual-key-codes
    config.menuKey = VK_INSERT;
    config.unloadKey = VK_F11;
#else
    // X11 keysyms, listed in X11/keysymdef.h: 0xff63 is XK_Insert, 0xffc8 XK_F11.
    config.menuKey = 0xff63;
    config.unloadKey = 0xffc8;
#endif

    // Controller buttons held together to open and close the menu, 0 to
    // disable. First XInput controller, Windows only.
    config.menuButtons = uih::GamepadL3 | uih::GamepadR3;

    // Graphics API to hook. None detects it: the first API that presents a frame.
    config.backend = uih::Backend::None;

    // Size of the interface: 0 follows the DPI of the game window, 1.5 is 150%.
    config.scale = 0.0f;

#ifdef _WIN32
    // Remote control by an AI assistant such as Claude Code (see the README):
    // screenshots, messages over the game and key presses, served on the local
    // named pipe \\.\pipe\<remotePipe> to remote/uih_mcp.py. Any program you
    // run can then see the game and press its keys: leave it off otherwise.
    config.remoteControl = false;
    config.remotePipe = "universal-imgui-hook";
#endif
}
