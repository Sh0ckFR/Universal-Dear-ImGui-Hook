// Declarations shared by the Windows and the Linux test hosts.
#pragma once

#ifdef _WIN32
#include <windows.h>
#else
#include <X11/Xlib.h>
#endif

#include <chrono>
#include <memory>
#include <string>
#include <thread>

constexpr int kSkip = 77;
constexpr float kClear[4] = {0.1f, 0.2f, 0.3f, 1.0f};
constexpr int kWidth = 800;
constexpr int kHeight = 600;

#ifdef _WIN32
using HostWindow = HWND;
#else
struct HostWindow {
    Display* display;
    Window window;
};
#endif

struct Options {
    std::string api;
    std::string dll;
    std::string proxy;
    // Library shown with its menu open until the window is closed.
    std::string preview;
    int injectAt = 30;
    // Hotkeys the library was built with, 0 for INSERT and F11.
    int menuKey = 0;
    int unloadKey = 0;
    float scale = 0.0f;
    // Delay before the swap chain is created, so hooks started earlier are in place.
    int settleMs = 0;
    bool warp = false;
    bool capture = false;
    bool deviceProcAddr = false;
    bool decoyQueue = false;
    // "srgb" or "float": a back buffer that stores linear values.
    std::string format;
    // "raw" or "directinput": checks the input the host reads while the menu is open.
    std::string input;
    // Fake xinput1_4.dll driving the controller scenario.
    std::string gamepad;
    bool egl = false;
    bool lookup = false;
    // Changes the size of the window and of the back buffers while the menu is open.
    bool resize = false;
    // Starts the overlay a second time after it was removed.
    bool reload = false;
    // Switches the display to kFullscreen and back while the menu is open.
    bool fullscreen = false;
    // Removes the overlay with the unload key and brings it back with the menu key.
    bool remove = false;
    // "access" or "exception": the render callback faults, as a bug in a menu would.
    std::string fault;
    // A second window with its own swap chain presents too.
    bool secondWindow = false;
    // Images created with uih::CreateTexture() and drawn every frame.
    int textures = 0;
    // Removes the device of the host while the menu is open, then creates a
    // new one, as a game recovering from a GPU fault does.
    bool removeDevice = false;
    // The Vulkan swap chain images are only written by transfers, as by
    // engines that copy their frame into them.
    bool transferOnly = false;
    // The host renders on a thread of its own while the thread of the window
    // dispatches a flood of input messages, as engines with a render thread do.
    bool renderThread = false;
    // The host subclasses its window after the overlay did.
    bool resubclass = false;
    // Deletes the OpenGL context while the menu is open and renders with a
    // new one, as games do when the video settings change.
    bool recreateContext = false;
};

// Display mode of the fullscreen scenario.
constexpr int kFullscreen[2] = {800, 600};

// Sizes the resize scenario goes through: larger, then smaller than the
// start, within the 1024x768 and 1280x720 screens of the CI runners.
constexpr int kResizes[][2] = {{960, 640}, {720, 560}};

struct Host {
    virtual ~Host() = default;
    virtual bool Init(HostWindow window, const Options& options) = 0;
    virtual void Frame() = 0;
    // Resizes the back buffers to the new size of the window, as a game does
    // when the resolution changes. False when the host cannot.
    virtual bool Resize(int, int) { return false; }
    // Exclusive fullscreen at kFullscreen, or back to the window, through the
    // API. False when the API has no exclusive mode.
    virtual bool Fullscreen(bool) { return false; }
    // Makes the device removed, as a GPU fault does. False when the API cannot.
    virtual bool RemoveDevice() { return false; }
    // Deletes the context and makes a new one current, with textures of its
    // own. False when the API has no context.
    virtual bool RecreateContext() { return false; }
    // False when the textures of RecreateContext() were deleted or changed.
    virtual bool TexturesIntact() { return true; }
    // Errors reported by the validation layers enabled for the test, 0 without them.
    virtual int ValidationErrors() const { return 0; }
};

inline void Settle(const Options& options) {
    std::this_thread::sleep_for(std::chrono::milliseconds(options.settleMs));
}

std::unique_ptr<Host> CreateVulkanHost();
