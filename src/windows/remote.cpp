// Remote control on Windows: the protocol of src/remote.cpp on the named pipe
// \\.\pipe\<Config::remotePipe>, which only local clients can open, and the
// keys sent with SendInput or as window messages.
#include "internal.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace uih::detail::remote {
namespace {

constexpr size_t kMaxLine = 65536;
// A client that neither sends nor reads for this long is dropped: it would
// keep the pipe from the next one.
constexpr DWORD kIdleMs = 60000;

HANDLE g_thread = nullptr;
// Set by Stop(): every wait of the server thread ends on it.
HANDLE g_stopEvent = nullptr;
std::wstring g_pipeName;

bool Stopping() { return WaitForSingleObject(g_stopEvent, 0) == WAIT_OBJECT_0; }

// Overlapped I/O on the pipe, cancelled by Stop().
class Connection {
public:
    explicit Connection(HANDLE pipe) : pipe_(pipe), event_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
    ~Connection() {
        if (event_)
            CloseHandle(event_);
    }
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    bool Accept() {
        OVERLAPPED overlapped = Begin();
        if (ConnectNamedPipe(pipe_, &overlapped))
            return true;
        DWORD error = GetLastError();
        DWORD bytes = 0;
        return error == ERROR_PIPE_CONNECTED || (error == ERROR_IO_PENDING && Finish(overlapped, bytes, INFINITE));
    }

    bool Write(const void* data, size_t size) {
        const auto* bytes = static_cast<const uint8_t*>(data);
        while (size > 0) {
            OVERLAPPED overlapped = Begin();
            DWORD written = 0;
            DWORD chunk = static_cast<DWORD>(std::min<size_t>(size, 1 << 20));
            if ((!WriteFile(pipe_, bytes, chunk, nullptr, &overlapped) && GetLastError() != ERROR_IO_PENDING) || !Finish(overlapped, written, kIdleMs) ||
                written == 0)
                return false;
            bytes += written;
            size -= written;
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
            if (buffer_.size() > kMaxLine)
                return false;
            char chunk[4096];
            OVERLAPPED overlapped = Begin();
            DWORD read = 0;
            if ((!ReadFile(pipe_, chunk, sizeof(chunk), nullptr, &overlapped) && GetLastError() != ERROR_IO_PENDING) || !Finish(overlapped, read, kIdleMs) ||
                read == 0)
                return false;
            buffer_.append(chunk, read);
        }
    }

private:
    OVERLAPPED Begin() {
        OVERLAPPED overlapped{};
        ResetEvent(event_);
        overlapped.hEvent = event_;
        return overlapped;
    }

    bool Finish(OVERLAPPED& overlapped, DWORD& bytes, DWORD timeoutMs) {
        HANDLE events[2] = {event_, g_stopEvent};
        if (WaitForMultipleObjects(2, events, FALSE, timeoutMs) != WAIT_OBJECT_0) {
            CancelIoEx(pipe_, &overlapped);
            GetOverlappedResult(pipe_, &overlapped, &bytes, TRUE);
            return false;
        }
        return GetOverlappedResult(pipe_, &overlapped, &bytes, FALSE) != FALSE;
    }

    HANDLE pipe_;
    HANDLE event_;
    std::string buffer_;
};

struct NamedKey {
    const char* name;
    WORD vk;
};

constexpr NamedKey kKeys[] = {
    {"space", VK_SPACE},  {"enter", VK_RETURN}, {"escape", VK_ESCAPE}, {"tab", VK_TAB},       {"backspace", VK_BACK}, {"up", VK_UP},
    {"down", VK_DOWN},    {"left", VK_LEFT},    {"right", VK_RIGHT},   {"shift", VK_SHIFT},   {"ctrl", VK_CONTROL},   {"alt", VK_MENU},
    {"insert", VK_INSERT}, {"delete", VK_DELETE}, {"home", VK_HOME},  {"end", VK_END},       {"pageup", VK_PRIOR},   {"pagedown", VK_NEXT},
};

bool Extended(WORD vk) {
    switch (vk) {
    case VK_UP:
    case VK_DOWN:
    case VK_LEFT:
    case VK_RIGHT:
    case VK_INSERT:
    case VK_DELETE:
    case VK_HOME:
    case VK_END:
    case VK_PRIOR:
    case VK_NEXT:
        return true;
    default:
        return false;
    }
}

void SendKeys(const std::vector<int>& keys, bool up) {
    std::vector<INPUT> inputs(keys.size());
    for (size_t i = 0; i < keys.size(); ++i) {
        // Released in the reverse order.
        WORD vk = static_cast<WORD>(up ? keys[keys.size() - 1 - i] : keys[i]);
        inputs[i].type = INPUT_KEYBOARD;
        inputs[i].ki.wVk = vk;
        inputs[i].ki.wScan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
        inputs[i].ki.dwFlags = (Extended(vk) ? KEYEVENTF_EXTENDEDKEY : 0) | (up ? KEYEVENTF_KEYUP : 0);
    }
    SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
}

void PostKeys(HWND window, const std::vector<int>& keys, bool repeat, bool up) {
    for (size_t i = 0; i < keys.size(); ++i) {
        WORD vk = static_cast<WORD>(up ? keys[keys.size() - 1 - i] : keys[i]);
        bool system = vk == VK_MENU;
        LPARAM flags = 1 | static_cast<LPARAM>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC)) << 16 | (Extended(vk) ? 1 << 24 : 0) |
                       (repeat || up ? 1 << 30 : 0) | (up ? static_cast<LPARAM>(1u << 31) : 0);
        UINT message = up ? (system ? WM_SYSKEYUP : WM_KEYUP) : (system ? WM_SYSKEYDOWN : WM_KEYDOWN);
        PostMessageW(window, message, vk, flags);
    }
}

DWORD WINAPI Serve(void*) {
    while (!Stopping()) {
        HANDLE pipe = CreateNamedPipeW(g_pipeName.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                       PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 1 << 20, 4096, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            Log("Remote control unavailable: pipe %ls cannot be created (error %lu), another process may serve it", g_pipeName.c_str(), GetLastError());
            return 0;
        }
        {
            Connection connection(pipe);
            std::string line;
            std::vector<uint8_t> payload;
            if (connection.Accept()) {
                while (connection.ReadLine(line)) {
                    std::string answer = Handle(line, payload) + "\n";
                    bool sent = connection.Write(answer.data(), answer.size()) && connection.Write(payload.data(), payload.size());
                    payload.clear();
                    if (!sent)
                        break;
                }
            }
        }
        // Disconnecting would drop the answer the client has not read yet,
        // such as the one to the unload key that stopped the hook.
        if (!Stopping())
            DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
    return 0;
}

} // namespace

int KeyCode(const std::string& name) {
    if (name.size() == 1 && isalnum(static_cast<unsigned char>(name[0])))
        return toupper(static_cast<unsigned char>(name[0]));
    if (name.size() >= 2 && name[0] == 'f' && isdigit(static_cast<unsigned char>(name[1]))) {
        int number = atoi(name.c_str() + 1);
        if (number >= 1 && number <= 12)
            return VK_F1 + number - 1;
    }
    for (const NamedKey& key : kKeys)
        if (name == key.name)
            return key.vk;
    return 0;
}

// SendInput when the game has the focus. Otherwise, as when the player types
// to the assistant in another window, the keys go to the game window as
// messages, which games that poll the keyboard state do not see. Stop()
// releases the keys at once.
std::string HoldKeys(const std::vector<int>& keys, unsigned ms) {
    HWND window = input::FindProcessWindow();
    if (!window)
        return "error the game has no visible window";
    if (GetForegroundWindow() == window) {
        SendKeys(keys, false);
        WaitForSingleObject(g_stopEvent, ms);
        SendKeys(keys, true);
        return "ok sent as keyboard input, the game has the focus";
    }
    PostKeys(window, keys, false, false);
    // The keyboard repeats held keys.
    for (DWORD held = 0; held + 50 < ms && WaitForSingleObject(g_stopEvent, 50) == WAIT_TIMEOUT; held += 50)
        PostKeys(window, keys, true, false);
    WaitForSingleObject(g_stopEvent, ms % 50);
    PostKeys(window, keys, false, true);
    return "ok sent as window messages, the game does not have the focus: games that read the keyboard state directly do not see them";
}

bool GameWindow(uint32_t& width, uint32_t& height, bool& focused) {
    HWND window = input::FindProcessWindow();
    RECT client{};
    if (!window || !GetClientRect(window, &client))
        return false;
    width = static_cast<uint32_t>(client.right);
    height = static_cast<uint32_t>(client.bottom);
    focused = GetForegroundWindow() == window;
    return true;
}

void Start(const char* pipe) {
    if (g_thread)
        return;
    std::string name = pipe ? pipe : "";
    if (name.empty() || name.size() > 200 || name.find('\\') != std::string::npos) {
        Log("Remote control off: the pipe name must have 1 to 200 characters, without backslashes");
        return;
    }
    int length = MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, nullptr, 0);
    std::wstring wide(static_cast<size_t>(std::max(length, 1)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, wide.data(), length);
    wide.resize(wcslen(wide.c_str()));
    g_pipeName = L"\\\\.\\pipe\\" + wide;
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_thread = g_stopEvent ? CreateThread(nullptr, 0, Serve, nullptr, 0, nullptr) : nullptr;
    if (!g_thread) {
        Log("Remote control off: its thread cannot start");
        if (g_stopEvent)
            CloseHandle(g_stopEvent);
        g_stopEvent = nullptr;
        return;
    }
    Log("Remote control on %ls", g_pipeName.c_str());
}

void Stop() {
    if (!g_thread)
        return;
    SetEvent(g_stopEvent);
    WaitForSingleObject(g_thread, INFINITE);
    CloseHandle(g_thread);
    CloseHandle(g_stopEvent);
    g_thread = nullptr;
    g_stopEvent = nullptr;
    ClearText();
}

} // namespace uih::detail::remote
