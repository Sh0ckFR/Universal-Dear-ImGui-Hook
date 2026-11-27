// Tools of the game for the assistant, registered with uih::RegisterTool(): the
// remote control lists them with the tools command and runs one with the tool
// command, in the frame of the overlay.
//   tools                      -> ok <bytes>, then a JSON array of {"name", "description", "inputSchema"}
//   tool <name> <JSON object>  -> ok <bytes>, then the text the tool returned
#include "internal.h"

#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <exception>

namespace uih {
namespace detail {
namespace {

constexpr size_t kMaxName = 64;
constexpr int kMaxDepth = 64;
// A call waits this long for the frame of the overlay, and for the tool.
constexpr auto kCallTimeout = std::chrono::seconds(10);

struct Tool {
    std::string name;
    std::string description;
    std::string parameters;
    ToolCall call;
};

std::mutex g_toolsMutex;
// Constructed on first use: a preloaded library registers its tools from its
// constructor, which may run before the variables of this file are initialized.
std::vector<Tool>& Tools() {
    static std::vector<Tool> tools;
    return tools;
}

// The call waiting for the frame of the overlay. The server thread answers
// one request at a time, so there is at most one.
std::mutex g_callMutex;
std::condition_variable g_callDone;
uint64_t g_callId = 0;
bool g_callWaiting = false;
bool g_callFinished = false;
ToolCall g_call;
std::string g_callArguments;
std::string g_callResult;
std::string g_callError;

// JSON ----------------------------------------------------------------------

void SkipSpace(const std::string& text, size_t& at) {
    while (at < text.size() && (text[at] == ' ' || text[at] == '\t' || text[at] == '\n' || text[at] == '\r'))
        ++at;
}

void AppendUtf8(std::string& out, uint32_t point) {
    if (point < 0x80) {
        out += static_cast<char>(point);
    } else if (point < 0x800) {
        out += static_cast<char>(0xc0 | point >> 6);
        out += static_cast<char>(0x80 | (point & 0x3f));
    } else if (point < 0x10000) {
        out += static_cast<char>(0xe0 | point >> 12);
        out += static_cast<char>(0x80 | (point >> 6 & 0x3f));
        out += static_cast<char>(0x80 | (point & 0x3f));
    } else {
        out += static_cast<char>(0xf0 | point >> 18);
        out += static_cast<char>(0x80 | (point >> 12 & 0x3f));
        out += static_cast<char>(0x80 | (point >> 6 & 0x3f));
        out += static_cast<char>(0x80 | (point & 0x3f));
    }
}

bool ParseHex4(const std::string& text, size_t at, uint32_t& value) {
    if (at + 4 > text.size())
        return false;
    value = 0;
    for (size_t i = at; i < at + 4; ++i) {
        char c = text[i];
        int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (digit < 0)
            return false;
        value = value << 4 | static_cast<uint32_t>(digit);
    }
    return true;
}

// The JSON string at text[at], decoded into `out` when given; `at` moves past it.
bool ParseString(const std::string& text, size_t& at, std::string* out) {
    if (at >= text.size() || text[at] != '"')
        return false;
    for (++at; at < text.size(); ++at) {
        char c = text[at];
        if (c == '"') {
            ++at;
            return true;
        }
        if (static_cast<unsigned char>(c) < 0x20)
            return false;
        if (c != '\\') {
            if (out)
                *out += c;
            continue;
        }
        if (++at >= text.size())
            return false;
        uint32_t point = 0;
        switch (text[at]) {
        case '"':
        case '\\':
        case '/':
            point = static_cast<unsigned char>(text[at]);
            break;
        case 'b':
            point = '\b';
            break;
        case 'f':
            point = '\f';
            break;
        case 'n':
            point = '\n';
            break;
        case 'r':
            point = '\r';
            break;
        case 't':
            point = '\t';
            break;
        case 'u': {
            if (!ParseHex4(text, at + 1, point))
                return false;
            at += 4;
            // A surrogate pair: 😀.
            uint32_t low = 0;
            if (point >= 0xd800 && point < 0xdc00 && at + 2 < text.size() && text[at + 1] == '\\' && text[at + 2] == 'u' &&
                ParseHex4(text, at + 3, low) && low >= 0xdc00 && low < 0xe000) {
                point = 0x10000 + ((point - 0xd800) << 10) + (low - 0xdc00);
                at += 6;
            }
            break;
        }
        default:
            return false;
        }
        if (out)
            AppendUtf8(*out, point);
    }
    return false;
}

// Any JSON value at text[at]; `at` moves past it. False when it is malformed.
bool SkipValue(const std::string& text, size_t& at, int depth = 0) {
    SkipSpace(text, at);
    if (at >= text.size())
        return false;
    char c = text[at];
    if (c == '"')
        return ParseString(text, at, nullptr);
    if (c == '{' || c == '[') {
        if (depth >= kMaxDepth)
            return false;
        char close = c == '{' ? '}' : ']';
        ++at;
        SkipSpace(text, at);
        if (at < text.size() && text[at] == close) {
            ++at;
            return true;
        }
        for (;;) {
            if (c == '{') {
                SkipSpace(text, at);
                if (!ParseString(text, at, nullptr))
                    return false;
                SkipSpace(text, at);
                if (at >= text.size() || text[at] != ':')
                    return false;
                ++at;
            }
            if (!SkipValue(text, at, depth + 1))
                return false;
            SkipSpace(text, at);
            if (at >= text.size())
                return false;
            if (text[at] == close) {
                ++at;
                return true;
            }
            if (text[at] != ',')
                return false;
            ++at;
        }
    }
    // A number, true, false or null.
    size_t start = at;
    while (at < text.size() && (isalnum(static_cast<unsigned char>(text[at])) || text[at] == '-' || text[at] == '+' || text[at] == '.'))
        ++at;
    return at > start;
}

// True when `text` is one JSON object and nothing else.
bool IsObject(const std::string& text) {
    size_t at = 0;
    SkipSpace(text, at);
    if (at >= text.size() || text[at] != '{' || !SkipValue(text, at))
        return false;
    SkipSpace(text, at);
    return at == text.size();
}

std::string JsonString(const std::string& text) {
    std::string out = "\"";
    for (char c : text) {
        auto byte = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\t') {
            out += "\\t";
        } else if (byte < 0x20) {
            char escape[8];
            snprintf(escape, sizeof(escape), "\\u%04x", byte);
            out += escape;
        } else {
            out += c;
        }
    }
    return out + "\"";
}

bool ValidName(const char* name) {
    if (!name || !*name)
        return false;
    size_t length = 0;
    for (const char* c = name; *c; ++c, ++length) {
        if (length >= kMaxName || !(isalnum(static_cast<unsigned char>(*c)) || *c == '_' || *c == '-'))
            return false;
    }
    return true;
}

std::string Payload(const std::string& text, std::vector<uint8_t>& payload) {
    payload.assign(text.begin(), text.end());
    return "ok " + std::to_string(payload.size());
}

std::string ListTools(std::vector<uint8_t>& payload) {
    std::string json = "[";
    std::lock_guard<std::mutex> lock(g_toolsMutex);
    for (const Tool& tool : Tools()) {
        if (json.size() > 1)
            json += ",";
        json += "{\"name\":" + JsonString(tool.name) + ",\"description\":" + JsonString(tool.description) + ",\"inputSchema\":" + tool.parameters + "}";
    }
    return Payload(json + "]", payload);
}

std::string CallTool(const std::string& rest, std::vector<uint8_t>& payload) {
    size_t space = rest.find(' ');
    std::string name = rest.substr(0, space);
    std::string arguments = space == std::string::npos ? "{}" : rest.substr(space + 1);
    if (!IsObject(arguments))
        return "error give the arguments of the tool as a JSON object on the same line";
    ToolCall call;
    {
        std::lock_guard<std::mutex> lock(g_toolsMutex);
        for (const Tool& tool : Tools()) {
            if (tool.name == name)
                call = tool.call;
        }
    }
    if (!call)
        return "error the game has no tool named " + name;

    std::unique_lock<std::mutex> lock(g_callMutex);
    uint64_t id = ++g_callId;
    g_call = std::move(call);
    g_callArguments = std::move(arguments);
    g_callResult.clear();
    g_callError.clear();
    g_callFinished = false;
    g_callWaiting = true;
    auto deadline = std::chrono::steady_clock::now() + kCallTimeout;
    while (!g_callFinished && !remote::StopRequested() && std::chrono::steady_clock::now() < deadline)
        g_callDone.wait_for(lock, std::chrono::milliseconds(100));
    bool finished = g_callFinished && g_callId == id;
    g_callWaiting = false;
    g_call = nullptr;
    if (!finished)
        return remote::StopRequested() ? "error the remote control is stopping"
                                       : "error the tool did not run within 10 seconds: the game renders no frame, or the tool takes too long";
    if (!g_callError.empty())
        return "error " + g_callError;
    return Payload(g_callResult, payload);
}

} // namespace

namespace remote {

bool ParseJsonArray(const std::string& text, std::vector<std::string>& values, std::vector<bool>* strings) {
    values.clear();
    if (strings)
        strings->clear();
    size_t at = 0;
    SkipSpace(text, at);
    if (at >= text.size() || text[at] != '[')
        return false;
    ++at;
    SkipSpace(text, at);
    if (at < text.size() && text[at] == ']') {
        ++at;
    } else {
        for (;;) {
            SkipSpace(text, at);
            size_t start = at;
            std::string value;
            bool isString = at < text.size() && text[at] == '"';
            if (isString ? !ParseString(text, at, &value) : !SkipValue(text, at))
                return false;
            values.push_back(isString ? value : text.substr(start, at - start));
            if (strings)
                strings->push_back(isString);
            SkipSpace(text, at);
            if (at >= text.size())
                return false;
            if (text[at] == ']') {
                ++at;
                break;
            }
            if (text[at] != ',')
                return false;
            ++at;
        }
    }
    SkipSpace(text, at);
    return at == text.size();
}

std::string JsonQuote(const std::string& text) { return JsonString(text); }

bool HandleTools(const std::string& command, const std::string& rest, std::vector<uint8_t>& payload, std::string& answer) {
    if (command == "tools")
        answer = ListTools(payload);
    else if (command == "tool")
        answer = CallTool(rest, payload);
    else
        return false;
    return true;
}

void RunTools() {
    ToolCall call;
    std::string arguments;
    uint64_t id = 0;
    {
        std::lock_guard<std::mutex> lock(g_callMutex);
        if (!g_callWaiting || g_callFinished || !g_call)
            return;
        call = g_call;
        arguments = g_callArguments;
        id = g_callId;
    }
    // Outside the lock: the server thread gives up after its timeout even when
    // the tool never returns.
    std::string result;
    std::string error;
    try {
        result = call(arguments);
    } catch (const std::exception& exception) {
        error = *exception.what() ? exception.what() : "the tool failed";
    } catch (...) {
        error = "the tool failed";
    }
    for (char& c : error) {
        if (c == '\n' || c == '\r')
            c = ' ';
    }
    std::lock_guard<std::mutex> lock(g_callMutex);
    if (!g_callWaiting || g_callId != id)
        return;
    g_callResult = std::move(result);
    g_callError = std::move(error);
    g_callFinished = true;
    g_callDone.notify_all();
}

} // namespace remote
} // namespace detail

bool RegisterTool(const char* name, const char* description, const char* parameters, ToolCall call) {
    std::string schema = parameters && *parameters ? parameters : "{\"type\":\"object\",\"properties\":{}}";
    if (!detail::ValidName(name) || !call || !detail::IsObject(schema))
        return false;
    detail::Tool tool{name, description ? description : "", schema, std::move(call)};
    std::lock_guard<std::mutex> lock(detail::g_toolsMutex);
    for (detail::Tool& existing : detail::Tools()) {
        if (existing.name == tool.name) {
            existing = std::move(tool);
            return true;
        }
    }
    detail::Tools().push_back(std::move(tool));
    return true;
}

void UnregisterTool(const char* name) {
    if (!name)
        return;
    std::lock_guard<std::mutex> lock(detail::g_toolsMutex);
    for (auto it = detail::Tools().begin(); it != detail::Tools().end(); ++it) {
        if (it->name == name) {
            detail::Tools().erase(it);
            return;
        }
    }
}

std::string ToolArgument(const std::string& arguments, const char* key) {
    using detail::ParseString;
    using detail::SkipSpace;
    using detail::SkipValue;
    if (!key)
        return "";
    size_t at = 0;
    SkipSpace(arguments, at);
    if (at >= arguments.size() || arguments[at] != '{')
        return "";
    ++at;
    for (;;) {
        SkipSpace(arguments, at);
        std::string name;
        if (!ParseString(arguments, at, &name))
            return "";
        SkipSpace(arguments, at);
        if (at >= arguments.size() || arguments[at] != ':')
            return "";
        ++at;
        SkipSpace(arguments, at);
        size_t start = at;
        if (name == key) {
            std::string value;
            if (at < arguments.size() && arguments[at] == '"')
                return ParseString(arguments, at, &value) ? value : "";
            return SkipValue(arguments, at) ? arguments.substr(start, at - start) : "";
        }
        if (!SkipValue(arguments, at))
            return "";
        SkipSpace(arguments, at);
        if (at >= arguments.size() || arguments[at] != ',')
            return "";
        ++at;
    }
}

} // namespace uih
