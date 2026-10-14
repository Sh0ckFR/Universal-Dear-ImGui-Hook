// Declarations shared by the Windows and the Linux test hosts.
#pragma once

#ifdef _WIN32
#include <windows.h>
#else
#include <X11/Xlib.h>
#endif

#include <memory>
#include <string>

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
    bool warp = false;
    bool capture = false;
    bool deviceProcAddr = false;
    bool decoyQueue = false;
    bool egl = false;
    bool lookup = false;
};

struct Host {
    virtual ~Host() = default;
    virtual bool Init(HostWindow window, const Options& options) = 0;
    virtual void Frame() = 0;
};

std::unique_ptr<Host> CreateVulkanHost();
