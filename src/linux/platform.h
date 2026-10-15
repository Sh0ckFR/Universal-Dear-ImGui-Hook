#pragma once

// Hooks are exported under the name of the function they stand for (see exports.map).
#define UIH_EXPORT extern "C" __attribute__((visibility("default")))

namespace uih::detail {

void TeardownVulkan();

namespace input {
void SetSurfaceSize(uint32_t width, uint32_t height);
bool WindowSize(NativeWindow window, uint32_t& width, uint32_t& height);
void NewFrame();
bool IsKeyDown(int keysym);
bool IsFocused();
// Xft.dpi of the X server relative to 96 DPI.
float Scale();
// Polled while the hook is stopped: true when `keysym` is down and a window
// of this process has the focus, or the focus follows the pointer.
bool KeyDownInApplication(int keysym);
} // namespace input

namespace interpose {

struct Function {
    const char* name;
    void* detour;
};

// A system library and the hooks standing for its functions.
struct Library {
    const char* const* sonames;
    const Function* functions;
    size_t count;

    void* Detour(const char* name) const;
    bool Contains(const void* address) const;
    // Real function behind the hook `name`.
    void* Next(const char* name) const;
    // Hook to return instead of `function`, unless the library itself is the caller.
    void* Substitute(void* function, const char* name, const void* caller) const;
};

extern const Library kGlx;
extern const Library kEgl;
extern const Library kVulkan;

} // namespace interpose

} // namespace uih::detail
