// Hotkeys read from an INI file, with key names shared by Windows and Linux.
#include "internal.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace uih {
namespace {

struct Name {
    const char* name;
    int code;
};

#ifdef _WIN32
constexpr int kF1 = VK_F1;
constexpr int kLetterA = 'A';
constexpr int kDigit0 = '0';
constexpr int kNumpad0 = VK_NUMPAD0;
constexpr Name kKeys[] = {
    {"INSERT", VK_INSERT}, {"DELETE", VK_DELETE}, {"HOME", VK_HOME}, {"END", VK_END},
    {"PAGEUP", VK_PRIOR}, {"PAGEDOWN", VK_NEXT}, {"UP", VK_UP}, {"DOWN", VK_DOWN},
    {"LEFT", VK_LEFT}, {"RIGHT", VK_RIGHT}, {"TAB", VK_TAB}, {"ESCAPE", VK_ESCAPE},
    {"SPACE", VK_SPACE}, {"BACKSPACE", VK_BACK}, {"ENTER", VK_RETURN}, {"PAUSE", VK_PAUSE},
    {"SCROLLLOCK", VK_SCROLL}, {"GRAVE", VK_OEM_3},
};
#else
// X11 keysyms. Letters are the lowercase keysyms, the first one of each key.
constexpr int kF1 = 0xffbe;
constexpr int kLetterA = 'a';
constexpr int kDigit0 = '0';
constexpr int kNumpad0 = 0xffb0;
constexpr Name kKeys[] = {
    {"INSERT", 0xff63}, {"DELETE", 0xffff}, {"HOME", 0xff50}, {"END", 0xff57},
    {"PAGEUP", 0xff55}, {"PAGEDOWN", 0xff56}, {"UP", 0xff52}, {"DOWN", 0xff54},
    {"LEFT", 0xff51}, {"RIGHT", 0xff53}, {"TAB", 0xff09}, {"ESCAPE", 0xff1b},
    {"SPACE", 0x0020}, {"BACKSPACE", 0xff08}, {"ENTER", 0xff0d}, {"PAUSE", 0xff13},
    {"SCROLLLOCK", 0xff14}, {"GRAVE", 0x0060},
};
#endif

constexpr Name kButtons[] = {
    {"A", GamepadA}, {"B", GamepadB}, {"X", GamepadX}, {"Y", GamepadY},
    {"L1", GamepadL1}, {"R1", GamepadR1}, {"L3", GamepadL3}, {"R3", GamepadR3},
    {"START", GamepadStart}, {"BACK", GamepadBack},
    {"DPAD_UP", GamepadDpadUp}, {"DPAD_DOWN", GamepadDpadDown}, {"DPAD_LEFT", GamepadDpadLeft}, {"DPAD_RIGHT", GamepadDpadRight},
};

std::string Trim(const std::string& text) {
    size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos)
        return {};
    size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

std::string Upper(std::string text) {
    for (char& c : text)
        c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    return text;
}

// A key name, NONE, or a raw code such as 0x2D. Returns -1 when unknown.
int ParseKey(const std::string& value) {
    std::string name = Upper(value);
    if (name == "NONE")
        return 0;
    if (name.size() == 1 && name[0] >= 'A' && name[0] <= 'Z')
        return kLetterA + (name[0] - 'A');
    if (name.size() == 1 && name[0] >= '0' && name[0] <= '9')
        return kDigit0 + (name[0] - '0');
    if (name.size() >= 2 && name[0] == 'F' && isdigit(static_cast<unsigned char>(name[1]))) {
        int number = atoi(name.c_str() + 1);
        if (number >= 1 && number <= 24 && name == "F" + std::to_string(number))
            return kF1 + number - 1;
    }
    if (name.size() == 7 && name.compare(0, 6, "NUMPAD") == 0 && isdigit(static_cast<unsigned char>(name[6])))
        return kNumpad0 + (name[6] - '0');
    for (const Name& key : kKeys)
        if (name == key.name)
            return key.code;
    if (name.size() > 2 && name.compare(0, 2, "0X") == 0) {
        char* end = nullptr;
        long code = strtol(name.c_str(), &end, 16);
        if (*end == '\0' && code > 0)
            return static_cast<int>(code);
    }
    return -1;
}

// Button names joined with '+', or NONE. Returns false when a name is unknown.
bool ParseButtons(const std::string& value, unsigned& buttons) {
    std::string names = Upper(value);
    if (names == "NONE") {
        buttons = 0;
        return true;
    }
    unsigned result = 0;
    size_t start = 0;
    while (start <= names.size()) {
        size_t plus = names.find('+', start);
        std::string name = Trim(names.substr(start, plus == std::string::npos ? std::string::npos : plus - start));
        unsigned button = 0;
        for (const Name& entry : kButtons)
            if (name == entry.name)
                button = static_cast<unsigned>(entry.code);
        if (!button)
            return false;
        result |= button;
        if (plus == std::string::npos)
            break;
        start = plus + 1;
    }
    buttons = result;
    return true;
}

void Apply(Config& config, const std::string& entry, const std::string& value) {
    if (entry == "menu_key" || entry == "unload_key") {
        int key = ParseKey(value);
        if (key >= 0)
            (entry == "menu_key" ? config.menuKey : config.unloadKey) = key;
    } else if (entry == "menu_buttons") {
        ParseButtons(value, config.menuButtons);
    }
}

bool Load(Config& config, FILE* file) {
    if (!file)
        return false;
    char line[512];
    while (fgets(line, sizeof(line), file)) {
        std::string text = line;
        size_t comment = text.find_first_of(";#");
        if (comment != std::string::npos)
            text.erase(comment);
        size_t equals = text.find('=');
        if (equals == std::string::npos)
            continue;
        Apply(config, Trim(text.substr(0, equals)), Trim(text.substr(equals + 1)));
    }
    fclose(file);
    return true;
}

} // namespace

#ifdef _WIN32
bool LoadKeyFile(Config& config, const wchar_t* path) {
    FILE* file = nullptr;
    return path && _wfopen_s(&file, path, L"r") == 0 && Load(config, file);
}
#else
bool LoadKeyFile(Config& config, const char* path) {
    return path && Load(config, fopen(path, "r"));
}
#endif

} // namespace uih
