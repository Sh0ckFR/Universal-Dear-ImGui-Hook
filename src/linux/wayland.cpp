// Input of native Wayland applications. The listeners the application gives
// to its wl_pointer, wl_keyboard and relative and locked pointers are wrapped:
// the overlay reads their events, hides them from the application while the
// menu is open, and adds those of the remote control. libwayland-client and
// libxkbcommon are not linked: the application brings the first one, and the
// second one is opened at run time when the application gets a keymap.
#include "internal.h"

#include <dlfcn.h>
#include <sys/mman.h>
#include <time.h>

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstring>
#include <deque>
#include <string>
#include <thread>
#include <unordered_map>
#include <iterator>
#include <vector>

namespace uih::detail::wayland {

// Types of libwayland-client, whose layout is part of its ABI.
struct Proxy;
struct Message {
    const char* name;
    const char* signature;
    const void* const* types;
};
struct Interface {
    const char* name;
    int version;
    int methodCount;
    const Message* methods;
    int eventCount;
    const Message* events;
};
struct Array {
    size_t size;
    size_t alloc;
    void* data;
};
union Argument {
    int32_t i;
    uint32_t u;
    int32_t f;
    const char* s;
    void* o;
    uint32_t n;
    Array* a;
    int32_t h;
};
namespace {

// WL_CLOSURE_MAX_ARGS
constexpr int kMaxArguments = 20;
constexpr uint32_t kMarshalDestroy = 1;
// struct wl_proxy starts with its struct wl_object, which starts with its
// interface, and goes on with its display.
struct ProxyHeader {
    const Interface* interface;
    const void* implementation;
    uint32_t id;
    void* display;
};
const Interface* InterfaceOf(const Proxy* proxy) { return reinterpret_cast<const ProxyHeader*>(proxy)->interface; }

using Fn = void (*)();

template <typename T>
T Next(const char* name) {
    return reinterpret_cast<T>(interpose::kWayland.Next(name));
}

const char* ClassOf(Proxy* proxy) {
    static const auto getClass = Next<const char* (*)(Proxy*)>("wl_proxy_get_class");
    return getClass ? getClass(proxy) : "";
}

void* DisplayOf(Proxy* proxy) {
    // libwayland-client 1.23 and later.
    static const auto getDisplay = Next<void* (*)(Proxy*)>("wl_proxy_get_display");
    return getDisplay ? getDisplay(proxy) : reinterpret_cast<const ProxyHeader*>(proxy)->display;
}

uint32_t VersionOf(Proxy* proxy) {
    static const auto getVersion = Next<uint32_t (*)(Proxy*)>("wl_proxy_get_version");
    return getVersion ? getVersion(proxy) : 0;
}

void* UserDataOf(Proxy* proxy) {
    static const auto getUserData = Next<void* (*)(Proxy*)>("wl_proxy_get_user_data");
    return getUserData ? getUserData(proxy) : nullptr;
}

double Fixed(int32_t value) { return value / 256.0; }
int32_t ToFixed(double value) { return static_cast<int32_t>(value * 256.0); }

uint32_t NowMs() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<uint32_t>(now.tv_sec * 1000 + now.tv_nsec / 1000000);
}

// linux/input-event-codes.h
constexpr uint32_t kButtonLeft = 0x110;
constexpr uint32_t kButtonRight = 0x111;
constexpr uint32_t kButtonMiddle = 0x112;
// Events per interface, up to wl_seat version 10 and the version 1 of the pointer protocols.
constexpr int kPointerEvents = 11;
constexpr int kKeyboardEvents = 6;

enum class Kind : uint8_t { Pointer, Keyboard, RelativePointer, LockedPointer };

struct Wrapped {
    Kind kind;
    // The application's listener.
    const void* listener;
    // Its connection: graphics libraries open and close their own.
    void* display;
};

// X11 keysyms (X11/keysymdef.h), which xkbcommon uses too.
constexpr uint32_t kShiftL = 0xffe1, kShiftR = 0xffe2, kControlL = 0xffe3, kControlR = 0xffe4;
constexpr uint32_t kAltL = 0xffe9, kAltR = 0xffea, kSuperL = 0xffeb, kSuperR = 0xffec;

// libxkbcommon, opened at run time --------------------------------------------

struct Xkb {
    void* library = nullptr;
    bool tried = false;
    void* context = nullptr;
    void* keymap = nullptr;
    void* state = nullptr;
    void* (*contextNew)(int) = nullptr;
    void (*contextUnref)(void*) = nullptr;
    void* (*keymapNewFromString)(void*, const char*, int, int) = nullptr;
    void (*keymapUnref)(void*) = nullptr;
    uint32_t (*keymapMinKeycode)(void*) = nullptr;
    uint32_t (*keymapMaxKeycode)(void*) = nullptr;
    int (*keymapKeyGetSymsByLevel)(void*, uint32_t, uint32_t, uint32_t, const uint32_t**) = nullptr;
    uint32_t (*keymapModGetIndex)(void*, const char*) = nullptr;
    void* (*stateNew)(void*) = nullptr;
    void (*stateUnref)(void*) = nullptr;
    int (*stateUpdateMask)(void*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) = nullptr;
    uint32_t (*stateKeyGetUtf32)(void*, uint32_t) = nullptr;
    uint32_t (*stateKeyGetLayout)(void*, uint32_t) = nullptr;

    bool Load() {
        if (tried)
            return library != nullptr;
        tried = true;
        library = dlopen("libxkbcommon.so.0", RTLD_NOW | RTLD_LOCAL);
        if (!library) {
            Log("libxkbcommon is not installed: the keys are read with the US layout");
            return false;
        }
        bool found = true;
        auto load = [&](auto& function, const char* name) {
            function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(dlsym(library, name));
            found = found && function;
        };
        load(contextNew, "xkb_context_new");
        load(contextUnref, "xkb_context_unref");
        load(keymapNewFromString, "xkb_keymap_new_from_string");
        load(keymapUnref, "xkb_keymap_unref");
        load(keymapMinKeycode, "xkb_keymap_min_keycode");
        load(keymapMaxKeycode, "xkb_keymap_max_keycode");
        load(keymapKeyGetSymsByLevel, "xkb_keymap_key_get_syms_by_level");
        load(keymapModGetIndex, "xkb_keymap_mod_get_index");
        load(stateNew, "xkb_state_new");
        load(stateUnref, "xkb_state_unref");
        load(stateUpdateMask, "xkb_state_update_mask");
        load(stateKeyGetUtf32, "xkb_state_key_get_utf32");
        load(stateKeyGetLayout, "xkb_state_key_get_layout");
        if (found)
            context = contextNew(0);
        if (!context) {
            Log("libxkbcommon lacks functions: the keys are read with the US layout");
            dlclose(library);
            library = nullptr;
        }
        return library != nullptr;
    }

    void Clear() {
        if (state)
            stateUnref(state);
        if (keymap)
            keymapUnref(keymap);
        state = nullptr;
        keymap = nullptr;
    }

    // XKB_KEYMAP_FORMAT_TEXT_V1
    void SetKeymap(const std::string& text) {
        if (!Load())
            return;
        Clear();
        keymap = keymapNewFromString(context, text.c_str(), 1, 0);
        state = keymap ? stateNew(keymap) : nullptr;
        if (!state)
            Clear();
    }

    uint32_t Mask(const char* name) const {
        uint32_t index = keymap ? keymapModGetIndex(keymap, name) : 0xffffffff;
        return index < 32 ? 1u << index : 0;
    }
};

// The keys of a US keyboard by evdev code, without and with Shift, when libxkbcommon is missing.
struct FallbackKey {
    uint32_t plain;
    uint32_t shifted;
};
const FallbackKey kFallback[128] = {
    {0, 0}, {0xff1b, 0xff1b}, {'1', '!'}, {'2', '@'}, {'3', '#'}, {'4', '$'}, {'5', '%'}, {'6', '^'},
    {'7', '&'}, {'8', '*'}, {'9', '('}, {'0', ')'}, {'-', '_'}, {'=', '+'}, {0xff08, 0xff08}, {0xff09, 0xff09},
    {'q', 'Q'}, {'w', 'W'}, {'e', 'E'}, {'r', 'R'}, {'t', 'T'}, {'y', 'Y'}, {'u', 'U'}, {'i', 'I'},
    {'o', 'O'}, {'p', 'P'}, {'[', '{'}, {']', '}'}, {0xff0d, 0xff0d}, {kControlL, kControlL}, {'a', 'A'}, {'s', 'S'},
    {'d', 'D'}, {'f', 'F'}, {'g', 'G'}, {'h', 'H'}, {'j', 'J'}, {'k', 'K'}, {'l', 'L'}, {';', ':'},
    {'\'', '"'}, {'`', '~'}, {kShiftL, kShiftL}, {'\\', '|'}, {'z', 'Z'}, {'x', 'X'}, {'c', 'C'}, {'v', 'V'},
    {'b', 'B'}, {'n', 'N'}, {'m', 'M'}, {',', '<'}, {'.', '>'}, {'/', '?'}, {kShiftR, kShiftR}, {0, 0},
    {kAltL, kAltL}, {' ', ' '}, {0, 0}, {0xffbe, 0xffbe}, {0xffbf, 0xffbf}, {0xffc0, 0xffc0}, {0xffc1, 0xffc1}, {0xffc2, 0xffc2},
    {0xffc3, 0xffc3}, {0xffc4, 0xffc4}, {0xffc5, 0xffc5}, {0xffc6, 0xffc6}, {0xffc7, 0xffc7}, {0, 0}, {0, 0}, {0, 0},
    {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
    {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0xffc8, 0xffc8},
    {0xffc9, 0xffc9}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
    {0xff8d, 0xff8d}, {kControlR, kControlR}, {0, 0}, {0, 0}, {kAltR, kAltR}, {0, 0}, {0xff50, 0xff50}, {0xff52, 0xff52},
    {0xff55, 0xff55}, {0xff51, 0xff51}, {0xff53, 0xff53}, {0xff57, 0xff57}, {0xff54, 0xff54}, {0xff56, 0xff56}, {0xff63, 0xff63}, {0xffff, 0xffff},
    {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
    {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {kSuperL, kSuperL}, {kSuperR, kSuperR}, {0, 0},
};

// State ---------------------------------------------------------------------

struct Key {
    uint32_t code;
    uint32_t syms[2];
    // The application saw the press, and sees the release.
    bool shown;
};

struct Button {
    uint32_t code;
    bool shown;
};

// Input for Dear ImGui, replayed by NewFrame() on the thread that draws.
struct InputEvent {
    enum Type : uint8_t { Position, Key, Character, Button, Wheel } type;
    bool down;
    uint32_t value;
    float x;
    float y;
};
constexpr size_t kMaxEvents = 512;

// A made-up event of the remote control.
struct Synthetic {
    enum Type : uint8_t { Key, Shift, Motion, Button, End } type;
    bool down;
    uint32_t code;
    double x;
    double y;
};

std::mutex g_mutex;
std::unordered_map<const Proxy*, Wrapped> g_wrapped;
std::atomic<int> g_devices{0};
Proxy* g_pointer = nullptr;
Proxy* g_keyboard = nullptr;
// Surfaces the application draws on with Vulkan or EGL, and the last one it got input on.
// Surfaces the application draws on, with their connection.
std::unordered_map<const Proxy*, void*> g_renderSurfaces;
Proxy* g_lastSurface = nullptr;
std::atomic<uint32_t> g_width{0};
std::atomic<uint32_t> g_height{0};

Proxy* g_pointerSurface = nullptr;
double g_pointerX = 0.0;
double g_pointerY = 0.0;
bool g_pointerLocked = false;
std::vector<Button> g_buttons;

Xkb g_xkb;
Proxy* g_keyboardSurface = nullptr;
std::vector<Key> g_keys;
// Keysyms pressed since the last frame, so that a press shorter than a frame counts.
std::vector<uint32_t> g_pressedSyms;
uint32_t g_depressed = 0;
uint32_t g_latched = 0;
uint32_t g_locked = 0;
uint32_t g_group = 0;
uint32_t g_serial = 0;

bool g_attached = false;
std::vector<InputEvent> g_events;
bool g_sentFocus = false;
// Keysyms down for the current frame, read on the thread that draws.
std::vector<uint32_t> g_frameSyms;

std::deque<Synthetic> g_synthetic;
std::atomic<size_t> g_syntheticCount{0};
std::condition_variable g_delivered;
// Thread on which the application reads its input events.
std::atomic<std::thread::id> g_inputThread{};
thread_local bool t_injecting = false;
thread_local int t_graphicsCall = 0;
uint32_t g_injectedMods = 0;
bool g_injectedPointerEnter = false;
bool g_injectedKeyboardEnter = false;

void Push(const InputEvent& event) {
    if (!g_attached)
        return;
    if (g_events.size() >= kMaxEvents)
        g_events.erase(g_events.begin());
    g_events.push_back(event);
}

bool OnRenderSurface(const Proxy* surface) {
    return surface && (g_renderSurfaces.empty() || g_renderSurfaces.count(surface));
}

void PushPosition() {
    bool valid = OnRenderSurface(g_pointerSurface);
    if (!g_events.empty() && g_events.back().type == InputEvent::Position)
        g_events.pop_back();
    Push({InputEvent::Position, valid, 0, static_cast<float>(g_pointerX), static_cast<float>(g_pointerY)});
}

void Syms(uint32_t code, uint32_t syms[2]) {
    syms[0] = syms[1] = 0;
    if (g_xkb.state) {
        uint32_t keycode = code + 8;
        uint32_t layout = g_xkb.stateKeyGetLayout(g_xkb.state, keycode);
        for (uint32_t level = 0; level < 2; ++level) {
            const uint32_t* found = nullptr;
            if (g_xkb.keymapKeyGetSymsByLevel(g_xkb.keymap, keycode, layout == 0xffffffff ? 0 : layout, level, &found) > 0 && found)
                syms[level] = found[0];
        }
        return;
    }
    if (code < 128) {
        syms[0] = kFallback[code].plain;
        syms[1] = kFallback[code].shifted;
    }
}

uint32_t ShiftMask() { return g_xkb.state ? g_xkb.Mask("Shift") : 1u; }
uint32_t LockMask() { return g_xkb.state ? g_xkb.Mask("Lock") : 2u; }
uint32_t ControlMask() { return g_xkb.state ? g_xkb.Mask("Control") : 4u; }
uint32_t AltMask() { return g_xkb.state ? g_xkb.Mask("Mod1") : 8u; }
uint32_t SuperMask() { return g_xkb.state ? g_xkb.Mask("Mod4") : 64u; }
uint32_t Modifiers() { return g_depressed | g_latched | g_locked | g_injectedMods; }

uint32_t ModifierOf(uint32_t keysym) {
    switch (keysym) {
    case kShiftL:
    case kShiftR: return ShiftMask();
    case kControlL:
    case kControlR: return ControlMask();
    case kAltL:
    case kAltR: return AltMask();
    case kSuperL:
    case kSuperR: return SuperMask();
    default: return 0;
    }
}

// The character `code` types, 0 for none: printable characters only.
uint32_t Character(uint32_t code, const uint32_t syms[2]) {
    uint32_t modifiers = Modifiers();
    if (modifiers & (ControlMask() | AltMask()))
        return 0;
    uint32_t point = 0;
    if (g_xkb.state) {
        point = g_xkb.stateKeyGetUtf32(g_xkb.state, code + 8);
    } else {
        bool letter = syms[0] >= 'a' && syms[0] <= 'z';
        bool shift = (modifiers & ShiftMask()) != 0;
        bool capsLock = (modifiers & LockMask()) != 0;
        uint32_t keysym = shift != (capsLock && letter) ? syms[1] : syms[0];
        point = keysym < 0x100 ? keysym : 0;
    }
    return point >= 0x20 && point != 0x7f ? point : 0;
}

void LatchSyms(const uint32_t syms[2]) {
    if (g_pressedSyms.size() > 64)
        g_pressedSyms.clear();
    g_pressedSyms.push_back(syms[0]);
    g_pressedSyms.push_back(syms[1]);
}

void ReleaseAllForImGui() {
    for (const Key& key : g_keys) {
        ImGuiKey imguiKey = input::KeysymToImGuiKey(key.syms[0]);
        if (imguiKey != ImGuiKey_None)
            Push({InputEvent::Key, false, static_cast<uint32_t>(imguiKey), 0, 0});
    }
}

void ForgetLocked(const Proxy* proxy) {
    auto it = g_wrapped.find(proxy);
    if (it != g_wrapped.end()) {
        if (it->second.kind == Kind::Pointer || it->second.kind == Kind::Keyboard) {
            --g_devices;
            Log("Stopped reading the input of a %s", it->second.kind == Kind::Pointer ? "wl_pointer" : "wl_keyboard");
        }
        g_wrapped.erase(it);
    }
    if (g_pointer == proxy)
        g_pointer = nullptr;
    if (g_keyboard == proxy)
        g_keyboard = nullptr;
    g_renderSurfaces.erase(proxy);
    if (g_lastSurface == proxy)
        g_lastSurface = nullptr;
    if (g_pointerSurface == proxy)
        g_pointerSurface = nullptr;
    if (g_keyboardSurface == proxy) {
        g_keyboardSurface = nullptr;
        g_keys.clear();
    }
}

void Forget(const Proxy* proxy) {
    if (!proxy)
        return;
    std::lock_guard<std::mutex> lock(g_mutex);
    ForgetLocked(proxy);
}

void NoteInputThread() {
    if (!t_injecting)
        g_inputThread = std::this_thread::get_id();
}

Fn Handler(const Proxy* proxy, int opcode) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_wrapped.find(proxy);
    return it != g_wrapped.end() && it->second.listener ? reinterpret_cast<const Fn*>(it->second.listener)[opcode] : nullptr;
}

// Hands the event to the application's listener.
template <typename... Args>
void Forward(Proxy* proxy, int opcode, void* data, Args... args) {
    if (Fn handler = Handler(proxy, opcode))
        reinterpret_cast<void (*)(void*, Proxy*, Args...)>(handler)(data, proxy, args...);
}

// wl_pointer ----------------------------------------------------------------

void PointerEnter(void* data, Proxy* pointer, uint32_t serial, Proxy* surface, int32_t x, int32_t y) {
    NoteInputThread();
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_pointerSurface = surface;
        g_lastSurface = surface;
        g_pointerX = Fixed(x);
        g_pointerY = Fixed(y);
        g_serial = serial;
        PushPosition();
    }
    Forward(pointer, 0, data, serial, surface, x, y);
}

void PointerLeave(void* data, Proxy* pointer, uint32_t serial, Proxy* surface) {
    NoteInputThread();
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_pointerSurface == surface)
            g_pointerSurface = nullptr;
        g_serial = serial;
        // No release comes for the buttons held.
        for (const Button& button : g_buttons)
            Push({InputEvent::Button, false, button.code, 0, 0});
        g_buttons.clear();
        PushPosition();
    }
    Forward(pointer, 1, data, serial, surface);
}

void PointerMotion(void* data, Proxy* pointer, uint32_t time, int32_t x, int32_t y) {
    NoteInputThread();
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_pointerX = Fixed(x);
        g_pointerY = Fixed(y);
        PushPosition();
    }
    if (!IsMenuOpen())
        Forward(pointer, 2, data, time, x, y);
}

// Releases reach the application when it saw the press, so that no button stays down.
void PointerButton(void* data, Proxy* pointer, uint32_t serial, uint32_t time, uint32_t code, uint32_t state) {
    NoteInputThread();
    bool down = state != 0;
    bool shown = !IsMenuOpen();
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_serial = serial;
        auto held = std::find_if(g_buttons.begin(), g_buttons.end(), [&](const Button& button) { return button.code == code; });
        if (!down && held != g_buttons.end()) {
            shown = held->shown;
            g_buttons.erase(held);
        } else if (down && held == g_buttons.end()) {
            g_buttons.push_back({code, shown});
        }
        PushPosition();
        Push({InputEvent::Button, down, code, 0, 0});
    }
    if (shown)
        Forward(pointer, 3, data, serial, time, code, state);
}

void PointerAxis(void* data, Proxy* pointer, uint32_t time, uint32_t axis, int32_t value) {
    NoteInputThread();
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        // About 10 per step of the wheel; positive values scroll down or right.
        float steps = static_cast<float>(-Fixed(value) / 10.0);
        Push({InputEvent::Wheel, false, 0, axis == 1 ? steps : 0.0f, axis == 1 ? 0.0f : steps});
    }
    if (!IsMenuOpen())
        Forward(pointer, 4, data, time, axis, value);
}

void PointerFrame(void* data, Proxy* pointer) { Forward(pointer, 5, data); }

void PointerAxisSource(void* data, Proxy* pointer, uint32_t source) {
    if (!IsMenuOpen())
        Forward(pointer, 6, data, source);
}

void PointerAxisStop(void* data, Proxy* pointer, uint32_t time, uint32_t axis) {
    if (!IsMenuOpen())
        Forward(pointer, 7, data, time, axis);
}

void PointerAxisDiscrete(void* data, Proxy* pointer, uint32_t axis, int32_t discrete) {
    if (!IsMenuOpen())
        Forward(pointer, 8, data, axis, discrete);
}

void PointerAxisValue120(void* data, Proxy* pointer, uint32_t axis, int32_t value) {
    if (!IsMenuOpen())
        Forward(pointer, 9, data, axis, value);
}

void PointerAxisDirection(void* data, Proxy* pointer, uint32_t axis, uint32_t direction) {
    if (!IsMenuOpen())
        Forward(pointer, 10, data, axis, direction);
}

const Fn kPointerListener[kPointerEvents] = {
    reinterpret_cast<Fn>(PointerEnter),        reinterpret_cast<Fn>(PointerLeave),         reinterpret_cast<Fn>(PointerMotion),
    reinterpret_cast<Fn>(PointerButton),       reinterpret_cast<Fn>(PointerAxis),          reinterpret_cast<Fn>(PointerFrame),
    reinterpret_cast<Fn>(PointerAxisSource),   reinterpret_cast<Fn>(PointerAxisStop),      reinterpret_cast<Fn>(PointerAxisDiscrete),
    reinterpret_cast<Fn>(PointerAxisValue120), reinterpret_cast<Fn>(PointerAxisDirection),
};

// zwp_relative_pointer_v1 and zwp_locked_pointer_v1 -----------------------------
// A locked pointer does not move: while the menu is open, its relative motion
// moves the cursor of the menu.

void RelativeMotion(void* data, Proxy* pointer, uint32_t timeHigh, uint32_t timeLow, int32_t dx, int32_t dy, int32_t dxRaw, int32_t dyRaw) {
    NoteInputThread();
    if (!IsMenuOpen()) {
        Forward(pointer, 0, data, timeHigh, timeLow, dx, dy, dxRaw, dyRaw);
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_pointerLocked)
        return;
    double width = std::max<uint32_t>(1, g_width.load());
    double height = std::max<uint32_t>(1, g_height.load());
    g_pointerX = std::clamp(g_pointerX + Fixed(dx), 0.0, width - 1);
    g_pointerY = std::clamp(g_pointerY + Fixed(dy), 0.0, height - 1);
    PushPosition();
}

void PointerLocked(void* data, Proxy* locked) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_pointerLocked = true;
    }
    Forward(locked, 0, data);
}

void PointerUnlocked(void* data, Proxy* locked) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_pointerLocked = false;
    }
    Forward(locked, 1, data);
}

const Fn kRelativePointerListener[1] = {reinterpret_cast<Fn>(RelativeMotion)};
const Fn kLockedPointerListener[2] = {reinterpret_cast<Fn>(PointerLocked), reinterpret_cast<Fn>(PointerUnlocked)};

// wl_keyboard ---------------------------------------------------------------

// The application usually closes the file of the keymap: it is read first.
void KeyboardKeymap(void* data, Proxy* keyboard, uint32_t format, int32_t fd, uint32_t size) {
    NoteInputThread();
    if (format == 1 && size > 0) {
        void* map = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (map != MAP_FAILED) {
            std::string text(static_cast<const char*>(map), strnlen(static_cast<const char*>(map), size));
            munmap(map, size);
            std::lock_guard<std::mutex> lock(g_mutex);
            g_xkb.SetKeymap(text);
            if (g_xkb.state)
                g_xkb.stateUpdateMask(g_xkb.state, g_depressed, g_latched, g_locked, 0, 0, g_group);
        }
    }
    Forward(keyboard, 0, data, format, fd, size);
}

void KeyboardEnter(void* data, Proxy* keyboard, uint32_t serial, Proxy* surface, Array* keys) {
    NoteInputThread();
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_keyboardSurface = surface;
        g_lastSurface = surface;
        g_serial = serial;
        g_keys.clear();
        for (size_t i = 0; keys && i + sizeof(uint32_t) <= keys->size; i += sizeof(uint32_t)) {
            Key key{};
            memcpy(&key.code, static_cast<const char*>(keys->data) + i, sizeof(key.code));
            Syms(key.code, key.syms);
            key.shown = true;
            g_keys.push_back(key);
        }
    }
    Forward(keyboard, 1, data, serial, surface, keys);
}

void KeyboardLeave(void* data, Proxy* keyboard, uint32_t serial, Proxy* surface) {
    NoteInputThread();
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_keyboardSurface == surface)
            g_keyboardSurface = nullptr;
        g_serial = serial;
        // No release comes for the keys held.
        ReleaseAllForImGui();
        g_keys.clear();
    }
    Forward(keyboard, 2, data, serial, surface);
}

// State 1 presses, 2 repeats (wl_seat version 10). Releases reach the
// application when it saw the press, so that no key stays down.
void KeyboardKey(void* data, Proxy* keyboard, uint32_t serial, uint32_t time, uint32_t code, uint32_t state) {
    NoteInputThread();
    bool down = state != 0;
    bool shown = !IsMenuOpen();
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_serial = serial;
        auto held = std::find_if(g_keys.begin(), g_keys.end(), [&](const Key& key) { return key.code == code; });
        Key key{};
        key.code = code;
        Syms(code, key.syms);
        if (held != g_keys.end()) {
            key = *held;
            shown = held->shown;
            if (!down)
                g_keys.erase(held);
        } else if (down) {
            key.shown = shown;
            g_keys.push_back(key);
            LatchSyms(key.syms);
        }
        ImGuiKey imguiKey = input::KeysymToImGuiKey(key.syms[0]);
        if (imguiKey != ImGuiKey_None)
            Push({InputEvent::Key, down, static_cast<uint32_t>(imguiKey), 0, 0});
        if (down)
            if (uint32_t character = Character(code, key.syms))
                Push({InputEvent::Character, true, character, 0, 0});
    }
    if (shown)
        Forward(keyboard, 3, data, serial, time, code, state);
}

void KeyboardModifiers(void* data, Proxy* keyboard, uint32_t serial, uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group) {
    NoteInputThread();
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_serial = serial;
        if (!t_injecting) {
            g_depressed = depressed;
            g_latched = latched;
            g_locked = locked;
            g_group = group;
        }
        if (g_xkb.state)
            g_xkb.stateUpdateMask(g_xkb.state, depressed, latched, locked, 0, 0, group);
    }
    Forward(keyboard, 4, data, serial, depressed, latched, locked, group);
}

void KeyboardRepeatInfo(void* data, Proxy* keyboard, int32_t rate, int32_t delay) { Forward(keyboard, 5, data, rate, delay); }

const Fn kKeyboardListener[kKeyboardEvents] = {
    reinterpret_cast<Fn>(KeyboardKeymap), reinterpret_cast<Fn>(KeyboardEnter),     reinterpret_cast<Fn>(KeyboardLeave),
    reinterpret_cast<Fn>(KeyboardKey),    reinterpret_cast<Fn>(KeyboardModifiers), reinterpret_cast<Fn>(KeyboardRepeatInfo),
};

// Events of the remote control ------------------------------------------------

Proxy* InjectionSurface() {
    if (g_keyboardSurface)
        return g_keyboardSurface;
    if (g_pointerSurface)
        return g_pointerSurface;
    if (g_lastSurface)
        return g_lastSurface;
    return g_renderSurfaces.empty() ? nullptr : const_cast<Proxy*>(g_renderSurfaces.begin()->first);
}

// On the thread that reads the input, through the wrapped listeners: the
// overlay sees the events, and hides them from the application while the menu is open.
void Deliver(const Synthetic& event) {
    Proxy* keyboard = nullptr;
    Proxy* pointer = nullptr;
    Proxy* surface = nullptr;
    Proxy* keyboardSurface = nullptr;
    bool keyboardEnter = false;
    bool pointerEnter = false;
    bool leavePointer = false;
    bool leaveKeyboard = false;
    bool sendModifiers = false;
    uint32_t serial = 0;
    uint32_t mods[4] = {};
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        keyboard = g_keyboard;
        pointer = g_pointer;
        surface = InjectionSurface();
        serial = g_serial;
        bool keys = event.type == Synthetic::Key || event.type == Synthetic::Shift;
        bool mouse = event.type == Synthetic::Motion || event.type == Synthetic::Button;
        if ((keys && !keyboard) || (mouse && !pointer) || ((keys || mouse) && !surface))
            return;
        if (keys) {
            keyboardEnter = !g_keyboardSurface;
            g_injectedKeyboardEnter |= keyboardEnter;
            uint32_t syms[2] = {};
            if (event.type == Synthetic::Key)
                Syms(event.code, syms);
            // A modifier key the compositor did not press: the application learns of it from wl_keyboard.modifiers.
            uint32_t modifier = event.type == Synthetic::Shift ? ShiftMask() : ModifierOf(syms[0]);
            sendModifiers = modifier != 0;
            if (event.down)
                g_injectedMods |= modifier;
            else
                g_injectedMods &= ~modifier;
            mods[0] = g_depressed | g_injectedMods;
            mods[1] = g_latched;
            mods[2] = g_locked;
            mods[3] = g_group;
        } else if (mouse) {
            pointerEnter = !g_pointerSurface;
            g_injectedPointerEnter |= pointerEnter;
        } else {
            leavePointer = g_injectedPointerEnter && pointer && g_pointerSurface;
            leaveKeyboard = g_injectedKeyboardEnter && keyboard && g_keyboardSurface;
            surface = g_pointerSurface;
            keyboardSurface = g_keyboardSurface;
            g_injectedPointerEnter = false;
            g_injectedKeyboardEnter = false;
        }
    }
    Array none{};
    uint32_t time = NowMs();
    switch (event.type) {
    case Synthetic::Key:
    case Synthetic::Shift: {
        void* data = UserDataOf(keyboard);
        if (keyboardEnter)
            KeyboardEnter(data, keyboard, serial, surface, &none);
        if (event.type == Synthetic::Key)
            KeyboardKey(data, keyboard, serial, time, event.code, event.down ? 1 : 0);
        if (sendModifiers)
            KeyboardModifiers(data, keyboard, serial, mods[0], mods[1], mods[2], mods[3]);
        break;
    }
    case Synthetic::Motion:
    case Synthetic::Button: {
        void* data = UserDataOf(pointer);
        if (pointerEnter)
            PointerEnter(data, pointer, serial, surface, ToFixed(event.x), ToFixed(event.y));
        else if (event.type == Synthetic::Motion)
            PointerMotion(data, pointer, time, ToFixed(event.x), ToFixed(event.y));
        if (event.type == Synthetic::Button)
            PointerButton(data, pointer, serial, time, event.code, event.down ? 1 : 0);
        if (VersionOf(pointer) >= 5)
            PointerFrame(data, pointer);
        break;
    }
    case Synthetic::End:
        if (leavePointer) {
            void* data = UserDataOf(pointer);
            PointerLeave(data, pointer, serial, surface);
            if (VersionOf(pointer) >= 5)
                PointerFrame(data, pointer);
        }
        if (leaveKeyboard)
            KeyboardLeave(UserDataOf(keyboard), keyboard, serial, keyboardSurface);
        break;
    }
}

void DeliverSynthetic(bool defaultQueue) {
    if (g_syntheticCount == 0 || t_injecting || (!defaultQueue && t_graphicsCall > 0))
        return;
    std::thread::id reader = g_inputThread;
    if (reader != std::thread::id() && reader != std::this_thread::get_id())
        return;
    t_injecting = true;
    for (;;) {
        Synthetic event{};
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_synthetic.empty())
                break;
            event = g_synthetic.front();
            g_synthetic.pop_front();
        }
        Deliver(event);
        std::lock_guard<std::mutex> lock(g_mutex);
        --g_syntheticCount;
    }
    t_injecting = false;
    g_delivered.notify_all();
}

bool Enqueue(const Synthetic& event) {
    std::lock_guard<std::mutex> lock(g_mutex);
    bool keyboard = event.type == Synthetic::Key || event.type == Synthetic::Shift;
    if ((keyboard && !g_keyboard) || (!keyboard && event.type != Synthetic::End && !g_pointer) || !InjectionSurface())
        return false;
    g_synthetic.push_back(event);
    ++g_syntheticCount;
    return true;
}

// The libraries -------------------------------------------------------------

const Fn* ListenerFor(Proxy* proxy, Kind& kind, int& events) {
    const char* name = ClassOf(proxy);
    if (!name)
        return nullptr;
    if (strcmp(name, "wl_pointer") == 0) {
        kind = Kind::Pointer;
        events = kPointerEvents;
        return kPointerListener;
    }
    if (strcmp(name, "wl_keyboard") == 0) {
        kind = Kind::Keyboard;
        events = kKeyboardEvents;
        return kKeyboardListener;
    }
    if (strcmp(name, "zwp_relative_pointer_v1") == 0) {
        kind = Kind::RelativePointer;
        events = 1;
        return kRelativePointerListener;
    }
    if (strcmp(name, "zwp_locked_pointer_v1") == 0) {
        kind = Kind::LockedPointer;
        events = 2;
        return kLockedPointerListener;
    }
    return nullptr;
}

// wl_argument_from_va_list() of libwayland-client.
void ArgumentsFromList(const char* signature, Argument* args, va_list list) {
    for (int i = 0; i < kMaxArguments && signature; ++i) {
        char type = '\0';
        for (; *signature; ++signature) {
            if (strchr("iufsonah", *signature)) {
                type = *signature++;
                break;
            }
        }
        switch (type) {
        case 'i': args[i].i = va_arg(list, int32_t); break;
        case 'u': args[i].u = va_arg(list, uint32_t); break;
        case 'f': args[i].f = va_arg(list, int32_t); break;
        case 's': args[i].s = va_arg(list, const char*); break;
        case 'o':
        case 'n': args[i].o = va_arg(list, void*); break;
        case 'a': args[i].a = va_arg(list, Array*); break;
        case 'h': args[i].h = va_arg(list, int32_t); break;
        default: return;
        }
    }
}

using MarshalArrayFn = Proxy* (*)(Proxy*, uint32_t, const Interface*, uint32_t, uint32_t, Argument*);

Proxy* MarshalArray(Proxy* proxy, uint32_t opcode, const Interface* interface, uint32_t version, uint32_t flags, Argument* args) {
    static const auto marshal = Next<MarshalArrayFn>("wl_proxy_marshal_array_flags");
    // Requests that destroy their object, as wl_pointer.release.
    if (flags & kMarshalDestroy)
        Forget(proxy);
    return marshal ? marshal(proxy, opcode, interface, version, flags, args) : nullptr;
}

} // namespace

bool Active() { return g_devices > 0; }

GraphicsCall::GraphicsCall() { ++t_graphicsCall; }
GraphicsCall::~GraphicsCall() { --t_graphicsCall; }

void Attach() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_attached = true;
    g_events.clear();
    g_sentFocus = false;
    g_frameSyms.clear();
}

void Detach() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_attached = false;
    g_events.clear();
    g_frameSyms.clear();
}

void SetSurfaceSize(uint32_t width, uint32_t height) {
    g_width = width;
    g_height = height;
}

void RecordSurface(const void* surface) {
    if (!surface)
        return;
    auto* proxy = static_cast<Proxy*>(const_cast<void*>(surface));
    void* display = DisplayOf(proxy);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_renderSurfaces[proxy] = display;
}

void NewFrame() {
    ImGuiIO& io = ImGui::GetIO();
    std::vector<InputEvent> events;
    bool focused = false;
    uint32_t modifiers = 0;
    uint32_t ctrl = 0, shift = 0, alt = 0, super = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        events.swap(g_events);
        focused = g_keyboardSurface != nullptr;
        modifiers = Modifiers();
        ctrl = ControlMask();
        shift = ShiftMask();
        alt = AltMask();
        super = SuperMask();
        g_frameSyms = g_pressedSyms;
        g_pressedSyms.clear();
        for (const Key& key : g_keys) {
            g_frameSyms.push_back(key.syms[0]);
            g_frameSyms.push_back(key.syms[1]);
        }
    }
    if (focused != g_sentFocus) {
        g_sentFocus = focused;
        io.AddFocusEvent(focused);
    }
    io.AddKeyEvent(ImGuiMod_Ctrl, focused && (modifiers & ctrl));
    io.AddKeyEvent(ImGuiMod_Shift, focused && (modifiers & shift));
    io.AddKeyEvent(ImGuiMod_Alt, focused && (modifiers & alt));
    io.AddKeyEvent(ImGuiMod_Super, focused && (modifiers & super));
    float width = static_cast<float>(g_width.load());
    float height = static_cast<float>(g_height.load());
    for (const InputEvent& event : events) {
        switch (event.type) {
        case InputEvent::Position:
            if (event.down && event.x >= 0 && event.y >= 0 && event.x < width && event.y < height)
                io.AddMousePosEvent(event.x, event.y);
            else
                io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
            break;
        case InputEvent::Key: io.AddKeyEvent(static_cast<ImGuiKey>(event.value), event.down); break;
        case InputEvent::Character: io.AddInputCharacter(event.value); break;
        case InputEvent::Button:
            if (event.value == kButtonLeft || event.value == kButtonRight || event.value == kButtonMiddle)
                io.AddMouseButtonEvent(event.value == kButtonLeft ? ImGuiMouseButton_Left : event.value == kButtonRight ? ImGuiMouseButton_Right : ImGuiMouseButton_Middle,
                                       event.down);
            break;
        case InputEvent::Wheel: io.AddMouseWheelEvent(event.x, event.y); break;
        }
    }
}

bool IsKeyDown(int keysym) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return std::find(g_frameSyms.begin(), g_frameSyms.end(), static_cast<uint32_t>(keysym)) != g_frameSyms.end();
}

bool IsFocused() { return g_sentFocus; }

bool KeyDownInApplication(int keysym) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_keyboardSurface)
        return false;
    auto sym = static_cast<uint32_t>(keysym);
    bool down = std::find(g_pressedSyms.begin(), g_pressedSyms.end(), sym) != g_pressedSyms.end();
    g_pressedSyms.clear();
    for (const Key& key : g_keys)
        down = down || key.syms[0] == sym || key.syms[1] == sym;
    return down;
}

bool Window(uint32_t& width, uint32_t& height, bool& focused) {
    std::lock_guard<std::mutex> lock(g_mutex);
    width = g_width;
    height = g_height;
    focused = g_keyboardSurface != nullptr;
    return width > 0 && height > 0 && (g_pointer || g_keyboard);
}

bool KeyFor(int keysym, uint32_t& code, bool& shift) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_keyboard)
        return false;
    uint32_t first = 8;
    uint32_t last = 255;
    if (g_xkb.state) {
        first = g_xkb.keymapMinKeycode(g_xkb.keymap);
        last = g_xkb.keymapMaxKeycode(g_xkb.keymap);
    }
    for (int level = 0; level < 2; ++level) {
        for (uint32_t keycode = std::max<uint32_t>(first, 8); keycode <= last; ++keycode) {
            uint32_t syms[2];
            Syms(keycode - 8, syms);
            if (syms[level] == static_cast<uint32_t>(keysym)) {
                code = keycode - 8;
                shift = level == 1 && syms[0] != syms[1];
                return true;
            }
        }
    }
    return false;
}

bool InjectKey(uint32_t code, bool down) { return Enqueue({Synthetic::Key, down, code, 0, 0}); }
bool InjectShift(bool down) { return Enqueue({Synthetic::Shift, down, 0, 0, 0}); }
bool InjectMotion(int x, int y) { return Enqueue({Synthetic::Motion, false, 0, static_cast<double>(x), static_cast<double>(y)}); }

bool InjectButton(int button, bool down) {
    uint32_t code = button == 2 ? kButtonMiddle : button == 3 ? kButtonRight : kButtonLeft;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_pointer || !InjectionSurface())
        return false;
    g_synthetic.push_back({Synthetic::Button, down, code, g_pointerX, g_pointerY});
    ++g_syntheticCount;
    return true;
}

void InjectEnd() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_synthetic.push_back({Synthetic::End, false, 0, 0, 0});
    ++g_syntheticCount;
}

bool WaitInjected(unsigned timeoutMs) {
    std::unique_lock<std::mutex> lock(g_mutex);
    bool delivered = g_delivered.wait_for(lock, std::chrono::milliseconds(timeoutMs), [] { return g_syntheticCount == 0; });
    if (!delivered) {
        // Not read in time: they would come at a moment nobody expects.
        g_syntheticCount -= g_synthetic.size();
        g_synthetic.clear();
    }
    return delivered;
}

} // namespace uih::detail::wayland

using namespace uih::detail;
using wayland::Argument;
using wayland::Interface;
using wayland::Proxy;

UIH_EXPORT int wl_proxy_add_listener(Proxy* proxy, void (**implementation)(void), void* data) {
    using AddListenerFn = int (*)(Proxy*, void (**)(void), void*);
    static const auto addListener = wayland::Next<AddListenerFn>("wl_proxy_add_listener");
    wayland::Kind kind{};
    int events = 0;
    const wayland::Fn* wrapper = proxy && implementation ? wayland::ListenerFor(proxy, kind, events) : nullptr;
    // Events this wrapper does not know, of a later version of the protocol: the listener stays the application's.
    if (wrapper && wayland::InterfaceOf(proxy)->eventCount > events) {
        Log("%s has events of a later version: its input is not read", wayland::ClassOf(proxy));
        wrapper = nullptr;
    }
    if (!wrapper)
        return addListener(proxy, implementation, data);
    {
        std::lock_guard<std::mutex> lock(wayland::g_mutex);
        wayland::ForgetLocked(proxy);
        wayland::g_wrapped[proxy] = {kind, reinterpret_cast<const void*>(implementation), wayland::DisplayOf(proxy)};
        if (kind == wayland::Kind::Pointer || kind == wayland::Kind::Keyboard) {
            ++wayland::g_devices;
            (kind == wayland::Kind::Pointer ? wayland::g_pointer : wayland::g_keyboard) = proxy;
        }
    }
    int result = addListener(proxy, const_cast<wayland::Fn*>(wrapper), data);
    if (result != 0)
        wayland::Forget(proxy);
    else if (kind == wayland::Kind::Pointer || kind == wayland::Kind::Keyboard)
        Log("Reading the input of %s", wayland::ClassOf(proxy));
    return result;
}

// The application's listener, not the wrapper.
UIH_EXPORT const void* wl_proxy_get_listener(Proxy* proxy) {
    using GetListenerFn = const void* (*)(Proxy*);
    static const auto getListener = wayland::Next<GetListenerFn>("wl_proxy_get_listener");
    {
        std::lock_guard<std::mutex> lock(wayland::g_mutex);
        auto it = wayland::g_wrapped.find(proxy);
        if (it != wayland::g_wrapped.end())
            return it->second.listener;
    }
    return getListener ? getListener(proxy) : nullptr;
}

UIH_EXPORT void wl_proxy_destroy(Proxy* proxy) {
    static const auto destroy = wayland::Next<void (*)(Proxy*)>("wl_proxy_destroy");
    wayland::Forget(proxy);
    destroy(proxy);
}

UIH_EXPORT Proxy* wl_proxy_marshal_array_flags(Proxy* proxy, uint32_t opcode, const Interface* interface, uint32_t version, uint32_t flags, Argument* args) {
    return wayland::MarshalArray(proxy, opcode, interface, version, flags, args);
}

// The generated request functions call this one, whose arguments are read as libwayland-client does.
UIH_EXPORT Proxy* wl_proxy_marshal_flags(Proxy* proxy, uint32_t opcode, const Interface* interface, uint32_t version, uint32_t flags, ...) {
    Argument args[wayland::kMaxArguments] = {};
    va_list list;
    va_start(list, flags);
    wayland::ArgumentsFromList(wayland::InterfaceOf(proxy)->methods[opcode].signature, args, list);
    va_end(list);
    return wayland::MarshalArray(proxy, opcode, interface, version, flags, args);
}

// The events of the remote control go to the application where it reads its own.
UIH_EXPORT int wl_display_dispatch(void* display) {
    static const auto next = wayland::Next<int (*)(void*)>("wl_display_dispatch");
    wayland::DeliverSynthetic(true);
    return next(display);
}

UIH_EXPORT int wl_display_dispatch_pending(void* display) {
    static const auto next = wayland::Next<int (*)(void*)>("wl_display_dispatch_pending");
    wayland::DeliverSynthetic(true);
    return next(display);
}

UIH_EXPORT int wl_display_roundtrip(void* display) {
    static const auto next = wayland::Next<int (*)(void*)>("wl_display_roundtrip");
    wayland::DeliverSynthetic(true);
    return next(display);
}

// The graphics libraries read their own events on queues of their own, during the presents.
UIH_EXPORT int wl_display_dispatch_queue(void* display, void* queue) {
    static const auto next = wayland::Next<int (*)(void*, void*)>("wl_display_dispatch_queue");
    wayland::DeliverSynthetic(false);
    return next(display, queue);
}

UIH_EXPORT int wl_display_dispatch_queue_pending(void* display, void* queue) {
    static const auto next = wayland::Next<int (*)(void*, void*)>("wl_display_dispatch_queue_pending");
    wayland::DeliverSynthetic(false);
    return next(display, queue);
}

UIH_EXPORT int wl_display_roundtrip_queue(void* display, void* queue) {
    static const auto next = wayland::Next<int (*)(void*, void*)>("wl_display_roundtrip_queue");
    wayland::DeliverSynthetic(false);
    return next(display, queue);
}

// Forgets the objects of the connection. Graphics libraries, such as Mesa's
// device selection layer, open and close connections of their own.
UIH_EXPORT void wl_display_disconnect(void* display) {
    static const auto disconnect = wayland::Next<void (*)(void*)>("wl_display_disconnect");
    {
        std::lock_guard<std::mutex> lock(wayland::g_mutex);
        bool devices = false;
        for (auto it = wayland::g_wrapped.begin(); it != wayland::g_wrapped.end();) {
            if (it->second.display != display) {
                ++it;
                continue;
            }
            if (it->second.kind == wayland::Kind::Pointer || it->second.kind == wayland::Kind::Keyboard) {
                --wayland::g_devices;
                devices = true;
            }
            it = wayland::g_wrapped.erase(it);
        }
        for (auto it = wayland::g_renderSurfaces.begin(); it != wayland::g_renderSurfaces.end();)
            it = it->second == display ? wayland::g_renderSurfaces.erase(it) : std::next(it);
        if (devices) {
            wayland::g_pointer = nullptr;
            wayland::g_keyboard = nullptr;
            wayland::g_lastSurface = nullptr;
            wayland::g_pointerSurface = nullptr;
            wayland::g_keyboardSurface = nullptr;
            wayland::g_keys.clear();
            wayland::g_buttons.clear();
            wayland::g_syntheticCount -= wayland::g_synthetic.size();
            wayland::g_synthetic.clear();
            wayland::g_inputThread = std::thread::id();
        }
    }
    wayland::g_delivered.notify_all();
    disconnect(display);
}

// libwayland-egl: the surface an EGL window draws on.
UIH_EXPORT void* wl_egl_window_create(Proxy* surface, int width, int height) {
    static const auto create = wayland::Next<void* (*)(Proxy*, int, int)>("wl_egl_window_create");
    void* window = create(surface, width, height);
    if (window)
        wayland::RecordSurface(surface);
    return window;
}

namespace uih::detail {
namespace {

const char* const kWaylandSonames[] = {"libwayland-client.so.0", "libwayland-egl.so.1", nullptr};

#define UIH_WL_EXPORT(name) {#name, reinterpret_cast<void*>(&name)}
const interpose::Function kWaylandExports[] = {
    UIH_WL_EXPORT(wl_proxy_add_listener),
    UIH_WL_EXPORT(wl_proxy_get_listener),
    UIH_WL_EXPORT(wl_proxy_destroy),
    UIH_WL_EXPORT(wl_proxy_marshal_flags),
    UIH_WL_EXPORT(wl_proxy_marshal_array_flags),
    UIH_WL_EXPORT(wl_display_dispatch),
    UIH_WL_EXPORT(wl_display_dispatch_pending),
    UIH_WL_EXPORT(wl_display_dispatch_queue),
    UIH_WL_EXPORT(wl_display_dispatch_queue_pending),
    UIH_WL_EXPORT(wl_display_roundtrip),
    UIH_WL_EXPORT(wl_display_roundtrip_queue),
    UIH_WL_EXPORT(wl_display_disconnect),
    UIH_WL_EXPORT(wl_egl_window_create),
};
#undef UIH_WL_EXPORT

} // namespace

const interpose::Library interpose::kWayland = {kWaylandSonames, kWaylandExports, sizeof(kWaylandExports) / sizeof(kWaylandExports[0])};

} // namespace uih::detail
