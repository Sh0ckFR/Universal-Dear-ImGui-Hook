#include "internal.h"

#include <algorithm>

#include "gl_state.h"
#include "imgui_impl_opengl3.h"

namespace uih::detail {
namespace {

using WglSwapBuffersFn = BOOL(WINAPI*)(HDC);
using WglGetCurrentContextFn = HGLRC(WINAPI*)();
using WglDeleteContextFn = BOOL(WINAPI*)(HGLRC);

// Presents to wait before following an application that renders with another context.
constexpr int kContextSwitchFrames = 60;

HMODULE g_module = nullptr;
WglSwapBuffersFn g_swapBuffers = nullptr;
WglGetCurrentContextFn g_getCurrentContext = nullptr;
WglDeleteContextFn g_deleteContext = nullptr;
HGLRC g_context = nullptr;
int g_contextMisses = 0;
bool g_ready = false;

// State the Dear ImGui backend changes while it draws and restores
// afterwards, set back here when a fault interrupts it, as in a draw
// callback of the application.
class BackendGlState {
public:
    BackendGlState() {
        glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture_);
        glActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture_);
        glActiveTexture(static_cast<GLenum>(activeTexture_));
        glGetIntegerv(GL_CURRENT_PROGRAM, &program_);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &arrayBuffer_);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vertexArray_);
        glGetIntegerv(GL_VIEWPORT, viewport_);
        glGetIntegerv(GL_SCISSOR_BOX, scissorBox_);
        for (size_t i = 0; i < kBlendCount; ++i)
            glGetIntegerv(kBlend[i], &blend_[i]);
        for (size_t i = 0; i < kCapabilityCount; ++i)
            enabled_[i] = glIsEnabled(kCapabilities[i]);
    }

    void Restore() const {
        glUseProgram(static_cast<GLuint>(program_));
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture_));
        glActiveTexture(static_cast<GLenum>(activeTexture_));
        glBindVertexArray(static_cast<GLuint>(vertexArray_));
        glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(arrayBuffer_));
        glBlendEquationSeparate(static_cast<GLenum>(blend_[0]), static_cast<GLenum>(blend_[1]));
        glBlendFuncSeparate(static_cast<GLenum>(blend_[2]), static_cast<GLenum>(blend_[3]), static_cast<GLenum>(blend_[4]),
                            static_cast<GLenum>(blend_[5]));
        for (size_t i = 0; i < kCapabilityCount; ++i)
            enabled_[i] ? glEnable(kCapabilities[i]) : glDisable(kCapabilities[i]);
        glViewport(viewport_[0], viewport_[1], viewport_[2], viewport_[3]);
        glScissor(scissorBox_[0], scissorBox_[1], scissorBox_[2], scissorBox_[3]);
    }

private:
    static constexpr GLenum kBlend[] = {GL_BLEND_EQUATION_RGB, GL_BLEND_EQUATION_ALPHA, GL_BLEND_SRC_RGB,
                                        GL_BLEND_DST_RGB,      GL_BLEND_SRC_ALPHA,      GL_BLEND_DST_ALPHA};
    static constexpr size_t kBlendCount = sizeof(kBlend) / sizeof(kBlend[0]);
    static constexpr GLenum kCapabilities[] = {GL_BLEND, GL_CULL_FACE, GL_DEPTH_TEST, GL_STENCIL_TEST, GL_SCISSOR_TEST};
    static constexpr size_t kCapabilityCount = sizeof(kCapabilities) / sizeof(kCapabilities[0]);
    GLint activeTexture_ = 0;
    GLint texture_ = 0;
    GLint program_ = 0;
    GLint arrayBuffer_ = 0;
    GLint vertexArray_ = 0;
    GLint viewport_[4] = {};
    GLint scissorBox_[4] = {};
    GLint blend_[kBlendCount] = {};
    GLboolean enabled_[kCapabilityCount] = {};
};

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
    if (CaptureWanted()) {
        RECT client{};
        GetClientRect(window, &client);
        CaptureBackBuffer(static_cast<uint32_t>(std::max<LONG>(client.right, 0)), static_cast<uint32_t>(std::max<LONG>(client.bottom, 0)));
    }
    ImGui_ImplOpenGL3_NewFrame();
    ImDrawData* drawData = Frame();
    OverlayGlState state;
    BackendGlState backend;
    if (!Guarded([&] { ImGui_ImplOpenGL3_RenderDrawData(drawData); }))
        backend.Restore();
}

BOOL WINAPI HookSwapBuffers(HDC hdc) {
    ScopedCall call;
    ForeignPresentScope presentScope;
    if (ShouldRender(Backend::OpenGL)) {
        HWND window = WindowFromDC(hdc);
        DrawOverlay(SwapchainKey(window), window, [&] { Render(hdc); });
    }
    ServiceTeardown(Backend::OpenGL, Teardown);
    return g_swapBuffers(hdc);
}

BOOL WINAPI HookDeleteContext(HGLRC context) {
    ScopedCall call;
    // A new context may get the same handle: the overlay must not take it for its own.
    if (context && context == g_context) {
        ReleaseOverlay(Teardown);
        ReleaseBackend(Backend::OpenGL);
    }
    return g_deleteContext(context);
}

bool Install() {
    g_module = LoadLibraryW(L"opengl32.dll");
    g_getCurrentContext = GetProc<WglGetCurrentContextFn>(g_module, "wglGetCurrentContext");
    void* target = GetProc<void*>(g_module, "wglSwapBuffers");
    void* deleteContext = GetProc<void*>(g_module, "wglDeleteContext");
    if (!g_getCurrentContext || !target || !deleteContext)
        return false;
    return CreateHook("wglDeleteContext", deleteContext, reinterpret_cast<void*>(HookDeleteContext), reinterpret_cast<void**>(&g_deleteContext)) &&
           CreateHook("wglSwapBuffers", target, reinterpret_cast<void*>(HookSwapBuffers), reinterpret_cast<void**>(&g_swapBuffers));
}

void Uninstall() {
    g_swapBuffers = nullptr;
    g_deleteContext = nullptr;
    g_getCurrentContext = nullptr;
    if (g_module) {
        FreeLibrary(g_module);
        g_module = nullptr;
    }
}

} // namespace

const Renderer kOpenGLRenderer = {"OpenGL", L"opengl32.dll", Install, Uninstall, Teardown};

} // namespace uih::detail
