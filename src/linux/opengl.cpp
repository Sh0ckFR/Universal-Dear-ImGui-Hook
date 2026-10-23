#include "internal.h"

#include <unordered_map>

#include "gl_state.h"
#include "imgui_impl_opengl3.h"

namespace uih::detail {
namespace {

using GlxDrawable = unsigned long;
using ProcAddress = void (*)();

constexpr int kEglHeight = 0x3056;
constexpr int kEglWidth = 0x3057;
constexpr int kEglDraw = 0x3059;
constexpr unsigned int kEglOpenGLApi = 0x30A2;
constexpr int kEglBufferAge = 0x313D;
// Swaps after the overlay left an EGL surface during which its buffers may
// still hold the overlay: more than any swap chain has buffers.
constexpr int kRedrawSwaps = 8;

// Presents to wait before following an application that renders with another context.
constexpr int kContextSwitchFrames = 60;

using GetCurrentContextFn = void* (*)();

void* g_context = nullptr;
// Current context of the API g_context belongs to, GLX or EGL.
GetCurrentContextFn g_getCurrentContext = nullptr;
int g_contextMisses = 0;
bool g_ready = false;

std::mutex g_mutex;
std::unordered_map<GlxDrawable, NativeWindow> g_glxWindows;
// EGL surfaces the overlay was drawn on, with the swaps left before their
// buffers are free of it.
std::unordered_map<void*, int> g_redrawSwaps;

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
    // Object names belong to a context: deleting them in another one would
    // delete objects of the application.
    if (g_getCurrentContext() == g_context)
        ImGui_ImplOpenGL3_Shutdown();
    else
        AbandonRenderer();
    UnbindWindow();
    g_ready = false;
    g_context = nullptr;
    g_contextMisses = 0;
}

bool Init(NativeWindow window, void* context, GetCurrentContextFn getCurrentContext) {
    if (!ClaimBackend(Backend::OpenGL) || !BindWindow(window))
        return false;
    if (!ImGui_ImplOpenGL3_Init()) {
        Log("OpenGL 3 backend initialization failed");
        UnbindWindow();
        return false;
    }
    g_context = context;
    g_getCurrentContext = getCurrentContext;
    g_ready = true;
    return true;
}

// `drawn` is set when the overlay draws anything.
void Render(GetCurrentContextFn getCurrentContext, NativeWindow window, uint32_t width, uint32_t height, bool& drawn) {
    void* context = getCurrentContext();
    if (!context || width == 0 || height == 0)
        return;
    if (g_ready && context != g_context) {
        if (++g_contextMisses < kContextSwitchFrames)
            return;
        Teardown();
    }
    if (!g_ready && !Init(window, context, getCurrentContext))
        return;
    g_contextMisses = 0;
    if (!BindWindow(window))
        return;
    input::SetSurfaceSize(width, height);
    ImGui_ImplOpenGL3_NewFrame();
    ImDrawData* drawData = Frame();
    drawn = drawData && drawData->TotalVtxCount > 0;
    OverlayGlState state;
    ImGui_ImplOpenGL3_RenderDrawData(drawData);
}

// Stop() gave up waiting for this thread: release here, where the context of the overlay may be current.
void ReleaseAbandoned() {
    if (g_ready && ActiveBackend() != Backend::OpenGL)
        Teardown();
}

// A new context may get the address of a destroyed one: the overlay must not take it for its own.
void OnContextDestroyed(void* context) {
    if (context && context == g_context) {
        ReleaseOverlay(Teardown);
        ReleaseBackend(Backend::OpenGL);
    }
}

NativeWindow WindowOf(GlxDrawable drawable) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_glxWindows.find(drawable);
    return it != g_glxWindows.end() ? it->second : static_cast<NativeWindow>(drawable);
}

// Draws the overlay on `drawable` before it is swapped.
void BeforeGlxSwap(GlxDrawable drawable) {
    static const auto getCurrentContext = Next<void* (*)()>(interpose::kGlx, "glXGetCurrentContext");
    static const auto getCurrentDrawable = Next<GlxDrawable (*)()>(interpose::kGlx, "glXGetCurrentDrawable");

    ReleaseAbandoned();
    if (ShouldRender(Backend::OpenGL) && getCurrentContext && getCurrentDrawable && getCurrentDrawable() == drawable) {
        NativeWindow window = WindowOf(drawable);
        uint32_t width = 0;
        uint32_t height = 0;
        bool drawn = false;
        if (input::WindowSize(window, width, height))
            DrawOverlay(drawable, window, [&] { Render(getCurrentContext, window, width, height, drawn); });
    }
    ServiceTeardown(Backend::OpenGL, Teardown);
}

// Applications that only redraw and post what changed must not cut the
// overlay, nor keep it once it is gone: while the overlay is on `surface`,
// and for kRedrawSwaps swaps after, the surface is posted whole and its
// buffers are reported without content. True when it is.
bool RedrawWhole(void* surface, bool drawn) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_redrawSwaps.find(surface);
    if (drawn) {
        g_redrawSwaps[surface] = kRedrawSwaps;
        return true;
    }
    if (it == g_redrawSwaps.end())
        return false;
    if (--it->second == 0)
        g_redrawSwaps.erase(it);
    return true;
}

bool MustRedraw(void* surface) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_redrawSwaps.count(surface) != 0;
}

// Draws the overlay on `surface` before it is posted. True when the whole surface must be posted.
bool BeforeEglSwap(void* display, void* surface) {
    static const auto getCurrentContext = Next<void* (*)()>(interpose::kEgl, "eglGetCurrentContext");
    static const auto getCurrentSurface = Next<void* (*)(int)>(interpose::kEgl, "eglGetCurrentSurface");
    static const auto queryApi = Next<unsigned int (*)()>(interpose::kEgl, "eglQueryAPI");
    static const auto querySurface = Next<unsigned int (*)(void*, void*, int, int*)>(interpose::kEgl, "eglQuerySurface");

    bool drawn = false;
    ReleaseAbandoned();
    // OpenGL ES is not supported.
    if (ShouldRender(Backend::OpenGL) && getCurrentContext && getCurrentSurface && queryApi && querySurface &&
        queryApi() == kEglOpenGLApi && getCurrentSurface(kEglDraw) == surface) {
        int width = 0;
        int height = 0;
        querySurface(display, surface, kEglWidth, &width);
        querySurface(display, surface, kEglHeight, &height);
        if (width > 0 && height > 0)
            DrawOverlay(SwapchainKey(surface), 0, [&] { Render(getCurrentContext, 0, static_cast<uint32_t>(width), static_cast<uint32_t>(height), drawn); });
    }
    ServiceTeardown(Backend::OpenGL, Teardown);
    return RedrawWhole(surface, drawn);
}

// Functions of extensions, which applications look up by name.
ProcAddress GlxExtension(const char* name) {
    static const auto getProcAddress = Next<ProcAddress (*)(const unsigned char*)>(interpose::kGlx, "glXGetProcAddressARB");
    return getProcAddress ? getProcAddress(reinterpret_cast<const unsigned char*>(name)) : nullptr;
}

ProcAddress EglExtension(const char* name) {
    static const auto getProcAddress = Next<ProcAddress (*)(const char*)>(interpose::kEgl, "eglGetProcAddress");
    return getProcAddress ? getProcAddress(name) : nullptr;
}

int64_t SwapBuffersMscOML(void* display, GlxDrawable drawable, int64_t targetMsc, int64_t divisor, int64_t remainder) {
    static const auto swapBuffers = reinterpret_cast<int64_t (*)(void*, GlxDrawable, int64_t, int64_t, int64_t)>(GlxExtension("glXSwapBuffersMscOML"));
    if (!swapBuffers)
        return 0;
    ScopedCall call;
    ForeignPresentScope presentScope;
    BeforeGlxSwap(drawable);
    return swapBuffers(display, drawable, targetMsc, divisor, remainder);
}

using SwapBuffersWithDamageFn = unsigned int (*)(void*, void*, const int*, int);

unsigned int SwapWithDamage(SwapBuffersWithDamageFn swapBuffers, void* display, void* surface, const int* rects, int count) {
    if (!swapBuffers)
        return 0;
    ScopedCall call;
    ForeignPresentScope presentScope;
    // No rectangle: the whole surface.
    if (BeforeEglSwap(display, surface))
        count = 0;
    return swapBuffers(display, surface, rects, count);
}

unsigned int SwapBuffersWithDamageKHR(void* display, void* surface, const int* rects, int count) {
    static const auto swapBuffers = reinterpret_cast<SwapBuffersWithDamageFn>(EglExtension("eglSwapBuffersWithDamageKHR"));
    return SwapWithDamage(swapBuffers, display, surface, rects, count);
}

unsigned int SwapBuffersWithDamageEXT(void* display, void* surface, const int* rects, int count) {
    static const auto swapBuffers = reinterpret_cast<SwapBuffersWithDamageFn>(EglExtension("eglSwapBuffersWithDamageEXT"));
    return SwapWithDamage(swapBuffers, display, surface, rects, count);
}

} // namespace

UIH_EXPORT void glXSwapBuffers(void* display, GlxDrawable drawable) {
    static const auto swapBuffers = Next<void (*)(void*, GlxDrawable)>(interpose::kGlx, "glXSwapBuffers");
    ScopedCall call;
    ForeignPresentScope presentScope;
    BeforeGlxSwap(drawable);
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

UIH_EXPORT void glXDestroyContext(void* display, void* context) {
    static const auto destroyContext = Next<void (*)(void*, void*)>(interpose::kGlx, "glXDestroyContext");
    ScopedCall call;
    OnContextDestroyed(context);
    destroyContext(display, context);
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
    ScopedCall call;
    ForeignPresentScope presentScope;
    BeforeEglSwap(display, surface);
    return swapBuffers(display, surface);
}

UIH_EXPORT unsigned int eglQuerySurface(void* display, void* surface, int attribute, int* value) {
    static const auto querySurface = Next<unsigned int (*)(void*, void*, int, int*)>(interpose::kEgl, "eglQuerySurface");
    unsigned int result = querySurface(display, surface, attribute, value);
    if (result && value && attribute == kEglBufferAge && MustRedraw(surface))
        *value = 0;
    return result;
}

UIH_EXPORT unsigned int eglDestroyContext(void* display, void* context) {
    static const auto destroyContext = Next<unsigned int (*)(void*, void*)>(interpose::kEgl, "eglDestroyContext");
    ScopedCall call;
    OnContextDestroyed(context);
    return destroyContext(display, context);
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
    UIH_GL_EXPORT(glXDestroyContext),
    UIH_GL_EXPORT(glXGetProcAddress),
    UIH_GL_EXPORT(glXGetProcAddressARB),
    // Not exported: only found through glXGetProcAddress().
    {"glXSwapBuffersMscOML", reinterpret_cast<void*>(&SwapBuffersMscOML)},
};
const interpose::Function kEglExports[] = {
    UIH_GL_EXPORT(eglSwapBuffers),
    UIH_GL_EXPORT(eglQuerySurface),
    UIH_GL_EXPORT(eglDestroyContext),
    UIH_GL_EXPORT(eglGetProcAddress),
    // Not exported: only found through eglGetProcAddress().
    {"eglSwapBuffersWithDamageKHR", reinterpret_cast<void*>(&SwapBuffersWithDamageKHR)},
    {"eglSwapBuffersWithDamageEXT", reinterpret_cast<void*>(&SwapBuffersWithDamageEXT)},
};
#undef UIH_GL_EXPORT

} // namespace

const interpose::Library interpose::kGlx = {kGlxSonames, kGlxExports, sizeof(kGlxExports) / sizeof(kGlxExports[0])};
const interpose::Library interpose::kEgl = {kEglSonames, kEglExports, sizeof(kEglExports) / sizeof(kEglExports[0])};

} // namespace uih::detail
