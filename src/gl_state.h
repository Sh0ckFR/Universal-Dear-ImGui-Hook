#pragma once

#include "internal.h"

#include <vector>

#include "imgui_impl_opengl3_loader.h"

namespace uih::detail {

// State the application may leave behind when it swaps buffers, set aside
// while the overlay is drawn:
// - a framebuffer object: the overlay goes to the window's back buffer;
// - GL_FRAMEBUFFER_SRGB, which would encode the overlay's colors, already in
//   sRGB, a second time;
// - a pixel unpack buffer, which texture uploads would read instead of the
//   overlay's pixels.
// Needs the loader of the Dear ImGui backend, which its NewFrame() loads.
class OverlayGlState {
public:
    // Not cached: the loader may unload the library on shutdown and load it at
    // another address on the next start.
    OverlayGlState() : bindFramebuffer_(reinterpret_cast<void(APIENTRYP)(GLenum, GLuint)>(imgl3wGetProcAddress("glBindFramebuffer"))) {
        if (bindFramebuffer_) {
            glGetIntegerv(kDrawFramebufferBinding, &drawFramebuffer_);
            bindFramebuffer_(kDrawFramebuffer, 0);
        }
        glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpackBuffer_);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        srgb_ = glIsEnabled(kFramebufferSrgb);
        glDisable(kFramebufferSrgb);
    }
    ~OverlayGlState() {
        if (srgb_)
            glEnable(kFramebufferSrgb);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, static_cast<GLuint>(unpackBuffer_));
        if (bindFramebuffer_)
            bindFramebuffer_(kDrawFramebuffer, static_cast<GLuint>(drawFramebuffer_));
    }
    OverlayGlState(const OverlayGlState&) = delete;
    OverlayGlState& operator=(const OverlayGlState&) = delete;

private:
    static constexpr GLenum kDrawFramebuffer = 0x8CA9;
    static constexpr GLenum kDrawFramebufferBinding = 0x8CA6;
    static constexpr GLenum kFramebufferSrgb = 0x8DB9;
    void(APIENTRYP bindFramebuffer_)(GLenum, GLuint);
    GLint drawFramebuffer_ = 0;
    GLint unpackBuffer_ = 0;
    GLboolean srgb_ = GL_FALSE;
};

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

// Copies the back buffer of the window, before the overlay is drawn on it,
// for CaptureFrame(), and sets back the state the read changes.
inline void CaptureBackBuffer(uint32_t width, uint32_t height) {
    constexpr GLenum kReadFramebuffer = 0x8CA8;
    constexpr GLenum kReadFramebufferBinding = 0x8CAA;
    constexpr GLenum kReadBuffer = 0x0C02;
    constexpr GLenum kPixelPackBuffer = 0x88EB;
    constexpr GLenum kPixelPackBufferBinding = 0x88ED;
    constexpr GLenum kPack[] = {GL_PACK_ALIGNMENT, 0x0D02 /* row length */, 0x0D03 /* skip rows */, 0x0D04 /* skip pixels */};
    if (width == 0 || height == 0) {
        CompleteCapture({}, 0, 0);
        return;
    }
    auto bindFramebuffer = reinterpret_cast<void(APIENTRYP)(GLenum, GLuint)>(imgl3wGetProcAddress("glBindFramebuffer"));
    auto readBuffer = reinterpret_cast<void(APIENTRYP)(GLenum)>(imgl3wGetProcAddress("glReadBuffer"));
    GLint readFramebuffer = 0;
    GLint readMode = 0;
    GLint packBuffer = 0;
    GLint pack[4] = {};
    if (bindFramebuffer) {
        glGetIntegerv(kReadFramebufferBinding, &readFramebuffer);
        bindFramebuffer(kReadFramebuffer, 0);
    }
    if (readBuffer) {
        glGetIntegerv(kReadBuffer, &readMode);
        readBuffer(GL_BACK);
    }
    glGetIntegerv(kPixelPackBufferBinding, &packBuffer);
    glBindBuffer(kPixelPackBuffer, 0);
    for (int i = 0; i < 4; ++i) {
        glGetIntegerv(kPack[i], &pack[i]);
        glPixelStorei(kPack[i], i == 0 ? 4 : 0);
    }
    std::vector<uint8_t> texels(static_cast<size_t>(width) * height * 4);
    glReadPixels(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height), GL_RGBA, GL_UNSIGNED_BYTE, texels.data());
    for (int i = 0; i < 4; ++i)
        glPixelStorei(kPack[i], pack[i]);
    glBindBuffer(kPixelPackBuffer, static_cast<GLuint>(packBuffer));
    if (readBuffer)
        readBuffer(static_cast<GLenum>(readMode));
    if (bindFramebuffer)
        bindFramebuffer(kReadFramebuffer, static_cast<GLuint>(readFramebuffer));
    // Rows from the bottom.
    CompleteCapture(ToRgba(texels.data(), static_cast<size_t>(width) * 4, width, height, TexelLayout::RGBA8, true), width, height);
}

} // namespace uih::detail
