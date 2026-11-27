// Remote control of the game by an AI assistant: remote/uih_mcp.py turns these
// commands into MCP tools, so that the assistant sees the game, writes in it
// and presses its keys. Served when Config::remoteControl is set, on a named
// pipe (src/windows/remote.cpp) or a Unix socket (src/linux/remote.cpp).
//
// One request per line, local clients only:
//   info                       -> ok <backend> <width>x<height> focused=0|1 menu=0|1 memory=0|1 advanced=0|1 speed=<speed>
//   screenshot <max size>      -> ok <width> <height>, then width * height RGB bytes
//   text <seconds> <message>   -> ok         (\n in the message starts a new line)
//   key <keys> <milliseconds>  -> ok <how the keys were sent>  (keys: space, or left+space held together)
//   click <x> <y> [left|right|middle] [milliseconds]  -> ok <how the click was sent>  (pixels of the game window)
// with Config::aiAdvancedFeatures:
//   type <text>                -> ok <how the text was sent>  (\n for Enter, \t for Tab, \\ for \)
//   move <x> <y>               -> ok <how the pointer was moved>
//   drag <x0> <y0> <x1> <y1> [left|right|middle] [milliseconds]  -> ok <how the drag was sent>
//   wait <milliseconds> [percent]  -> ok changed|unchanged <milliseconds> <percent of the pixels that changed>
//   time [<speed>|advance <milliseconds>]  -> ok <speed>   (speed of the game's clocks, src/timing.cpp)
// and the memory commands of src/memory.cpp and src/advanced.cpp, the objects
// of the game's engine, src/engine.cpp, and the tools of the game, src/tools.cpp.
// Failures answer: error <reason>
#include "internal.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstdlib>

namespace uih::detail::remote {
namespace {

constexpr unsigned kMaxHoldMs = 10000;
constexpr size_t kMaxKeys = 4;
constexpr uint32_t kCaptureTimeoutMs = 2000;
constexpr size_t kMaxTyped = 2000;
constexpr unsigned kMaxWaitMs = 30000;
// The frames wait compares, at most this many pixels wide or high.
constexpr uint32_t kWaitSide = 160;
// A pixel changed when one of its channels moved by more than this.
constexpr int kPixelChange = 24;

using Clock = std::chrono::steady_clock;

std::mutex g_textMutex;
std::string g_text;
Clock::time_point g_textUntil;

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

std::string CaptureError() {
    return ActiveBackend() == Backend::None
               ? "error the overlay is not drawn yet: the game has not presented a frame since the hook started"
               : "error no frame within 2 seconds: the game may be minimized or loading, or draw in HDR, which cannot be captured";
}

std::string Screenshot(uint32_t maxSide, std::vector<uint8_t>& payload) {
    std::vector<uint8_t> rgba;
    uint32_t width = 0;
    uint32_t height = 0;
    if (!CaptureFrame(rgba, width, height, kCaptureTimeoutMs))
        return CaptureError();
    uint32_t outWidth = 0;
    uint32_t outHeight = 0;
    payload = Shrink(rgba, width, height, std::clamp<uint32_t>(maxSide, 64, 4096), outWidth, outHeight);
    return "ok " + std::to_string(outWidth) + " " + std::to_string(outHeight);
}

std::string Text(const std::string& rest) {
    char* end = nullptr;
    double seconds = strtod(rest.c_str(), &end);
    // NaN would turn into an undefined duration.
    if (!(seconds > 0.0))
        seconds = 0.0;
    std::string text = *end == ' ' ? end + 1 : "";
    for (size_t at = text.find("\\n"); at != std::string::npos; at = text.find("\\n", at + 1))
        text.replace(at, 2, "\n");
    ShowMessage(text, seconds);
    return "ok";
}

std::string Keys(const std::string& rest) {
    size_t split = rest.find(' ');
    std::string names = rest.substr(0, split);
    unsigned ms = split == std::string::npos ? 100 : static_cast<unsigned>(strtoul(rest.c_str() + split + 1, nullptr, 10));
    std::vector<int> keys;
    for (size_t start = 0;;) {
        size_t plus = names.find('+', start);
        std::string name = names.substr(start, plus == std::string::npos ? std::string::npos : plus - start);
        for (char& c : name)
            c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        int code = KeyCode(name);
        if (!code)
            return "error unknown key " + name + ": use a letter, a digit, f1 to f12, space, enter, escape, tab, backspace, up, "
                   "down, left, right, shift, ctrl, alt, insert, delete, home, end, pageup or pagedown";
        keys.push_back(code);
        if (plus == std::string::npos)
            break;
        start = plus + 1;
    }
    if (keys.size() > kMaxKeys)
        return "error at most 4 keys at once";
    return HoldKeys(keys, std::clamp<unsigned>(ms, 30, kMaxHoldMs));
}

int ParseButton(const std::string& name) { return name == "left" ? 1 : name == "middle" ? 2 : name == "right" ? 3 : 0; }

// Parses `count` integers from `text`; false when one is missing or out of range.
bool ParsePoint(const char*& text, int* values, int count) {
    for (int i = 0; i < count; ++i) {
        char* end = nullptr;
        long value = strtol(text, &end, 10);
        if (end == text || value < INT32_MIN || value > INT32_MAX)
            return false;
        values[i] = static_cast<int>(value);
        text = end;
    }
    return true;
}

// Empty when x, y is in the game window, the error otherwise.
std::string OutsideWindow(int x, int y) {
    uint32_t width = 0;
    uint32_t height = 0;
    bool focused = false;
    if (GameWindow(width, height, focused) && (x < 0 || y < 0 || static_cast<uint32_t>(x) >= width || static_cast<uint32_t>(y) >= height))
        return "error the point is outside the game window, which is " + std::to_string(width) + "x" + std::to_string(height);
    return "";
}

std::string Click(const std::string& rest) {
    const char* start = rest.c_str();
    char* end = nullptr;
    long x = strtol(start, &end, 10);
    bool parsed = end != start;
    start = end;
    long y = strtol(start, &end, 10);
    if (!parsed || end == start || x < INT32_MIN || x > INT32_MAX || y < INT32_MIN || y > INT32_MAX)
        return "error give x and y in pixels of the game window, then optionally left, right or middle and milliseconds";
    std::string name = "left";
    unsigned ms = 100;
    if (*end == ' ') {
        std::string options = end + 1;
        size_t split = options.find(' ');
        name = options.substr(0, split);
        if (split != std::string::npos)
            ms = static_cast<unsigned>(strtoul(options.c_str() + split + 1, nullptr, 10));
    }
    int button = ParseButton(name);
    if (!button)
        return "error unknown button " + name + ": use left, right or middle";
    std::string outside = OutsideWindow(static_cast<int>(x), static_cast<int>(y));
    if (!outside.empty())
        return outside;
    return ClickAt(static_cast<int>(x), static_cast<int>(y), button, std::clamp<unsigned>(ms, 30, kMaxHoldMs));
}

// UTF-8 to code points, with the escapes \n, \t and \\.
bool DecodeText(const std::string& text, std::vector<uint32_t>& out) {
    for (size_t i = 0; i < text.size();) {
        auto c = static_cast<unsigned char>(text[i]);
        if (c == '\\' && i + 1 < text.size() && (text[i + 1] == 'n' || text[i + 1] == 't' || text[i + 1] == '\\')) {
            out.push_back(text[i + 1] == 'n' ? '\n' : text[i + 1] == 't' ? '\t' : '\\');
            i += 2;
            continue;
        }
        int extra = c < 0x80 ? 0 : (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2 : (c & 0xf8) == 0xf0 ? 3 : -1;
        if (extra < 0 || i + static_cast<size_t>(extra) >= text.size())
            return false;
        uint32_t point = extra == 0 ? c : c & (0x3f >> extra);
        for (int k = 1; k <= extra; ++k) {
            auto next = static_cast<unsigned char>(text[i + k]);
            if ((next & 0xc0) != 0x80)
                return false;
            point = point << 6 | (next & 0x3f);
        }
        if (point < 0x20 && point != '\n' && point != '\t')
            return false;
        out.push_back(point);
        i += static_cast<size_t>(extra) + 1;
    }
    return true;
}

std::string Type(const std::string& rest) {
    std::vector<uint32_t> text;
    if (!DecodeText(rest, text) || text.empty())
        return "error give the text to type, in UTF-8, with \\n for Enter and \\t for Tab";
    if (text.size() > kMaxTyped)
        return "error at most 2000 characters at once";
    return TypeText(text);
}

std::string Move(const std::string& rest) {
    const char* text = rest.c_str();
    int point[2] = {};
    if (!ParsePoint(text, point, 2))
        return "error give x and y in pixels of the game window";
    std::string outside = OutsideWindow(point[0], point[1]);
    return outside.empty() ? MoveTo(point[0], point[1]) : outside;
}

std::string Drag(const std::string& rest) {
    const char* text = rest.c_str();
    int points[4] = {};
    if (!ParsePoint(text, points, 4))
        return "error give x0 y0 x1 y1 in pixels of the game window, then optionally left, right or middle and milliseconds";
    std::string name = "left";
    unsigned ms = 300;
    if (*text == ' ') {
        std::string options = text + 1;
        size_t split = options.find(' ');
        name = options.substr(0, split);
        if (split != std::string::npos)
            ms = static_cast<unsigned>(strtoul(options.c_str() + split + 1, nullptr, 10));
    }
    int button = ParseButton(name);
    if (!button)
        return "error unknown button " + name + ": use left, right or middle";
    for (int i = 0; i < 4; i += 2) {
        std::string outside = OutsideWindow(points[i], points[i + 1]);
        if (!outside.empty())
            return outside;
    }
    return DragTo(points[0], points[1], points[2], points[3], button, std::clamp<unsigned>(ms, 50, kMaxHoldMs));
}

// Percent of the pixels of `a` and `b`, of the same size, that changed.
double ChangedPercent(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    size_t changed = 0;
    for (size_t i = 0; i + 2 < a.size(); i += 3) {
        int most = 0;
        for (int c = 0; c < 3; ++c)
            most = std::max(most, std::abs(a[i + c] - b[i + c]));
        changed += most > kPixelChange;
    }
    return a.empty() ? 0.0 : 100.0 * static_cast<double>(changed) / static_cast<double>(a.size() / 3);
}

// Waits until the frame of the game differs from the first one by at least
// `percent` of its pixels, or for `ms`: the assistant then looks only once
// something happened, a menu opened or a loading screen ended.
std::string Wait(const std::string& rest) {
    char* end = nullptr;
    unsigned long ms = strtoul(rest.c_str(), &end, 10);
    if (end == rest.c_str())
        ms = 5000;
    double percent = *end == ' ' ? strtod(end + 1, nullptr) : 1.0;
    // NaN fails both comparisons.
    if (!(percent > 0.0) || !(percent <= 100.0))
        percent = 1.0;
    ms = std::clamp<unsigned long>(ms, 100, kMaxWaitMs);
    Clock::time_point start = Clock::now();
    auto elapsed = [&] { return static_cast<unsigned long>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count()); };
    std::vector<uint8_t> rgba;
    uint32_t width = 0;
    uint32_t height = 0;
    if (!CaptureFrame(rgba, width, height, kCaptureTimeoutMs))
        return CaptureError();
    uint32_t firstWidth = 0;
    uint32_t firstHeight = 0;
    std::vector<uint8_t> first = Shrink(rgba, width, height, kWaitSide, firstWidth, firstHeight);
    double most = 0.0;
    char answer[96];
    while (elapsed() < ms && !StopRequested()) {
        if (!CaptureFrame(rgba, width, height, std::min<uint32_t>(kCaptureTimeoutMs, static_cast<uint32_t>(ms - std::min(ms, elapsed())) + 1))) {
            if (ActiveBackend() == Backend::None)
                break;
            continue;
        }
        uint32_t frameWidth = 0;
        uint32_t frameHeight = 0;
        std::vector<uint8_t> frame = Shrink(rgba, width, height, kWaitSide, frameWidth, frameHeight);
        double changed = frameWidth == firstWidth && frameHeight == firstHeight ? ChangedPercent(first, frame) : 100.0;
        most = std::max(most, changed);
        if (changed >= percent) {
            snprintf(answer, sizeof(answer), "ok changed %lu %.2f", elapsed(), changed);
            return answer;
        }
    }
    snprintf(answer, sizeof(answer), "ok unchanged %lu %.2f", elapsed(), most);
    return answer;
}

bool IsInput(const std::string& command) {
    return command == "key" || command == "click" || command == "type" || command == "move" || command == "drag";
}

bool IsAdvanced(const std::string& command) {
    return command == "type" || command == "move" || command == "drag" || command == "wait" || command == "time";
}

std::string Input(const std::string& command, const std::string& rest) {
    // A game the assistant paused runs while it acts, then pauses again.
    timing::RunWhilePaused run;
    if (command == "key")
        return Keys(rest);
    if (command == "click")
        return Click(rest);
    if (command == "type")
        return Type(rest);
    if (command == "move")
        return Move(rest);
    return Drag(rest);
}

std::string Answer(const std::string& command, const std::string& rest, std::vector<uint8_t>& payload) {
    const Config& config = GetConfig();
    if (command == "info") {
        uint32_t width = 0;
        uint32_t height = 0;
        bool focused = false;
        GameWindow(width, height, focused);
        char answer[160];
        snprintf(answer, sizeof(answer), "ok %s %ux%u focused=%d menu=%d memory=%d advanced=%d speed=%g", BackendName(ActiveBackend()), width, height,
                 focused, IsMenuOpen(), config.remoteMemory, config.aiAdvancedFeatures, timing::Speed());
        return answer;
    }
    if (IsAdvanced(command) && !config.aiAdvancedFeatures)
        return "error the advanced features are off: set config.aiAdvancedFeatures in example/config.cpp and rebuild the library";
    if (IsInput(command) && !InputAllowed())
        return "error the player paused the input of the assistant in the AI Advanced Features window of the overlay";
    if (command == "screenshot")
        return Screenshot(static_cast<uint32_t>(rest.empty() ? 1280 : strtoul(rest.c_str(), nullptr, 10)), payload);
    if (command == "text")
        return Text(rest);
    if (IsInput(command))
        return Input(command, rest);
    if (command == "wait") {
        timing::RunWhilePaused run;
        return Wait(rest);
    }
    if (command == "time")
        return timing::Command(rest);
    std::string answer;
    if (HandleTools(command, rest, payload, answer) || HandleMemory(command, rest, payload, answer) || HandleEngine(command, rest, payload, answer) ||
        HandleAdvanced(command, rest, payload, answer))
        return answer;
    return "error unknown command";
}

} // namespace

std::string Handle(const std::string& line, std::vector<uint8_t>& payload) {
    size_t space = line.find(' ');
    std::string command = line.substr(0, space);
    std::string rest = space == std::string::npos ? "" : line.substr(space + 1);
    std::string answer = Answer(command, rest, payload);
    // The chat polls several times a second: its own window shows it.
    if (GetConfig().aiAdvancedFeatures && command != "info" && command != "chat")
        RecordActivity(line, answer);
    return answer;
}

void ShowMessage(const std::string& text, double seconds) {
    std::lock_guard<std::mutex> lock(g_textMutex);
    g_text = text;
    g_textUntil = Clock::now() + std::chrono::milliseconds(static_cast<int64_t>(std::clamp(seconds, 0.0, 3600.0) * 1000.0));
}

void ClearText() {
    std::lock_guard<std::mutex> lock(g_textMutex);
    g_text.clear();
    g_textUntil = {};
}

// The last message of the assistant, at the top of the screen, until it expires.
void Draw() {
    std::string text;
    {
        std::lock_guard<std::mutex> lock(g_textMutex);
        if (Clock::now() < g_textUntil)
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
