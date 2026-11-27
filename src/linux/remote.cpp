// Remote control on Linux: the protocol of src/remote.cpp on the Unix socket
// $XDG_RUNTIME_DIR/<Config::remotePipe>, which only the user can open, and
// the keys sent to the X server, or to the listeners of a native Wayland
// application (wayland.cpp).
#include "internal.h"

#include <fcntl.h>
#include <link.h>
#include <sys/mman.h>

#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>
#include <xcb/xcb.h>
#include <xcb/xcbext.h>

#include <cctype>
#include <algorithm>
#include <cstdint>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>

namespace uih::detail::remote {
namespace {

constexpr size_t kMaxLine = 65536;
// A client that neither sends nor reads for this long is dropped: it would
// keep the socket from the next one.
constexpr int kIdleMs = 60000;
constexpr int kMaxTreeDepth = 32;

struct FreeDeleter {
    void operator()(void* reply) const { free(reply); }
};
template <typename T>
using Reply = std::unique_ptr<T, FreeDeleter>;

// Start() and Stop() run on the thread that starts and stops the hook,
// Listen() on the rendering thread.
std::mutex g_lifecycle;
std::atomic<bool> g_wanted{false};
// An array, not a string: Start() may run from a constructor of the library
// that preloads it, before the dynamic initialization of this file.
char g_path[sizeof(sockaddr_un::sun_path)] = {};
int g_listener = -1;
// Readable once Stop() was called: every wait of the server thread ends on it.
int g_stopEvent = -1;
// Never destroyed while it runs: a joinable std::thread would call
// std::terminate() in the static destructors when the game exits.
std::thread* g_thread = nullptr;

// Waits for `events` on `fd`; false on timeout or once Stop() was called.
bool WaitFor(int fd, short events, int timeoutMs) {
    pollfd fds[2] = {{fd, events, 0}, {g_stopEvent, POLLIN, 0}};
    return poll(fds, 2, timeoutMs) > 0 && fds[1].revents == 0;
}

// False when Stop() ended the wait.
bool Sleep(int ms) {
    pollfd stop{g_stopEvent, POLLIN, 0};
    return poll(&stop, 1, ms) == 0;
}

class Connection {
public:
    explicit Connection(int socket) : socket_(socket) {}
    ~Connection() { close(socket_); }
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    bool Write(const void* data, size_t size) {
        const auto* bytes = static_cast<const uint8_t*>(data);
        while (size > 0) {
            // MSG_NOSIGNAL: a client that left must not raise SIGPIPE in the game.
            ssize_t written = send(socket_, bytes, size, MSG_NOSIGNAL);
            if (written < 0 && errno == EAGAIN && WaitFor(socket_, POLLOUT, kIdleMs))
                continue;
            if (written <= 0)
                return false;
            bytes += written;
            size -= static_cast<size_t>(written);
        }
        return true;
    }

    bool ReadLine(std::string& line) {
        for (;;) {
            size_t end = buffer_.find('\n');
            if (end != std::string::npos) {
                line = buffer_.substr(0, end);
                buffer_.erase(0, end + 1);
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                return true;
            }
            if (buffer_.size() > kMaxLine || !WaitFor(socket_, POLLIN, kIdleMs))
                return false;
            char chunk[4096];
            ssize_t received = recv(socket_, chunk, sizeof(chunk), 0);
            if (received < 0 && errno == EAGAIN)
                continue;
            if (received <= 0)
                return false;
            buffer_.append(chunk, static_cast<size_t>(received));
        }
    }

private:
    int socket_;
    std::string buffer_;
};

// X server ----------------------------------------------------------------
// A connection of the server thread, apart from the one the overlay polls.

xcb_connection_t* g_x = nullptr;
xcb_window_t g_root = XCB_NONE;
uint8_t g_minKeycode = 0;
uint8_t g_keysymsPerKeycode = 0;
std::vector<xcb_keysym_t> g_keysyms;
xcb_atom_t g_pidAtom = XCB_NONE;
// Window of this process found by ProcessWindow().
xcb_window_t g_processWindow = XCB_NONE;
// XTEST, its requests sent without libxcb-xtest, which Debian and Ubuntu
// package apart and do not always install.
xcb_extension_t g_xtest = {"XTEST", 0};
bool g_hasXTest = false;

void DisconnectX() {
    if (g_x)
        xcb_disconnect(g_x);
    g_x = nullptr;
    g_keysyms.clear();
    g_processWindow = XCB_NONE;
}

void LoadKeyboardMapping() {
    const xcb_setup_t* setup = xcb_get_setup(g_x);
    xcb_get_keyboard_mapping_cookie_t cookie = xcb_get_keyboard_mapping(g_x, setup->min_keycode, setup->max_keycode - setup->min_keycode + 1);
    Reply<xcb_get_keyboard_mapping_reply_t> mapping(xcb_get_keyboard_mapping_reply(g_x, cookie, nullptr));
    if (!mapping)
        return;
    const xcb_keysym_t* keysyms = xcb_get_keyboard_mapping_keysyms(mapping.get());
    g_keysyms.assign(keysyms, keysyms + xcb_get_keyboard_mapping_keysyms_length(mapping.get()));
    g_minKeycode = setup->min_keycode;
    g_keysymsPerKeycode = mapping->keysyms_per_keycode;
}

// The X server tells every client when the keyboard layout changes. The
// events come in with the replies: called after a request was answered.
void DrainEvents() {
    bool remapped = false;
    while (xcb_generic_event_t* event = xcb_poll_for_event(g_x)) {
        remapped |= (event->response_type & 0x7f) == XCB_MAPPING_NOTIFY;
        free(event);
    }
    if (remapped)
        LoadKeyboardMapping();
}

bool ConnectX() {
    if (g_x && !xcb_connection_has_error(g_x))
        return true;
    DisconnectX();
    int screenIndex = 0;
    xcb_connection_t* connection = xcb_connect(nullptr, &screenIndex);
    if (xcb_connection_has_error(connection)) {
        xcb_disconnect(connection);
        return false;
    }
    const xcb_setup_t* setup = xcb_get_setup(connection);
    xcb_screen_iterator_t screen = xcb_setup_roots_iterator(setup);
    for (int i = 0; i < screenIndex && screen.rem > 1; ++i)
        xcb_screen_next(&screen);
    g_x = connection;
    g_root = screen.data->root;
    LoadKeyboardMapping();

    static const char kPid[] = "_NET_WM_PID";
    Reply<xcb_intern_atom_reply_t> atom(xcb_intern_atom_reply(connection, xcb_intern_atom(connection, 0, sizeof(kPid) - 1, kPid), nullptr));
    g_pidAtom = atom ? atom->atom : XCB_NONE;

    // Owned by the connection.
    const xcb_query_extension_reply_t* xtest = xcb_get_extension_data(connection, &g_xtest);
    g_hasXTest = xtest && xtest->present;
    return true;
}

uint8_t Keycode(int keysym) {
    for (size_t i = 0; g_keysymsPerKeycode && i < g_keysyms.size(); ++i)
        if (g_keysyms[i] == static_cast<xcb_keysym_t>(keysym) && i % g_keysymsPerKeycode < 2)
            return static_cast<uint8_t>(g_minKeycode + i / g_keysymsPerKeycode);
    return 0;
}

xcb_window_t Parent(xcb_window_t window) {
    Reply<xcb_query_tree_reply_t> tree(xcb_query_tree_reply(g_x, xcb_query_tree(g_x, window), nullptr));
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

bool Viewable(xcb_window_t window) {
    Reply<xcb_get_window_attributes_reply_t> attributes(xcb_get_window_attributes_reply(g_x, xcb_get_window_attributes(g_x, window), nullptr));
    return attributes && attributes->map_state == XCB_MAP_STATE_VIEWABLE;
}

// The first viewable window of this process with _NET_WM_PID, up to three
// levels under the root, as under the frames of a window manager.
xcb_window_t ProcessWindow() {
    if (g_processWindow != XCB_NONE && Viewable(g_processWindow))
        return g_processWindow;
    g_processWindow = XCB_NONE;
    std::vector<xcb_window_t> level{g_root};
    for (int depth = 0; depth < 3 && g_pidAtom != XCB_NONE && !level.empty(); ++depth) {
        std::vector<xcb_window_t> children;
        for (xcb_window_t parent : level) {
            Reply<xcb_query_tree_reply_t> tree(xcb_query_tree_reply(g_x, xcb_query_tree(g_x, parent), nullptr));
            if (tree)
                children.insert(children.end(), xcb_query_tree_children(tree.get()),
                                xcb_query_tree_children(tree.get()) + xcb_query_tree_children_length(tree.get()));
        }
        // One round trip for the properties of the whole level.
        std::vector<xcb_get_property_cookie_t> cookies;
        for (xcb_window_t window : children)
            cookies.push_back(xcb_get_property(g_x, 0, window, g_pidAtom, XCB_ATOM_CARDINAL, 0, 1));
        for (size_t i = 0; i < children.size(); ++i) {
            Reply<xcb_get_property_reply_t> property(xcb_get_property_reply(g_x, cookies[i], nullptr));
            uint32_t pid = 0;
            if (g_processWindow == XCB_NONE && property && property->format == 32 && xcb_get_property_value_length(property.get()) == sizeof(pid)) {
                memcpy(&pid, xcb_get_property_value(property.get()), sizeof(pid));
                if (pid == static_cast<uint32_t>(getpid()) && Viewable(children[i]))
                    g_processWindow = children[i];
            }
        }
        if (g_processWindow != XCB_NONE)
            return g_processWindow;
        level.swap(children);
    }
    return XCB_NONE;
}

// The X11 window the overlay draws on, with its size. The overlay does not
// know the window of an EGL surface: the window of this process then.
bool FindWindow(xcb_window_t& window, uint32_t& width, uint32_t& height) {
    if (!ConnectX())
        return false;
    for (xcb_window_t candidate : {static_cast<xcb_window_t>(BoundWindow()), ProcessWindow()}) {
        if (candidate == XCB_NONE)
            continue;
        Reply<xcb_get_geometry_reply_t> geometry(xcb_get_geometry_reply(g_x, xcb_get_geometry(g_x, candidate), nullptr));
        if (!geometry)
            continue;
        window = candidate;
        width = geometry->width;
        height = geometry->height;
        DrainEvents();
        return true;
    }
    return false;
}

// Keys typed now reach `window`: it has the focus, or the pointer is over it
// while the focus is PointerRoot, as without a window manager, or the root
// window, as some window managers leave it when no window is active.
bool HasKeyboard(xcb_window_t window, uint32_t width, uint32_t height) {
    Reply<xcb_get_input_focus_reply_t> focus(xcb_get_input_focus_reply(g_x, xcb_get_input_focus(g_x), nullptr));
    if (!focus || focus->focus == XCB_NONE)
        return false;
    if (focus->focus != XCB_INPUT_FOCUS_POINTER_ROOT && focus->focus != g_root)
        return Encloses(window, focus->focus) || Encloses(focus->focus, window);
    Reply<xcb_query_pointer_reply_t> pointer(xcb_query_pointer_reply(g_x, xcb_query_pointer(g_x, window), nullptr));
    return pointer && pointer->same_screen && pointer->win_x >= 0 && pointer->win_y >= 0 && static_cast<uint32_t>(pointer->win_x) < width &&
           static_cast<uint32_t>(pointer->win_y) < height;
}

// The FakeInput request of XTEST, as xcb_test_fake_input() sends it.
void FakeInput(uint8_t type, uint8_t detail, int16_t rootX = 0, int16_t rootY = 0) {
    struct {
        uint8_t majorOpcode;
        uint8_t minorOpcode;
        uint16_t length;
        uint8_t type;
        uint8_t detail;
        uint8_t pad0[2];
        uint32_t time;
        xcb_window_t root;
        uint8_t pad1[8];
        int16_t rootX;
        int16_t rootY;
        uint8_t pad2[7];
        uint8_t deviceid;
    } request{};
    static_assert(sizeof(request) == 36, "the FakeInput request is 36 bytes");
    request.type = type;
    request.detail = detail;
    request.root = type == XCB_MOTION_NOTIFY ? g_root : XCB_NONE;
    request.rootX = rootX;
    request.rootY = rootY;
    // xcb_send_request() fills the opcodes and the length, and uses the two
    // entries before the request.
    iovec parts[3] = {};
    parts[2].iov_base = &request;
    parts[2].iov_len = sizeof(request);
    xcb_protocol_request_t protocol = {1, &g_xtest, 2 /* FakeInput */, 1};
    xcb_send_request(g_x, 0, parts + 2, &protocol);
}

void FakeKeys(const std::vector<uint8_t>& keycodes, bool up) {
    for (size_t i = 0; i < keycodes.size(); ++i) {
        // Released in the reverse order.
        uint8_t keycode = up ? keycodes[keycodes.size() - 1 - i] : keycodes[i];
        FakeInput(up ? XCB_KEY_RELEASE : XCB_KEY_PRESS, keycode);
    }
    xcb_flush(g_x);
}

void SendKeys(xcb_window_t window, const std::vector<uint8_t>& keycodes, bool up) {
    for (size_t i = 0; i < keycodes.size(); ++i) {
        uint8_t keycode = up ? keycodes[keycodes.size() - 1 - i] : keycodes[i];
        xcb_key_press_event_t event{};
        event.response_type = up ? XCB_KEY_RELEASE : XCB_KEY_PRESS;
        event.detail = keycode;
        event.time = XCB_CURRENT_TIME;
        event.root = g_root;
        event.event = window;
        event.child = XCB_NONE;
        event.same_screen = 1;
        // Propagated to the closest ancestor that reads the keyboard, when the
        // overlay draws on a child of the game window.
        xcb_send_event(g_x, 1, window, up ? XCB_EVENT_MASK_KEY_RELEASE : XCB_EVENT_MASK_KEY_PRESS, reinterpret_cast<const char*>(&event));
    }
    xcb_flush(g_x);
}

// A move to x, y in `window` with the buttons of `state` held, as an event sent to the window.
void SendMotion(xcb_window_t window, int16_t x, int16_t y, uint16_t state) {
    xcb_motion_notify_event_t motion{};
    motion.response_type = XCB_MOTION_NOTIFY;
    motion.time = XCB_CURRENT_TIME;
    motion.root = g_root;
    motion.event = window;
    motion.event_x = x;
    motion.event_y = y;
    motion.state = state;
    motion.same_screen = 1;
    xcb_send_event(g_x, 1, window, XCB_EVENT_MASK_POINTER_MOTION, reinterpret_cast<const char*>(&motion));
}

// A move to x, y in `window`, then the press or the release of `button`, as
// events sent to the window.
void SendButton(xcb_window_t window, int16_t x, int16_t y, uint8_t button, bool up) {
    SendMotion(window, x, y, up ? static_cast<uint16_t>(XCB_BUTTON_MASK_1 << (button - 1)) : 0);
    xcb_button_press_event_t event{};
    event.response_type = up ? XCB_BUTTON_RELEASE : XCB_BUTTON_PRESS;
    event.detail = button;
    event.time = XCB_CURRENT_TIME;
    event.root = g_root;
    event.event = window;
    event.event_x = x;
    event.event_y = y;
    event.state = up ? static_cast<uint16_t>(XCB_BUTTON_MASK_1 << (button - 1)) : 0;
    event.same_screen = 1;
    xcb_send_event(g_x, 1, window, up ? XCB_EVENT_MASK_BUTTON_RELEASE : XCB_EVENT_MASK_BUTTON_PRESS, reinterpret_cast<const char*>(&event));
    xcb_flush(g_x);
}

// The press or the release of `keycode` with the modifiers of `state`, as an event sent to `window`.
void SendKey(xcb_window_t window, uint8_t keycode, uint16_t state, bool up) {
    xcb_key_press_event_t event{};
    event.response_type = up ? XCB_KEY_RELEASE : XCB_KEY_PRESS;
    event.detail = keycode;
    event.time = XCB_CURRENT_TIME;
    event.root = g_root;
    event.event = window;
    event.child = XCB_NONE;
    event.state = state;
    event.same_screen = 1;
    xcb_send_event(g_x, 1, window, up ? XCB_EVENT_MASK_KEY_RELEASE : XCB_EVENT_MASK_KEY_PRESS, reinterpret_cast<const char*>(&event));
}

// X11/keysymdef.h: Latin-1 keysyms are the code points, others 0x01000000 + the code point.
uint32_t KeysymOf(uint32_t point) {
    if (point == '\n')
        return 0xff0d;
    if (point == '\t')
        return 0xff09;
    if ((point >= 0x20 && point <= 0x7e) || (point >= 0xa0 && point <= 0xff))
        return point;
    return 0x01000000 | point;
}

// The keycode that types `keysym`, without or with Shift; 0 when the layout has none.
uint8_t KeycodeFor(uint32_t keysym, bool& shift) {
    for (size_t i = 0; g_keysymsPerKeycode && i < g_keysyms.size(); ++i) {
        if (g_keysyms[i] == keysym && i % g_keysymsPerKeycode < 2) {
            shift = i % g_keysymsPerKeycode == 1;
            return static_cast<uint8_t>(g_minKeycode + i / g_keysymsPerKeycode);
        }
    }
    return 0;
}

// A keycode without keysyms, for the characters the layout does not type.
uint8_t SpareKeycode() {
    for (size_t keycode = g_keysyms.size() / std::max<size_t>(1, g_keysymsPerKeycode); keycode-- > 0;) {
        bool empty = true;
        for (size_t i = 0; i < g_keysymsPerKeycode; ++i)
            empty = empty && g_keysyms[keycode * g_keysymsPerKeycode + i] == 0;
        if (empty)
            return static_cast<uint8_t>(g_minKeycode + keycode);
    }
    return 0;
}

// Gives `keycode` the keysym, or none, for every client of the X server.
void MapKeycode(uint8_t keycode, uint32_t keysym) {
    std::vector<xcb_keysym_t> keysyms(g_keysymsPerKeycode, keysym);
    xcb_change_keyboard_mapping(g_x, 1, keycode, g_keysymsPerKeycode, keysyms.data());
    size_t at = static_cast<size_t>(keycode - g_minKeycode) * g_keysymsPerKeycode;
    for (size_t i = 0; i < g_keysymsPerKeycode && at + i < g_keysyms.size(); ++i)
        g_keysyms[at + i] = keysym;
    xcb_flush(g_x);
}

void Serve() {
    timing::RealTime realTime;
    while (WaitFor(g_listener, POLLIN, -1)) {
        int client = accept4(g_listener, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (client < 0) {
            // Out of file descriptors, for instance: the socket stays readable.
            if (errno != EAGAIN && errno != ECONNABORTED && !Sleep(100))
                break;
            continue;
        }
        // The socket file is the user's alone, unless it sits in /tmp.
        ucred peer{};
        socklen_t size = sizeof(peer);
        if (getsockopt(client, SOL_SOCKET, SO_PEERCRED, &peer, &size) != 0 || peer.uid != getuid()) {
            close(client);
            continue;
        }
        Connection connection(client);
        std::string line;
        std::vector<uint8_t> payload;
        while (connection.ReadLine(line)) {
            std::string answer = Handle(line, payload) + "\n";
            bool sent = connection.Write(answer.data(), answer.size()) && connection.Write(payload.data(), payload.size());
            payload.clear();
            if (!sent)
                break;
        }
    }
    DisconnectX();
}

// Wayland -------------------------------------------------------------------
// The events go to the listeners of the application, which the overlay wraps:
// the menu sees them too, as it sees the input of XTEST.

constexpr unsigned kDeliveryMs = 1000;
const char kNotRead[] = "error the game does not read its Wayland events";
const char kNoPointer[] = "error the game has no Wayland pointer, or does not read its events";

bool OnWayland() { return wayland::Active() && BoundWindow() == 0; }

bool WaylandFocused() {
    uint32_t width = 0;
    uint32_t height = 0;
    bool focused = false;
    wayland::Window(width, height, focused);
    return focused;
}

std::string WaylandKeys(const std::vector<int>& keys, unsigned ms) {
    std::vector<uint32_t> codes;
    for (int keysym : keys) {
        uint32_t code = 0;
        bool shift = false;
        if (!wayland::KeyFor(keysym, code, shift))
            return "error the keyboard layout of the game has no key for one of these keys";
        codes.push_back(code);
    }
    bool focused = WaylandFocused();
    for (uint32_t code : codes)
        wayland::InjectKey(code, true);
    if (!wayland::WaitInjected(kDeliveryMs))
        return kNotRead;
    Sleep(static_cast<int>(ms));
    for (size_t i = codes.size(); i-- > 0;)
        wayland::InjectKey(codes[i], false);
    wayland::InjectEnd();
    if (!wayland::WaitInjected(kDeliveryMs))
        return kNotRead;
    return focused ? "ok sent as keyboard input, the game has the focus" : "ok sent as Wayland events, the game does not have the focus: it may ignore them";
}

bool WaylandMove(int x, int y) { return wayland::InjectMotion(x, y) && wayland::WaitInjected(kDeliveryMs); }

std::string WaylandClick(int x, int y, int button, unsigned ms) {
    bool focused = WaylandFocused();
    if (!WaylandMove(x, y))
        return kNoPointer;
    // Games read the pointer once per frame.
    Sleep(50);
    wayland::InjectButton(button, true);
    if (!wayland::WaitInjected(kDeliveryMs))
        return kNotRead;
    Sleep(static_cast<int>(ms));
    wayland::InjectButton(button, false);
    wayland::InjectEnd();
    if (!wayland::WaitInjected(kDeliveryMs))
        return kNotRead;
    return focused ? "ok sent as mouse input, the game has the focus" : "ok sent as Wayland events, the game does not have the focus";
}

std::string WaylandType(const std::vector<uint32_t>& text) {
    bool focused = WaylandFocused();
    size_t typed = 0;
    for (uint32_t point : text) {
        if (StopRequested())
            break;
        uint32_t code = 0;
        bool shift = false;
        if (!wayland::KeyFor(static_cast<int>(KeysymOf(point)), code, shift))
            continue;
        if (shift)
            wayland::InjectShift(true);
        wayland::InjectKey(code, true);
        wayland::InjectKey(code, false);
        if (shift)
            wayland::InjectShift(false);
        if (!wayland::WaitInjected(kDeliveryMs))
            return kNotRead;
        ++typed;
        // Games read text once per frame, some only the last character.
        Sleep(10);
    }
    wayland::InjectEnd();
    wayland::WaitInjected(kDeliveryMs);
    std::string count = std::to_string(typed);
    if (typed < text.size())
        return "ok typed " + count + " of " + std::to_string(text.size()) + " characters: the keyboard layout of the game has no key for the others";
    return focused ? "ok typed " + count + " characters as keyboard input, the game has the focus"
                   : "ok typed " + count + " characters as Wayland events, the game does not have the focus";
}

std::string WaylandMoveTo(int x, int y) {
    bool focused = WaylandFocused();
    bool moved = WaylandMove(x, y);
    // Games read the pointer once per frame.
    Sleep(50);
    wayland::InjectEnd();
    wayland::WaitInjected(kDeliveryMs);
    if (!moved)
        return kNoPointer;
    return focused ? "ok moved the pointer, the game has the focus" : "ok sent as a Wayland event, the game does not have the focus";
}

std::string WaylandDrag(int x0, int y0, int x1, int y1, int button, unsigned ms) {
    constexpr unsigned kStepMs = 16;
    bool focused = WaylandFocused();
    if (!WaylandMove(x0, y0))
        return kNoPointer;
    Sleep(50);
    wayland::InjectButton(button, true);
    unsigned steps = std::max(1u, ms / kStepMs);
    for (unsigned i = 1; i <= steps; ++i) {
        if (!wayland::WaitInjected(kDeliveryMs) || !Sleep(static_cast<int>(ms / steps)))
            break;
        int x = x0 + static_cast<int>((static_cast<int64_t>(x1 - x0) * i) / steps);
        int y = y0 + static_cast<int>((static_cast<int64_t>(y1 - y0) * i) / steps);
        wayland::InjectMotion(x, y);
    }
    wayland::WaitInjected(kDeliveryMs);
    Sleep(50);
    wayland::InjectButton(button, false);
    wayland::InjectEnd();
    if (!wayland::WaitInjected(kDeliveryMs))
        return kNotRead;
    return focused ? "ok sent as mouse input, the game has the focus" : "ok sent as Wayland events, the game does not have the focus";
}

bool Served(const sockaddr_un& address) {
    int probe = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (probe < 0)
        return false;
    bool served = connect(probe, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0 || errno == EAGAIN;
    close(probe);
    return served;
}

} // namespace

int KeyCode(const std::string& name) {
    struct NamedKey {
        const char* name;
        int keysym;
    };
    // X11/keysymdef.h
    static const NamedKey kKeys[] = {
        {"space", 0x0020},  {"enter", 0xff0d},  {"escape", 0xff1b}, {"tab", 0xff09},    {"backspace", 0xff08}, {"up", 0xff52},
        {"down", 0xff54},   {"left", 0xff51},   {"right", 0xff53},  {"shift", 0xffe1},  {"ctrl", 0xffe3},      {"alt", 0xffe9},
        {"insert", 0xff63}, {"delete", 0xffff}, {"home", 0xff50},   {"end", 0xff57},    {"pageup", 0xff55},    {"pagedown", 0xff56},
    };
    if (name.size() == 1 && isalnum(static_cast<unsigned char>(name[0])))
        return static_cast<unsigned char>(name[0]);
    if (name.size() >= 2 && name[0] == 'f' && isdigit(static_cast<unsigned char>(name[1]))) {
        int number = atoi(name.c_str() + 1);
        if (number >= 1 && number <= 12)
            return 0xffbe + number - 1;
    }
    for (const NamedKey& key : kKeys)
        if (name == key.name)
            return key.keysym;
    return 0;
}

// XTEST when the game has the keyboard, as real key presses. Otherwise, as
// when the player types to the assistant in another window, the keys go to
// the game window as events, which games that poll the keyboard state do not
// see. Stop() releases the keys at once.
std::string HoldKeys(const std::vector<int>& keys, unsigned ms) {
    if (OnWayland())
        return WaylandKeys(keys, ms);
    xcb_window_t window = XCB_NONE;
    uint32_t width = 0;
    uint32_t height = 0;
    if (!FindWindow(window, width, height))
        return "error the game has no X11 window: keys reach X11 and XWayland games only";
    std::vector<uint8_t> keycodes;
    for (int keysym : keys) {
        uint8_t keycode = Keycode(keysym);
        if (!keycode)
            return "error the keyboard layout of the X server has no key for one of these keys";
        keycodes.push_back(keycode);
    }
    if (g_hasXTest && HasKeyboard(window, width, height)) {
        FakeKeys(keycodes, false);
        Sleep(static_cast<int>(ms));
        FakeKeys(keycodes, true);
        return "ok sent as keyboard input, the game has the focus";
    }
    SendKeys(window, keycodes, false);
    Sleep(static_cast<int>(ms));
    SendKeys(window, keycodes, true);
    return g_hasXTest ? "ok sent as window events, the game does not have the focus: games that read the keyboard state directly do not see them"
                      : "ok sent as window events, the X server lacks XTEST: games that read the keyboard state directly do not see them";
}

// XTEST when the game has the keyboard: the pointer moves there and the
// button is pressed, which the overlay menu sees too. Otherwise events sent to
// the game window, which the menu does not read.
std::string ClickAt(int x, int y, int button, unsigned ms) {
    if (OnWayland())
        return WaylandClick(x, y, button, ms);
    xcb_window_t window = XCB_NONE;
    uint32_t width = 0;
    uint32_t height = 0;
    if (!FindWindow(window, width, height))
        return "error the game has no X11 window: clicks reach X11 and XWayland games only";
    uint8_t detail = static_cast<uint8_t>(button == 2 ? 2 : button == 3 ? 3 : 1);
    if (g_hasXTest && HasKeyboard(window, width, height)) {
        Reply<xcb_translate_coordinates_reply_t> root(
            xcb_translate_coordinates_reply(g_x, xcb_translate_coordinates(g_x, window, g_root, static_cast<int16_t>(x), static_cast<int16_t>(y)), nullptr));
        if (root) {
            FakeInput(XCB_MOTION_NOTIFY, 0, root->dst_x, root->dst_y);
            xcb_flush(g_x);
            // Games read the pointer once per frame.
            Sleep(50);
            FakeInput(XCB_BUTTON_PRESS, detail);
            xcb_flush(g_x);
            Sleep(static_cast<int>(ms));
            FakeInput(XCB_BUTTON_RELEASE, detail);
            xcb_flush(g_x);
            return "ok sent as mouse input, the game has the focus";
        }
    }
    SendButton(window, static_cast<int16_t>(x), static_cast<int16_t>(y), detail, false);
    Sleep(static_cast<int>(ms));
    SendButton(window, static_cast<int16_t>(x), static_cast<int16_t>(y), detail, true);
    return g_hasXTest ? "ok sent as window events, the game does not have the focus: the overlay menu and games that read the mouse directly do not see them"
                      : "ok sent as window events, the X server lacks XTEST: the overlay menu and games that read the mouse directly do not see them";
}

// XTEST when the game has the keyboard, events sent to its window otherwise.
// A character the layout lacks goes to a free keycode for the time of the
// text, as xdotool does: the other clients of the X server see the change.
std::string TypeText(const std::vector<uint32_t>& text) {
    if (OnWayland())
        return WaylandType(text);
    xcb_window_t window = XCB_NONE;
    uint32_t width = 0;
    uint32_t height = 0;
    if (!FindWindow(window, width, height))
        return "error the game has no X11 window: text reaches X11 and XWayland games only";
    bool fake = g_hasXTest && HasKeyboard(window, width, height);
    uint8_t shiftKeycode = Keycode(0xffe1);
    uint8_t spare = 0;
    uint32_t spareKeysym = 0;
    size_t typed = 0;
    for (uint32_t point : text) {
        if (StopRequested())
            break;
        uint32_t keysym = KeysymOf(point);
        bool shift = false;
        uint8_t keycode = KeycodeFor(keysym, shift);
        if (!keycode) {
            if (!spare && !(spare = SpareKeycode()))
                continue;
            if (spareKeysym != keysym) {
                MapKeycode(spare, keysym);
                spareKeysym = keysym;
                // The clients read the new mapping before the key.
                Sleep(20);
            }
            keycode = spare;
            shift = false;
        }
        if (fake) {
            if (shift && shiftKeycode)
                FakeInput(XCB_KEY_PRESS, shiftKeycode);
            FakeInput(XCB_KEY_PRESS, keycode);
            FakeInput(XCB_KEY_RELEASE, keycode);
            if (shift && shiftKeycode)
                FakeInput(XCB_KEY_RELEASE, shiftKeycode);
        } else {
            uint16_t state = shift ? XCB_MOD_MASK_SHIFT : 0;
            SendKey(window, keycode, state, false);
            SendKey(window, keycode, state, true);
        }
        xcb_flush(g_x);
        ++typed;
        // Games read text once per frame, some only the last character.
        Sleep(10);
    }
    if (spare)
        MapKeycode(spare, 0);
    DrainEvents();
    std::string count = std::to_string(typed);
    if (typed < text.size())
        return "ok typed " + count + " of " + std::to_string(text.size()) + " characters: the keyboard layout has no free keycode for the others";
    return fake ? "ok typed " + count + " characters as keyboard input, the game has the focus"
                : "ok typed " + count + " characters as window events, the game does not have the keyboard: games that read the keyboard state directly do not see them";
}

namespace {

// Root coordinates of x, y in `window`.
bool ToRoot(xcb_window_t window, int x, int y, int16_t& rootX, int16_t& rootY) {
    Reply<xcb_translate_coordinates_reply_t> root(
        xcb_translate_coordinates_reply(g_x, xcb_translate_coordinates(g_x, window, g_root, static_cast<int16_t>(x), static_cast<int16_t>(y)), nullptr));
    if (!root)
        return false;
    rootX = root->dst_x;
    rootY = root->dst_y;
    return true;
}

} // namespace

std::string MoveTo(int x, int y) {
    if (OnWayland())
        return WaylandMoveTo(x, y);
    xcb_window_t window = XCB_NONE;
    uint32_t width = 0;
    uint32_t height = 0;
    if (!FindWindow(window, width, height))
        return "error the game has no X11 window: the pointer moves in X11 and XWayland games only";
    int16_t rootX = 0;
    int16_t rootY = 0;
    if (g_hasXTest && HasKeyboard(window, width, height) && ToRoot(window, x, y, rootX, rootY)) {
        FakeInput(XCB_MOTION_NOTIFY, 0, rootX, rootY);
        xcb_flush(g_x);
        // Games read the pointer once per frame.
        Sleep(50);
        return "ok moved the pointer, the game has the focus";
    }
    SendMotion(window, static_cast<int16_t>(x), static_cast<int16_t>(y), 0);
    xcb_flush(g_x);
    Sleep(50);
    return "ok sent as a window event, the game does not have the focus: the overlay menu and games that read the mouse directly do not see it";
}

std::string DragTo(int x0, int y0, int x1, int y1, int button, unsigned ms) {
    if (OnWayland())
        return WaylandDrag(x0, y0, x1, y1, button, ms);
    constexpr unsigned kStepMs = 16;
    xcb_window_t window = XCB_NONE;
    uint32_t width = 0;
    uint32_t height = 0;
    if (!FindWindow(window, width, height))
        return "error the game has no X11 window: drags reach X11 and XWayland games only";
    uint8_t detail = static_cast<uint8_t>(button == 2 ? 2 : button == 3 ? 3 : 1);
    auto held = static_cast<uint16_t>(XCB_BUTTON_MASK_1 << (detail - 1));
    unsigned steps = std::max(1u, ms / kStepMs);
    int16_t rootX = 0;
    int16_t rootY = 0;
    bool fake = g_hasXTest && HasKeyboard(window, width, height) && ToRoot(window, x0, y0, rootX, rootY);
    if (fake) {
        FakeInput(XCB_MOTION_NOTIFY, 0, rootX, rootY);
        xcb_flush(g_x);
        Sleep(50);
        FakeInput(XCB_BUTTON_PRESS, detail);
    } else {
        SendButton(window, static_cast<int16_t>(x0), static_cast<int16_t>(y0), detail, false);
    }
    xcb_flush(g_x);
    for (unsigned i = 1; i <= steps; ++i) {
        if (!Sleep(static_cast<int>(ms / steps)))
            break;
        int x = x0 + static_cast<int>((static_cast<int64_t>(x1 - x0) * i) / steps);
        int y = y0 + static_cast<int>((static_cast<int64_t>(y1 - y0) * i) / steps);
        if (fake && ToRoot(window, x, y, rootX, rootY))
            FakeInput(XCB_MOTION_NOTIFY, 0, rootX, rootY);
        else if (!fake)
            SendMotion(window, static_cast<int16_t>(x), static_cast<int16_t>(y), held);
        xcb_flush(g_x);
    }
    Sleep(50);
    if (fake) {
        FakeInput(XCB_BUTTON_RELEASE, detail);
        xcb_flush(g_x);
        return "ok sent as mouse input, the game has the focus";
    }
    SendButton(window, static_cast<int16_t>(x1), static_cast<int16_t>(y1), detail, true);
    return "ok sent as window events, the game does not have the focus: the overlay menu and games that read the mouse directly do not see them";
}

bool GameWindow(uint32_t& width, uint32_t& height, bool& focused) {
    if (OnWayland())
        return wayland::Window(width, height, focused);
    xcb_window_t window = XCB_NONE;
    if (!FindWindow(window, width, height))
        return false;
    focused = HasKeyboard(window, width, height);
    return true;
}

// Only records the name: every process the game starts preloads the library,
// such as the shell of a launch script, and the socket belongs to the one
// that draws the overlay (Listen()).
void Start(const char* name) {
    std::lock_guard<std::mutex> lock(g_lifecycle);
    if (g_wanted || g_thread)
        return;
    const char* file = name ? name : "";
    const char* runtime = getenv("XDG_RUNTIME_DIR");
    int length = runtime && *runtime ? snprintf(g_path, sizeof(g_path), "%s/%s", runtime, file)
                                     : snprintf(g_path, sizeof(g_path), "/tmp/%s-%u", file, static_cast<unsigned>(getuid()));
    if (!*file || strchr(file, '/') || length < 0 || static_cast<size_t>(length) >= sizeof(g_path)) {
        g_path[0] = '\0';
        Log("Remote control off: the socket name must be short and without slashes");
        return;
    }
    g_wanted = true;
}

void Listen() {
    if (!g_wanted.load(std::memory_order_relaxed))
        return;
    std::lock_guard<std::mutex> lock(g_lifecycle);
    if (!g_wanted)
        return;
    g_wanted = false;
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, g_path, sizeof(g_path));
    if (Served(address)) {
        Log("Remote control unavailable: another process serves %s", g_path);
        return;
    }
    // Left by a game that ended without Stop().
    unlink(g_path);
    g_listener = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (g_listener < 0 || bind(g_listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 || chmod(g_path, 0600) != 0 ||
        listen(g_listener, 1) != 0) {
        Log("Remote control unavailable: socket %s cannot be created (%s)", g_path, strerror(errno));
        if (g_listener >= 0)
            close(g_listener);
        g_listener = -1;
        return;
    }
    g_stopEvent = eventfd(0, EFD_CLOEXEC);
    // The signals of the game go to its own threads.
    sigset_t all;
    sigset_t previous;
    sigfillset(&all);
    pthread_sigmask(SIG_SETMASK, &all, &previous);
    try {
        if (g_stopEvent >= 0)
            g_thread = new std::thread(Serve);
    } catch (const std::exception&) {
        // Thrown into the present of the game, it would end the game.
    }
    pthread_sigmask(SIG_SETMASK, &previous, nullptr);
    if (!g_thread) {
        Log("Remote control off: its thread cannot start");
        if (g_stopEvent >= 0)
            close(g_stopEvent);
        g_stopEvent = -1;
        close(g_listener);
        unlink(g_path);
        g_listener = -1;
        return;
    }
    Log("Remote control on %s", g_path);
}

void Stop() {
    std::lock_guard<std::mutex> lock(g_lifecycle);
    g_wanted = false;
    if (!g_thread)
        return;
    uint64_t one = 1;
    if (write(g_stopEvent, &one, sizeof(one)) != sizeof(one))
        Log("Remote control: the stop event cannot be set");
    g_thread->join();
    delete g_thread;
    g_thread = nullptr;
    close(g_listener);
    close(g_stopEvent);
    unlink(g_path);
    g_listener = -1;
    g_stopEvent = -1;
    ClearText();
}


bool StopRequested() {
    pollfd stop{g_stopEvent, POLLIN, 0};
    return g_stopEvent >= 0 && poll(&stop, 1, 0) > 0;
}

namespace {

// Anonymous mappings can be huge and nearly empty, as the shadow memory of
// AddressSanitizer or the reservations of allocators and JIT compilers: only
// their resident pages are listed, the others hold zeros or are swapped out.
void AddResidentRuns(std::vector<MemoryRegion>& regions, uintptr_t start, uintptr_t end, bool writable) {
    constexpr uintptr_t kWindow = 64 << 20;
    const uintptr_t page = static_cast<uintptr_t>(sysconf(_SC_PAGESIZE));
    std::vector<unsigned char> resident(kWindow / page);
    uintptr_t runStart = 0;
    bool inRun = false;
    for (uintptr_t window = start; window < end; window += kWindow) {
        uintptr_t length = std::min(kWindow, end - window);
        size_t pages = static_cast<size_t>((length + page - 1) / page);
        if (mincore(reinterpret_cast<void*>(window), length, resident.data()) != 0)
            std::fill(resident.begin(), resident.begin() + static_cast<std::ptrdiff_t>(pages), 0);
        for (size_t i = 0; i < pages; ++i) {
            bool in = resident[i] & 1;
            if (in == inRun)
                continue;
            uintptr_t address = window + i * page;
            if (in)
                runStart = address;
            else
                regions.push_back({runStart, address - runStart, writable});
            inRun = in;
        }
    }
    if (inRun)
        regions.push_back({runStart, end - runStart, writable});
}

} // namespace

std::vector<MemoryRegion> MemoryRegions() {
    constexpr unsigned long kLargeAnonymous = 64ul << 20;
    std::vector<MemoryRegion> regions;
    FILE* maps = fopen("/proc/self/maps", "re");
    if (!maps)
        return regions;
    char line[4096];
    while (fgets(line, sizeof(line), maps)) {
        unsigned long start = 0;
        unsigned long end = 0;
        char permissions[8] = {};
        int pathAt = 0;
        if (sscanf(line, "%lx-%lx %7s %*s %*s %*s %n", &start, &end, permissions, &pathAt) < 3)
            continue;
        const char* path = line + pathAt;
        // Device memory, such as the GPU's, and the kernel's pages.
        if (permissions[0] != 'r' || strncmp(path, "/dev/", 5) == 0 || strncmp(path, "[vvar", 5) == 0 || strncmp(path, "[vsyscall]", 10) == 0)
            continue;
        bool anonymous = path[0] == '\0' || path[0] == '\n' || path[0] == '[';
        if (anonymous && end - start > kLargeAnonymous)
            AddResidentRuns(regions, start, end, permissions[1] == 'w');
        else
            regions.push_back({start, end - start, permissions[1] == 'w'});
    }
    fclose(maps);
    return regions;
}

std::vector<LoadedModule> LoadedModules() {
    std::vector<LoadedModule> modules;
    dl_iterate_phdr(
        [](dl_phdr_info* info, size_t, void* context) {
            uintptr_t low = UINTPTR_MAX;
            uintptr_t high = 0;
            for (int i = 0; i < info->dlpi_phnum; ++i) {
                const auto& header = info->dlpi_phdr[i];
                if (header.p_type != PT_LOAD)
                    continue;
                low = std::min<uintptr_t>(low, info->dlpi_addr + header.p_vaddr);
                high = std::max<uintptr_t>(high, info->dlpi_addr + header.p_vaddr + header.p_memsz);
            }
            if (low >= high)
                return 0;
            auto* list = static_cast<std::vector<LoadedModule>*>(context);
            // The executable comes first, without a name.
            bool executable = list->empty() && (!info->dlpi_name || !info->dlpi_name[0]);
            std::string path = executable ? "" : (info->dlpi_name ? info->dlpi_name : "");
            if (executable) {
                char buffer[4096];
                ssize_t length = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
                path.assign(buffer, length > 0 ? static_cast<size_t>(length) : 0);
            }
            size_t slash = path.rfind('/');
            list->push_back({slash == std::string::npos ? path : path.substr(slash + 1), low, high - low, executable});
            return 0;
        },
        &modules);
    return modules;
}

bool ReadMemory(uintptr_t address, void* out, size_t size) {
    iovec local{out, size};
    iovec remote{reinterpret_cast<void*>(address), size};
    return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == static_cast<ssize_t>(size);
}

// /proc/self/mem writes read-only pages too, such as code.
bool WriteMemory(uintptr_t address, const void* data, size_t size) {
    int memory = open("/proc/self/mem", O_RDWR | O_CLOEXEC);
    bool written = memory >= 0 && pwrite(memory, data, size, static_cast<off_t>(address)) == static_cast<ssize_t>(size);
    if (memory >= 0)
        close(memory);
    if (written)
        return true;
    iovec local{const_cast<void*>(data), size};
    iovec remote{reinterpret_cast<void*>(address), size};
    return process_vm_writev(getpid(), &local, 1, &remote, 1, 0) == static_cast<ssize_t>(size);
}

void* AllocateScanMemory(size_t size) {
    void* memory = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return memory == MAP_FAILED ? nullptr : memory;
}

} // namespace uih::detail::remote
