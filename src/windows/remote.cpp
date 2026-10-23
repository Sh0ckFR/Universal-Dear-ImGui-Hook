// Remote control of the game by an AI assistant: remote/uih_mcp.py turns these
// commands into MCP tools, so that the assistant sees the game, writes in it
// and presses its keys. Served when Config::remoteControl is set.
//
// One request per line on \\.\pipe\<Config::remotePipe>, local clients only:
//   info                       -> ok <backend> <width>x<height> focused=0|1 menu=0|1
//   screenshot <max size>      -> ok <width> <height>, then width * height RGB bytes
//   text <seconds> <message>   -> ok         (\n in the message starts a new line)
//   key <keys> <milliseconds>  -> ok <how the keys were sent>  (keys: space, or left+space held together)
// Failures answer: error <reason>
#include "internal.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace uih::detail::remote {
namespace {

constexpr DWORD kMaxHoldMs = 10000;
constexpr size_t kMaxKeys = 4;
constexpr size_t kMaxLine = 65536;
constexpr uint32_t kCaptureTimeoutMs = 2000;
// A client that neither sends nor reads for this long is dropped: it would
// keep the pipe from the next one.
constexpr DWORD kIdleMs = 60000;

HANDLE g_thread = nullptr;
// Set by Stop(): every wait of the server thread ends on it.
HANDLE g_stopEvent = nullptr;
std::wstring g_pipeName;

std::mutex g_textMutex;
std::string g_text;
ULONGLONG g_textUntil = 0;

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

    bool Answer(const std::string& line) { return Write((line + "\n").data(), line.size() + 1); }

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

// Averages the pixels of `rgba` into RGB at most `maxSide` pixels wide and high.
std::vector<uint8_t> Shrink(const std::vector<uint8_t>& rgba, uint32_t width, uint32_t height, uint32_t maxSide, uint32_t& outWidth,
                            uint32_t& outHeight) {
    double scale = std::min(1.0, static_cast<double>(maxSide) / std::max(width, height));
    outWidth = std::max<uint32_t>(1, static_cast<uint32_t>(width * scale + 0.5));
    outHeight = std::max<uint32_t>(1, static_cast<uint32_t>(height * scale + 0.5));
    std::vector<uint8_t> rgb(static_cast<size_t>(outWidth) * outHeight * 3);
    uint8_t* out = rgb.data();
    for (uint32_t oy = 0; oy < outHeight; ++oy) {
        uint32_t y0 = oy * height / outHeight;
        uint32_t y1 = std::max(y0 + 1, (oy + 1) * height / outHeight);
        for (uint32_t ox = 0; ox < outWidth; ++ox, out += 3) {
            uint32_t x0 = ox * width / outWidth;
            uint32_t x1 = std::max(x0 + 1, (ox + 1) * width / outWidth);
            uint32_t sum[3] = {};
            for (uint32_t y = y0; y < y1; ++y) {
                const uint8_t* p = rgba.data() + (static_cast<size_t>(y) * width + x0) * 4;
                for (uint32_t x = x0; x < x1; ++x, p += 4) {
                    sum[0] += p[0];
                    sum[1] += p[1];
                    sum[2] += p[2];
                }
            }
            uint32_t count = (y1 - y0) * (x1 - x0);
            for (int i = 0; i < 3; ++i)
                out[i] = static_cast<uint8_t>(sum[i] / count);
        }
    }
    return rgb;
}

void Screenshot(Connection& connection, uint32_t maxSide) {
    std::vector<uint8_t> rgba;
    uint32_t width = 0;
    uint32_t height = 0;
    if (!CaptureFrame(rgba, width, height, kCaptureTimeoutMs)) {
        connection.Answer(ActiveBackend() == Backend::None
                              ? "error the overlay is not drawn yet: the game has not presented a frame since the hook started"
                              : "error no frame within 2 seconds: the game may be minimized or loading, or draw in HDR, which cannot be captured");
        return;
    }
    uint32_t outWidth = 0;
    uint32_t outHeight = 0;
    std::vector<uint8_t> rgb = Shrink(rgba, width, height, std::clamp<uint32_t>(maxSide, 64, 4096), outWidth, outHeight);
    if (connection.Answer("ok " + std::to_string(outWidth) + " " + std::to_string(outHeight)))
        connection.Write(rgb.data(), rgb.size());
}

struct NamedKey {
    const char* name;
    WORD vk;
};

constexpr NamedKey kKeys[] = {
    {"space", VK_SPACE},  {"enter", VK_RETURN}, {"escape", VK_ESCAPE}, {"tab", VK_TAB},       {"backspace", VK_BACK}, {"up", VK_UP},
    {"down", VK_DOWN},    {"left", VK_LEFT},    {"right", VK_RIGHT},   {"shift", VK_SHIFT},   {"ctrl", VK_CONTROL},   {"alt", VK_MENU},
    {"insert", VK_INSERT}, {"delete", VK_DELETE}, {"home", VK_HOME},  {"end", VK_END},       {"pageup", VK_PRIOR},   {"pagedown", VK_NEXT},
};

WORD KeyCode(std::string name) {
    for (char& c : name)
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    if (name.size() == 1 && isalnum(static_cast<unsigned char>(name[0])))
        return static_cast<WORD>(toupper(static_cast<unsigned char>(name[0])));
    if (name.size() >= 2 && name[0] == 'f' && isdigit(static_cast<unsigned char>(name[1]))) {
        int number = atoi(name.c_str() + 1);
        if (number >= 1 && number <= 12)
            return static_cast<WORD>(VK_F1 + number - 1);
    }
    for (const NamedKey& key : kKeys)
        if (name == key.name)
            return key.vk;
    return 0;
}

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

void SendKeys(const std::vector<WORD>& keys, bool up) {
    std::vector<INPUT> inputs(keys.size());
    for (size_t i = 0; i < keys.size(); ++i) {
        // Released in the reverse order.
        WORD vk = up ? keys[keys.size() - 1 - i] : keys[i];
        inputs[i].type = INPUT_KEYBOARD;
        inputs[i].ki.wVk = vk;
        inputs[i].ki.wScan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
        inputs[i].ki.dwFlags = (Extended(vk) ? KEYEVENTF_EXTENDEDKEY : 0) | (up ? KEYEVENTF_KEYUP : 0);
    }
    SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
}

void PostKeys(HWND window, const std::vector<WORD>& keys, bool repeat, bool up) {
    for (size_t i = 0; i < keys.size(); ++i) {
        WORD vk = up ? keys[keys.size() - 1 - i] : keys[i];
        bool system = vk == VK_MENU;
        LPARAM flags = 1 | static_cast<LPARAM>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC)) << 16 | (Extended(vk) ? 1 << 24 : 0) |
                       (repeat || up ? 1 << 30 : 0) | (up ? static_cast<LPARAM>(1u << 31) : 0);
        UINT message = up ? (system ? WM_SYSKEYUP : WM_KEYUP) : (system ? WM_SYSKEYDOWN : WM_KEYDOWN);
        PostMessageW(window, message, vk, flags);
    }
}

// SendInput when the game has the focus. Otherwise, as when the player types
// to the assistant in another window, the keys go to the game window as
// messages, which games that poll the keyboard state do not see. Stop()
// releases the keys at once.
std::string HoldKeys(const std::vector<WORD>& keys, DWORD ms) {
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

void Handle(Connection& connection, const std::string& line) {
    size_t space = line.find(' ');
    std::string command = line.substr(0, space);
    std::string rest = space == std::string::npos ? "" : line.substr(space + 1);
    if (command == "info") {
        HWND window = input::FindProcessWindow();
        RECT client{};
        if (window)
            GetClientRect(window, &client);
        char answer[160];
        snprintf(answer, sizeof(answer), "ok %s %ldx%ld focused=%d menu=%d", BackendName(ActiveBackend()), client.right, client.bottom,
                 window && GetForegroundWindow() == window, IsMenuOpen());
        connection.Answer(answer);
    } else if (command == "screenshot") {
        Screenshot(connection, static_cast<uint32_t>(rest.empty() ? 1280 : strtoul(rest.c_str(), nullptr, 10)));
    } else if (command == "text") {
        char* end = nullptr;
        double seconds = strtod(rest.c_str(), &end);
        std::string text = *end == ' ' ? end + 1 : "";
        for (size_t at = text.find("\\n"); at != std::string::npos; at = text.find("\\n", at + 1))
            text.replace(at, 2, "\n");
        {
            std::lock_guard<std::mutex> lock(g_textMutex);
            g_text = text;
            g_textUntil = GetTickCount64() + static_cast<ULONGLONG>(std::clamp(seconds, 0.0, 3600.0) * 1000.0);
        }
        connection.Answer("ok");
    } else if (command == "key") {
        size_t split = rest.find(' ');
        std::string names = rest.substr(0, split);
        DWORD ms = split == std::string::npos ? 100 : strtoul(rest.c_str() + split + 1, nullptr, 10);
        std::vector<WORD> keys;
        for (size_t start = 0;;) {
            size_t plus = names.find('+', start);
            std::string name = names.substr(start, plus == std::string::npos ? std::string::npos : plus - start);
            WORD vk = KeyCode(name);
            if (!vk) {
                connection.Answer("error unknown key " + name + ": use a letter, a digit, f1 to f12, space, enter, escape, tab, backspace, up, "
                                  "down, left, right, shift, ctrl, alt, insert, delete, home, end, pageup or pagedown");
                return;
            }
            keys.push_back(vk);
            if (plus == std::string::npos)
                break;
            start = plus + 1;
        }
        if (keys.size() > kMaxKeys)
            connection.Answer("error at most 4 keys at once");
        else
            connection.Answer(HoldKeys(keys, std::clamp<DWORD>(ms, 30, kMaxHoldMs)));
    } else {
        connection.Answer("error unknown command");
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
            if (connection.Accept())
                while (connection.ReadLine(line))
                    Handle(connection, line);
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
    std::lock_guard<std::mutex> lock(g_textMutex);
    g_text.clear();
    g_textUntil = 0;
}

// The last message of the assistant, at the top of the screen, until it expires.
void Draw() {
    std::string text;
    {
        std::lock_guard<std::mutex> lock(g_textMutex);
        if (GetTickCount64() < g_textUntil)
            text = g_text;
    }
    if (text.empty())
        return;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f, viewport->Pos.y + 30.0f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.85f);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                             ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings;
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.5f);
    ImGui::Begin("##uih_remote_message", nullptr, flags);
    ImGui::TextColored(ImVec4(0.85f, 0.55f, 0.35f, 1.0f), "Claude");
    ImGui::PushTextWrapPos(std::min(viewport->Size.x * 0.6f, ImGui::GetFontSize() * 40.0f));
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopTextWrapPos();
    ImGui::End();
    ImGui::PopFont();
}

} // namespace uih::detail::remote
