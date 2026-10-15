#include "internal.h"

#include <unordered_map>

#include "gl_srgb.h"
#include "imgui_impl_opengl3.h"

namespace uih::detail {
namespace {

using GlxDrawable = unsigned long;
using ProcAddress = void (*)();

constexpr int kEglHeight = 0x3056;
constexpr int kEglWidth = 0x3057;
constexpr int kEglDraw = 0x3059;
constexpr unsigned int kEglOpenGLApi = 0x30A2;

// Presents to wait before following an application that renders with another context.
constexpr int kContextSwitchFrames = 60;

void* g_context = nullptr;
int g_contextMisses = 0;
bool g_ready = false;

std::mutex g_mutex;
std::unordered_map<GlxDrawable, NativeWindow> g_glxWindows;

template <typename Fn>
Fn Next(const interpose::Library& library, const char* name) {
    return reinterpret_cast<Fn>(library.Next(name));
}

ProcAddress Substitute(const interpose::Library& library, ProcAddress function, const char* name, const void* caller) {
    return reinterpret_cast<ProcAddress>(library.Substitute(reinterpret_cast<void*>(function), name, caller));
}

void Teardown() {
    if (!g_ready)
        return;
    ImGui_ImplOpenGL3_Shutdown();
    UnbindWindow();
    g_ready = false;
    g_context = nullptr;
    g_contextMisses = 0;
}

bool Init(NativeWindow window, void* context) {
    if (!ClaimBackend(Backend::OpenGL) || !BindWindow(window))
        return false;
    if (!ImGui_ImplOpenGL3_Init()) {
        Log("OpenGL 3 backend initialization failed");
        UnbindWindow();
        return false;
    }
    g_context = context;
    g_ready = true;
    return true;
}

void Render(void* context, NativeWindow window, uint32_t width, uint32_t height) {
    if (!context || width == 0 || height == 0)
        return;
    if (g_ready && context != g_context) {
        if (++g_contextMisses < kContextSwitchFrames)
            return;
        Teardown();
    }
    if (!g_ready && !Init(window, context))
        return;
    g_contextMisses = 0;
    if (!BindWindow(window))
        return;
    input::SetSurfaceSize(width, height);
    ImGui_ImplOpenGL3_NewFrame();
    ImDrawData* drawData = Frame();
    FramebufferSrgbOff srgbOff([](const char* name) { return interpose::kGlx.Next(name); });
    ImGui_ImplOpenGL3_RenderDrawData(drawData);
}

// Stop() gave up waiting for this thread: release here, where the context is current.
void ReleaseAbandoned() {
    if (g_ready && ActiveBackend() != Backend::OpenGL)
        Teardown();
}

NativeWindow WindowOf(GlxDrawable drawable) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_glxWindows.find(drawable);
    return it != g_glxWindows.end() ? it->second : static_cast<NativeWindow>(drawable);
}

} // namespace

UIH_EXPORT void glXSwapBuffers(void* display, GlxDrawable drawable) {
    static const auto swapBuffers = Next<void (*)(void*, GlxDrawable)>(interpose::kGlx, "glXSwapBuffers");
    static const auto getCurrentContext = Next<void* (*)()>(interpose::kGlx, "glXGetCurrentContext");
    static const auto getCurrentDrawable = Next<GlxDrawable (*)()>(interpose::kGlx, "glXGetCurrentDrawable");

    ScopedCall call;
    ForeignPresentScope presentScope;
    ReleaseAbandoned();
    if (ShouldRender(Backend::OpenGL) && getCurrentContext && getCurrentDrawable && getCurrentDrawable() == drawable) {
        NativeWindow window = WindowOf(drawable);
        uint32_t width = 0;
        uint32_t height = 0;
        if (input::WindowSize(window, width, height))
            DrawOverlay(drawable, window, [&] { Render(getCurrentContext(), window, width, height); });
    }
    ServiceTeardown(Backend::OpenGL, Teardown);
    swapBuffers(display, drawable);
}

UIH_EXPORT GlxDrawable glXCreateWindow(void* display, void* config, unsigned long window, const int* attributes) {
    static const auto createWindow = Next<GlxDrawable (*)(void*, void*, unsigned long, const int*)>(interpose::kGlx, "glXCreateWindow");
    GlxDrawable drawable = createWindow(display, config, window, attributes);
    if (drawable) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_glxWindows[drawable] = static_cast<NativeWindow>(window);
    }
    return drawable;
}

UIH_EXPORT void glXDestroyWindow(void* display, GlxDrawable drawable) {
    static const auto destroyWindow = Next<void (*)(void*, GlxDrawable)>(interpose::kGlx, "glXDestroyWindow");
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_glxWindows.erase(drawable);
    }
    destroyWindow(display, drawable);
}

UIH_EXPORT ProcAddress glXGetProcAddress(const unsigned char* name) {
    static const auto getProcAddress = Next<ProcAddress (*)(const unsigned char*)>(interpose::kGlx, "glXGetProcAddress");
    return Substitute(interpose::kGlx, getProcAddress(name), reinterpret_cast<const char*>(name), __builtin_return_address(0));
}

UIH_EXPORT ProcAddress glXGetProcAddressARB(const unsigned char* name) {
    static const auto getProcAddress = Next<ProcAddress (*)(const unsigned char*)>(interpose::kGlx, "glXGetProcAddressARB");
    return Substitute(interpose::kGlx, getProcAddress(name), reinterpret_cast<const char*>(name), __builtin_return_address(0));
}

UIH_EXPORT unsigned int eglSwapBuffers(void* display, void* surface) {
    static const auto swapBuffers = Next<unsigned int (*)(void*, void*)>(interpose::kEgl, "eglSwapBuffers");
    static const auto getCurrentContext = Next<void* (*)()>(interpose::kEgl, "eglGetCurrentContext");
    static const auto getCurrentSurface = Next<void* (*)(int)>(interpose::kEgl, "eglGetCurrentSurface");
    static const auto queryApi = Next<unsigned int (*)()>(interpose::kEgl, "eglQueryAPI");
    static const auto querySurface = Next<unsigned int (*)(void*, void*, int, int*)>(interpose::kEgl, "eglQuerySurface");

    ScopedCall call;
    ForeignPresentScope presentScope;
    ReleaseAbandoned();
    // OpenGL ES is not supported.
    if (ShouldRender(Backend::OpenGL) && getCurrentContext && getCurrentSurface && queryApi && querySurface &&
        queryApi() == kEglOpenGLApi && getCurrentSurface(kEglDraw) == surface) {
        int width = 0;
        int height = 0;
        querySurface(display, surface, kEglWidth, &width);
        querySurface(display, surface, kEglHeight, &height);
        if (width > 0 && height > 0)
            DrawOverlay(SwapchainKey(surface), 0, [&] { Render(getCurrentContext(), 0, static_cast<uint32_t>(width), static_cast<uint32_t>(height)); });
    }
    ServiceTeardown(Backend::OpenGL, Teardown);
    return swapBuffers(display, surface);
}

UIH_EXPORT ProcAddress eglGetProcAddress(const char* name) {
    static const auto getProcAddress = Next<ProcAddress (*)(const char*)>(interpose::kEgl, "eglGetProcAddress");
    return Substitute(interpose::kEgl, getProcAddress(name), name, __builtin_return_address(0));
}

namespace {

const char* const kGlxSonames[] = {"libGL.so.1", "libGLX.so.0", nullptr};
const char* const kEglSonames[] = {"libEGL.so.1", nullptr};

#define UIH_GL_EXPORT(name) {#name, reinterpret_cast<void*>(&name)}
const interpose::Function kGlxExports[] = {
    UIH_GL_EXPORT(glXSwapBuffers),
    UIH_GL_EXPORT(glXCreateWindow),
    UIH_GL_EXPORT(glXDestroyWindow),
    UIH_GL_EXPORT(glXGetProcAddress),
    UIH_GL_EXPORT(glXGetProcAddressARB),
};
const interpose::Function kEglExports[] = {
    UIH_GL_EXPORT(eglSwapBuffers),
    UIH_GL_EXPORT(eglGetProcAddress),
};
#undef UIH_GL_EXPORT

} // namespace

const interpose::Library interpose::kGlx = {kGlxSonames, kGlxExports, sizeof(kGlxExports) / sizeof(kGlxExports[0])};
const interpose::Library interpose::kEgl = {kEglSonames, kEglExports, sizeof(kEglExports) / sizeof(kEglExports[0])};

} // namespace uih::detail
