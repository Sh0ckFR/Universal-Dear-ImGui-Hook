// Runs a game, injects the library into it once its window shows, and drives
// the overlay as a player does: the menu key opens the menu on screen, the
// unload key removes it, and the game must keep running.
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr DWORD kStartTimeoutMs = 60000;
constexpr DWORD kLogTimeoutMs = 30000;

int Fail(const char* message) {
    printf("FAIL: %s\n", message);
    return 1;
}

// What the processes of the session write with OutputDebugString(), read
// as DebugView does. Only one such reader can run at a time.
class DebugLog {
public:
    bool Start(DWORD pid) {
        pid_ = pid;
        bufferReady_ = CreateEventW(nullptr, FALSE, FALSE, L"DBWIN_BUFFER_READY");
        if (!bufferReady_ || GetLastError() == ERROR_ALREADY_EXISTS)
            return false;
        dataReady_ = CreateEventW(nullptr, FALSE, FALSE, L"DBWIN_DATA_READY");
        mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Buffer), L"DBWIN_BUFFER");
        buffer_ = mapping_ ? static_cast<const Buffer*>(MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, sizeof(Buffer))) : nullptr;
        if (!dataReady_ || !buffer_)
            return false;
        thread_ = std::thread([this] { Read(); });
        return true;
    }

    void SetProcess(DWORD pid) { pid_ = pid; }

    bool Contains(const char* text) {
        std::lock_guard<std::mutex> lock(mutex_);
        return text_.find(text) != std::string::npos;
    }

    void Print() {
        std::lock_guard<std::mutex> lock(mutex_);
        printf("--- library log ---\n%s-------------------\n", text_.c_str());
    }

    ~DebugLog() {
        stop_ = true;
        if (thread_.joinable())
            thread_.join();
        if (buffer_)
            UnmapViewOfFile(buffer_);
        for (HANDLE handle : {mapping_, dataReady_, bufferReady_})
            if (handle)
                CloseHandle(handle);
    }

private:
    struct Buffer {
        DWORD pid;
        char text[4096 - sizeof(DWORD)];
    };

    void Read() {
        while (!stop_) {
            SetEvent(bufferReady_);
            if (WaitForSingleObject(dataReady_, 100) != WAIT_OBJECT_0)
                continue;
            if (buffer_->pid != pid_)
                continue;
            std::lock_guard<std::mutex> lock(mutex_);
            text_.append(buffer_->text, strnlen(buffer_->text, sizeof(buffer_->text)));
        }
    }

    std::atomic<DWORD> pid_{0};
    HANDLE bufferReady_ = nullptr;
    HANDLE dataReady_ = nullptr;
    HANDLE mapping_ = nullptr;
    const Buffer* buffer_ = nullptr;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::mutex mutex_;
    std::string text_;
};

struct WindowSearch {
    DWORD pid;
    HWND window;
};

// The largest visible top-level window of the game.
HWND FindGameWindow(DWORD pid) {
    WindowSearch search{pid, nullptr};
    EnumWindows(
        [](HWND window, LPARAM param) -> BOOL {
            auto* search = reinterpret_cast<WindowSearch*>(param);
            DWORD owner = 0;
            GetWindowThreadProcessId(window, &owner);
            RECT client{};
            if (owner != search->pid || !IsWindowVisible(window) || GetWindow(window, GW_OWNER) || !GetClientRect(window, &client) ||
                client.right < 320 || client.bottom < 240)
                return TRUE;
            search->window = window;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&search));
    return search.window;
}

bool Inject(HANDLE process, const std::wstring& dll) {
    size_t size = (dll.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(process, nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote || !WriteProcessMemory(process, remote, dll.c_str(), size, nullptr))
        return false;
    auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
    HANDLE thread = CreateRemoteThread(process, nullptr, 0, loadLibrary, remote, 0, nullptr);
    DWORD loaded = 0;
    if (thread) {
        WaitForSingleObject(thread, 10000);
        GetExitCodeThread(thread, &loaded);
        CloseHandle(thread);
    }
    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    return loaded != 0;
}

void HoldKey(WORD key, bool down) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = key;
    input.ki.wScan = static_cast<WORD>(MapVirtualKeyW(key, MAPVK_VK_TO_VSC));
    input.ki.dwFlags = (down ? 0 : KEYEVENTF_KEYUP) | (key == VK_INSERT ? KEYEVENTF_EXTENDEDKEY : 0);
    SendInput(1, &input, sizeof(input));
}

bool BringToFront(HWND window) {
    for (int attempt = 0; attempt < 10 && GetForegroundWindow() != window; ++attempt) {
        HoldKey(VK_MENU, true);
        HoldKey(VK_MENU, false);
        SetForegroundWindow(window);
        Sleep(200);
    }
    return GetForegroundWindow() == window;
}

void PressKey(WORD key) {
    HoldKey(key, true);
    Sleep(300);
    HoldKey(key, false);
}

// Client area of `window` as shown on screen, top-down BGRA rows.
std::vector<uint8_t> CaptureWindow(HWND window, int& width, int& height) {
    RECT rect{};
    GetClientRect(window, &rect);
    POINT origin{};
    ClientToScreen(window, &origin);
    width = rect.right;
    height = rect.bottom;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateDIBSection(memory, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ previous = SelectObject(memory, bitmap);
    BitBlt(memory, 0, 0, width, height, screen, origin.x, origin.y, SRCCOPY | CAPTUREBLT);
    std::vector<uint8_t> pixels(static_cast<const uint8_t*>(bits), static_cast<const uint8_t*>(bits) + static_cast<size_t>(width) * height * 4);
    SelectObject(memory, previous);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    return pixels;
}

void SaveCapture(const std::vector<uint8_t>& pixels, int width, int height, const std::string& path) {
    BITMAPINFOHEADER header{};
    header.biSize = sizeof(header);
    header.biWidth = width;
    header.biHeight = -height;
    header.biPlanes = 1;
    header.biBitCount = 32;
    header.biCompression = BI_RGB;
    BITMAPFILEHEADER file{};
    file.bfType = 0x4D42;
    file.bfOffBits = sizeof(file) + sizeof(header);
    file.bfSize = file.bfOffBits + static_cast<DWORD>(pixels.size());
    FILE* out = nullptr;
    if (fopen_s(&out, path.c_str(), "wb") == 0 && out) {
        fwrite(&file, sizeof(file), 1, out);
        fwrite(&header, sizeof(header), 1, out);
        fwrite(pixels.data(), 1, pixels.size(), out);
        fclose(out);
    }
}

// Pixels that differ between two captures of the same size.
int Changed(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    if (a.size() != b.size())
        return -1;
    int changed = 0;
    for (size_t i = 0; i + 3 < a.size(); i += 4)
        if (abs(a[i] - b[i]) > 24 || abs(a[i + 1] - b[i + 1]) > 24 || abs(a[i + 2] - b[i + 2]) > 24)
            ++changed;
    return changed;
}

std::wstring Widen(const char* text) {
    int size = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    std::wstring wide(size > 0 ? size - 1 : 0, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, wide.data(), size);
    return wide;
}

std::wstring Quote(const std::wstring& argument) {
    if (!argument.empty() && argument.find_first_of(L" \t\"") == std::wstring::npos)
        return argument;
    std::wstring quoted = L"\"";
    for (wchar_t c : argument)
        quoted += c == L'"' ? std::wstring(L"\\\"") : std::wstring(1, c);
    return quoted + L"\"";
}

} // namespace

int main(int argc, char** argv) {
    std::wstring dll;
    std::string backend;
    std::string captures = "game";
    int first = 0;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--dll" && i + 1 < argc) dll = Widen(argv[++i]);
        else if (arg == "--backend" && i + 1 < argc) backend = argv[++i];
        else if (arg == "--captures" && i + 1 < argc) captures = argv[++i];
        else if (arg == "--") {
            first = i + 1;
            break;
        }
    }
    if (dll.empty() || backend.empty() || first == 0 || first >= argc) {
        printf("usage: uih_game --dll path --backend D3D9|D3D10|D3D11|D3D12|Vulkan|OpenGL [--captures prefix] -- game.exe [arguments]\n");
        return 2;
    }
    setvbuf(stdout, nullptr, _IONBF, 0);

    wchar_t path[MAX_PATH] = {};
    GetFullPathNameW(dll.c_str(), MAX_PATH, path, nullptr);
    dll = path;
    std::wstring game = Widen(argv[first]);
    GetFullPathNameW(game.c_str(), MAX_PATH, path, nullptr);
    game = path;
    std::wstring directory = game.substr(0, game.find_last_of(L"\\/"));
    std::wstring commandLine = Quote(game);
    for (int i = first + 1; i < argc; ++i)
        commandLine += L" " + Quote(Widen(argv[i]));

    // The library writes its log with OutputDebugString() when UIH_DEBUG is set.
    SetEnvironmentVariableW(L"UIH_DEBUG", L"1");
    DebugLog log;
    if (!log.Start(0))
        return Fail("another program reads the debug output, such as DebugView");

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(game.c_str(), commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr, directory.c_str(), &startup, &process))
        return Fail("the game could not be started");
    log.SetProcess(process.dwProcessId);
    auto alive = [&] { return WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT; };
    auto finish = [&](int result) {
        if (result != 0)
            log.Print();
        TerminateProcess(process.hProcess, 0);
        WaitForSingleObject(process.hProcess, 5000);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        if (result == 0)
            printf("PASS: %s\n", backend.c_str());
        return result;
    };
    auto waitFor = [&](const char* text) {
        for (ULONGLONG start = GetTickCount64(); GetTickCount64() - start < kLogTimeoutMs; Sleep(100)) {
            if (log.Contains(text))
                return true;
            if (!alive())
                return false;
        }
        return log.Contains(text);
    };

    HWND window = nullptr;
    for (ULONGLONG start = GetTickCount64(); !window && alive() && GetTickCount64() - start < kStartTimeoutMs; Sleep(200))
        window = FindGameWindow(process.dwProcessId);
    if (!window)
        return finish(Fail("the game did not show a window"));
    // The game renders its first frames before the library comes in, as with an injector.
    Sleep(5000);
    if (!BringToFront(window))
        return finish(Fail("the game window does not get the focus"));
    int width = 0;
    int height = 0;
    std::vector<uint8_t> before = CaptureWindow(window, width, height);
    SaveCapture(before, width, height, captures + "-before.bmp");

    if (!Inject(process.hProcess, dll))
        return finish(Fail("the library could not be loaded into the game"));
    std::string active = "[uih] " + backend + " backend active";
    if (!waitFor(active.c_str()))
        return finish(Fail(alive() ? "the expected backend did not become active" : "the game exited after the library was loaded"));

    BringToFront(window);
    PressKey(VK_INSERT);
    if (!waitFor("[uih] Menu opened"))
        return finish(Fail(alive() ? "the menu key did not open the menu" : "the game exited when the menu opened"));
    Sleep(1500);
    std::vector<uint8_t> open = CaptureWindow(window, width, height);
    SaveCapture(open, width, height, captures + "-menu.bmp");
    int changed = Changed(before, open);
    printf("%d of %d pixels changed with the menu open\n", changed, width * height);
    if (changed < width * height / 50)
        return finish(Fail("the menu is not visible on screen"));

    BringToFront(window);
    PressKey(VK_F11);
    if (!waitFor("[uih] Stopped"))
        return finish(Fail(alive() ? "the unload key did not remove the overlay" : "the game exited when the overlay was removed"));
    Sleep(2000);
    if (!alive())
        return finish(Fail("the game exited after the overlay was removed"));
    std::vector<uint8_t> after = CaptureWindow(window, width, height);
    SaveCapture(after, width, height, captures + "-after.bmp");
    DWORD_PTR ignored = 0;
    if (!SendMessageTimeoutW(window, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 5000, &ignored))
        return finish(Fail("the game no longer answers"));
    changed = Changed(open, after);
    printf("%d of %d pixels changed once the overlay was removed\n", changed, width * height);
    if (changed < width * height / 50)
        return finish(Fail("the menu stays on screen after the overlay was removed"));
    log.Print();
    return finish(0);
}
