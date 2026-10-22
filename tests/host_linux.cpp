// OpenGL and Vulkan application driven with the hotkeys while the hook is preloaded.
#include "host.h"

#include <EGL/egl.h>
#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>
#include <dlfcn.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

// Position and size of the second window, beside the first one on the 1280x720 screen of the CI.
constexpr int kSecondWindow[4] = {820, 0, 400, 300};

struct OpenGLHost : Host {
    virtual VisualID ChooseVisual(Display* display) = 0;

    static bool IsVersion3() {
        const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
        return version && version[0] >= '3';
    }

    static void Clear() {
        glClearColor(kClear[0], kClear[1], kClear[2], kClear[3]);
        glClear(GL_COLOR_BUFFER_BIT);
    }

    bool Resize(int width, int height) override {
        glViewport(0, 0, width, height);
        return true;
    }

    // Textures of the host, which the overlay must leave alone.
    std::vector<GLuint> textures;
    static constexpr uint32_t kTexel = 0xff996633;

    void CreateTextures() {
        textures.assign(8, 0);
        glGenTextures(static_cast<GLsizei>(textures.size()), textures.data());
        for (GLuint texture : textures) {
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &kTexel);
        }
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    bool TexturesIntact() override {
        bool intact = true;
        for (GLuint texture : textures) {
            if (!glIsTexture(texture)) {
                intact = false;
                continue;
            }
            // Another texture may have been created under the name of a deleted one.
            GLint width = 0;
            GLint height = 0;
            uint32_t texel = 0;
            glBindTexture(GL_TEXTURE_2D, texture);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
            if (width == 1 && height == 1)
                glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, &texel);
            intact = intact && texel == kTexel;
        }
        glBindTexture(GL_TEXTURE_2D, 0);
        return intact;
    }
};

// GLX --------------------------------------------------------------------

struct GlxHost : OpenGLHost {
    Display* display = nullptr;
    GLXFBConfig config = nullptr;
    GLXDrawable drawable = 0;
    GLXContext context = nullptr;
    void (*swapBuffers)(Display*, GLXDrawable) = glXSwapBuffers;

    VisualID ChooseVisual(Display* dpy) override {
        const int attributes[] = {GLX_RENDER_TYPE, GLX_RGBA_BIT, GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT, GLX_DOUBLEBUFFER, True,
                                  GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8, None};
        int count = 0;
        GLXFBConfig* configs = glXChooseFBConfig(dpy, DefaultScreen(dpy), attributes, &count);
        if (!configs || count == 0)
            return 0;
        config = configs[0];
        XFree(configs);
        XVisualInfo* visual = glXGetVisualFromFBConfig(dpy, config);
        VisualID id = visual ? visual->visualid : 0;
        XFree(visual);
        return id;
    }

    bool Init(HostWindow window, const Options& options) override {
        display = window.display;
        drawable = window.window;
        if (options.lookup) {
            // Like GLFW: a GLX window, and functions looked up by name.
            using GetProcAddressFn = void (*(*)(const GLubyte*))();
            void* library = dlopen("libGL.so.1", RTLD_NOW | RTLD_GLOBAL);
            auto getProcAddress = library ? reinterpret_cast<GetProcAddressFn>(dlsym(library, "glXGetProcAddressARB")) : nullptr;
            if (!getProcAddress)
                return false;
            swapBuffers = reinterpret_cast<void (*)(Display*, GLXDrawable)>(getProcAddress(reinterpret_cast<const GLubyte*>("glXSwapBuffers")));
            drawable = glXCreateWindow(display, config, window.window, nullptr);
        }
        context = glXCreateNewContext(display, config, GLX_RGBA_TYPE, nullptr, True);
        return swapBuffers && context && glXMakeCurrent(display, drawable, context) && IsVersion3();
    }

    void Frame() override {
        // Another host may have made its context current.
        glXMakeCurrent(display, drawable, context);
        Clear();
        swapBuffers(display, drawable);
    }

    bool RecreateContext() override {
        glXMakeCurrent(display, None, nullptr);
        glXDestroyContext(display, context);
        context = glXCreateNewContext(display, config, GLX_RGBA_TYPE, nullptr, True);
        if (!context || !glXMakeCurrent(display, drawable, context))
            return false;
        CreateTextures();
        return true;
    }
};

// EGL --------------------------------------------------------------------

struct EglHost : OpenGLHost {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLConfig config = nullptr;
    EGLSurface surface = EGL_NO_SURFACE;
    EGLContext context = EGL_NO_CONTEXT;
    EGLBoolean (*swapBuffers)(EGLDisplay, EGLSurface) = eglSwapBuffers;
    static constexpr EGLint kContextAttributes[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3, EGL_NONE};

    VisualID ChooseVisual(Display* dpy) override {
        display = eglGetDisplay(dpy);
        if (display == EGL_NO_DISPLAY || !eglInitialize(display, nullptr, nullptr) || !eglBindAPI(EGL_OPENGL_API))
            return 0;
        const EGLint attributes[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                                     EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE};
        EGLint count = 0;
        EGLint visual = 0;
        if (!eglChooseConfig(display, attributes, &config, 1, &count) || count == 0)
            return 0;
        eglGetConfigAttrib(display, config, EGL_NATIVE_VISUAL_ID, &visual);
        return static_cast<VisualID>(visual);
    }

    bool Init(HostWindow window, const Options& options) override {
        if (options.lookup) {
            // Like SDL: functions looked up by name.
            void* library = dlopen("libEGL.so.1", RTLD_NOW);
            swapBuffers = library ? reinterpret_cast<EGLBoolean (*)(EGLDisplay, EGLSurface)>(dlsym(library, "eglSwapBuffers")) : nullptr;
        }
        surface = eglCreateWindowSurface(display, config, window.window, nullptr);
        context = eglCreateContext(display, config, EGL_NO_CONTEXT, kContextAttributes);
        return swapBuffers && surface != EGL_NO_SURFACE && context != EGL_NO_CONTEXT &&
               eglMakeCurrent(display, surface, surface, context) && IsVersion3();
    }

    void Frame() override {
        eglMakeCurrent(display, surface, surface, context);
        Clear();
        swapBuffers(display, surface);
    }

    bool RecreateContext() override {
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(display, context);
        context = eglCreateContext(display, config, EGL_NO_CONTEXT, kContextAttributes);
        if (context == EGL_NO_CONTEXT || !eglMakeCurrent(display, surface, surface, context))
            return false;
        CreateTextures();
        return true;
    }
};

// Driver -----------------------------------------------------------------

int Fail(const char* message) {
    printf("FAIL: %s\n", message);
    return 1;
}

int Skip(const char* message) {
    printf("SKIP: %s\n", message);
    return kSkip;
}

// dlsym(RTLD_NEXT) from here has to find the hook, not what follows it.
bool CallerSeenThroughHook() {
    Dl_info info{};
    void* next = dlsym(RTLD_NEXT, "glXSwapBuffers");
    return next && dladdr(next, &info) && info.dli_fname && !strstr(info.dli_fname, "libGL");
}

Window CreateHostWindow(Display* display, VisualID visualId, int x = 0, int y = 0, int width = kWidth, int height = kHeight) {
    XVisualInfo wanted{};
    wanted.visualid = visualId;
    int count = 0;
    XVisualInfo* visual = XGetVisualInfo(display, VisualIDMask, &wanted, &count);
    if (!visual || count == 0)
        return 0;
    Window root = DefaultRootWindow(display);
    XSetWindowAttributes attributes{};
    attributes.colormap = XCreateColormap(display, root, visual->visual, AllocNone);
    attributes.event_mask = StructureNotifyMask;
    Window window = XCreateWindow(display, root, x, y, width, height, 0, visual->depth, InputOutput, visual->visual,
                                  CWColormap | CWBorderPixel | CWEventMask, &attributes);
    XFree(visual);

    long pid = getpid();
    XChangeProperty(display, window, XInternAtom(display, "_NET_WM_PID", False), XA_CARDINAL, 32, PropModeReplace,
                    reinterpret_cast<const unsigned char*>(&pid), 1);
    XStoreName(display, window, "uih host");
    XMapWindow(display, window);
    for (XEvent event{}; event.type != MapNotify;)
        XNextEvent(display, &event);
    XSetInputFocus(display, window, RevertToParent, CurrentTime);
    XSync(display, False);
    return window;
}

void PumpEvents(Display* display) {
    while (XPending(display)) {
        XEvent event;
        XNextEvent(display, &event);
    }
}

void HoldKey(Display* display, KeySym key, bool down) {
    XTestFakeKeyEvent(display, XKeysymToKeycode(display, key), down ? True : False, CurrentTime);
    XFlush(display);
}

// Real key presses: only sent while the host window has the focus.
bool PressKey(Display* display, Window window, KeySym key) {
    Window focus = 0;
    int revert = 0;
    XGetInputFocus(display, &focus, &revert);
    if (focus != window)
        return false;
    HoldKey(display, key, true);
    return true;
}

int CountOverlayPixels(Display* display, Window window) {
    XWindowAttributes attributes{};
    XGetWindowAttributes(display, window, &attributes);
    int width = attributes.width < kWidth ? attributes.width : kWidth;
    int height = attributes.height < kHeight ? attributes.height : kHeight;
    XImage* image = XGetImage(display, window, 0, 0, width, height, AllPlanes, ZPixmap);
    if (!image)
        return 0;
    int count = 0;
    for (int y = 0; y < image->height; ++y) {
        for (int x = 0; x < image->width; ++x) {
            unsigned long pixel = XGetPixel(image, x, y);
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

// INSERT opens the menu, F11 removes the overlay, unless the library was built with other keys.
int Run(Display* display, Window window, Host& host, const std::string& name, const Options& options) {
    const KeySym menuKey = options.menuKey ? options.menuKey : XK_Insert;
    const KeySym unloadKey = options.unloadKey ? options.unloadKey : XK_F11;
    const int visible = kWidth * kHeight / 100;
    for (int frame = 0;; ++frame) {
        PumpEvents(display);
        if (frame == 90 && !PressKey(display, window, menuKey))
            return Skip("the host window does not have the focus");
        if (frame == 95)
            HoldKey(display, menuKey, false);
        if (frame == 180) {
            int pixels = CountOverlayPixels(display, window);
            printf("%d overlay pixels captured\n", pixels);
            if (pixels < visible)
                return Fail("overlay is not visible on screen");
            for (const auto& size : kResizes) {
                if (!options.resize)
                    break;
                XResizeWindow(display, window, size[0], size[1]);
                XSync(display, False);
                if (!host.Resize(size[0], size[1]))
                    return Fail("the host could not resize its back buffers");
                for (int i = 0; i < 30; ++i) {
                    PumpEvents(display);
                    host.Frame();
                    usleep(5000);
                }
                pixels = CountOverlayPixels(display, window);
                printf("resized to %dx%d: %d overlay pixels\n", size[0], size[1], pixels);
                if (pixels < visible)
                    return Fail("overlay is not visible after the resize");
            }
            if (options.recreateContext) {
                if (!host.RecreateContext())
                    return Fail("the host could not create a new context");
                // Beyond the frames the overlay waits before it follows another context.
                for (int i = 0; i < 120; ++i) {
                    PumpEvents(display);
                    host.Frame();
                    usleep(5000);
                }
                pixels = CountOverlayPixels(display, window);
                printf("new context: %d overlay pixels\n", pixels);
                if (pixels < visible)
                    return Fail("the overlay did not come back in the new context");
                if (!host.TexturesIntact())
                    return Fail("the overlay deleted or changed textures of the new context");
            }
            if (!PressKey(display, window, unloadKey))
                return Skip("the host window lost the focus");
        }
        if (frame == 185)
            HoldKey(display, unloadKey, false);
        if (frame == 400) {
            int pixels = CountOverlayPixels(display, window);
            printf("%d overlay pixels left after the unload key\n", pixels);
            if (pixels >= visible)
                return Fail("overlay is still visible after the unload key");
            if (!options.reload) {
                if (host.ValidationErrors() > 0)
                    return Fail("the validation layer reported errors");
                printf("PASS: %s\n", name.c_str());
                return 0;
            }
        }
        // The preloaded library stays: the menu key brings the overlay back.
        if (options.reload && frame == 420 && !PressKey(display, window, menuKey))
            return Skip("the host window lost the focus");
        if (options.reload && frame == 435)
            HoldKey(display, menuKey, false);
        if (options.reload && frame == 520) {
            int pixels = CountOverlayPixels(display, window);
            printf("%d overlay pixels after the menu key\n", pixels);
            if (pixels < visible)
                return Fail("the menu key did not bring the overlay back");
            if (!PressKey(display, window, unloadKey))
                return Skip("the host window lost the focus");
        }
        if (options.reload && frame == 535)
            HoldKey(display, unloadKey, false);
        if (options.reload && frame == 700) {
            int pixels = CountOverlayPixels(display, window);
            printf("%d overlay pixels after the second unload\n", pixels);
            if (pixels >= visible)
                return Fail("overlay is still visible after the second unload");
            if (host.ValidationErrors() > 0)
                return Fail("the validation layer reported errors");
            printf("PASS: %s, removed and brought back\n", name.c_str());
            return 0;
        }
        host.Frame();
        usleep(5000);
    }
}

// A second window, with its own context or device, presents after the first
// one every frame. The overlay stays on one window, follows the focus, and
// goes back to the first window when the second one stops presenting.
int RunSecondWindow(Display* display, Window window, Host& host, Window second, Host& secondHost, const std::string& name) {
    const int visible = kWidth * kHeight / 100;
    const int secondVisible = kSecondWindow[2] * kSecondWindow[3] / 100;
    bool presentSecond = true;
    auto run = [&](int frames) {
        for (int frame = 0; frame < frames; ++frame) {
            PumpEvents(display);
            host.Frame();
            if (presentSecond)
                secondHost.Frame();
            usleep(5000);
        }
    };
    auto onFirst = [&] { return CountOverlayPixels(display, window) >= visible && CountOverlayPixels(display, second) < secondVisible; };
    auto onSecond = [&] { return CountOverlayPixels(display, second) >= secondVisible && CountOverlayPixels(display, window) < visible; };
    auto waitFor = [&](auto condition) {
        for (int attempt = 0; attempt < 50 && !condition(); ++attempt)
            run(20);
        return condition();
    };
    auto focus = [&](Window target) {
        XSetInputFocus(display, target, RevertToParent, CurrentTime);
        XSync(display, False);
    };

    focus(window);
    run(90);
    if (!PressKey(display, window, XK_Insert))
        return Skip("the host window does not have the focus");
    run(5);
    HoldKey(display, XK_Insert, false);
    if (!waitFor(onFirst))
        return Fail("the overlay is not on the focused window only");
    run(60);
    if (!onFirst())
        return Fail("the overlay does not stay on the focused window");
    focus(second);
    if (!waitFor(onSecond))
        return Fail("the overlay did not follow the focus to the second window");
    run(60);
    if (!onSecond())
        return Fail("the overlay does not stay on the second window");
    presentSecond = false;
    if (!waitFor([&] { return CountOverlayPixels(display, window) >= visible; }))
        return Fail("the overlay did not go back when the second window stopped presenting");
    if (host.ValidationErrors() > 0 || secondHost.ValidationErrors() > 0)
        return Fail("the validation layer reported errors");
    printf("PASS: %s with a second window\n", name.c_str());
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--api" && i + 1 < argc) options.api = argv[++i];
        else if (arg == "--egl") options.egl = true;
        else if (arg == "--lookup") options.lookup = true;
        else if (arg == "--device-proc-addr") options.deviceProcAddr = true;
        else if (arg == "--transfer-only") options.transferOnly = true;
        else if (arg == "--menu-key" && i + 1 < argc) options.menuKey = static_cast<int>(strtol(argv[++i], nullptr, 0));
        else if (arg == "--unload-key" && i + 1 < argc) options.unloadKey = static_cast<int>(strtol(argv[++i], nullptr, 0));
        else if (arg == "--resize") options.resize = true;
        else if (arg == "--reload") options.reload = true;
        else if (arg == "--second-window") options.secondWindow = true;
        else if (arg == "--recreate-context") options.recreateContext = true;
    }
    if (options.api != "opengl" && options.api != "vulkan") {
        printf("usage: LD_PRELOAD=Universal-ImGui-Hook.so uih_host --api opengl|vulkan [--egl] [--lookup] [--device-proc-addr] [--transfer-only] [--menu-key keysym] [--unload-key keysym] [--resize] [--reload] [--second-window] [--recreate-context]\n");
        return 2;
    }
    setvbuf(stdout, nullptr, _IOLBF, 0);

    Display* display = XOpenDisplay(nullptr);
    if (!display)
        return Skip("no X server");
    if (!CallerSeenThroughHook())
        return Fail("the hook is not preloaded, or its dlsym() hides the caller from the C library");

    VisualID visual = XVisualIDFromVisual(DefaultVisual(display, DefaultScreen(display)));
    auto createHost = [&]() -> std::unique_ptr<Host> {
        if (options.api == "vulkan")
            return CreateVulkanHost();
        std::unique_ptr<OpenGLHost> openGL;
        if (options.egl)
            openGL = std::make_unique<EglHost>();
        else
            openGL = std::make_unique<GlxHost>();
        visual = openGL->ChooseVisual(display);
        return openGL;
    };
    std::unique_ptr<Host> host = createHost();
    std::string name = options.api + (options.egl ? " egl" : "") + (options.lookup ? " lookup" : "") + (options.deviceProcAddr ? " device-proc-addr" : "") +
                       (options.transferOnly ? " transfer-only" : "");

    Window window = visual ? CreateHostWindow(display, visual) : 0;
    if (!window || !host->Init({display, window}, options))
        return Skip((name + " is not available on this machine").c_str());
    if (options.secondWindow) {
        std::unique_ptr<Host> secondHost = createHost();
        Window second = CreateHostWindow(display, visual, kSecondWindow[0], kSecondWindow[1], kSecondWindow[2], kSecondWindow[3]);
        if (!second || !secondHost->Init({display, second}, options))
            return Fail("the second window could not render");
        return RunSecondWindow(display, window, *host, second, *secondHost, name);
    }
    return Run(display, window, *host, name, options);
}
