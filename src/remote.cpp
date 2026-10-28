// Remote control of the game by an AI assistant: remote/uih_mcp.py turns these
// commands into MCP tools, so that the assistant sees the game, writes in it
// and presses its keys. Served when Config::remoteControl is set, on a named
// pipe (src/windows/remote.cpp) or a Unix socket (src/linux/remote.cpp).
//
// One request per line, local clients only:
//   info                       -> ok <backend> <width>x<height> focused=0|1 menu=0|1
//   screenshot <max size>      -> ok <width> <height>, then width * height RGB bytes
//   text <seconds> <message>   -> ok         (\n in the message starts a new line)
//   key <keys> <milliseconds>  -> ok <how the keys were sent>  (keys: space, or left+space held together)
// Failures answer: error <reason>
#include "internal.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace uih::detail::remote {
namespace {

constexpr unsigned kMaxHoldMs = 10000;
constexpr size_t kMaxKeys = 4;
constexpr uint32_t kCaptureTimeoutMs = 2000;

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

std::string Screenshot(uint32_t maxSide, std::vector<uint8_t>& payload) {
    std::vector<uint8_t> rgba;
    uint32_t width = 0;
    uint32_t height = 0;
    if (!CaptureFrame(rgba, width, height, kCaptureTimeoutMs))
        return ActiveBackend() == Backend::None
                   ? "error the overlay is not drawn yet: the game has not presented a frame since the hook started"
                   : "error no frame within 2 seconds: the game may be minimized or loading, or draw in HDR, which cannot be captured";
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
    std::lock_guard<std::mutex> lock(g_textMutex);
    g_text = text;
    g_textUntil = Clock::now() + std::chrono::milliseconds(static_cast<int64_t>(std::clamp(seconds, 0.0, 3600.0) * 1000.0));
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

} // namespace

std::string Handle(const std::string& line, std::vector<uint8_t>& payload) {
    size_t space = line.find(' ');
    std::string command = line.substr(0, space);
    std::string rest = space == std::string::npos ? "" : line.substr(space + 1);
    if (command == "info") {
        uint32_t width = 0;
        uint32_t height = 0;
        bool focused = false;
        GameWindow(width, height, focused);
        char answer[160];
        snprintf(answer, sizeof(answer), "ok %s %ux%u focused=%d menu=%d", BackendName(ActiveBackend()), width, height, focused, IsMenuOpen());
        return answer;
    }
    if (command == "screenshot")
        return Screenshot(static_cast<uint32_t>(rest.empty() ? 1280 : strtoul(rest.c_str(), nullptr, 10)), payload);
    if (command == "text")
        return Text(rest);
    if (command == "key")
        return Keys(rest);
    return "error unknown command";
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
