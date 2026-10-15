#include "internal.h"

#include "gl_srgb.h"
#include "imgui_impl_opengl3.h"

namespace uih::detail {
namespace {

using WglSwapBuffersFn = BOOL(WINAPI*)(HDC);
using WglGetCurrentContextFn = HGLRC(WINAPI*)();

// Presents to wait before following an application that renders with another context.
constexpr int kContextSwitchFrames = 60;

HMODULE g_module = nullptr;
WglSwapBuffersFn g_swapBuffers = nullptr;
WglGetCurrentContextFn g_getCurrentContext = nullptr;
HGLRC g_context = nullptr;
int g_contextMisses = 0;
bool g_ready = false;

void Teardown() {
    if (!g_ready)
        return;
    ImGui_ImplOpenGL3_Shutdown();
    UnbindWindow();
    g_ready = false;
    g_context = nullptr;
    g_contextMisses = 0;
}

bool Init(HWND window, HGLRC context) {
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

void Render(HDC hdc) {
    HGLRC context = g_getCurrentContext();
    HWND window = WindowFromDC(hdc);
    if (!context || !window)
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
    ImGui_ImplOpenGL3_NewFrame();
    ImDrawData* drawData = Frame();
    FramebufferSrgbOff srgbOff([](const char* name) { return GetProc<void*>(g_module, name); });
    ImGui_ImplOpenGL3_RenderDrawData(drawData);
}

BOOL WINAPI HookSwapBuffers(HDC hdc) {
    ScopedCall call;
    ForeignPresentScope presentScope;
    if (ShouldRender(Backend::OpenGL))
        Guarded([&] { Render(hdc); });
    ServiceTeardown(Backend::OpenGL, Teardown);
    return g_swapBuffers(hdc);
}

bool Install() {
    g_module = LoadLibraryW(L"opengl32.dll");
    g_getCurrentContext = GetProc<WglGetCurrentContextFn>(g_module, "wglGetCurrentContext");
    void* target = GetProc<void*>(g_module, "wglSwapBuffers");
    if (!g_getCurrentContext || !target)
        return false;
    return CreateHook("wglSwapBuffers", target, reinterpret_cast<void*>(HookSwapBuffers), reinterpret_cast<void**>(&g_swapBuffers));
}

void Uninstall() {
    g_swapBuffers = nullptr;
    g_getCurrentContext = nullptr;
    if (g_module) {
        FreeLibrary(g_module);
        g_module = nullptr;
    }
}

} // namespace

const Renderer kOpenGLRenderer = {"OpenGL", L"opengl32.dll", Install, Uninstall, Teardown};

} // namespace uih::detail
