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
// Dear ImGui key of an X11 or xkbcommon keysym, ImGuiKey_None for others.
ImGuiKey KeysymToImGuiKey(uint32_t keysym);
} // namespace input

// Input of native Wayland applications (wayland.cpp), which the overlay reads
// instead of the X server's when the application has a wl_pointer or a
// wl_keyboard and the overlay does not draw on an X11 window.
namespace wayland {
bool Active();
void Attach();
void Detach();
void SetSurfaceSize(uint32_t width, uint32_t height);
// A wl_surface the application draws on with Vulkan or EGL.
void RecordSurface(const void* surface);
void NewFrame();
bool IsKeyDown(int keysym);
bool IsFocused();
bool KeyDownInApplication(int keysym);

// Kept alive by the hooks of presents and acquires: the graphics libraries
// read their own Wayland events there, where the application does not expect
// its input.
class GraphicsCall {
public:
    GraphicsCall();
    ~GraphicsCall();
    GraphicsCall(const GraphicsCall&) = delete;
    GraphicsCall& operator=(const GraphicsCall&) = delete;
};

// The remote control: events handed to the application's listeners the next
// time it reads its events. Size of the surface the overlay draws on, and
// whether the application has the keyboard focus; false without a seat.
bool Window(uint32_t& width, uint32_t& height, bool& focused);
// The evdev code of the key that types `keysym` in the application's keymap, with or without Shift.
bool KeyFor(int keysym, uint32_t& code, bool& shift);
bool InjectKey(uint32_t code, bool down);
bool InjectShift(bool down);
bool InjectMotion(int x, int y);
// Button 1 left, 2 middle, 3 right.
bool InjectButton(int button, bool down);
// Leaves the surface the events above entered, when the application had neither the focus nor the pointer.
void InjectEnd();
// False when the application did not read the events within `timeoutMs`: they are dropped.
bool WaitInjected(unsigned timeoutMs);
} // namespace wayland

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
extern const Library kWayland;

} // namespace interpose

} // namespace uih::detail
