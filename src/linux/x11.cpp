// Dear ImGui platform backend polling the X server on its own connection.
#include "internal.h"

#include <unistd.h>
#include <xcb/xcb.h>

#include <cfloat>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace uih::detail::input {
namespace {

struct FreeDeleter {
    void operator()(void* reply) const { free(reply); }
};
template <typename T>
using Reply = std::unique_ptr<T, FreeDeleter>;

constexpr int kKeycodes = 256;
constexpr int kMaxTreeDepth = 32;

xcb_connection_t* g_connection = nullptr;
bool g_unavailable = false;
xcb_window_t g_root = XCB_NONE;
xcb_atom_t g_pidAtom = XCB_NONE;

uint8_t g_minKeycode = 0;
uint8_t g_keysymsPerKeycode = 0;
std::vector<xcb_keysym_t> g_keysyms;

xcb_window_t g_window = XCB_NONE;
xcb_window_t g_focus = XCB_NONE;
bool g_focusKnown = false;
bool g_focusRelated = false;
bool g_focused = false;

// Keys held on the server, and keys reported to Dear ImGui.
uint8_t g_keys[kKeycodes / 8] = {};
uint8_t g_sent[kKeycodes / 8] = {};

float g_scale = 1.0f;

uint32_t g_width = 0;
uint32_t g_height = 0;
std::chrono::steady_clock::time_point g_lastFrame;

void LoadKeyboardMapping() {
    const xcb_setup_t* setup = xcb_get_setup(g_connection);
    xcb_get_keyboard_mapping_cookie_t cookie = xcb_get_keyboard_mapping(g_connection, setup->min_keycode, setup->max_keycode - setup->min_keycode + 1);
    Reply<xcb_get_keyboard_mapping_reply_t> reply(xcb_get_keyboard_mapping_reply(g_connection, cookie, nullptr));
    if (!reply)
        return;
    const xcb_keysym_t* keysyms = xcb_get_keyboard_mapping_keysyms(reply.get());
    g_keysyms.assign(keysyms, keysyms + xcb_get_keyboard_mapping_keysyms_length(reply.get()));
    g_minKeycode = setup->min_keycode;
    g_keysymsPerKeycode = reply->keysyms_per_keycode;
}

// Desktops publish their DPI as the Xft.dpi resource.
void LoadScale() {
    xcb_get_property_cookie_t cookie = xcb_get_property(g_connection, 0, g_root, XCB_ATOM_RESOURCE_MANAGER, XCB_ATOM_STRING, 0, 1 << 16);
    Reply<xcb_get_property_reply_t> property(xcb_get_property_reply(g_connection, cookie, nullptr));
    if (!property || property->format != 8)
        return;
    std::string resources(static_cast<const char*>(xcb_get_property_value(property.get())), xcb_get_property_value_length(property.get()));
    static const char kDpi[] = "Xft.dpi:";
    size_t line = 0;
    while (line < resources.size()) {
        if (resources.compare(line, sizeof(kDpi) - 1, kDpi) == 0) {
            float dpi = strtof(resources.c_str() + line + sizeof(kDpi) - 1, nullptr);
            if (dpi > 0.0f)
                g_scale = dpi / 96.0f;
            return;
        }
        size_t end = resources.find('\n', line);
        if (end == std::string::npos)
            return;
        line = end + 1;
    }
}

void Disconnect() {
    if (g_connection)
        xcb_disconnect(g_connection);
    g_connection = nullptr;
    g_keysyms.clear();
    g_focusKnown = false;
}

bool Connect() {
    if (g_connection)
        return true;
    if (g_unavailable)
        return false;
    int screenIndex = 0;
    xcb_connection_t* connection = xcb_connect(nullptr, &screenIndex);
    if (xcb_connection_has_error(connection)) {
        xcb_disconnect(connection);
        g_unavailable = true;
        Log("No X server to read the input from");
        return false;
    }
    xcb_screen_iterator_t screen = xcb_setup_roots_iterator(xcb_get_setup(connection));
    for (int i = 0; i < screenIndex && screen.rem > 1; ++i)
        xcb_screen_next(&screen);
    g_connection = connection;
    g_root = screen.data->root;

    static const char kPid[] = "_NET_WM_PID";
    Reply<xcb_intern_atom_reply_t> atom(xcb_intern_atom_reply(connection, xcb_intern_atom(connection, 0, sizeof(kPid) - 1, kPid), nullptr));
    g_pidAtom = atom ? atom->atom : XCB_NONE;
    LoadKeyboardMapping();
    LoadScale();
    return true;
}

xcb_window_t Parent(xcb_window_t window) {
    Reply<xcb_query_tree_reply_t> tree(xcb_query_tree_reply(g_connection, xcb_query_tree(g_connection, window), nullptr));
    return tree ? tree->parent : XCB_NONE;
}

// True when `inner` is `outer` or one of its descendants.
bool Encloses(xcb_window_t outer, xcb_window_t inner) {
    for (int depth = 0; inner != XCB_NONE && depth < kMaxTreeDepth; ++depth) {
        if (inner == outer)
            return true;
        inner = Parent(inner);
    }
    return false;
}

bool OwnedByProcess(xcb_window_t window) {
    for (int depth = 0; window != XCB_NONE && window != g_root && depth < kMaxTreeDepth; ++depth) {
        xcb_get_property_cookie_t cookie = xcb_get_property(g_connection, 0, window, g_pidAtom, XCB_ATOM_CARDINAL, 0, 1);
        Reply<xcb_get_property_reply_t> property(xcb_get_property_reply(g_connection, cookie, nullptr));
        if (property && property->format == 32 && xcb_get_property_value_length(property.get()) == sizeof(uint32_t)) {
            uint32_t pid = 0;
            memcpy(&pid, xcb_get_property_value(property.get()), sizeof(pid));
            return pid == static_cast<uint32_t>(getpid());
        }
        window = Parent(window);
    }
    return false;
}

xcb_window_t FocusOwner() {
    Reply<xcb_get_input_focus_reply_t> reply(xcb_get_input_focus_reply(g_connection, xcb_get_input_focus(g_connection), nullptr));
    return reply ? reply->focus : static_cast<xcb_window_t>(XCB_NONE);
}

bool IsWindowId(xcb_window_t focus) {
    return focus != XCB_NONE && focus != XCB_INPUT_FOCUS_POINTER_ROOT;
}

void UpdateFocus() {
    xcb_window_t focus = FocusOwner();
    if (g_focusKnown && focus == g_focus)
        return;
    g_focus = focus;
    g_focusKnown = true;
    if (!IsWindowId(focus))
        g_focusRelated = false;
    else if (g_window != XCB_NONE)
        g_focusRelated = Encloses(g_window, focus) || Encloses(focus, g_window);
    else
        g_focusRelated = OwnedByProcess(focus);
}

void DrainEvents() {
    bool remapped = false;
    while (xcb_generic_event_t* event = xcb_poll_for_event(g_connection)) {
        remapped |= (event->response_type & 0x7f) == XCB_MAPPING_NOTIFY;
        free(event);
    }
    if (remapped)
        LoadKeyboardMapping();
}

xcb_keysym_t Keysym(int keycode, int column) {
    if (keycode < g_minKeycode || column >= g_keysymsPerKeycode)
        return XCB_NO_SYMBOL;
    size_t index = static_cast<size_t>(keycode - g_minKeycode) * g_keysymsPerKeycode + column;
    return index < g_keysyms.size() ? g_keysyms[index] : static_cast<xcb_keysym_t>(XCB_NO_SYMBOL);
}

bool TestKey(const uint8_t* keys, int keycode) {
    return (keys[keycode >> 3] & (1 << (keycode & 7))) != 0;
}

ImGuiKey ToImGuiKey(xcb_keysym_t keysym) {
    if (keysym >= 'a' && keysym <= 'z')
        return static_cast<ImGuiKey>(ImGuiKey_A + (keysym - 'a'));
    if (keysym >= '0' && keysym <= '9')
        return static_cast<ImGuiKey>(ImGuiKey_0 + (keysym - '0'));
    if (keysym >= 0xffbe && keysym <= 0xffc9)
        return static_cast<ImGuiKey>(ImGuiKey_F1 + (keysym - 0xffbe));
    switch (keysym) {
    case 0x0020: return ImGuiKey_Space;
    case 0xff08: return ImGuiKey_Backspace;
    case 0xff09: return ImGuiKey_Tab;
    case 0xff0d: return ImGuiKey_Enter;
    case 0xff1b: return ImGuiKey_Escape;
    case 0xff50: return ImGuiKey_Home;
    case 0xff51: return ImGuiKey_LeftArrow;
    case 0xff52: return ImGuiKey_UpArrow;
    case 0xff53: return ImGuiKey_RightArrow;
    case 0xff54: return ImGuiKey_DownArrow;
    case 0xff55: return ImGuiKey_PageUp;
    case 0xff56: return ImGuiKey_PageDown;
    case 0xff57: return ImGuiKey_End;
    case 0xff63: return ImGuiKey_Insert;
    case 0xff8d: return ImGuiKey_KeypadEnter;
    case 0xffe1: return ImGuiKey_LeftShift;
    case 0xffe2: return ImGuiKey_RightShift;
    case 0xffe3: return ImGuiKey_LeftCtrl;
    case 0xffe4: return ImGuiKey_RightCtrl;
    case 0xffe9: return ImGuiKey_LeftAlt;
    case 0xffea: return ImGuiKey_RightAlt;
    case 0xffeb: return ImGuiKey_LeftSuper;
    case 0xffec: return ImGuiKey_RightSuper;
    case 0xffff: return ImGuiKey_Delete;
    default: return ImGuiKey_None;
    }
}

// Latin-1 and Unicode keysyms only: no dead keys or input methods.
unsigned int Character(int keycode, uint16_t modifiers) {
    xcb_keysym_t plain = Keysym(keycode, 0);
    xcb_keysym_t shifted = Keysym(keycode, 1);
    bool letter = plain >= 'a' && plain <= 'z';
    bool shift = (modifiers & XCB_MOD_MASK_SHIFT) != 0;
    bool capsLock = (modifiers & XCB_MOD_MASK_LOCK) != 0;
    xcb_keysym_t keysym = plain;
    if (shift != (capsLock && letter))
        keysym = shifted != XCB_NO_SYMBOL ? shifted : (letter ? plain - ('a' - 'A') : plain);
    if ((keysym >= 0x20 && keysym <= 0x7e) || (keysym >= 0xa0 && keysym <= 0xff))
        return keysym;
    if ((keysym & 0xff000000) == 0x01000000)
        return keysym & 0x00ffffff;
    return 0;
}

} // namespace

void Attach(NativeWindow window) {
    g_window = XCB_NONE;
    g_focusKnown = false;
    if (!window || !Connect())
        return;
    Reply<xcb_get_window_attributes_reply_t> attributes(xcb_get_window_attributes_reply(g_connection, xcb_get_window_attributes(g_connection, window), nullptr));
    if (attributes)
        g_window = window;
    else
        Log("Surface 0x%x is not an X11 window, reading the input of the focused one", window);
}

void Detach() {
    Disconnect();
    g_unavailable = false;
    g_window = XCB_NONE;
    g_focused = false;
    memset(g_keys, 0, sizeof(g_keys));
    memset(g_sent, 0, sizeof(g_sent));
    g_lastFrame = {};
}

// A grab held by the application cannot be released from another connection.
void OnMenuToggled(bool) {}

NativeWindow FindProcessWindow(uint32_t, uint32_t) {
    if (!Connect())
        return XCB_NONE;
    xcb_window_t focus = FocusOwner();
    return IsWindowId(focus) && OwnedByProcess(focus) ? focus : static_cast<xcb_window_t>(XCB_NONE);
}

void SetSurfaceSize(uint32_t width, uint32_t height) {
    g_width = width;
    g_height = height;
}

bool WindowSize(NativeWindow window, uint32_t& width, uint32_t& height) {
    if (!window || !Connect())
        return false;
    Reply<xcb_get_geometry_reply_t> geometry(xcb_get_geometry_reply(g_connection, xcb_get_geometry(g_connection, window), nullptr));
    if (!geometry)
        return false;
    width = geometry->width;
    height = geometry->height;
    return true;
}

void NewFrame() {
    ImGuiIO& io = ImGui::GetIO();
    auto now = std::chrono::steady_clock::now();
    float elapsed = std::chrono::duration<float>(now - g_lastFrame).count();
    io.DeltaTime = g_lastFrame.time_since_epoch().count() != 0 && elapsed > 0.0f ? elapsed : 1.0f / 60.0f;
    g_lastFrame = now;
    io.DisplaySize = ImVec2(static_cast<float>(g_width), static_cast<float>(g_height));

    if (!Connect())
        return;
    DrainEvents();
    UpdateFocus();
    xcb_window_t target = g_window != XCB_NONE ? g_window : (g_focusRelated ? g_focus : static_cast<xcb_window_t>(XCB_NONE));

    xcb_query_pointer_cookie_t pointerCookie = xcb_query_pointer(g_connection, target != XCB_NONE ? target : g_root);
    xcb_query_keymap_cookie_t keymapCookie = xcb_query_keymap(g_connection);
    Reply<xcb_query_pointer_reply_t> pointer(xcb_query_pointer_reply(g_connection, pointerCookie, nullptr));
    Reply<xcb_query_keymap_reply_t> keymap(xcb_query_keymap_reply(g_connection, keymapCookie, nullptr));
    if (!pointer || !keymap) {
        if (xcb_connection_has_error(g_connection))
            Disconnect();
        return;
    }
    memcpy(g_keys, keymap->keys, sizeof(g_keys));

    bool inside = target != XCB_NONE && pointer->same_screen && pointer->win_x >= 0 && pointer->win_y >= 0 &&
                  static_cast<uint32_t>(pointer->win_x) < g_width && static_cast<uint32_t>(pointer->win_y) < g_height;
    // Without a window manager the focus follows the pointer.
    bool focused = g_focus == XCB_INPUT_FOCUS_POINTER_ROOT ? inside : g_focusRelated;
    if (focused != g_focused) {
        g_focused = focused;
        io.AddFocusEvent(focused);
    }

    if (inside)
        io.AddMousePosEvent(static_cast<float>(pointer->win_x), static_cast<float>(pointer->win_y));
    else
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    uint16_t state = pointer->mask;
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, focused && (state & XCB_BUTTON_MASK_1) != 0);
    io.AddMouseButtonEvent(ImGuiMouseButton_Right, focused && (state & XCB_BUTTON_MASK_3) != 0);
    io.AddMouseButtonEvent(ImGuiMouseButton_Middle, focused && (state & XCB_BUTTON_MASK_2) != 0);
    io.AddKeyEvent(ImGuiMod_Ctrl, focused && (state & XCB_MOD_MASK_CONTROL) != 0);
    io.AddKeyEvent(ImGuiMod_Shift, focused && (state & XCB_MOD_MASK_SHIFT) != 0);
    io.AddKeyEvent(ImGuiMod_Alt, focused && (state & XCB_MOD_MASK_1) != 0);
    io.AddKeyEvent(ImGuiMod_Super, focused && (state & XCB_MOD_MASK_4) != 0);

    for (int keycode = g_minKeycode; keycode < kKeycodes; ++keycode) {
        bool down = focused && TestKey(g_keys, keycode);
        if (down == TestKey(g_sent, keycode))
            continue;
        g_sent[keycode >> 3] ^= static_cast<uint8_t>(1 << (keycode & 7));
        ImGuiKey key = ToImGuiKey(Keysym(keycode, 0));
        if (key != ImGuiKey_None)
            io.AddKeyEvent(key, down);
        if (down && !(state & (XCB_MOD_MASK_CONTROL | XCB_MOD_MASK_1)))
            if (unsigned int character = Character(keycode, state))
                io.AddInputCharacter(character);
    }
}

bool IsKeyDown(int keysym) {
    for (int keycode = g_minKeycode; keycode < kKeycodes; ++keycode)
        if (TestKey(g_keys, keycode) && (Keysym(keycode, 0) == static_cast<xcb_keysym_t>(keysym) || Keysym(keycode, 1) == static_cast<xcb_keysym_t>(keysym)))
            return true;
    return false;
}

bool IsFocused() { return g_focused; }

bool HasFocus(NativeWindow window) {
    if (!g_connection || window == XCB_NONE)
        return false;
    xcb_window_t focus = FocusOwner();
    return IsWindowId(focus) && (Encloses(window, focus) || Encloses(focus, window));
}

bool KeyDownInApplication(int keysym) {
    if (!Connect())
        return false;
    DrainEvents();
    Reply<xcb_query_keymap_reply_t> keymap(xcb_query_keymap_reply(g_connection, xcb_query_keymap(g_connection), nullptr));
    if (!keymap) {
        if (xcb_connection_has_error(g_connection))
            Disconnect();
        return false;
    }
    memcpy(g_keys, keymap->keys, sizeof(g_keys));
    if (!IsKeyDown(keysym))
        return false;
    xcb_window_t focus = FocusOwner();
    return focus == XCB_INPUT_FOCUS_POINTER_ROOT || (IsWindowId(focus) && OwnedByProcess(focus));
}

float Scale() { return g_scale; }

} // namespace uih::detail::input
