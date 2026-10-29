// Remote control on Linux: the protocol of src/remote.cpp on the Unix socket
// $XDG_RUNTIME_DIR/<Config::remotePipe>, which only the user can open, and
// the keys sent to the X server.
#include "internal.h"

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
void FakeInput(uint8_t type, uint8_t keycode) {
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
    request.detail = keycode;
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

void Serve() {
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

bool GameWindow(uint32_t& width, uint32_t& height, bool& focused) {
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

} // namespace uih::detail::remote
