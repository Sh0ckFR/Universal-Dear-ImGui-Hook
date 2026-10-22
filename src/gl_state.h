#pragma once

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
    OverlayGlState() {
        static const auto bindFramebuffer = reinterpret_cast<void(APIENTRYP)(GLenum, GLuint)>(imgl3wGetProcAddress("glBindFramebuffer"));
        bindFramebuffer_ = bindFramebuffer;
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
    void(APIENTRYP bindFramebuffer_)(GLenum, GLuint) = nullptr;
    GLint drawFramebuffer_ = 0;
    GLint unpackBuffer_ = 0;
    GLboolean srgb_ = GL_FALSE;
};

} // namespace uih::detail
