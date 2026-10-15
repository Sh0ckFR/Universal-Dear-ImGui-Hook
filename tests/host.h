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
    // Errors reported by the validation layers enabled for the test, 0 without them.
    virtual int ValidationErrors() const { return 0; }
};

inline void Settle(const Options& options) {
    std::this_thread::sleep_for(std::chrono::milliseconds(options.settleMs));
}

std::unique_ptr<Host> CreateVulkanHost();
