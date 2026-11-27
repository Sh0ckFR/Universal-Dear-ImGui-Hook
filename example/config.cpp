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

    // Remote control by an AI assistant such as Claude Code (see the README):
    // screenshots, messages over the game and key presses, served to
    // remote/uih_mcp.py on the local named pipe \\.\pipe\<remotePipe> on
    // Windows, or the Unix socket $XDG_RUNTIME_DIR/<remotePipe> on Linux. Any
    // program you run can then see the game and press its keys: leave it off
    // otherwise.
    config.remoteControl = false;
    config.remotePipe = "universal-imgui-hook";
    // With remoteControl: the assistant can also read, write and scan the
    // memory of the game, as a memory editor does. Single-player games only.
    config.remoteMemory = false;
    // With remoteControl: advanced tools for the assistant, and the "AI
    // Advanced Features" button of the menu, which shows what it does: typing
    // text, moving the mouse and dragging, waiting for the screen to change.
    // With remoteMemory too: pointer chains, a watch list with frozen values,
    // and on Windows the instructions that write a value.
    config.aiAdvancedFeatures = false;

    // The variants of the release libraries, built with
    // cmake -DUIH_EXAMPLE_VARIANT=remote, memory, advanced or ai:
    //   -remote    the remote control: Claude sees the game and presses its keys
    //   -memory    the remote control and the memory and engine tools
    //   -advanced  the remote control and the advanced tools, with the chat
    //   -ai        all of them
#ifdef UIH_EXAMPLE_REMOTE
    config.remoteControl = true;
#endif
#ifdef UIH_EXAMPLE_MEMORY
    config.remoteMemory = true;
#endif
#ifdef UIH_EXAMPLE_ADVANCED
    config.aiAdvancedFeatures = true;
#endif
}
