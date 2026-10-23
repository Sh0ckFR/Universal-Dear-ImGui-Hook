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
    // The second window renders with the device of the first one.
    bool sharedDevice = false;
    // Copies a frame with uih::CaptureFrame() while the menu is open.
    bool captureFrame = false;
    // Images created with uih::CreateTexture() and drawn every frame.
    int textures = 0;
    // Removes the device of the host while the menu is open, then creates a
    // new one, as a game recovering from a GPU fault does.
    bool removeDevice = false;
    // The Vulkan host recreates its swap chain after VK_ERROR_OUT_OF_DATE_KHR
    // only, not after VK_SUBOPTIMAL_KHR, as many engines do.
    bool ignoreSuboptimal = false;
    // The Vulkan swap chain images are only written by transfers, as by
    // engines that copy their frame into them.
    bool transferOnly = false;
    // Linux: the EGL host presents with eglSwapBuffersWithDamage, which
    // applications look up by name, and redraws and reports a single pixel
    // once the buffer holds the rest of the window.
    bool swapWithDamage = false;
    // Linux: path the host opens the Vulkan loader from instead of
    // libvulkan.so.1, as applications that open libvulkan.so do.
    std::string vulkanLibrary;
    // The host renders on a thread of its own while the thread of the window
    // dispatches a flood of input messages, as engines with a render thread do.
    bool renderThread = false;
    // The host subclasses its window after the overlay did.
    bool resubclass = false;
    // Deletes the OpenGL context while the menu is open and renders with a
    // new one, as games do when the video settings change.
    bool recreateContext = false;
    // Once the overlay showed, the host destroys its context and renders
    // with this API in a new window, as games that draw their splash screen
    // with another API do.
    std::string switchApi;
    // D3D12: the GPU runs each frame of the host this long after it was
    // submitted, as a busy GPU does, so the frames of the overlay are still in
    // flight when the next one starts. 0 without it.
    int gpuLatencyMs = 0;
    // Duration of a frame of the overlay that outlasts the wait of
    // uih::Stop() for the thread that presents, as a slow render callback or
    // a stalled GPU make it. 0 without it.
    int slowFrameMs = 0;
    // The OpenGL host leaves a framebuffer object and a pixel unpack buffer
    // of its own bound when it swaps buffers, as engines that render offscreen do.
    bool boundFramebuffer = false;
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
    // A host on `window` that renders with the device of this one, as an
    // application with several windows does. Null when the API cannot.
    virtual std::unique_ptr<Host> ShareDevice(HostWindow, const Options&) { return nullptr; }
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
