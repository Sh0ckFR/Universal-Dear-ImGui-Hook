// A native Wayland application, rendering with OpenGL through EGL or with
// Vulkan, driven while the hook is preloaded. It runs in Weston, nested in an
// X server by tests/wayland.sh: the keys and clicks of the tests go to that X
// server through XTEST, and reach the application through Weston, and the
// screen is read from Weston's window. The application itself only speaks
// Wayland: it unsets DISPLAY, so that the hook does not see the X server.
#include "host.h"
#include "remote_client.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <X11/Xutil.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-egl.h>

#include "xdg-shell-client-protocol.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

void PrintClientState();

int Fail(const std::string& message) {
    printf("FAIL: %s\n", message.c_str());
    PrintClientState();
    return 1;
}

int Skip(const std::string& message) {
    printf("SKIP: %s\n", message.c_str());
    return kSkip;
}

// The Wayland client ----------------------------------------------------------

// What the application received from its wl_pointer and wl_keyboard.
struct Seen {
    int keyPresses[KEY_MAX + 1] = {};
    int keyReleases[KEY_MAX + 1] = {};
    // Keys pressed, with whether Shift was down.
    std::vector<std::pair<uint32_t, bool>> typed;
    int buttonPresses = 0;
    int buttonReleases = 0;
    int motions = 0;
    int dragMotions = 0;
    int axes = 0;
    // Position of the last motion, press and release.
    int motionX = -1, motionY = -1;
    uint32_t pressed = 0, released = 0;
    int pressX = -1, pressY = -1, releaseX = -1, releaseY = -1;
};

struct Client {
    wl_display* display = nullptr;
    wl_compositor* compositor = nullptr;
    xdg_wm_base* wmBase = nullptr;
    wl_seat* seat = nullptr;
    wl_pointer* pointer = nullptr;
    wl_keyboard* keyboard = nullptr;
    wl_surface* surface = nullptr;
    xdg_surface* xdgSurface = nullptr;
    xdg_toplevel* toplevel = nullptr;
    bool configured = false;
    bool keyboardFocus = false;
    bool pointerInside = false;
    uint32_t modifiers = 0;
    uint32_t buttonsHeld = 0;
    double x = 0, y = 0;
    Seen seen;
    uint32_t capabilities = 0;
    int pointerEnters = 0;
    int keyboardEnters = 0;
    int keyboardLeaves = 0;
    bool activated = false;
} g_client;

void PrintClientState() {
    printf("client: seat capabilities %u, pointer %s (%d enters, inside %d), keyboard %s (%d enters, %d leaves, focus %d), toplevel activated %d\n",
           g_client.capabilities, g_client.pointer ? "yes" : "no", g_client.pointerEnters, g_client.pointerInside, g_client.keyboard ? "yes" : "no",
           g_client.keyboardEnters, g_client.keyboardLeaves, g_client.keyboardFocus, g_client.activated);
}

void PointerEnter(void*, wl_pointer*, uint32_t, wl_surface*, wl_fixed_t x, wl_fixed_t y) {
    g_client.pointerInside = true;
    ++g_client.pointerEnters;
    g_client.x = wl_fixed_to_double(x);
    g_client.y = wl_fixed_to_double(y);
}

void PointerLeave(void*, wl_pointer*, uint32_t, wl_surface*) { g_client.pointerInside = false; }

void PointerMotion(void*, wl_pointer*, uint32_t, wl_fixed_t x, wl_fixed_t y) {
    g_client.x = wl_fixed_to_double(x);
    g_client.y = wl_fixed_to_double(y);
    Seen& seen = g_client.seen;
    ++(g_client.buttonsHeld ? seen.dragMotions : seen.motions);
    seen.motionX = static_cast<int>(g_client.x);
    seen.motionY = static_cast<int>(g_client.y);
}

void PointerButton(void*, wl_pointer*, uint32_t, uint32_t, uint32_t button, uint32_t state) {
    Seen& seen = g_client.seen;
    if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
        ++seen.buttonPresses;
        ++g_client.buttonsHeld;
        seen.pressed = button;
        seen.pressX = static_cast<int>(g_client.x);
        seen.pressY = static_cast<int>(g_client.y);
    } else {
        ++seen.buttonReleases;
        if (g_client.buttonsHeld)
            --g_client.buttonsHeld;
        seen.released = button;
        seen.releaseX = static_cast<int>(g_client.x);
        seen.releaseY = static_cast<int>(g_client.y);
    }
}

void PointerAxis(void*, wl_pointer*, uint32_t, uint32_t, wl_fixed_t) { ++g_client.seen.axes; }
void PointerFrame(void*, wl_pointer*) {}
void PointerAxisSource(void*, wl_pointer*, uint32_t) {}
void PointerAxisStop(void*, wl_pointer*, uint32_t, uint32_t) {}
void PointerAxisDiscrete(void*, wl_pointer*, uint32_t, int32_t) {}

// Up to version 5 of wl_seat.
const wl_pointer_listener kPointerListener = {PointerEnter, PointerLeave, PointerMotion, PointerButton, PointerAxis,
                                              PointerFrame, PointerAxisSource, PointerAxisStop, PointerAxisDiscrete};

void KeyboardKeymap(void*, wl_keyboard*, uint32_t, int32_t fd, uint32_t) { close(fd); }
void KeyboardEnter(void*, wl_keyboard*, uint32_t, wl_surface*, wl_array*) {
    g_client.keyboardFocus = true;
    ++g_client.keyboardEnters;
}

void KeyboardLeave(void*, wl_keyboard*, uint32_t, wl_surface*) {
    g_client.keyboardFocus = false;
    ++g_client.keyboardLeaves;
}

void KeyboardKey(void*, wl_keyboard*, uint32_t, uint32_t, uint32_t key, uint32_t state) {
    if (key > KEY_MAX)
        return;
    Seen& seen = g_client.seen;
    if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        ++seen.keyPresses[key];
        // Shift is the first modifier of the keymaps of Weston.
        seen.typed.push_back({key, (g_client.modifiers & 1) != 0});
    } else {
        ++seen.keyReleases[key];
    }
}

void KeyboardModifiers(void*, wl_keyboard*, uint32_t, uint32_t depressed, uint32_t, uint32_t, uint32_t) { g_client.modifiers = depressed; }
void KeyboardRepeatInfo(void*, wl_keyboard*, int32_t, int32_t) {}

const wl_keyboard_listener kKeyboardListener = {KeyboardKeymap, KeyboardEnter, KeyboardLeave, KeyboardKey, KeyboardModifiers, KeyboardRepeatInfo};

// The compositor adds and removes the devices of the seat as the X server's come and go.
void SeatCapabilities(void*, wl_seat* seat, uint32_t capabilities) {
    g_client.capabilities = capabilities;
    bool hasPointer = capabilities & WL_SEAT_CAPABILITY_POINTER;
    bool hasKeyboard = capabilities & WL_SEAT_CAPABILITY_KEYBOARD;
    if (hasPointer && !g_client.pointer) {
        g_client.pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(g_client.pointer, &kPointerListener, nullptr);
    } else if (!hasPointer && g_client.pointer) {
        wl_pointer_release(g_client.pointer);
        g_client.pointer = nullptr;
    }
    if (hasKeyboard && !g_client.keyboard) {
        g_client.keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(g_client.keyboard, &kKeyboardListener, nullptr);
    } else if (!hasKeyboard && g_client.keyboard) {
        wl_keyboard_release(g_client.keyboard);
        g_client.keyboard = nullptr;
    }
}

void SeatName(void*, wl_seat*, const char*) {}

const wl_seat_listener kSeatListener = {SeatCapabilities, SeatName};

void Ping(void*, xdg_wm_base* wmBase, uint32_t serial) { xdg_wm_base_pong(wmBase, serial); }
const xdg_wm_base_listener kWmBaseListener = {Ping};

void Configure(void*, xdg_surface* surface, uint32_t serial) {
    xdg_surface_ack_configure(surface, serial);
    g_client.configured = true;
}
const xdg_surface_listener kXdgSurfaceListener = {Configure};

void ToplevelConfigure(void*, xdg_toplevel*, int32_t, int32_t, wl_array* states) {
    g_client.activated = false;
    for (size_t i = 0; states && i + sizeof(uint32_t) <= states->size; i += sizeof(uint32_t)) {
        uint32_t state = 0;
        memcpy(&state, static_cast<const char*>(states->data) + i, sizeof(state));
        g_client.activated = g_client.activated || state == XDG_TOPLEVEL_STATE_ACTIVATED;
    }
}
void ToplevelClose(void*, xdg_toplevel*) {}
const xdg_toplevel_listener kToplevelListener = {ToplevelConfigure, ToplevelClose};

void Global(void*, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
    if (strcmp(interface, wl_compositor_interface.name) == 0)
        g_client.compositor = static_cast<wl_compositor*>(wl_registry_bind(registry, name, &wl_compositor_interface, 4));
    else if (strcmp(interface, xdg_wm_base_interface.name) == 0)
        g_client.wmBase = static_cast<xdg_wm_base*>(wl_registry_bind(registry, name, &xdg_wm_base_interface, 1));
    else if (strcmp(interface, wl_seat_interface.name) == 0 && !g_client.seat)
        g_client.seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, version < 5 ? version : 5));
}

void GlobalRemove(void*, wl_registry*, uint32_t) {}
const wl_registry_listener kRegistryListener = {Global, GlobalRemove};

// Reads the events of the compositor without waiting, as a game does once per frame.
void Pump() {
    wl_display* display = g_client.display;
    while (wl_display_prepare_read(display) != 0)
        wl_display_dispatch_pending(display);
    wl_display_flush(display);
    pollfd fd{wl_display_get_fd(display), POLLIN, 0};
    if (poll(&fd, 1, 0) > 0)
        wl_display_read_events(display);
    else
        wl_display_cancel_read(display);
    wl_display_dispatch_pending(display);
}

bool CreateWindow() {
    wl_display* display = g_client.display;
    wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &kRegistryListener, nullptr);
    wl_display_roundtrip(display);
    if (!g_client.compositor || !g_client.wmBase || !g_client.seat)
        return false;
    xdg_wm_base_add_listener(g_client.wmBase, &kWmBaseListener, nullptr);
    wl_seat_add_listener(g_client.seat, &kSeatListener, nullptr);
    g_client.surface = wl_compositor_create_surface(g_client.compositor);
    g_client.xdgSurface = xdg_wm_base_get_xdg_surface(g_client.wmBase, g_client.surface);
    xdg_surface_add_listener(g_client.xdgSurface, &kXdgSurfaceListener, nullptr);
    g_client.toplevel = xdg_surface_get_toplevel(g_client.xdgSurface);
    xdg_toplevel_add_listener(g_client.toplevel, &kToplevelListener, nullptr);
    xdg_toplevel_set_title(g_client.toplevel, "uih host");
    wl_surface_commit(g_client.surface);
    for (int i = 0; i < 100 && !g_client.configured; ++i)
        wl_display_roundtrip(display);
    return g_client.configured;
}

// EGL ----------------------------------------------------------------------

struct EglHost : Host {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLSurface surface = EGL_NO_SURFACE;
    EGLContext context = EGL_NO_CONTEXT;
    wl_egl_window* window = nullptr;

    bool Init(HostWindow host, const Options&) override {
        display = eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR, host.wlDisplay, nullptr);
        if (display == EGL_NO_DISPLAY || !eglInitialize(display, nullptr, nullptr) || !eglBindAPI(EGL_OPENGL_API))
            return false;
        const EGLint attributes[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                                     EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE};
        EGLConfig config = nullptr;
        EGLint count = 0;
        if (!eglChooseConfig(display, attributes, &config, 1, &count) || count == 0)
            return false;
        // As SDL and GLFW do: libwayland-egl, then an EGL window surface on it.
        window = wl_egl_window_create(static_cast<wl_surface*>(host.wlSurface), kWidth, kHeight);
        surface = window ? eglCreateWindowSurface(display, config, reinterpret_cast<EGLNativeWindowType>(window), nullptr) : EGL_NO_SURFACE;
        const EGLint contextAttributes[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3, EGL_NONE};
        context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttributes);
        if (surface == EGL_NO_SURFACE || context == EGL_NO_CONTEXT || !eglMakeCurrent(display, surface, surface, context))
            return false;
        // The tests pace the frames: swaps do not wait for the compositor.
        eglSwapInterval(display, 0);
        return true;
    }

    void Frame() override {
        eglMakeCurrent(display, surface, surface, context);
        glClearColor(ClearColor()[0], ClearColor()[1], ClearColor()[2], ClearColor()[3]);
        glClear(GL_COLOR_BUFFER_BIT);
        eglSwapBuffers(display, surface);
    }
};

// The X server Weston runs in ------------------------------------------------

struct XServer {
    Display* display = nullptr;
    // Weston's window, the output of the compositor.
    Window output = 0;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    bool Open(const char* name) {
        display = XOpenDisplay(name);
        if (!display)
            return false;
        Window root = DefaultRootWindow(display);
        Window parent = 0;
        Window* children = nullptr;
        unsigned count = 0;
        if (!XQueryTree(display, root, &root, &parent, &children, &count))
            return false;
        for (unsigned i = 0; i < count; ++i) {
            XWindowAttributes attributes{};
            if (XGetWindowAttributes(display, children[i], &attributes) && attributes.map_state == IsViewable &&
                attributes.width * attributes.height > width * height) {
                output = children[i];
                x = attributes.x;
                y = attributes.y;
                width = attributes.width;
                height = attributes.height;
            }
        }
        XFree(children);
        if (!output)
            return false;
        // Weston gives the keyboard to its clients while its window has the focus of the X server.
        XSetInputFocus(display, output, RevertToParent, CurrentTime);
        XSync(display, False);
        return true;
    }

    void Move(int toX, int toY) {
        XTestFakeMotionEvent(display, DefaultScreen(display), x + toX, y + toY, CurrentTime);
        XFlush(display);
    }

    void Key(KeySym key, bool down) {
        XTestFakeKeyEvent(display, XKeysymToKeycode(display, key), down ? True : False, CurrentTime);
        XFlush(display);
    }

    void Button(unsigned button, bool down) {
        XTestFakeButtonEvent(display, button, down ? True : False, CurrentTime);
        XFlush(display);
    }

    // Pixels of the output that are not the clear color of the host.
    int CountOverlayPixels() {
        int w = width < kWidth ? width : kWidth;
        int h = height < kHeight ? height : kHeight;
        XImage* image = XGetImage(display, output, 0, 0, w, h, AllPlanes, ZPixmap);
        if (!image)
            return 0;
        int count = 0;
        for (int row = 0; row < image->height; ++row) {
            for (int column = 0; column < image->width; ++column) {
                unsigned long pixel = XGetPixel(image, column, row);
                int red = (pixel >> 16) & 0xff;
                int green = (pixel >> 8) & 0xff;
                int blue = pixel & 0xff;
                if (abs(red - 26) > 12 || abs(green - 51) > 12 || abs(blue - 77) > 12)
                    ++count;
            }
        }
        XDestroyImage(image);
        return count;
    }

    // The probes of tests/input_preload.cpp.
    bool Green(const int (&point)[2]) {
        XImage* image = XGetImage(display, output, point[0], point[1], 1, 1, AllPlanes, ZPixmap);
        if (!image)
            return false;
        unsigned long pixel = XGetPixel(image, 0, 0);
        XDestroyImage(image);
        return ((pixel >> 8) & 0xff) > 200 && ((pixel >> 16) & 0xff) < 60;
    }
};

// Scenarios ------------------------------------------------------------------

const int kVisible = kWidth * kHeight / 100;

// Renders and reads the events for `ms` milliseconds.
void RunFor(Host& host, int ms) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        Pump();
        host.Frame();
        usleep(5000);
    }
}

void Tap(XServer& screen, Host& host, KeySym key) {
    screen.Key(key, true);
    RunFor(host, 150);
    screen.Key(key, false);
}

// INSERT opens the menu, F11 removes the overlay, and with --reload INSERT brings it back.
int Run(XServer& screen, Host& host, const std::string& name, const Options& options) {
    const KeySym menuKey = options.menuKey ? options.menuKey : XK_Insert;
    const KeySym unloadKey = options.unloadKey ? options.unloadKey : XK_F11;
    screen.Move(400, 300);
    RunFor(host, 1500);
    if (!g_client.keyboardFocus)
        return Fail("Weston did not give the keyboard focus to the host");
    int before = screen.CountOverlayPixels();
    Tap(screen, host, menuKey);
    RunFor(host, 1000);
    int pixels = screen.CountOverlayPixels();
    printf("%d pixels apart from the clear color before the menu key, %d after\n", before, pixels);
    if (before >= kVisible)
        return Fail("the screen does not show the host alone before the menu key");
    if (pixels < kVisible)
        return Fail("overlay is not visible on screen");
    for (int round = 0; round < (options.reload ? 2 : 1); ++round) {
        if (round == 1) {
            Tap(screen, host, menuKey);
            RunFor(host, 1500);
            pixels = screen.CountOverlayPixels();
            printf("%d overlay pixels after the menu key\n", pixels);
            if (pixels < kVisible)
                return Fail("the menu key did not bring the overlay back");
        }
        Tap(screen, host, unloadKey);
        RunFor(host, 1500);
        pixels = screen.CountOverlayPixels();
        printf("%d overlay pixels left after the unload key\n", pixels);
        if (pixels >= kVisible)
            return Fail("overlay is still visible after the unload key");
    }
    if (host.ValidationErrors() > 0)
        return Fail("the validation layer reported errors");
    printf("PASS: %s%s\n", name.c_str(), options.reload ? ", removed and brought back" : "");
    return 0;
}

// While the menu is open, the host reads no key or button presses, no motion
// and no wheel, but the release of a key held before the menu opened; the
// wheel scrolls the menu and the clicks land in it. Before and after, the
// host reads everything.
int RunInput(XServer& screen, Host& host, const std::string& name) {
    auto everything = [&](int x, int y) {
        g_client.seen = Seen{};
        screen.Move(x, y);
        RunFor(host, 100);
        screen.Key(XK_a, true);
        RunFor(host, 100);
        screen.Button(1, true);
        RunFor(host, 50);
        screen.Button(1, false);
        screen.Button(5, true);
        screen.Button(5, false);
        screen.Move(x + 10, y + 10);
        RunFor(host, 200);
        screen.Key(XK_a, false);
        RunFor(host, 300);
    };
    auto print = [&](const char* when) {
        const Seen& seen = g_client.seen;
        printf("%s: %d presses and %d releases of a, %d releases of b, %d button presses, %d motions, %d wheel events\n", when,
               seen.keyPresses[KEY_A], seen.keyReleases[KEY_A], seen.keyReleases[KEY_B], seen.buttonPresses, seen.motions, seen.axes);
    };
    auto sawEverything = [] {
        const Seen& seen = g_client.seen;
        return seen.keyPresses[KEY_A] && seen.keyReleases[KEY_A] && seen.buttonPresses && seen.motions && seen.axes;
    };

    screen.Move(600, 450);
    RunFor(host, 1500);
    if (!g_client.keyboardFocus || !g_client.pointer)
        return Fail("the host has no keyboard focus or no pointer in Weston");
    everything(600, 450);
    print("menu closed");
    if (!sawEverything())
        return Fail("the host did not read its input before the menu opened");

    // b is held while the menu opens.
    screen.Key(XK_b, true);
    RunFor(host, 100);
    Tap(screen, host, XK_Insert);
    RunFor(host, 800);
    if (screen.CountOverlayPixels() < kVisible)
        return Fail("overlay is not visible on screen");
    if (screen.Green(kInputProbe) || screen.Green(kClickProbe))
        return Fail("a probe is green before the wheel turned or the click");
    g_client.seen = Seen{};
    screen.Key(XK_b, false);
    everything(kClickProbe[0], kClickProbe[1]);
    print("menu open");
    const Seen& seen = g_client.seen;
    if (seen.keyPresses[KEY_A] || seen.keyReleases[KEY_A] || seen.buttonPresses || seen.motions || seen.axes)
        return Fail("the host read input while the menu was open");
    if (!seen.keyReleases[KEY_B])
        return Fail("the host did not read the release of a key held before the menu opened");
    if (!screen.Green(kInputProbe))
        return Fail("the wheel did not reach the menu");
    if (!screen.Green(kClickProbe))
        return Fail("the click did not land in the menu");

    Tap(screen, host, XK_Insert);
    RunFor(host, 500);
    everything(600, 450);
    print("menu closed again");
    if (!sawEverything())
        return Fail("the host did not read its input after the menu closed");
    printf("PASS: %s input\n", name.c_str());
    return 0;
}

// The commands of remote/uih_mcp.py: their keys and clicks reach the
// listeners of the host, and the menu.
int RunRemote(XServer& screen, Host& host, const std::string& name) {
    constexpr int kClick[2] = {120, 90};
    constexpr int kMove[2] = {150, 160};
    constexpr int kDragFrom[2] = {200, 200};
    constexpr int kDragTo[2] = {300, 250};
    auto frame = [&] {
        Pump();
        host.Frame();
        usleep(5000);
    };
    auto request = [&](const std::string& command) {
        std::string answer;
        WhileRendering(
            [&] {
                RemoteClient client;
                if (client.Open())
                    answer = client.Request(command);
            },
            frame);
        RunFor(host, 200);
        printf("%s: %s\n", command.c_str(), answer.c_str());
        return answer;
    };
    screen.Move(700, 500);
    RunFor(host, 1500);

    std::string info = request("info");
    if (info.find(" 800x600 focused=1 menu=0") == std::string::npos)
        return Fail("the remote control does not describe the game");
    std::string screenshot;
    std::vector<uint8_t> rgb;
    WhileRendering(
        [&] {
            RemoteClient client;
            if (!client.Open())
                return;
            screenshot = client.Request("screenshot 400");
            if (screenshot == "ok 400 300") {
                rgb.resize(400 * 300 * 3);
                if (!client.Read(rgb.data(), rgb.size()))
                    rgb.clear();
            }
        },
        frame);
    printf("screenshot: %s\n", screenshot.c_str());
    if (rgb.empty())
        return Fail("the remote control did not send the screenshot");
    for (size_t i = 0; i < rgb.size(); ++i)
        if (abs(rgb[i] - static_cast<int>(kClear[i % 3] * 255.0f + 0.5f)) > 3)
            return Fail("the screenshot does not show the frame of the game alone");

    g_client.seen = Seen{};
    std::string key = request("key u 100");
    if (key.rfind("ok sent as keyboard input", 0) != 0 || !g_client.seen.keyPresses[KEY_U] || !g_client.seen.keyReleases[KEY_U])
        return Fail("the keys of the remote control did not reach the host");

    g_client.seen = Seen{};
    std::string typed = request("type Hi!");
    std::vector<std::pair<uint32_t, bool>> expected = {{KEY_H, true}, {KEY_I, false}, {KEY_1, true}};
    if (typed.rfind("ok typed 3 characters as keyboard input", 0) != 0 || g_client.seen.typed != expected)
        return Fail("the text typed by the remote control did not reach the host");

    g_client.seen = Seen{};
    std::string moved = request("move " + std::to_string(kMove[0]) + " " + std::to_string(kMove[1]));
    if (moved.rfind("ok moved the pointer", 0) != 0 || g_client.seen.motionX != kMove[0] || g_client.seen.motionY != kMove[1])
        return Fail("the pointer moved by the remote control did not reach the host");

    g_client.seen = Seen{};
    std::string click = request("click " + std::to_string(kClick[0]) + " " + std::to_string(kClick[1]) + " right 50");
    const Seen& seen = g_client.seen;
    printf("click: pressed %u at %d,%d, released %u\n", seen.pressed, seen.pressX, seen.pressY, seen.released);
    if (click.rfind("ok sent as mouse input", 0) != 0 || seen.pressed != BTN_RIGHT || seen.released != BTN_RIGHT || seen.pressX != kClick[0] ||
        seen.pressY != kClick[1])
        return Fail("the click of the remote control did not reach the host");

    g_client.seen = Seen{};
    std::string drag = request("drag " + std::to_string(kDragFrom[0]) + " " + std::to_string(kDragFrom[1]) + " " + std::to_string(kDragTo[0]) + " " +
                               std::to_string(kDragTo[1]) + " left 200");
    printf("drag: pressed at %d,%d, %d moves, released at %d,%d\n", seen.pressX, seen.pressY, seen.dragMotions, seen.releaseX, seen.releaseY);
    if (drag.rfind("ok sent as mouse input", 0) != 0 || seen.pressed != BTN_LEFT || seen.pressX != kDragFrom[0] || seen.pressY != kDragFrom[1] ||
        seen.dragMotions < 3 || seen.releaseX != kDragTo[0] || seen.releaseY != kDragTo[1])
        return Fail("the drag of the remote control did not reach the host");

    if (request("click 800 10").rfind("error the point is outside the game window", 0) != 0)
        return Fail("the remote control clicked outside the game window");

    // The menu key of the remote control opens the menu, which hides the next keys from the host.
    if (request("key insert 100").rfind("ok sent as keyboard input", 0) != 0)
        return Fail("the menu key of the remote control was not sent");
    RunFor(host, 500);
    if (request("info").find(" menu=1") == std::string::npos)
        return Fail("the menu key of the remote control did not open the menu");
    if (screen.CountOverlayPixels() < kVisible)
        return Fail("overlay is not visible on screen");
    g_client.seen = Seen{};
    request("key u 100");
    request("click 120 90");
    if (g_client.seen.keyPresses[KEY_U] || g_client.seen.buttonPresses)
        return Fail("the host read the keys or clicks of the remote control while the menu was open");

    // The unload key ends the remote control with the overlay: the answer may be lost.
    request("key f11 100");
    RunFor(host, 1500);
    int pixels = screen.CountOverlayPixels();
    printf("%d overlay pixels left after the unload key\n", pixels);
    if (pixels >= kVisible)
        return Fail("overlay is still visible after the unload key of the remote control");
    unlink(RemoteSocketPath().c_str());
    printf("PASS: %s remote control\n", name.c_str());
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--api" && i + 1 < argc) options.api = argv[++i];
        else if (arg == "--menu-key" && i + 1 < argc) options.menuKey = static_cast<int>(strtol(argv[++i], nullptr, 0));
        else if (arg == "--unload-key" && i + 1 < argc) options.unloadKey = static_cast<int>(strtol(argv[++i], nullptr, 0));
        else if (arg == "--reload") options.reload = true;
        else if (arg == "--remote") options.remote = true;
        else if (arg == "--input" && i + 1 < argc) options.input = argv[++i];
    }
    if (options.api != "opengl" && options.api != "vulkan") {
        printf("usage: tests/wayland.sh env LD_PRELOAD=Universal-ImGui-Hook.so uih_host_wayland --api opengl|vulkan [--reload] [--remote] [--input wayland] [--menu-key keysym] [--unload-key keysym]\n");
        return 2;
    }
    setvbuf(stdout, nullptr, _IOLBF, 0);

    // The X server is the test's, not the application's.
    const char* x = getenv("DISPLAY");
    std::string xDisplay = x ? x : "";
    unsetenv("DISPLAY");
    if (!getenv("WAYLAND_DISPLAY") || xDisplay.empty())
        return Skip("run in Weston with tests/wayland.sh");
    XServer screen;
    if (!screen.Open(xDisplay.c_str()))
        return Skip("no window of Weston on the X server " + xDisplay);
    g_client.display = wl_display_connect(nullptr);
    if (!g_client.display)
        return Skip("no Wayland compositor");
    if (!CreateWindow())
        return Fail("the compositor has no xdg_wm_base or no seat, or did not configure the window");

    std::unique_ptr<Host> host;
    if (options.api == "vulkan")
        host = CreateVulkanHost();
    else
        host = std::make_unique<EglHost>();
    std::string name = options.api + " wayland";
    HostWindow window{nullptr, 0, g_client.display, g_client.surface};
    if (!host->Init(window, options))
        return Skip(name + " is not available on this machine");
    if (options.remote)
        return RunRemote(screen, *host, name);
    if (options.input == "wayland")
        return RunInput(screen, *host, name);
    return Run(screen, *host, name, options);
}
