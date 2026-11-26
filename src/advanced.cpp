// Advanced features of the remote control, Config::aiAdvancedFeatures: the
// watch list and its frozen values, the instruction watch, a record of what
// the assistant does, and the "AI Advanced Features" window that shows them
// to the player. With Config::remoteMemory as well:
//   watch add <type> <address> [label]  -> ok <id>
//   watch remove <id>                   -> ok
//   watch clear                         -> ok
//   watch list                          -> ok <bytes>, then lines: <id> <type> <address> <resolved address|-> <value|?> frozen=0|1 <label>
//   freeze <id> [value]                 -> ok   (the current value by default, written again every frame)
//   unfreeze <id>                       -> ok
//   writes start <address> [1|2|4|8] [write|access]  -> ok
//   writes stop                         -> ok
//   writes results                      -> ok <bytes>, then lines: <address of the next instruction> <module+offset> <hits> <16 bytes before it>
//   chat poll                           -> ok <bytes>, then what the player did in the chat since: lines
//                                          message <text>, reset (new conversation) or cancel (stop the answer)
//   chat say <text>                     -> ok   (an answer of Claude, also at the top of the screen while the chat is hidden)
//   chat note <text>                    -> ok   (a line of the bridge, such as an error)
//   chat status [text]                  -> ok   (what Claude is doing, empty once it answered)
// Texts escape a new line as \n, a tab as \t and a backslash as \\. remote/uih_chat.py
// is the bridge: it runs Claude Code for each message of the player.
// The addresses are those of src/memory.cpp, pointer chains included: a
// watched [game.exe+1a2b]+10 follows the pointer when the game moves the value.
// The input commands and wait are in src/remote.cpp, pointer and pointers in src/memory.cpp.
#include "internal.h"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <thread>

#ifndef _WIN32
#include <pthread.h>
#include <signal.h>
#endif

namespace uih::detail::remote {
namespace {

using Clock = std::chrono::steady_clock;

constexpr size_t kMaxWatches = 64;
constexpr size_t kMaxActivity = 100;
constexpr size_t kMaxShown = 160;
constexpr size_t kPanelScanLimit = 200;

std::atomic<bool> g_inputAllowed{true};
std::atomic<bool> g_memoryAllowed{true};

struct Watch {
    int id = 0;
    std::string expression;
    ValueType type = ValueType::I32;
    std::string label;
    bool frozen = false;
    uint8_t frozenBytes[8] = {};
};

struct Activity {
    Clock::time_point at;
    std::string command;
    std::string answer;
};

// The watch list, the modules it resolves names with, the record of the
// assistant and the results of the window's pointer scan.
std::mutex g_mutex;
std::vector<Watch> g_watches;
int g_nextId = 1;
std::vector<LoadedModule> g_modules;
Clock::time_point g_modulesAt;
std::deque<Activity> g_activity;

// The pointer scan of the window runs on a thread of its own.
std::thread g_scanThread;
std::atomic<bool> g_scanRunning{false};
std::atomic<bool> g_scanCancel{false};
std::vector<PointerHit> g_scanHits;
std::string g_scanStatus;
uintptr_t g_scanTarget = 0;

// What the window watches with WatchInstructions().
std::string g_writesStatus;
uintptr_t g_writesAddress = 0;

// The chat with Claude, through remote/uih_chat.py.
constexpr size_t kMaxChat = 200;
constexpr size_t kMaxChatText = 8000;
// The bridge polls 4 times a second, less while a command of Claude holds the pipe.
constexpr auto kBridgeGone = std::chrono::seconds(10);
// Beyond this, the chat is not on screen: the answers show at the top too.
constexpr auto kChatHidden = std::chrono::milliseconds(500);
enum class Speaker { Player, Claude, Note };
struct ChatLine {
    Speaker speaker;
    std::string text;
};
std::deque<ChatLine> g_chat;
// For the bridge: message <text>, reset, cancel.
std::deque<std::string> g_outbox;
std::string g_chatStatus;
Clock::time_point g_bridgeSeen;
Clock::time_point g_chatDrawn;
bool g_chatScroll = false;
bool g_waitingNoted = false;

bool MemoryOn() {
    const Config& config = GetConfig();
    return config.remoteControl && config.aiAdvancedFeatures && config.remoteMemory;
}

// With g_mutex held. Listing the modules is slow: once a second at most.
const std::vector<LoadedModule>& Modules() {
    Clock::time_point now = Clock::now();
    if (g_modules.empty() || now - g_modulesAt > std::chrono::seconds(1)) {
        g_modules = SortedModules();
        g_modulesAt = now;
    }
    return g_modules;
}

std::string Hex(uintptr_t value) {
    char text[32];
    snprintf(text, sizeof(text), "0x%" PRIxPTR, value);
    return text;
}

std::string Shorten(const std::string& text) { return text.size() > kMaxShown ? text.substr(0, kMaxShown) + "..." : text; }

std::string EscapeLine(const std::string& text) {
    std::string out;
    for (char c : text) {
        if (c == '\\')
            out += "\\\\";
        else if (c == '\n')
            out += "\\n";
        else if (c == '\t')
            out += "\\t";
        else if (c != '\r')
            out += c;
    }
    return out;
}

std::string UnescapeLine(const std::string& text) {
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '\\' || i + 1 == text.size()) {
            out += text[i];
            continue;
        }
        char next = text[++i];
        out += next == 'n' ? '\n' : next == 't' ? '\t' : next;
    }
    return out;
}

// Cut at `size` bytes, without splitting a UTF-8 character.
void Truncate(std::string& text, size_t size) {
    if (text.size() <= size)
        return;
    while (size > 0 && (static_cast<unsigned char>(text[size]) & 0xc0) == 0x80)
        --size;
    text.resize(size);
}

// With g_mutex held.
void AddChat(Speaker speaker, std::string text) {
    Truncate(text, kMaxChatText);
    g_chat.push_back({speaker, std::move(text)});
    while (g_chat.size() > kMaxChat)
        g_chat.pop_front();
    g_chatScroll = true;
}

std::string ChatCommand(const std::string& rest, std::vector<uint8_t>& payload) {
    size_t space = rest.find(' ');
    std::string action = rest.substr(0, space);
    std::string text = space == std::string::npos ? "" : UnescapeLine(rest.substr(space + 1));
    std::lock_guard<std::mutex> lock(g_mutex);
    if (action == "poll") {
        g_bridgeSeen = Clock::now();
        g_waitingNoted = false;
        std::string lines;
        for (const std::string& line : g_outbox)
            lines += line + "\n";
        g_outbox.clear();
        payload.assign(lines.begin(), lines.end());
        return "ok " + std::to_string(payload.size());
    }
    if (action == "status") {
        Truncate(text, kMaxShown);
        g_chatStatus = text;
        return "ok";
    }
    if (action != "say" && action != "note")
        return "error use chat poll, chat say <text>, chat note <text> or chat status [text]";
    if (text.empty())
        return "error give the text";
    AddChat(action == "say" ? Speaker::Claude : Speaker::Note, text);
    if (action == "say" && Clock::now() - g_chatDrawn > kChatHidden)
        ShowMessage(g_chat.back().text, std::clamp(4.0 + static_cast<double>(text.size()) / 15.0, 6.0, 30.0));
    return "ok";
}

Watch* FindWatch(int id) {
    for (Watch& watch : g_watches)
        if (watch.id == id)
            return &watch;
    return nullptr;
}

// With g_mutex held: the address and the current bytes of `watch`.
bool ReadWatch(const Watch& watch, uintptr_t& address, uint8_t* bytes) {
    return ResolveAddress(watch.expression, Modules(), address) && ReadMemory(address, bytes, ValueSize(watch.type));
}

// With g_mutex held. Empty when added, the error otherwise.
std::string AddWatch(const std::string& expression, ValueType type, const std::string& label, int& id) {
    if (g_watches.size() >= kMaxWatches)
        return "at most 64 values in the watch list: remove some first";
    uintptr_t address = 0;
    std::string error;
    if (!ResolveAddress(expression, Modules(), address, &error))
        return error;
    Watch watch;
    watch.id = id = g_nextId++;
    watch.expression = expression;
    watch.type = type;
    watch.label = label;
    g_watches.push_back(watch);
    return "";
}

// With g_mutex held. Freezes `watch` at `value`, or at its current value when empty.
std::string Freeze(Watch& watch, const std::string& value) {
    uint8_t bytes[8] = {};
    uintptr_t address = 0;
    if (value.empty()) {
        if (!ReadWatch(watch, address, bytes))
            return "the value is not readable now";
    } else if (!EncodeValue(value, watch.type, bytes)) {
        return std::string("the value is not a number of type ") + ValueTypeName(watch.type);
    }
    memcpy(watch.frozenBytes, bytes, sizeof(bytes));
    watch.frozen = true;
    if (ResolveAddress(watch.expression, Modules(), address))
        WriteMemory(address, bytes, ValueSize(watch.type));
    return "";
}

std::string WatchCommand(const std::string& rest, std::vector<uint8_t>& payload) {
    size_t space = rest.find(' ');
    std::string action = rest.substr(0, space);
    std::string arguments = space == std::string::npos ? "" : rest.substr(space + 1);
    std::lock_guard<std::mutex> lock(g_mutex);
    if (action == "add") {
        size_t typeEnd = arguments.find(' ');
        ValueType type = ValueType::I32;
        if (typeEnd == std::string::npos || !ParseValueType(arguments.substr(0, typeEnd), type))
            return "error give a type, i8, i16, i32, i64, f32 or f64, an address, then optionally a label";
        size_t addressEnd = arguments.find(' ', typeEnd + 1);
        std::string expression = arguments.substr(typeEnd + 1, addressEnd == std::string::npos ? std::string::npos : addressEnd - typeEnd - 1);
        std::string label = addressEnd == std::string::npos ? "" : arguments.substr(addressEnd + 1);
        int id = 0;
        std::string error = AddWatch(expression, type, label, id);
        return error.empty() ? "ok " + std::to_string(id) : "error " + error;
    }
    if (action == "remove") {
        int id = atoi(arguments.c_str());
        auto it = std::find_if(g_watches.begin(), g_watches.end(), [&](const Watch& watch) { return watch.id == id; });
        if (it == g_watches.end())
            return "error no value " + arguments + " in the watch list";
        g_watches.erase(it);
        return "ok";
    }
    if (action == "clear") {
        g_watches.clear();
        return "ok";
    }
    if (action == "list") {
        std::string text;
        for (const Watch& watch : g_watches) {
            uintptr_t address = 0;
            uint8_t bytes[8] = {};
            bool resolved = ResolveAddress(watch.expression, Modules(), address);
            bool read = resolved && ReadMemory(address, bytes, ValueSize(watch.type));
            text += std::to_string(watch.id) + " " + ValueTypeName(watch.type) + " " + watch.expression + " " + (resolved ? Hex(address) : "-") + " " +
                    (read ? FormatValue(bytes, watch.type) : "?") + " frozen=" + (watch.frozen ? "1" : "0") + (watch.label.empty() ? "" : " " + watch.label) +
                    "\n";
        }
        payload.assign(text.begin(), text.end());
        return "ok " + std::to_string(payload.size());
    }
    return "error use watch add, watch remove, watch clear or watch list";
}

std::string FreezeCommand(bool freeze, const std::string& rest) {
    size_t space = rest.find(' ');
    int id = atoi(rest.substr(0, space).c_str());
    std::lock_guard<std::mutex> lock(g_mutex);
    Watch* watch = FindWatch(id);
    if (!watch)
        return "error no value " + rest.substr(0, space) + " in the watch list: add it with watch add";
    if (!freeze) {
        watch->frozen = false;
        return "ok";
    }
    std::string error = Freeze(*watch, space == std::string::npos ? "" : rest.substr(space + 1));
    return error.empty() ? "ok" : "error " + error;
}

std::string HitsText() {
    std::vector<LoadedModule> modules = SortedModules();
    std::string text;
    for (const InstructionHit& hit : InstructionHits()) {
        uint8_t before[16] = {};
        char bytes[16 * 3 + 1] = {};
        if (ReadMemory(hit.next - sizeof(before), before, sizeof(before)))
            for (size_t i = 0; i < sizeof(before); ++i)
                snprintf(bytes + i * 3, 4, i + 1 < sizeof(before) ? "%02x " : "%02x", before[i]);
        text += Hex(hit.next) + " " + LocateAddress(hit.next, modules) + " " + std::to_string(hit.count) + " " + bytes + "\n";
    }
    return text;
}

// Empty when the watch started, the reason otherwise.
std::string StartWrites(const std::string& expression, unsigned size, bool access) {
    uintptr_t address = 0;
    std::string error;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!ResolveAddress(expression, Modules(), address, &error))
            return error;
    }
    error = WatchInstructions(address, size, access);
    if (error.empty()) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_writesAddress = address;
        g_writesStatus = std::string("Watching the instructions that ") + (access ? "access " : "write ") + std::to_string(size) + " bytes at " + Hex(address);
    }
    return error;
}

void StopWrites() {
    UnwatchInstructions();
    std::lock_guard<std::mutex> lock(g_mutex);
    g_writesAddress = 0;
    g_writesStatus = "Stopped.";
}

std::string WritesCommand(const std::string& rest, std::vector<uint8_t>& payload) {
    size_t space = rest.find(' ');
    std::string action = rest.substr(0, space);
    if (action == "start") {
        std::string arguments = space == std::string::npos ? "" : rest.substr(space + 1);
        size_t end = arguments.find(' ');
        std::string expression = arguments.substr(0, end);
        unsigned size = 4;
        bool access = false;
        while (end != std::string::npos) {
            size_t next = arguments.find(' ', end + 1);
            std::string option = arguments.substr(end + 1, next == std::string::npos ? std::string::npos : next - end - 1);
            if (option == "access")
                access = true;
            else if (option != "write" && !option.empty())
                size = static_cast<unsigned>(strtoul(option.c_str(), nullptr, 10));
            end = next;
        }
        if (expression.empty())
            return "error give an address, then optionally 1, 2, 4 or 8 bytes and write or access";
        std::string error = StartWrites(expression, size, access);
        return error.empty() ? "ok" : "error " + error;
    }
    if (action == "stop") {
        StopWrites();
        return "ok";
    }
    if (action == "results") {
        std::string text = HitsText();
        payload.assign(text.begin(), text.end());
        return "ok " + std::to_string(payload.size());
    }
    return "error use writes start, writes stop or writes results";
}

// The window ---------------------------------------------------------------

bool PanelScanCancelled() { return g_scanCancel.load(); }

void StartPanelScan(uintptr_t target, size_t maxOffset) {
    if (g_scanRunning)
        return;
    if (g_scanThread.joinable())
        g_scanThread.join();
    g_scanCancel = false;
    g_scanRunning = true;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_scanStatus = "Scanning the memory for pointers to " + Hex(target) + "...";
        g_scanTarget = target;
        g_scanHits.clear();
    }
#ifndef _WIN32
    // The signals of the game go to its own threads.
    sigset_t all;
    sigset_t previous;
    sigfillset(&all);
    pthread_sigmask(SIG_SETMASK, &all, &previous);
#endif
    try {
        g_scanThread = std::thread([target, maxOffset] {
            std::vector<PointerHit> hits;
            bool done = ScanPointers(target, maxOffset, kPanelScanLimit, PanelScanCancelled, hits);
            size_t inModules = static_cast<size_t>(std::count_if(hits.begin(), hits.end(), [](const PointerHit& hit) { return hit.inModule; }));
            std::lock_guard<std::mutex> lock(g_mutex);
            g_scanHits = hits;
            g_scanStatus = done ? std::to_string(hits.size()) + " pointers, " + std::to_string(inModules) + " in modules (kept after a restart)." : "Cancelled.";
            g_scanRunning = false;
        });
    } catch (const std::exception&) {
        g_scanRunning = false;
        std::lock_guard<std::mutex> lock(g_mutex);
        g_scanStatus = "The scan cannot start.";
    }
#ifndef _WIN32
    pthread_sigmask(SIG_SETMASK, &previous, nullptr);
#endif
}

const char* const kTypeNames[] = {"i8", "i16", "i32", "i64", "f32", "f64"};

void TypeCombo(const char* label, int& type) { ImGui::Combo(label, &type, kTypeNames, IM_ARRAYSIZE(kTypeNames)); }

void DrawActivity() {
    std::vector<Activity> activity;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        activity.assign(g_activity.rbegin(), g_activity.rend());
    }
    if (ImGui::SmallButton("Clear")) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_activity.clear();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Commands of the assistant, newest first.");
    if (activity.empty()) {
        ImGui::TextWrapped("Nothing yet: ask the assistant to look at the game.");
        return;
    }
    if (!ImGui::BeginTable("activity", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Ago", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 3.5f);
    ImGui::TableSetupColumn("Command");
    ImGui::TableSetupColumn("Answer");
    ImGui::TableHeadersRow();
    Clock::time_point now = Clock::now();
    for (const Activity& entry : activity) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::Text("%llds", static_cast<long long>(std::chrono::duration_cast<std::chrono::seconds>(now - entry.at).count()));
        ImGui::TableNextColumn();
        ImGui::TextWrapped("%s", entry.command.c_str());
        ImGui::TableNextColumn();
        bool failed = entry.answer.rfind("error ", 0) == 0;
        if (failed)
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.4f, 1.0f));
        ImGui::TextWrapped("%s", entry.answer.c_str());
        if (failed)
            ImGui::PopStyleColor();
    }
    ImGui::EndTable();
}

void DrawWatchList() {
    static char expression[256] = "";
    static char label[64] = "";
    static int type = 2;
    static int selected = 0;
    static char value[64] = "";
    static std::string error;

    std::lock_guard<std::mutex> lock(g_mutex);
    if (ImGui::BeginTable("watches", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("Label");
        ImGui::TableSetupColumn("Address");
        ImGui::TableSetupColumn("Value");
        ImGui::TableSetupColumn("Frozen", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableHeadersRow();
        int removed = 0;
        for (Watch& watch : g_watches) {
            ImGui::PushID(watch.id);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            std::string name = (watch.label.empty() ? "#" + std::to_string(watch.id) : watch.label) + " (" + ValueTypeName(watch.type) + ")";
            if (ImGui::Selectable(name.c_str(), selected == watch.id, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
                selected = watch.id;
            ImGui::TableNextColumn();
            uintptr_t address = 0;
            uint8_t bytes[8] = {};
            bool read = ReadWatch(watch, address, bytes);
            ImGui::TextUnformatted(watch.expression.c_str());
            if (address && ImGui::IsItemHovered())
                ImGui::SetTooltip("%s, %s", Hex(address).c_str(), LocateAddress(address, Modules()).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(read ? FormatValue(bytes, watch.type).c_str() : "?");
            ImGui::TableNextColumn();
            bool frozen = watch.frozen;
            if (ImGui::Checkbox("##frozen", &frozen)) {
                if (frozen)
                    error = Freeze(watch, "");
                else
                    watch.frozen = false;
            }
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("X"))
                removed = watch.id;
            ImGui::PopID();
        }
        ImGui::EndTable();
        if (removed)
            g_watches.erase(std::remove_if(g_watches.begin(), g_watches.end(), [&](const Watch& watch) { return watch.id == removed; }), g_watches.end());
    }
    if (g_watches.empty())
        ImGui::TextWrapped("The watch list is empty. Add a value below, or ask the assistant to find one (\"find my money and keep it at 99999\").");

    if (Watch* watch = FindWatch(selected)) {
        ImGui::SeparatorText("Selected value");
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
        ImGui::InputTextWithHint("##value", "new value", value, sizeof(value));
        ImGui::SameLine();
        if (ImGui::Button("Write")) {
            uint8_t bytes[8] = {};
            uintptr_t address = 0;
            if (!EncodeValue(value, watch->type, bytes))
                error = std::string("Not a number of type ") + ValueTypeName(watch->type) + ".";
            else if (!ResolveAddress(watch->expression, Modules(), address) || !WriteMemory(address, bytes, ValueSize(watch->type)))
                error = "The memory cannot be written there.";
            else
                error.clear();
        }
        ImGui::SameLine();
        if (ImGui::Button("Freeze at this value"))
            error = Freeze(*watch, value);
    }

    ImGui::SeparatorText("Add a value");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##expression", "game.exe+1a2b, 0x7ff6a1b2c3d4 or [game.exe+1a2b]+10", expression, sizeof(expression));
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5.0f);
    TypeCombo("##type", type);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
    ImGui::InputTextWithHint("##label", "label", label, sizeof(label));
    ImGui::SameLine();
    if (ImGui::Button("Add")) {
        int id = 0;
        error = AddWatch(expression, static_cast<ValueType>(type), label, id);
        if (error.empty()) {
            selected = id;
            expression[0] = '\0';
            label[0] = '\0';
        }
    }
    if (!error.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", error.c_str());
}

void DrawPointers() {
    static char chain[256] = "";
    static char target[64] = "";
    static int maxOffset = 0x1000;
    static int type = 2;

    ImGui::SeparatorText("Follow a pointer chain");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##chain", "[[game.exe+1a2b]+10]+8", chain, sizeof(chain));
    if (chain[0]) {
        std::vector<std::string> steps;
        uintptr_t address = 0;
        std::string error;
        bool resolved = false;
        std::string where;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            resolved = ResolveAddress(chain, Modules(), address, &error, &steps);
            if (resolved)
                where = LocateAddress(address, Modules());
        }
        for (const std::string& step : steps)
            ImGui::BulletText("%s", step.c_str());
        if (resolved)
            ImGui::Text("Address: %s (%s)", Hex(address).c_str(), where.c_str());
        else
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", error.c_str());
    }

    ImGui::SeparatorText("Find the pointers to an address");
    ImGui::TextWrapped("The pointers stored in a module come first: game.exe+offset stays the same after a restart, where the value itself moves.");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
    ImGui::InputTextWithHint("##target", "address of the value", target, sizeof(target));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
    ImGui::InputInt("max offset", &maxOffset, 0x10, 0x100, ImGuiInputTextFlags_CharsHexadecimal);
    maxOffset = std::clamp(maxOffset, 0, 1 << 20);
    if (g_scanRunning) {
        if (ImGui::Button("Cancel"))
            g_scanCancel = true;
    } else if (ImGui::Button("Scan")) {
        uintptr_t address = 0;
        std::string error;
        bool resolved = false;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            resolved = ResolveAddress(target, Modules(), address, &error);
        }
        if (resolved) {
            StartPanelScan(address, static_cast<size_t>(maxOffset));
        } else {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_scanStatus = error;
        }
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    ImGui::SameLine();
    ImGui::TextWrapped("%s", g_scanStatus.c_str());
    if (g_scanHits.empty() || g_scanRunning)
        return;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5.0f);
    TypeCombo("type to watch", type);
    if (!ImGui::BeginTable("pointers", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY,
                           ImVec2(0.0f, ImGui::GetTextLineHeightWithSpacing() * 10)))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Pointer");
    ImGui::TableSetupColumn("Address of the value");
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableHeadersRow();
    std::string add;
    for (size_t i = 0; i < g_scanHits.size(); ++i) {
        const PointerHit& hit = g_scanHits[i];
        std::string location = hit.inModule ? LocateAddress(hit.location, Modules()) : Hex(hit.location);
        char delta[24];
        snprintf(delta, sizeof(delta), "+%" PRIxPTR, hit.delta);
        std::string expression = "[" + location + "]" + (hit.delta ? delta : "");
        ImGui::PushID(static_cast<int>(i));
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if (hit.inModule)
            ImGui::TextUnformatted(location.c_str());
        else
            ImGui::TextDisabled("%s", location.c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(expression.c_str());
        ImGui::TableNextColumn();
        if (ImGui::SmallButton("Watch"))
            add = expression;
        ImGui::PopID();
    }
    ImGui::EndTable();
    if (!add.empty()) {
        int id = 0;
        std::string error = AddWatch(add, static_cast<ValueType>(type), "pointer to " + Hex(g_scanTarget), id);
        g_scanStatus = error.empty() ? "Added " + add + " to the watch list." : error;
    }
}

void DrawChat() {
    const ImGuiStyle& style = ImGui::GetStyle();
    Clock::time_point now = Clock::now();
    std::vector<ChatLine> chat;
    std::string status;
    bool connected = false;
    bool scroll = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_chatDrawn = now;
        chat.assign(g_chat.begin(), g_chat.end());
        status = g_chatStatus;
        connected = g_bridgeSeen != Clock::time_point{} && now - g_bridgeSeen < kBridgeGone;
        scroll = g_chatScroll;
        g_chatScroll = false;
    }
    if (connected) {
        ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1.0f), "Claude is listening");
    } else {
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Claude is not connected");
        ImGui::SameLine();
        ImGui::TextDisabled("(run python remote/uih_chat.py)");
    }
    const char* kNew = "New conversation";
    ImGui::SameLine();
    // At the right edge.
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(kNew).x - style.FramePadding.x * 2.0f));
    if (ImGui::SmallButton(kNew)) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_outbox.push_back("reset");
        AddChat(Speaker::Note, "New conversation: Claude forgets what was said before.");
    }

    float footer = ImGui::GetFrameHeightWithSpacing() * (status.empty() ? 1.0f : 2.0f);
    if (ImGui::BeginChild("chat_messages", ImVec2(0.0f, -footer), ImGuiChildFlags_Borders)) {
        if (chat.empty())
            ImGui::TextDisabled("Ask Claude about the game, or tell it what to do:\n  What do you see?\n  Find my health and keep it at 100.\n"
                                "  Drag the gem into the chest.\n  Type hello in the chat of the game.");
        const ImVec4 kColors[] = {ImVec4(0.45f, 0.7f, 1.0f, 1.0f), ImVec4(0.85f, 0.55f, 0.35f, 1.0f), ImVec4(0.6f, 0.6f, 0.6f, 1.0f)};
        for (const ChatLine& line : chat) {
            int speaker = static_cast<int>(line.speaker);
            if (line.speaker != Speaker::Note)
                ImGui::TextColored(kColors[speaker], "%s", line.speaker == Speaker::Player ? "You" : "Claude");
            if (line.speaker == Speaker::Note)
                ImGui::PushStyleColor(ImGuiCol_Text, kColors[speaker]);
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(line.text.c_str());
            ImGui::PopTextWrapPos();
            if (line.speaker == Speaker::Note)
                ImGui::PopStyleColor();
            ImGui::Spacing();
        }
        if (scroll || ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f)
            ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();

    if (!status.empty()) {
        static const char* const kDots[] = {"", ".", "..", "..."};
        ImGui::TextColored(ImVec4(0.85f, 0.55f, 0.35f, 1.0f), "Claude is working%-3s", kDots[static_cast<int>(ImGui::GetTime() * 3.0) % 4]);
        ImGui::SameLine();
        ImGui::TextDisabled("%s", status.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Stop")) {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_outbox.push_back("cancel");
        }
    }
    static char input[2048] = "";
    const char* kSend = "Send";
    ImGui::SetNextItemWidth(-(ImGui::CalcTextSize(kSend).x + style.FramePadding.x * 2.0f + style.ItemSpacing.x));
    bool send = ImGui::InputTextWithHint("##chat_input", "Message Claude...", input, sizeof(input), ImGuiInputTextFlags_EnterReturnsTrue);
    if (send)
        ImGui::SetKeyboardFocusHere(-1);
    ImGui::SameLine();
    send |= ImGui::Button(kSend);
    std::string text = input;
    size_t first = text.find_first_not_of(" \t");
    if (send && first != std::string::npos) {
        text = text.substr(first, text.find_last_not_of(" \t") - first + 1);
        std::lock_guard<std::mutex> lock(g_mutex);
        AddChat(Speaker::Player, text);
        g_outbox.push_back("message " + EscapeLine(text));
        if (!connected && !g_waitingNoted) {
            AddChat(Speaker::Note, "The message waits for Claude: run python remote/uih_chat.py next to the game.");
            g_waitingNoted = true;
        }
        input[0] = 0;
    }
}

void DrawWrites() {
    static char address[256] = "";
    static int size = 2;
    static bool access = false;
    static const char* const kSizes[] = {"1 byte", "2 bytes", "4 bytes", "8 bytes"};
    static std::string error;

    ImGui::TextWrapped("Finds the code that changes a value, as the data breakpoint of a debugger does: then patch it with memory_write, "
                       "or ask the assistant to.");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
    ImGui::InputTextWithHint("##address", "address of the value", address, sizeof(address));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5.0f);
    ImGui::Combo("##size", &size, kSizes, sizeof(void*) == 8 ? 4 : 3);
    ImGui::SameLine();
    ImGui::Checkbox("Reads too", &access);
    bool watching = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        watching = g_writesAddress != 0;
    }
    if (watching ? ImGui::Button("Stop") : ImGui::Button("Start")) {
        if (watching) {
            StopWrites();
            error.clear();
        } else {
            error = StartWrites(address, 1u << size, access);
        }
    }
    ImGui::SameLine();
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        ImGui::TextWrapped("%s", error.empty() ? g_writesStatus.c_str() : error.c_str());
    }
    std::vector<InstructionHit> hits = InstructionHits();
    if (hits.empty())
        return;
    std::vector<LoadedModule> modules;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        modules = Modules();
    }
    if (!ImGui::BeginTable("writes", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable))
        return;
    ImGui::TableSetupColumn("After the instruction");
    ImGui::TableSetupColumn("Hits", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("Bytes before");
    ImGui::TableHeadersRow();
    for (const InstructionHit& hit : hits) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::Text("%s", LocateAddress(hit.next, modules).c_str());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", Hex(hit.next).c_str());
        ImGui::TableNextColumn();
        ImGui::Text("%llu", static_cast<unsigned long long>(hit.count));
        ImGui::TableNextColumn();
        uint8_t before[16] = {};
        if (ReadMemory(hit.next - sizeof(before), before, sizeof(before))) {
            char bytes[16 * 3 + 1] = {};
            for (size_t i = 0; i < sizeof(before); ++i)
                snprintf(bytes + i * 3, 4, "%02x ", before[i]);
            ImGui::TextUnformatted(bytes);
        }
    }
    ImGui::EndTable();
}

} // namespace

bool InputAllowed() { return g_inputAllowed; }

bool MemoryAllowed() { return g_memoryAllowed; }

void RecordActivity(const std::string& line, const std::string& answer) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_activity.push_back({Clock::now(), Shorten(line), Shorten(answer)});
    while (g_activity.size() > kMaxActivity)
        g_activity.pop_front();
}

bool HandleAdvanced(const std::string& command, const std::string& rest, std::vector<uint8_t>& payload, std::string& answer) {
    if (command != "watch" && command != "freeze" && command != "unfreeze" && command != "writes" && command != "chat")
        return false;
    const Config& config = GetConfig();
    if (!config.aiAdvancedFeatures)
        answer = "error the advanced features are off: set config.aiAdvancedFeatures in example/config.cpp and rebuild the library";
    else if (command == "chat")
        answer = ChatCommand(rest, payload);
    else if (!config.remoteMemory)
        answer = "error memory access is off: set config.remoteMemory in example/config.cpp and rebuild the library";
    else if (!MemoryAllowed())
        answer = "error the player paused the memory tools in the AI Advanced Features window of the overlay";
    else if (command == "watch")
        answer = WatchCommand(rest, payload);
    else if (command == "writes")
        answer = WritesCommand(rest, payload);
    else
        answer = FreezeCommand(command == "freeze", rest);
    return true;
}

void Tick() {
    const Config& config = GetConfig();
    if (!config.remoteControl || !config.aiAdvancedFeatures)
        return;
    RefreshInstructionWatch();
    if (!config.remoteMemory || !g_memoryAllowed)
        return;
    std::unique_lock<std::mutex> lock(g_mutex, std::try_to_lock);
    if (!lock.owns_lock())
        return;
    for (const Watch& watch : g_watches) {
        if (!watch.frozen)
            continue;
        uintptr_t address = 0;
        uint8_t bytes[8] = {};
        size_t size = ValueSize(watch.type);
        // Writes only what changed: code pages change protection for a write.
        if (ResolveAddress(watch.expression, Modules(), address) && ReadMemory(address, bytes, size) && memcmp(bytes, watch.frozenBytes, size) != 0)
            WriteMemory(address, watch.frozenBytes, size);
    }
}

void StopAdvanced() {
    g_scanCancel = true;
    if (g_scanThread.joinable())
        g_scanThread.join();
    ShutdownInstructionWatch();
    std::lock_guard<std::mutex> lock(g_mutex);
    g_writesAddress = 0;
    g_writesStatus.clear();
}

} // namespace uih::detail::remote

namespace uih {

bool AIAdvancedFeaturesEnabled() {
    const Config& config = detail::GetConfig();
    return config.remoteControl && config.aiAdvancedFeatures;
}

void DrawAIAdvancedFeatures(bool* open) {
    using namespace detail::remote;
    if (!AIAdvancedFeaturesEnabled())
        return;
    const Config& config = detail::GetConfig();
    ImGui::SetNextWindowSize(ImVec2(620.0f, 460.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("AI Advanced Features", open)) {
        ImGui::End();
        return;
    }
    ImGui::Text("Remote control on %s", config.remotePipe);
    ImGui::SameLine();
    ImGui::TextDisabled("(memory tools %s)", config.remoteMemory ? "on" : "off");
    bool input = g_inputAllowed;
    if (ImGui::Checkbox("Let the assistant press keys, click, type and move the mouse", &input))
        g_inputAllowed = input;
    ImGui::BeginDisabled(!config.remoteMemory);
    bool memory = g_memoryAllowed;
    if (ImGui::Checkbox("Let the assistant read and change the memory of the game", &memory))
        g_memoryAllowed = memory;
    ImGui::EndDisabled();

    if (ImGui::BeginTabBar("ai_tabs")) {
        if (ImGui::BeginTabItem("Chat")) {
            DrawChat();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Activity")) {
            DrawActivity();
            ImGui::EndTabItem();
        }
        const char* const kMemoryTabs[] = {"Watch list", "Pointers", "What writes"};
        void (*const kDraw[])() = {DrawWatchList, DrawPointers, DrawWrites};
        for (int i = 0; i < 3; ++i) {
            if (!ImGui::BeginTabItem(kMemoryTabs[i]))
                continue;
            if (MemoryOn())
                kDraw[i]();
            else
                ImGui::TextWrapped("Needs config.remoteMemory = true in example/config.cpp as well: it gives the assistant, and this window, "
                                   "access to the memory of the game.");
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

} // namespace uih
