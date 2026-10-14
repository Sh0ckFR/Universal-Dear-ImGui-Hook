#pragma once

#ifdef _WIN32
#define UIH_GL_API __stdcall
#else
#define UIH_GL_API
#endif

namespace uih::detail {

// GL_FRAMEBUFFER_SRGB makes the GPU encode the overlay's colors, already in
// sRGB, a second time: it is turned off while the overlay is drawn.
class FramebufferSrgbOff {
public:
    // `resolve` returns the address of a GL 1.0 function from its name.
    template <typename Resolve>
    explicit FramebufferSrgbOff(Resolve resolve) {
        static const auto isEnabled = reinterpret_cast<unsigned char(UIH_GL_API*)(unsigned int)>(resolve("glIsEnabled"));
        static const auto disable = reinterpret_cast<void(UIH_GL_API*)(unsigned int)>(resolve("glDisable"));
        static const auto enable = reinterpret_cast<void(UIH_GL_API*)(unsigned int)>(resolve("glEnable"));
        if (isEnabled && disable && enable && isEnabled(kFramebufferSrgb)) {
            disable(kFramebufferSrgb);
            enable_ = enable;
        }
    }
    ~FramebufferSrgbOff() {
        if (enable_)
            enable_(kFramebufferSrgb);
    }
    FramebufferSrgbOff(const FramebufferSrgbOff&) = delete;
    FramebufferSrgbOff& operator=(const FramebufferSrgbOff&) = delete;

private:
    static constexpr unsigned int kFramebufferSrgb = 0x8DB9;
    void(UIH_GL_API* enable_)(unsigned int) = nullptr;
};

} // namespace uih::detail
