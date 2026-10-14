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
    int injectAt = 30;
    // Delay before the swap chain is created, so hooks started earlier are in place.
    int settleMs = 0;
    bool warp = false;
    bool capture = false;
    bool deviceProcAddr = false;
    // "srgb" or "float": a back buffer that stores linear values.
    std::string format;
    bool egl = false;
    bool lookup = false;
};

struct Host {
    virtual ~Host() = default;
    virtual bool Init(HostWindow window, const Options& options) = 0;
    virtual void Frame() = 0;
};

inline void Settle(const Options& options) {
    std::this_thread::sleep_for(std::chrono::milliseconds(options.settleMs));
}

std::unique_ptr<Host> CreateVulkanHost();
