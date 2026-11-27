// Remote control on Windows: the protocol of src/remote.cpp on the named pipe
// \\.\pipe\<Config::remotePipe>, which only local clients can open, and the
// keys sent with SendInput or as window messages.
#include "internal.h"

#include <tlhelp32.h>

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
    timing::RealTime realTime;
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

namespace {

// The window the overlay draws on, or before its first frame the main window
// of the process: another window of the game may have the focus.
HWND TargetWindow() {
    HWND window = BoundWindow();
    return window && IsWindowVisible(window) ? window : input::FindProcessWindow();
}

bool HasFocus(HWND window) {
    HWND foreground = GetForegroundWindow();
    return foreground && (foreground == window || foreground == GetAncestor(window, GA_ROOT));
}

constexpr DWORD kHoverMs = 50;
const DWORD kDown[] = {0, MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_MIDDLEDOWN, MOUSEEVENTF_RIGHTDOWN};
const DWORD kUp[] = {0, MOUSEEVENTF_LEFTUP, MOUSEEVENTF_MIDDLEUP, MOUSEEVENTF_RIGHTUP};
const UINT kDownMessage[] = {0, WM_LBUTTONDOWN, WM_MBUTTONDOWN, WM_RBUTTONDOWN};
const UINT kUpMessage[] = {0, WM_LBUTTONUP, WM_MBUTTONUP, WM_RBUTTONUP};
const WPARAM kHeld[] = {0, MK_LBUTTON, MK_MBUTTON, MK_RBUTTON};

// Moves the cursor to x, y in the client area of `window` with SendInput.
void SendMove(HWND window, int x, int y) {
    POINT point{x, y};
    ClientToScreen(window, &point);
    int left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int width = std::max(2, GetSystemMetrics(SM_CXVIRTUALSCREEN));
    int height = std::max(2, GetSystemMetrics(SM_CYVIRTUALSCREEN));
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dx = static_cast<LONG>((static_cast<int64_t>(point.x - left) * 65535 + (width - 1) / 2) / (width - 1));
    input.mi.dy = static_cast<LONG>((static_cast<int64_t>(point.y - top) * 65535 + (height - 1) / 2) / (height - 1));
    input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    SendInput(1, &input, sizeof(INPUT));
}

void SendButton(DWORD flags) {
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = flags;
    SendInput(1, &input, sizeof(INPUT));
}

// One UTF-16 unit, pressed and released, which the window gets as WM_CHAR.
void SendUnit(wchar_t unit) {
    INPUT inputs[2] = {};
    for (int i = 0; i < 2; ++i) {
        inputs[i].type = INPUT_KEYBOARD;
        inputs[i].ki.wScan = unit;
        inputs[i].ki.dwFlags = KEYEVENTF_UNICODE | (i ? KEYEVENTF_KEYUP : 0);
    }
    SendInput(2, inputs, sizeof(INPUT));
}

} // namespace

// SendInput when the game has the focus. Otherwise, as when the player types
// to the assistant in another window, the keys go to the game window as
// messages, which games that poll the keyboard state do not see. Stop()
// releases the keys at once.
std::string HoldKeys(const std::vector<int>& keys, unsigned ms) {
    HWND window = TargetWindow();
    if (!window)
        return "error the game has no visible window";
    if (HasFocus(window)) {
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

// Mouse input when the game has the focus, moved there with SendInput: the hook
// of SetCursorPos keeps the cursor still while the menu is open. Otherwise
// window messages, a move before the release too: Dear ImGui forgets the
// cursor when Windows tells it the real one is not over the window. The
// cursor rests on the point for a few frames before the press, as games read
// it once per frame.
std::string ClickAt(int x, int y, int button, unsigned ms) {
    HWND window = TargetWindow();
    if (!window)
        return "error the game has no visible window";
    if (HasFocus(window)) {
        SendMove(window, x, y);
        WaitForSingleObject(g_stopEvent, kHoverMs);
        SendButton(kDown[button]);
        WaitForSingleObject(g_stopEvent, ms);
        SendButton(kUp[button]);
        return "ok sent as mouse input, the game has the focus";
    }
    LPARAM position = MAKELPARAM(x, y);
    PostMessageW(window, WM_MOUSEMOVE, 0, position);
    WaitForSingleObject(g_stopEvent, kHoverMs);
    PostMessageW(window, WM_MOUSEMOVE, 0, position);
    PostMessageW(window, kDownMessage[button], kHeld[button], position);
    WaitForSingleObject(g_stopEvent, ms);
    PostMessageW(window, WM_MOUSEMOVE, kHeld[button], position);
    PostMessageW(window, kUpMessage[button], 0, position);
    return "ok sent as window messages, the game does not have the focus: games that read the mouse directly do not see them";
}

// Unicode input when the game has the focus, which reaches the window as
// WM_CHAR, as text typed with an input method does. Otherwise WM_CHAR
// messages. Enter and Tab are keys, for games that read them as keys.
std::string TypeText(const std::vector<uint32_t>& text) {
    HWND window = TargetWindow();
    if (!window)
        return "error the game has no visible window";
    bool focused = HasFocus(window);
    for (uint32_t point : text) {
        if (Stopping())
            break;
        if (point == '\n' || point == '\t') {
            std::vector<int> key{point == '\n' ? VK_RETURN : VK_TAB};
            if (focused) {
                SendKeys(key, false);
                SendKeys(key, true);
            } else {
                PostKeys(window, key, false, false);
                PostMessageW(window, WM_CHAR, point == '\n' ? '\r' : '\t', 1);
                PostKeys(window, key, false, true);
            }
        } else {
            wchar_t units[2] = {static_cast<wchar_t>(point), 0};
            int count = 1;
            if (point > 0xffff) {
                units[0] = static_cast<wchar_t>(0xd800 + ((point - 0x10000) >> 10));
                units[1] = static_cast<wchar_t>(0xdc00 + ((point - 0x10000) & 0x3ff));
                count = 2;
            }
            for (int i = 0; i < count; ++i) {
                if (focused)
                    SendUnit(units[i]);
                else
                    PostMessageW(window, WM_CHAR, units[i], 1);
            }
        }
        // Games read text once per frame, some only the last character.
        WaitForSingleObject(g_stopEvent, 10);
    }
    std::string count = std::to_string(text.size());
    return focused ? "ok typed " + count + " characters as keyboard input, the game has the focus: games that read the keys directly see Enter and Tab only"
                   : "ok typed " + count + " characters as window messages, the game does not have the focus";
}

// Returns once the cursor rested on the point for a few frames: Windows merges
// the moves a window has not read yet, so the next command would hide it.
std::string MoveTo(int x, int y) {
    HWND window = TargetWindow();
    if (!window)
        return "error the game has no visible window";
    if (HasFocus(window)) {
        SendMove(window, x, y);
        WaitForSingleObject(g_stopEvent, kHoverMs);
        return "ok moved the cursor, the game has the focus";
    }
    PostMessageW(window, WM_MOUSEMOVE, 0, MAKELPARAM(x, y));
    WaitForSingleObject(g_stopEvent, kHoverMs);
    return "ok sent as a window message, the game does not have the focus: games that read the mouse directly do not see it";
}

// The button goes down at the start, the cursor moves there once a frame, and
// the button goes up at the end, after the cursor rested there.
std::string DragTo(int x0, int y0, int x1, int y1, int button, unsigned ms) {
    constexpr unsigned kStepMs = 16;
    HWND window = TargetWindow();
    if (!window)
        return "error the game has no visible window";
    bool focused = HasFocus(window);
    unsigned steps = std::max(1u, ms / kStepMs);
    if (focused) {
        SendMove(window, x0, y0);
        WaitForSingleObject(g_stopEvent, kHoverMs);
        SendButton(kDown[button]);
    } else {
        PostMessageW(window, WM_MOUSEMOVE, 0, MAKELPARAM(x0, y0));
        WaitForSingleObject(g_stopEvent, kHoverMs);
        PostMessageW(window, kDownMessage[button], kHeld[button], MAKELPARAM(x0, y0));
    }
    for (unsigned i = 1; i <= steps && !Stopping(); ++i) {
        WaitForSingleObject(g_stopEvent, ms / steps);
        int x = x0 + static_cast<int>((static_cast<int64_t>(x1 - x0) * i) / steps);
        int y = y0 + static_cast<int>((static_cast<int64_t>(y1 - y0) * i) / steps);
        if (focused)
            SendMove(window, x, y);
        else
            PostMessageW(window, WM_MOUSEMOVE, kHeld[button], MAKELPARAM(x, y));
    }
    WaitForSingleObject(g_stopEvent, kHoverMs);
    if (focused) {
        SendButton(kUp[button]);
        return "ok sent as mouse input, the game has the focus";
    }
    PostMessageW(window, kUpMessage[button], 0, MAKELPARAM(x1, y1));
    return "ok sent as window messages, the game does not have the focus: games that read the mouse directly do not see them";
}

bool GameWindow(uint32_t& width, uint32_t& height, bool& focused) {
    HWND window = TargetWindow();
    RECT client{};
    if (!window || !GetClientRect(window, &client))
        return false;
    width = static_cast<uint32_t>(client.right);
    height = static_cast<uint32_t>(client.bottom);
    focused = HasFocus(window);
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


bool StopRequested() { return g_stopEvent && Stopping(); }

std::vector<MemoryRegion> MemoryRegions() {
    constexpr DWORD kWritable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    constexpr DWORD kReadable = kWritable | PAGE_READONLY | PAGE_EXECUTE_READ;
    // Uncached memory, such as memory of the GPU, is slow to read.
    constexpr DWORD kSkipped = PAGE_GUARD | PAGE_NOACCESS | PAGE_NOCACHE | PAGE_WRITECOMBINE;
    std::vector<MemoryRegion> regions;
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    auto* address = static_cast<uint8_t*>(system.lpMinimumApplicationAddress);
    MEMORY_BASIC_INFORMATION info{};
    while (address < system.lpMaximumApplicationAddress && VirtualQuery(address, &info, sizeof(info)) == sizeof(info)) {
        if (info.State == MEM_COMMIT && (info.Protect & kReadable) && !(info.Protect & kSkipped))
            regions.push_back({reinterpret_cast<uintptr_t>(info.BaseAddress), info.RegionSize, (info.Protect & kWritable) != 0, info.Type == MEM_IMAGE});
        address = static_cast<uint8_t*>(info.BaseAddress) + info.RegionSize;
    }
    return regions;
}

std::vector<LoadedModule> LoadedModules() {
    std::vector<LoadedModule> modules;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return modules;
    HMODULE executable = GetModuleHandleW(nullptr);
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Module32FirstW(snapshot, &entry); more; more = Module32NextW(snapshot, &entry)) {
        char name[MAX_PATH * 3] = {};
        WideCharToMultiByte(CP_UTF8, 0, entry.szModule, -1, name, sizeof(name), nullptr, nullptr);
        modules.push_back({name, reinterpret_cast<uintptr_t>(entry.modBaseAddr), entry.modBaseSize, entry.hModule == executable});
    }
    CloseHandle(snapshot);
    return modules;
}

bool ReadMemory(uintptr_t address, void* out, size_t size) {
    SIZE_T read = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), out, size, &read) && read == size;
}

// WriteProcessMemory makes read-only pages, such as code, writable for the write.
bool WriteMemory(uintptr_t address, const void* data, size_t size) {
    SIZE_T written = 0;
    if (!WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address), data, size, &written) || written != size)
        return false;
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(address), size);
    return true;
}

void* AllocateScanMemory(size_t size) { return VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE); }

void* FindModule(const char* name) {
    wchar_t wide[MAX_PATH] = {};
    if (!MultiByteToWideChar(CP_UTF8, 0, name, -1, wide, MAX_PATH))
        return nullptr;
    return GetModuleHandleW(wide);
}

void* ModuleSymbol(void* module, const char* name) {
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(module), name));
}

UINT GameThreadMessage() {
    static const UINT message = RegisterWindowMessageW(L"uih-game-thread-call");
    return message;
}

// The window procedure of the overlay runs the call on the thread of the game
// window, the main thread of most engines.
bool WakeGameThread() {
    HWND window = BoundWindow();
    return window && GameThreadMessage() && PostMessageW(window, GameThreadMessage(), 0, 0);
}

} // namespace uih::detail::remote
