#pragma once

// Hooks are exported under the name of the function they stand for (see exports.map).
#define UIH_EXPORT extern "C" __attribute__((visibility("default")))

namespace uih::detail {

void TeardownVulkan();

namespace remote {
// Opens the socket of the remote control once this process draws the overlay.
void Listen();
} // namespace remote

namespace input {
void SetSurfaceSize(uint32_t width, uint32_t height);
bool WindowSize(NativeWindow window, uint32_t& width, uint32_t& height);
void NewFrame();
bool IsKeyDown(int keysym);
bool IsFocused();
bool HasFocus(NativeWindow window);
// Xft.dpi of the X server relative to 96 DPI.
float Scale();
// Polled while the hook is stopped: true when `keysym` is down and a window
// of this process has the focus, or the focus follows the pointer.
bool KeyDownInApplication(int keysym);
// Major opcode of XInput on the X server, 0 when unknown.
uint8_t XInputOpcode();
// True for the connection the overlay reads the input from.
bool OwnConnection(const void* connection);
// Turns of the mouse wheel the application did not see, for the menu.
void AddWheel(int x, int y);
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
extern const Library kXcb;

} // namespace interpose

} // namespace uih::detail
