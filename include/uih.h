#pragma once

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct ImTextureData;

namespace uih {

// Controller buttons, with the values of XInput's XINPUT_GAMEPAD_* flags.
enum GamepadButton : unsigned {
    GamepadDpadUp = 0x0001,
    GamepadDpadDown = 0x0002,
    GamepadDpadLeft = 0x0004,
    GamepadDpadRight = 0x0008,
    GamepadStart = 0x0010,
    GamepadBack = 0x0020,
    GamepadL3 = 0x0040,
    GamepadR3 = 0x0080,
    GamepadL1 = 0x0100,
    GamepadR1 = 0x0200,
    GamepadA = 0x1000,
    GamepadB = 0x2000,
    GamepadX = 0x4000,
    GamepadY = 0x8000,
};

enum class Backend {
    None,
    D3D9,
    D3D10,
    D3D11,
    D3D12,
    Vulkan,
    OpenGL,
};

struct Config {
#ifdef _WIN32
    // Module freed by `unloadKey`. When null, `unloadKey` removes the overlay
    // and the menu key or buttons bring it back.
    HMODULE module = nullptr;
#endif
    // Backend to hook. `None` hooks every graphics API loaded in the process and
    // keeps the first one that renders a frame.
    Backend backend = Backend::None;
#ifdef _WIN32
    // Virtual-key codes.
    int menuKey = VK_INSERT;
    int unloadKey = VK_F11;
#else
    // X11 keysyms: XK_Insert, XK_F11.
    int menuKey = 0xff63;
    int unloadKey = 0xffc8;
#endif
    // Controller buttons that toggle the menu when held together, 0 to disable.
    // Read from the first XInput controller; Windows only.
    unsigned menuButtons = GamepadL3 | GamepadR3;
    // Size of the interface, 1.5 for 150%. 0 follows the DPI of the target window.
    float scale = 0.0f;
    bool debugLog = false;
    // Remote control by an AI assistant through remote/uih_mcp.py: screenshots,
    // messages over the game and key presses, served to local processes on the
    // named pipe \\.\pipe\<remotePipe> on Windows, and to the processes of the
    // user on the Unix socket $XDG_RUNTIME_DIR/<remotePipe> on Linux. Any
    // program the user runs can then see the game and press its keys. Start()
    // copies the name.
    bool remoteControl = false;
    const char* remotePipe = "universal-imgui-hook";
    // With remoteControl: the assistant can also list the modules, read,
    // write and scan the memory of the game, as a memory editor does. On
    // Linux, that bypasses the ptrace restrictions of Yama for the user's
    // programs.
    bool remoteMemory = false;
    // With remoteControl: advanced tools for the assistant, and the "AI
    // Advanced Features" window that shows them (DrawAIAdvancedFeatures()):
    // typing text, moving the mouse and dragging, waiting for the screen to
    // change. With remoteMemory as well: pointer chains and pointer scans, a
    // watch list with frozen values, and on Windows the instructions that
    // write an address.
    bool aiAdvancedFeatures = false;
    // Called every frame between ImGui::NewFrame() and ImGui::Render().
    void (*render)() = nullptr;
    // Called when the ImGui context is created, before its first frame: load
    // fonts and set the style here. A new context is created when the game
    // switches to another device or window, so this may run more than once.
    void (*setup)() = nullptr;
};

// Installs the hooks. Safe to call from DllMain or from a library constructor.
void Start(const Config& config);
// Closes the menu, removes the overlay and releases all resources. Blocks
// until it is done.
// Must not be called from DllMain or from inside a hook.
void Stop();

Backend ActiveBackend();
bool IsMenuOpen();
void SetMenuOpen(bool open);

const char* BackendName(Backend backend);

// Textures that CreateTexture() keeps at once, until DestroyTexture() releases them.
constexpr int kMaxTextures = 256;

// Creates an image from `width` * `height` RGBA pixels, 8 bits per channel,
// rows from top to bottom. Draw it with ImGui::Image(texture->GetTexRef(), size).
// The pixels are copied and uploaded by the active backend on the next frame,
// and again after a backend switch. Callable from any thread, before or
// after Start(). Null when `kMaxTextures` textures already exist.
ImTextureData* CreateTexture(const void* rgba, int width, int height);
// Releases a texture on the next frame. Do not draw it afterwards.
void DestroyTexture(ImTextureData* texture);

// Copies the next frame the game presents, without the overlay, as `width` *
// `height` RGBA pixels, 8 bits per channel, rows from top to bottom, on every
// backend. Waits up to `timeoutMs` for that frame. False on timeout, when the
// hook is stopped, and when the back buffer is neither 8 nor 10 bits per
// channel, such as the floating-point one of HDR output. Any thread but the
// render callback's.
bool CaptureFrame(std::vector<uint8_t>& pixels, uint32_t& width, uint32_t& height, uint32_t timeoutMs = 1000);

// True when the hook was started with remoteControl and aiAdvancedFeatures.
bool AIAdvancedFeaturesEnabled();
// Draws the "AI Advanced Features" window: what the assistant does, switches
// that pause its input or its memory tools, the watch list and its frozen
// values, pointers, and the instructions that write an address. Call it from
// the render callback; the close button of the window clears `open`. Draws
// nothing unless AIAdvancedFeaturesEnabled().
void DrawAIAdvancedFeatures(bool* open = nullptr);

// A tool of your own for the assistant, next to the built-in ones of the
// remote control: remote/uih_mcp.py lists it, and the assistant calls it as it
// calls screenshot or press_key. `call` receives the arguments as a JSON object
// and returns the text the assistant reads; it runs in the frame of the
// overlay, on the thread of the render callback, so it can use the game's
// objects as the menu does. An exception it throws answers its message as an
// error. With remoteControl only.
using ToolCall = std::function<std::string(const std::string& arguments)>;
// `name`: 1 to 64 letters, digits, _ or -, other than the built-in tools'.
// `description`: what the tool does, when to use it, what it returns.
// `parameters`: the JSON Schema of the arguments, such as
// {"type":"object","properties":{"x":{"type":"number"}},"required":["x"]},
// or null for none. Registering a name again replaces the tool. Callable from
// any thread, before or after Start(). False when the name or the schema is
// not valid. The assistant sees the tools registered when its session starts.
bool RegisterTool(const char* name, const char* description, const char* parameters, ToolCall call);
void UnregisterTool(const char* name);
// The value of `key` in the JSON object `arguments`: the text of a string,
// unescaped, or the JSON of a number, true, false, null, object or array.
// Empty when the key is missing or the JSON is malformed.
std::string ToolArgument(const std::string& arguments, const char* key);

} // namespace uih
