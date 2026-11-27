// The Engine tab of the "AI Advanced Features" window: the classes, structs
// and enums of a Unity or Unreal Engine game, the properties and functions
// of one, its objects and their values, live and editable, a form to call a
// function, and the C++ SDK of an Unreal game (src/sdk.cpp).
//
// It runs the engine commands of src/engine.cpp, as the assistant does, on a
// thread of its own: they read the memory of the game, which may take a
// moment, and a call waits for the game thread, which the render thread
// that draws this tab must never do. The tab keeps what they answered and
// draws it every frame; its lists are filtered when the filter changes, and
// only their visible rows are drawn.
#include "internal.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#ifndef _WIN32
#include <pthread.h>
#include <signal.h>
#endif

namespace uih::detail::remote {
namespace {

using Clock = std::chrono::steady_clock;

// What the tab asks: one command at a time per slot, the last one wins.
enum SlotId { kInfo, kTypes, kClass, kInstances, kValues, kSet, kCall, kDump, kSlotCount };

struct Slot {
    bool pending = false;
    std::string command;
    bool running = false;
    // Counts the answers, for the tab to notice a new one.
    unsigned answers = 0;
    bool ok = false;
    std::string text;
};

std::mutex g_mutex;
std::condition_variable g_wake;
Slot g_slots[kSlotCount];
std::thread g_thread;
bool g_stop = false;

void Work() {
    // Lists everything: the tab filters and clips its lists itself.
    EngineListLimit() = SIZE_MAX;
    std::unique_lock<std::mutex> lock(g_mutex);
    while (!g_stop) {
        int next = -1;
        for (int i = 0; i < kSlotCount && next < 0; ++i)
            if (g_slots[i].pending)
                next = i;
        if (next < 0) {
            g_wake.wait(lock);
            continue;
        }
        Slot& slot = g_slots[next];
        std::string command = slot.command;
        slot.pending = false;
        slot.running = true;
        lock.unlock();
        std::vector<uint8_t> payload;
        std::string answer;
        HandleEngine("engine", command, payload, answer);
        bool ok = answer.rfind("ok", 0) == 0;
        std::string text = !payload.empty() ? std::string(payload.begin(), payload.end())
                           : ok             ? answer.substr(std::min<size_t>(3, answer.size()))
                           : answer.rfind("error ", 0) == 0 ? answer.substr(6)
                                                            : answer;
        lock.lock();
        slot.running = false;
        slot.ok = ok;
        slot.text = std::move(text);
        ++slot.answers;
    }
}

void Submit(SlotId id, const std::string& command) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_thread.joinable()) {
        g_stop = false;
#ifndef _WIN32
        // The signals of the game go to its own threads.
        sigset_t all;
        sigset_t previous;
        sigfillset(&all);
        pthread_sigmask(SIG_SETMASK, &all, &previous);
#endif
        try {
            g_thread = std::thread(Work);
        } catch (const std::exception&) {
        }
#ifndef _WIN32
        pthread_sigmask(SIG_SETMASK, &previous, nullptr);
#endif
        if (!g_thread.joinable())
            return;
    }
    g_slots[id].pending = true;
    g_slots[id].command = command;
    g_wake.notify_one();
}

bool Busy(SlotId id) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_slots[id].pending || g_slots[id].running;
}

// The answer of a slot when it is new since `seen`.
bool Answered(SlotId id, unsigned& seen, bool& ok, std::string& text) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const Slot& slot = g_slots[id];
    if (slot.answers == seen)
        return false;
    seen = slot.answers;
    ok = slot.ok;
    text = slot.text;
    return true;
}

// A word of the protocol: as it is, or quoted when it has spaces or quotes.
std::string Word(const std::string& text) {
    if (!text.empty() && text.find_first_of(" \t\"") == std::string::npos)
        return text;
    std::string out = "\"";
    for (char c : text) {
        if (c == '"' || c == '\\')
            out += '\\';
        out += c;
    }
    return out + "\"";
}

std::string Lowered(std::string text) {
    for (char& c : text)
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return text;
}

std::vector<std::string> Lines(const std::string& text) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos)
            end = text.size();
        lines.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return lines;
}

// A word of a signature: a type, whose <...> may hold spaces, or a name, maybe quoted.
std::string ReadToken(const std::string& text, size_t& at, const char* stops) {
    while (at < text.size() && text[at] == ' ')
        ++at;
    std::string out;
    if (at < text.size() && text[at] == '"') {
        for (++at; at < text.size() && text[at] != '"'; ++at) {
            if (text[at] == '\\' && at + 1 < text.size())
                ++at;
            out += text[at];
        }
        ++at;
        return out;
    }
    int depth = 0;
    for (; at < text.size(); ++at) {
        char c = text[at];
        if (c == '<' || c == '[')
            ++depth;
        else if ((c == '>' || c == ']') && depth > 0)
            --depth;
        else if (depth == 0 && strchr(stops, c))
            break;
        out += c;
    }
    return out;
}

struct Parameter {
    std::string type;
    std::string name;
    // out: given back only; ref: given and given back.
    std::string mode;
};

struct Function {
    std::string line;
    std::string name;
    std::string returned;
    bool isStatic = false;
    std::vector<Parameter> parameters;
};

// [static ][abstract ]<return type> <name>(<mode> <type> <name>, ...), as engine class lists them.
bool ParseFunction(const std::string& line, Function& function) {
    function = Function();
    function.line = line;
    size_t at = 0;
    for (;;) {
        size_t before = at;
        std::string word = ReadToken(line, at, " (");
        if (word == "static") {
            function.isStatic = true;
        } else if (word != "abstract") {
            at = before;
            break;
        }
    }
    function.returned = ReadToken(line, at, " (");
    function.name = ReadToken(line, at, "(");
    while (at < line.size() && line[at] == ' ')
        ++at;
    if (function.name.empty() || at >= line.size() || line[at] != '(')
        return false;
    ++at;
    while (at < line.size() && line[at] != ')') {
        Parameter parameter;
        size_t before = at;
        std::string word = ReadToken(line, at, " ,)");
        if (word == "out" || word == "ref")
            parameter.mode = word;
        else
            at = before;
        parameter.type = ReadToken(line, at, " ,)");
        parameter.name = ReadToken(line, at, ",)");
        while (at < line.size() && line[at] == ' ')
            ++at;
        if (at < line.size() && line[at] == ',')
            ++at;
        if (parameter.type.empty())
            break;
        function.parameters.push_back(parameter);
    }
    return true;
}

bool IsJsonLiteral(const std::string& value) {
    if (value == "true" || value == "false" || value == "null")
        return true;
    if (value.empty())
        return false;
    char* end = nullptr;
    strtod(value.c_str(), &end);
    return end && !*end;
}

// A value of the call form as JSON: numbers and booleans as they are, texts
// and enum names quoted, objects and arrays such as {"X":1,"Y":2} as written.
std::string JsonValue(const Parameter& parameter, std::string value) {
    value.erase(0, value.find_first_not_of(' '));
    value.erase(value.find_last_not_of(' ') + 1);
    std::string type = Lowered(parameter.type);
    bool text = type == "fstring" || type == "fname" || type == "ftext" || type == "string" || type == "system.string";
    if (!value.empty() && (value[0] == '{' || value[0] == '[' || value[0] == '"'))
        return value;
    if (!text && IsJsonLiteral(value))
        return value;
    return JsonQuote(value);
}

// What the tab shows, on the render thread only.
struct TypeItem {
    std::string package;
    std::string name;
    std::string lowerName;
    std::string lowerPackage;
};

struct ValueRow {
    std::string offset;
    std::string type;
    std::string name;
    std::string value;
};

struct Instance {
    std::string address;
    std::string label;
};

struct View {
    bool started = false;
    unsigned seen[kSlotCount] = {};
    std::string info;
    bool infoOk = false;

    std::vector<TypeItem> types;
    std::string typesStatus = "Loading...";
    char filter[128] = {};
    std::string appliedFilter = "\x01";
    std::vector<int> filtered;

    int selected = -1;
    std::string reference;
    std::string header;
    std::string kind;
    std::vector<std::string> parents;
    std::vector<std::string> properties;
    std::vector<Function> functions;
    std::vector<std::string> notes;
    std::string classError;

    std::vector<Instance> instances;
    std::string instancesStatus;
    std::string object;
    std::vector<ValueRow> values;
    std::string valuesError;
    bool live = true;
    Clock::time_point valuesAt;
    std::string editName;
    char edit[512] = {};
    std::string setStatus;

    int function = -1;
    std::vector<std::string> inputs;
    std::string callResult;
    bool callOk = false;

    char scope[256] = {};
    std::string dumpResult;
    std::string dumpPath;
    bool dumpOk = false;
};

View& GetView() {
    static View view;
    return view;
}

void SelectType(View& view, int index) {
    if (index < 0 || index >= static_cast<int>(view.types.size()))
        return;
    view.selected = index;
    const TypeItem& item = view.types[static_cast<size_t>(index)];
    view.reference = Word(item.package + ":" + item.name);
    view.header.clear();
    view.kind.clear();
    view.parents.clear();
    view.properties.clear();
    view.functions.clear();
    view.notes.clear();
    view.classError.clear();
    view.instances.clear();
    view.instancesStatus.clear();
    view.object.clear();
    view.values.clear();
    view.valuesError.clear();
    view.editName.clear();
    view.setStatus.clear();
    view.function = -1;
    view.callResult.clear();
    Submit(kClass, "class " + view.reference);
}

void SelectByName(View& view, const std::string& name) {
    for (size_t i = 0; i < view.types.size(); ++i)
        if (view.types[i].name == name) {
            SelectType(view, static_cast<int>(i));
            return;
        }
}

void ParseClass(View& view, const std::string& text) {
    std::vector<std::string> lines = Lines(text);
    view.header = lines.empty() ? "" : lines[0];
    view.kind = view.header.substr(0, view.header.find(' '));
    std::string lineage = view.header;
    size_t package = lineage.rfind(" (");
    if (package != std::string::npos)
        lineage.erase(package);
    // Unreal: class A : B : C (package); Unity: class A : B, C (assembly).
    size_t colon = lineage.find(" : ");
    size_t start = colon == std::string::npos ? lineage.size() : colon + 3;
    while (start < lineage.size()) {
        size_t next = lineage.find(" : ", start);
        size_t comma = lineage.find(", ", start);
        size_t skip = 3;
        if (comma != std::string::npos && (next == std::string::npos || comma < next)) {
            next = comma;
            skip = 2;
        }
        view.parents.push_back(lineage.substr(start, next == std::string::npos ? std::string::npos : next - start));
        start = next == std::string::npos ? lineage.size() : next + skip;
    }
    std::vector<std::string>* section = nullptr;
    bool functions = false;
    for (size_t i = 1; i < lines.size(); ++i) {
        const std::string& line = lines[i];
        if (line == "properties:" || line == "fields:" || line == "values:") {
            section = &view.properties;
            functions = false;
        } else if (line == "functions:" || line == "methods:") {
            section = nullptr;
            functions = true;
        } else if (line.rfind("  ", 0) == 0 && functions) {
            Function function;
            if (ParseFunction(line.substr(2), function))
                view.functions.push_back(function);
        } else if (line.rfind("  ", 0) == 0 && section) {
            section->push_back(line.substr(2));
        } else if (!line.empty()) {
            view.notes.push_back(line);
            functions = false;
            section = nullptr;
        }
    }
}

void ParseValues(View& view, const std::string& text) {
    view.values.clear();
    for (const std::string& line : Lines(text)) {
        ValueRow row;
        size_t a = line.find('\t');
        size_t b = a == std::string::npos ? a : line.find('\t', a + 1);
        size_t c = b == std::string::npos ? b : line.find('\t', b + 1);
        if (c == std::string::npos)
            continue;
        row.offset = line.substr(0, a);
        row.type = line.substr(a + 1, b - a - 1);
        row.name = line.substr(b + 1, c - b - 1);
        row.value = line.substr(c + 1);
        view.values.push_back(row);
    }
}

// The answers that came since the last frame.
void Collect(View& view) {
    bool ok = false;
    std::string text;
    if (Answered(kInfo, view.seen[kInfo], ok, text)) {
        view.infoOk = ok && text != "none";
        view.info = view.infoOk ? text : ok ? "The game runs no engine whose objects the library knows: Unity with Mono or IL2CPP, Unreal Engine 4 or 5." : text;
    }
    if (Answered(kTypes, view.seen[kTypes], ok, text)) {
        view.types.clear();
        view.typesStatus = ok ? "" : text;
        if (ok)
            for (const std::string& line : Lines(text)) {
                size_t space = line.find(' ');
                if (space == std::string::npos || line[0] == '+')
                    continue;
                TypeItem item;
                item.package = line.substr(0, space);
                item.name = line.substr(space + 1);
                item.lowerName = Lowered(item.name);
                item.lowerPackage = Lowered(item.package);
                view.types.push_back(item);
            }
        view.appliedFilter = "\x01";
        view.selected = -1;
    }
    if (Answered(kClass, view.seen[kClass], ok, text)) {
        if (ok)
            ParseClass(view, text);
        else
            view.classError = text;
    }
    if (Answered(kInstances, view.seen[kInstances], ok, text)) {
        view.instances.clear();
        view.instancesStatus = ok ? "" : text;
        if (ok) {
            for (const std::string& line : Lines(text)) {
                size_t space = line.find(' ');
                view.instances.push_back({line.substr(0, space), space == std::string::npos ? "" : line.substr(space + 1)});
            }
            if (view.instances.empty())
                view.instancesStatus = "No object of this class is loaded.";
        }
    }
    if (Answered(kValues, view.seen[kValues], ok, text)) {
        view.valuesError = ok ? "" : text;
        if (ok)
            ParseValues(view, text);
    }
    if (Answered(kSet, view.seen[kSet], ok, text)) {
        view.setStatus = ok ? "Written, read back: " + text : text;
        view.valuesAt = Clock::time_point();
    }
    if (Answered(kCall, view.seen[kCall], ok, text)) {
        view.callOk = ok;
        view.callResult = text;
        view.valuesAt = Clock::time_point();
    }
    if (Answered(kDump, view.seen[kDump], ok, text)) {
        view.dumpOk = ok;
        view.dumpResult = text;
        view.dumpPath = ok ? text.substr(0, text.find('\n')) : "";
    }
}

void DrawTypes(View& view) {
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##filter", "search classes, structs, enums, packages", view.filter, sizeof(view.filter));
    std::string filter = Lowered(view.filter);
    if (filter != view.appliedFilter) {
        view.appliedFilter = filter;
        view.filtered.clear();
        for (size_t i = 0; i < view.types.size(); ++i) {
            const TypeItem& item = view.types[i];
            if (filter.empty() || item.lowerName.find(filter) != std::string::npos || item.lowerPackage.find(filter) != std::string::npos)
                view.filtered.push_back(static_cast<int>(i));
        }
    }
    if (!view.typesStatus.empty())
        ImGui::TextWrapped("%s", view.typesStatus.c_str());
    ImGui::TextDisabled("%d of %d", static_cast<int>(view.filtered.size()), static_cast<int>(view.types.size()));
    if (!ImGui::BeginChild("types_list", ImVec2(0, 0), false))
        return ImGui::EndChild();
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(view.filtered.size()));
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            int index = view.filtered[static_cast<size_t>(row)];
            const TypeItem& item = view.types[static_cast<size_t>(index)];
            ImGui::PushID(index);
            if (ImGui::Selectable(item.name.c_str(), view.selected == index))
                SelectType(view, index);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", item.package.c_str());
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
}

void DrawValues(View& view) {
    if (view.object.empty())
        return;
    bool pending = Busy(kValues);
    if (view.live && !pending && Clock::now() - view.valuesAt > std::chrono::milliseconds(500)) {
        Submit(kValues, "get " + view.reference + " * " + view.object);
        view.valuesAt = Clock::now();
    }
    ImGui::Checkbox("Live", &view.live);
    ImGui::SameLine();
    if (ImGui::SmallButton("Refresh"))
        Submit(kValues, "get " + view.reference + " * " + view.object);
    ImGui::SameLine();
    ImGui::TextDisabled("values of %s, Enter writes", view.object.c_str());
    if (!view.valuesError.empty())
        ImGui::TextWrapped("%s", view.valuesError.c_str());
    if (!view.setStatus.empty())
        ImGui::TextWrapped("%s", view.setStatus.c_str());
    if (!ImGui::BeginTable("values", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY,
                           ImVec2(0, 260.0f)))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Offset", ImGuiTableColumnFlags_WidthFixed, 60.0f);
    ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 110.0f);
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 150.0f);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(view.values.size()));
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            const ValueRow& row = view.values[static_cast<size_t>(i)];
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(row.offset.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(row.type.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(row.name.c_str());
            ImGui::TableNextColumn();
            bool editing = view.editName == row.name;
            char shown[512];
            snprintf(shown, sizeof(shown), "%s", row.value.c_str());
            ImGui::SetNextItemWidth(-FLT_MIN);
            char* buffer = editing ? view.edit : shown;
            if (ImGui::InputText("##value", buffer, sizeof(shown), ImGuiInputTextFlags_EnterReturnsTrue)) {
                std::string object = row.offset == "static" ? "-" : view.object;
                Submit(kSet, "set " + view.reference + " " + Word(row.name) + " " + object + " " + buffer);
                view.editName.clear();
            }
            if (ImGui::IsItemActivated()) {
                view.editName = row.name;
                snprintf(view.edit, sizeof(view.edit), "%s", row.value.c_str());
            }
            if (ImGui::IsItemDeactivated() && view.editName == row.name)
                view.editName.clear();
            ImGui::PopID();
        }
    }
    ImGui::EndTable();
}

void DrawCall(View& view) {
    if (view.function < 0 || view.function >= static_cast<int>(view.functions.size())) {
        ImGui::TextDisabled("Select a function above.");
        return;
    }
    const Function& function = view.functions[static_cast<size_t>(view.function)];
    ImGui::TextWrapped("%s", function.line.c_str());
    std::vector<const Parameter*> inputs;
    for (const Parameter& parameter : function.parameters)
        if (parameter.mode != "out")
            inputs.push_back(&parameter);
    view.inputs.resize(inputs.size());
    for (size_t i = 0; i < inputs.size(); ++i) {
        char buffer[256];
        snprintf(buffer, sizeof(buffer), "%s", view.inputs[i].c_str());
        std::string label = inputs[i]->type + " " + inputs[i]->name + "##input" + std::to_string(i);
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::InputText(label.c_str(), buffer, sizeof(buffer)))
            view.inputs[i] = buffer;
    }
    std::string object = function.isStatic || view.object.empty() ? "-" : view.object;
    ImGui::TextDisabled("on %s", object == "-" ? (function.isStatic ? "the class (static)" : "no object: select one in Instances") : object.c_str());
    bool pending = Busy(kCall);
    ImGui::BeginDisabled(pending);
    if (ImGui::Button(pending ? "Calling...###call_run" : "Call###call_run")) {
        std::string json = "[";
        for (size_t i = 0; i < inputs.size(); ++i)
            json += (i ? "," : "") + JsonValue(*inputs[i], view.inputs[i]);
        json += "]";
        view.callResult.clear();
        Submit(kCall, "call " + view.reference + " " + Word(function.name) + " " + object + " " + json);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("on the game thread");
    if (!view.callResult.empty()) {
        if (!view.callOk)
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.4f, 1.0f));
        ImGui::TextWrapped("%s", view.callResult.c_str());
        if (!view.callOk)
            ImGui::PopStyleColor();
    }
}

void DrawClass(View& view) {
    if (view.selected < 0) {
        ImGui::TextWrapped("Select a class, struct or enum on the left: its parents, properties and functions, its objects and their values, "
                           "and a form to call its functions.");
        return;
    }
    if (!view.classError.empty()) {
        ImGui::TextWrapped("%s", view.classError.c_str());
        return;
    }
    if (view.header.empty()) {
        ImGui::TextDisabled("Loading...");
        return;
    }
    const TypeItem& item = view.types[static_cast<size_t>(view.selected)];
    ImGui::Text("%s %s", view.kind.c_str(), item.name.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("%s", item.package.c_str());
    if (!view.parents.empty()) {
        ImGui::TextDisabled("parents:");
        for (size_t i = 0; i < view.parents.size(); ++i) {
            ImGui::SameLine();
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::SmallButton(view.parents[i].c_str())) {
                std::string name = view.parents[i];
                ImGui::PopID();
                SelectByName(view, name);
                return;
            }
            ImGui::PopID();
        }
    }
    std::string propertiesTitle = (view.kind == "enum" ? "Values (" : "Properties (") + std::to_string(view.properties.size()) + ")###properties";
    if (ImGui::CollapsingHeader(propertiesTitle.c_str())) {
        if (ImGui::BeginChild("properties", ImVec2(0, 160.0f), true)) {
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(view.properties.size()));
            while (clipper.Step())
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
                    ImGui::TextUnformatted(view.properties[static_cast<size_t>(i)].c_str());
        }
        ImGui::EndChild();
    }
    for (const std::string& note : view.notes)
        ImGui::TextDisabled("%s", note.c_str());
    if (view.kind != "class")
        return;
    std::string functionsTitle = "Functions (" + std::to_string(view.functions.size()) + ")###functions";
    if (ImGui::CollapsingHeader(functionsTitle.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::BeginChild("functions", ImVec2(0, 140.0f), true)) {
            for (size_t i = 0; i < view.functions.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                if (ImGui::Selectable(view.functions[i].line.c_str(), view.function == static_cast<int>(i))) {
                    view.function = static_cast<int>(i);
                    view.inputs.clear();
                    view.callResult.clear();
                }
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
    }
    if (ImGui::CollapsingHeader("Instances and values", ImGuiTreeNodeFlags_DefaultOpen)) {
        bool pending = Busy(kInstances);
        ImGui::BeginDisabled(pending);
        if (ImGui::Button(pending ? "Searching...###find_objects" : view.instances.empty() ? "Find the objects###find_objects" : "Find them again###find_objects"))
            Submit(kInstances, "instances " + view.reference + " 1000");
        ImGui::EndDisabled();
        if (!view.instancesStatus.empty()) {
            ImGui::SameLine();
            ImGui::TextWrapped("%s", view.instancesStatus.c_str());
        }
        if (!view.instances.empty() && ImGui::BeginChild("instances", ImVec2(0, 110.0f), true)) {
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(view.instances.size()));
            while (clipper.Step()) {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                    const Instance& instance = view.instances[static_cast<size_t>(i)];
                    std::string label = instance.address + "  " + instance.label + "##" + std::to_string(i);
                    if (ImGui::Selectable(label.c_str(), view.object == instance.address)) {
                        view.object = instance.address;
                        view.values.clear();
                        view.valuesError.clear();
                        view.setStatus.clear();
                        view.valuesAt = Clock::time_point();
                    }
                }
            }
            ImGui::EndChild();
        } else if (!view.instances.empty()) {
            ImGui::EndChild();
        }
        DrawValues(view);
    }
    if (ImGui::CollapsingHeader("Call", ImGuiTreeNodeFlags_DefaultOpen))
        DrawCall(view);
}

void DrawDump(View& view) {
    ImGui::SetNextItemWidth(200.0f);
    ImGui::InputTextWithHint("##scope", "game (default), all, or packages", view.scope, sizeof(view.scope));
    ImGui::SameLine();
    bool pending = Busy(kDump);
    ImGui::BeginDisabled(pending);
    if (ImGui::Button(pending ? "Writing the SDK...###dump_sdk" : "Dump C++ SDK###dump_sdk")) {
        view.dumpResult.clear();
        view.dumpPath.clear();
        Submit(kDump, std::string("dump ") + view.scope);
    }
    ImGui::EndDisabled();
    if (view.dumpResult.empty())
        return;
    if (!view.dumpOk) {
        ImGui::TextWrapped("%s", view.dumpResult.c_str());
        return;
    }
    char path[1024];
    snprintf(path, sizeof(path), "%s", view.dumpPath.c_str());
    ImGui::SetNextItemWidth(-90.0f);
    ImGui::InputText("##path", path, sizeof(path), ImGuiInputTextFlags_ReadOnly);
    ImGui::SameLine();
    if (ImGui::Button("Copy path"))
        ImGui::SetClipboardText(view.dumpPath.c_str());
    size_t line = view.dumpResult.find('\n');
    if (line != std::string::npos)
        ImGui::TextDisabled("%s", view.dumpResult.substr(line + 1).c_str());
}

} // namespace

void DrawExplorer() {
    View& view = GetView();
    if (!view.started) {
        view.started = true;
        Submit(kInfo, "info");
        Submit(kTypes, "classes *");
    }
    Collect(view);
    ImGui::TextWrapped("%s", view.info.empty() ? "Looking for the engine..." : view.info.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Reload")) {
        Submit(kInfo, "info");
        Submit(kTypes, "classes *");
        view.typesStatus = "Loading...";
    }
    if (view.infoOk && view.info.rfind("unreal", 0) == 0)
        DrawDump(view);
    float left = std::max(180.0f, ImGui::GetContentRegionAvail().x * 0.32f);
    if (ImGui::BeginChild("explorer_types", ImVec2(left, 0), true))
        DrawTypes(view);
    ImGui::EndChild();
    ImGui::SameLine();
    if (ImGui::BeginChild("explorer_class", ImVec2(0, 0), true))
        DrawClass(view);
    ImGui::EndChild();
}

void StopExplorer() {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_stop = true;
        for (Slot& slot : g_slots)
            slot.pending = false;
        g_wake.notify_all();
    }
    if (g_thread.joinable())
        g_thread.join();
    std::lock_guard<std::mutex> lock(g_mutex);
    for (Slot& slot : g_slots)
        slot = Slot();
    GetView() = View();
}

} // namespace uih::detail::remote
