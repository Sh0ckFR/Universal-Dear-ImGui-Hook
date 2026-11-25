// Memory commands of the remote control, served when Config::remoteMemory is
// set as well, for an assistant that finds and changes values of the game:
//   modules                     -> ok <bytes>, then lines: <base> <size> <name>
//   read <address> <size>       -> ok <size>, then the bytes
//   write <address> <hex bytes> -> ok
//   find <type> <value>         -> ok <matches> [kept <count>]  (new scan of the writable memory)
//   next <value>|changed|unchanged|increased|decreased -> ok <matches>  (narrows the last scan)
//   results <count>             -> ok <bytes>, then lines: <address> <value> <module+offset>
//   pattern <bytes> [module]    -> ok <bytes>, then lines: <address> <module+offset>  (?? for any byte)
// Addresses: hexadecimal, or <module>+<hex offset>. Types: i8, i16, i32, i64,
// f32, f64. Values of floating-point types match when they round to the
// value given, to as many decimals as it has.
#include "internal.h"

#include <algorithm>
#include <cctype>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>

namespace uih::detail::remote {
namespace {

constexpr size_t kMaxRead = 65536;
constexpr size_t kChunk = 1 << 20;
// Matches a scan keeps, 16 bytes each.
constexpr size_t kMaxMatches = size_t{1} << 21;
constexpr size_t kMaxListed = 1000;

enum class Type { I8, I16, I32, I64, F32, F64 };

struct Match {
    uintptr_t address;
    uint64_t value;
};

// The scan's own memory, out of the heap, which it would otherwise find
// copies of the values in: the chunk read, then the matches.
struct ScanMemory {
    uint8_t* chunk = nullptr;
    Match* matches = nullptr;
};

std::mutex g_scanMutex;
ScanMemory g_memory;
Type g_type = Type::I32;
size_t g_count = 0;

size_t Size(Type type) {
    static const size_t kSizes[] = {1, 2, 4, 8, 4, 8};
    return kSizes[static_cast<int>(type)];
}

bool IsFloat(Type type) { return type == Type::F32 || type == Type::F64; }

bool ParseType(std::string name, Type& type) {
    for (char& c : name)
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    static const struct {
        const char* name;
        Type type;
    } kNames[] = {{"i8", Type::I8},   {"byte", Type::I8},   {"i16", Type::I16},    {"short", Type::I16}, {"i32", Type::I32},
                  {"int", Type::I32}, {"i64", Type::I64},   {"long", Type::I64},   {"f32", Type::F32},   {"float", Type::F32},
                  {"f64", Type::F64}, {"double", Type::F64}};
    for (const auto& entry : kNames) {
        if (name == entry.name) {
            type = entry.type;
            return true;
        }
    }
    return false;
}

uint64_t Load(const uint8_t* bytes, Type type) {
    uint64_t value = 0;
    memcpy(&value, bytes, Size(type));
    return value;
}

double AsDouble(uint64_t bits, Type type) {
    if (type == Type::F32) {
        float value = 0.0f;
        uint32_t low = static_cast<uint32_t>(bits);
        memcpy(&value, &low, sizeof(value));
        return value;
    }
    if (type == Type::F64) {
        double value = 0.0;
        memcpy(&value, &bits, sizeof(value));
        return value;
    }
    int shift = 64 - static_cast<int>(Size(type)) * 8;
    return static_cast<double>(static_cast<int64_t>(bits << shift) >> shift);
}

std::string Format(uint64_t bits, Type type) {
    char text[64];
    if (IsFloat(type)) {
        snprintf(text, sizeof(text), "%.6g", AsDouble(bits, type));
    } else {
        int shift = 64 - static_cast<int>(Size(type)) * 8;
        int64_t value = static_cast<int64_t>(bits << shift) >> shift;
        snprintf(text, sizeof(text), "%" PRId64, value);
    }
    return text;
}

// A value to match: the bits of an integer, or a range of floating-point values.
struct Target {
    uint64_t bits = 0;
    double low = 0.0;
    double high = 0.0;
};

bool ParseTarget(const std::string& text, Type type, Target& target) {
    if (text.empty())
        return false;
    char* end = nullptr;
    if (IsFloat(type)) {
        double value = strtod(text.c_str(), &end);
        if (*end || !std::isfinite(value))
            return false;
        size_t dot = text.find('.');
        int decimals = dot == std::string::npos ? 0 : static_cast<int>(text.size() - dot - 1);
        double half = 0.5 * std::pow(10.0, -decimals);
        target.low = value - half;
        target.high = value + half;
        return true;
    }
    // Signed or unsigned: only the bits of the type's size are compared.
    if (text[0] == '-')
        target.bits = static_cast<uint64_t>(strtoll(text.c_str(), &end, 0));
    else
        target.bits = strtoull(text.c_str(), &end, 0);
    return !*end;
}

uint64_t Mask(Type type) { return Size(type) == 8 ? ~uint64_t{0} : (uint64_t{1} << (Size(type) * 8)) - 1; }

bool Matches(uint64_t bits, Type type, const Target& target) {
    if (IsFloat(type)) {
        double value = AsDouble(bits, type);
        return value >= target.low && value < target.high;
    }
    return ((bits ^ target.bits) & Mask(type)) == 0;
}

bool EnsureScanMemory() {
    if (g_memory.chunk)
        return true;
    auto* memory = static_cast<uint8_t*>(AllocateScanMemory(kChunk + kMaxMatches * sizeof(Match)));
    if (!memory)
        return false;
    g_memory.chunk = memory;
    g_memory.matches = reinterpret_cast<Match*>(memory + kChunk);
    return true;
}

// Parts of `region` outside the scan's own memory.
std::vector<MemoryRegion> OutsideScanMemory(const MemoryRegion& region) {
    uintptr_t ownStart = reinterpret_cast<uintptr_t>(g_memory.chunk);
    uintptr_t ownEnd = ownStart + kChunk + kMaxMatches * sizeof(Match);
    uintptr_t end = region.base + region.size;
    if (end <= ownStart || region.base >= ownEnd)
        return {region};
    std::vector<MemoryRegion> parts;
    if (region.base < ownStart)
        parts.push_back({region.base, ownStart - region.base, region.writable});
    if (end > ownEnd)
        parts.push_back({ownEnd, end - ownEnd, region.writable});
    return parts;
}

std::vector<LoadedModule> SortedModules() {
    std::vector<LoadedModule> modules = LoadedModules();
    std::sort(modules.begin(), modules.end(), [](const LoadedModule& a, const LoadedModule& b) { return a.base < b.base; });
    return modules;
}

std::string Locate(uintptr_t address, const std::vector<LoadedModule>& modules) {
    auto it = std::upper_bound(modules.begin(), modules.end(), address, [](uintptr_t a, const LoadedModule& m) { return a < m.base; });
    if (it == modules.begin())
        return "-";
    --it;
    if (address - it->base >= it->size)
        return "-";
    char offset[32];
    snprintf(offset, sizeof(offset), "+0x%" PRIxPTR, address - it->base);
    return it->name + offset;
}

bool SameName(const std::string& a, const std::string& b) {
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (tolower(static_cast<unsigned char>(a[i])) != tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

// The executable for an empty name.
const LoadedModule* FindModule(const std::vector<LoadedModule>& modules, const std::string& name) {
    for (const LoadedModule& module : modules)
        if (name.empty() ? module.main : SameName(module.name, name))
            return &module;
    return nullptr;
}

bool ParseAddress(const std::string& text, uintptr_t& address) {
    size_t plus = text.rfind('+');
    std::string number = plus == std::string::npos ? text : text.substr(plus + 1);
    uintptr_t base = 0;
    if (plus != std::string::npos) {
        std::vector<LoadedModule> modules = LoadedModules();
        const LoadedModule* module = FindModule(modules, text.substr(0, plus));
        if (!module)
            return false;
        base = module->base;
    }
    if (number.empty())
        return false;
    char* end = nullptr;
    address = base + static_cast<uintptr_t>(strtoull(number.c_str(), &end, 16));
    return !*end;
}

std::string WithPayload(std::string text, std::vector<uint8_t>& payload) {
    payload.assign(text.begin(), text.end());
    return "ok " + std::to_string(payload.size());
}

std::string Modules(std::vector<uint8_t>& payload) {
    std::string text;
    char line[64];
    for (const LoadedModule& module : SortedModules()) {
        snprintf(line, sizeof(line), "0x%" PRIxPTR " 0x%zx ", module.base, module.size);
        text += line + module.name + "\n";
    }
    return WithPayload(text, payload);
}

std::string Read(const std::string& rest, std::vector<uint8_t>& payload) {
    size_t space = rest.find(' ');
    uintptr_t address = 0;
    if (space == std::string::npos || !ParseAddress(rest.substr(0, space), address))
        return "error give an address, hexadecimal or module+offset, and a size";
    size_t size = strtoul(rest.c_str() + space + 1, nullptr, 10);
    if (size == 0 || size > kMaxRead)
        return "error the size is 1 to 65536 bytes";
    payload.resize(size);
    if (!ReadMemory(address, payload.data(), size)) {
        payload.clear();
        return "error the memory is not readable there";
    }
    return "ok " + std::to_string(size);
}

std::string Write(const std::string& rest) {
    size_t space = rest.find(' ');
    uintptr_t address = 0;
    if (space == std::string::npos || !ParseAddress(rest.substr(0, space), address))
        return "error give an address, hexadecimal or module+offset, and bytes in hexadecimal";
    std::vector<uint8_t> bytes;
    std::string hex;
    for (char c : rest.substr(space + 1))
        if (isxdigit(static_cast<unsigned char>(c)))
            hex += c;
    if (hex.empty() || hex.size() % 2)
        return "error give whole bytes in hexadecimal, such as 64 00 00 00";
    for (size_t i = 0; i < hex.size(); i += 2)
        bytes.push_back(static_cast<uint8_t>(strtoul(hex.substr(i, 2).c_str(), nullptr, 16)));
    if (!WriteMemory(address, bytes.data(), bytes.size()))
        return "error the memory cannot be written there";
    return "ok";
}

std::string Find(const std::string& rest) {
    size_t space = rest.find(' ');
    Type type = Type::I32;
    Target target;
    if (space == std::string::npos || !ParseType(rest.substr(0, space), type))
        return "error give a type, i8, i16, i32, i64, f32 or f64, and a value";
    if (!ParseTarget(rest.substr(space + 1), type, target))
        return "error the value is not a number of that type";
    std::lock_guard<std::mutex> lock(g_scanMutex);
    if (!EnsureScanMemory())
        return "error out of memory";
    g_type = type;
    g_count = 0;
    size_t size = Size(type);
    size_t total = 0;
    std::vector<MemoryRegion> regions;
    for (const MemoryRegion& region : MemoryRegions())
        if (region.writable)
            for (const MemoryRegion& part : OutsideScanMemory(region))
                regions.push_back(part);
    for (const MemoryRegion& region : regions) {
        if (StopRequested())
            return "error the remote control is stopping";
        for (size_t offset = 0; offset < region.size; offset += kChunk) {
            size_t length = std::min(kChunk, region.size - offset);
            uintptr_t base = region.base + offset;
            if (!ReadMemory(base, g_memory.chunk, length))
                continue;
            for (size_t at = 0; at + size <= length; at += size) {
                uint64_t bits = Load(g_memory.chunk + at, type);
                if (!Matches(bits, type, target))
                    continue;
                if (g_count < kMaxMatches)
                    g_memory.matches[g_count++] = {base + at, bits};
                ++total;
            }
        }
    }
    if (total > g_count)
        return "ok " + std::to_string(total) + " kept " + std::to_string(g_count);
    return "ok " + std::to_string(total);
}

std::string Next(const std::string& rest) {
    enum class Rule { Value, Changed, Unchanged, Increased, Decreased } rule = Rule::Value;
    static const struct {
        const char* name;
        Rule rule;
    } kRules[] = {{"changed", Rule::Changed}, {"unchanged", Rule::Unchanged}, {"increased", Rule::Increased}, {"decreased", Rule::Decreased}};
    for (const auto& entry : kRules)
        if (rest == entry.name)
            rule = entry.rule;
    std::lock_guard<std::mutex> lock(g_scanMutex);
    if (!g_memory.chunk)
        return "error no scan yet: start one with find";
    Target target;
    if (rule == Rule::Value && !ParseTarget(rest, g_type, target))
        return "error give a value, changed, unchanged, increased or decreased";
    size_t kept = 0;
    // The matches are in address order: one read per page holding some.
    constexpr size_t kPage = 4096;
    uintptr_t page = 0;
    bool readable = false;
    for (size_t i = 0; i < g_count; ++i) {
        Match match = g_memory.matches[i];
        uintptr_t start = match.address & ~uintptr_t{kPage - 1};
        // Aligned values do not cross pages.
        if (start != page || i == 0) {
            page = start;
            readable = ReadMemory(page, g_memory.chunk, kPage);
        }
        if (!readable)
            continue;
        uint64_t bits = Load(g_memory.chunk + (match.address - page), g_type);
        double now = AsDouble(bits, g_type);
        double before = AsDouble(match.value, g_type);
        bool same = ((bits ^ match.value) & Mask(g_type)) == 0;
        bool keep = false;
        switch (rule) {
        case Rule::Value: keep = Matches(bits, g_type, target); break;
        case Rule::Changed: keep = !same; break;
        case Rule::Unchanged: keep = same; break;
        case Rule::Increased: keep = now > before; break;
        case Rule::Decreased: keep = now < before; break;
        }
        if (keep)
            g_memory.matches[kept++] = {match.address, bits};
    }
    g_count = kept;
    return "ok " + std::to_string(kept);
}

std::string Results(const std::string& rest, std::vector<uint8_t>& payload) {
    size_t count = std::min<size_t>(rest.empty() ? 100 : strtoul(rest.c_str(), nullptr, 10), kMaxListed);
    std::lock_guard<std::mutex> lock(g_scanMutex);
    std::vector<LoadedModule> modules = SortedModules();
    std::string text;
    char line[64];
    for (size_t i = 0; i < std::min(count, g_count); ++i) {
        const Match& match = g_memory.matches[i];
        uint8_t bytes[8] = {};
        uint64_t bits = ReadMemory(match.address, bytes, Size(g_type)) ? Load(bytes, g_type) : match.value;
        snprintf(line, sizeof(line), "0x%" PRIxPTR " ", match.address);
        text += line + Format(bits, g_type) + " " + Locate(match.address, modules) + "\n";
    }
    return WithPayload(text, payload);
}

std::string Pattern(const std::string& rest, std::vector<uint8_t>& payload) {
    std::vector<int> pattern;
    std::string moduleName;
    size_t at = 0;
    while (at < rest.size()) {
        size_t end = rest.find(' ', at);
        std::string token = rest.substr(at, end == std::string::npos ? std::string::npos : end - at);
        at = end == std::string::npos ? rest.size() : end + 1;
        if (token.empty())
            continue;
        if (token == "?" || token == "??")
            pattern.push_back(-1);
        else if (token.size() == 2 && isxdigit(static_cast<unsigned char>(token[0])) && isxdigit(static_cast<unsigned char>(token[1])))
            pattern.push_back(static_cast<int>(strtoul(token.c_str(), nullptr, 16)));
        else if (end == std::string::npos && !pattern.empty())
            moduleName = token;
        else
            return "error give bytes in hexadecimal, ?? for any byte, then optionally a module";
    }
    if (pattern.empty() || pattern.size() > 256)
        return "error give 1 to 256 bytes";
    std::vector<LoadedModule> modules = SortedModules();
    const LoadedModule* module = FindModule(modules, moduleName);
    if (!module)
        return "error no such module: list them with modules";
    std::lock_guard<std::mutex> lock(g_scanMutex);
    if (!EnsureScanMemory())
        return "error out of memory";
    // The readable parts of the module: segments aligned to 64 KiB, as on
    // aarch64, leave unmapped pages between them.
    std::vector<MemoryRegion> spans;
    uintptr_t moduleEnd = module->base + module->size;
    for (const MemoryRegion& region : MemoryRegions()) {
        uintptr_t start = std::max(region.base, module->base);
        uintptr_t end = std::min(region.base + region.size, moduleEnd);
        if (start >= end)
            continue;
        if (!spans.empty() && spans.back().base + spans.back().size == start)
            spans.back().size += end - start;
        else
            spans.push_back({start, end - start, region.writable});
    }
    std::string text;
    size_t found = 0;
    char line[48];
    // Chunks overlap by the length of the pattern, less one byte.
    size_t step = kChunk - (pattern.size() - 1);
    for (const MemoryRegion& span : spans) {
        for (size_t offset = 0; offset < span.size && found < kMaxListed; offset += step) {
            size_t length = std::min(kChunk, span.size - offset);
            if (length < pattern.size() || !ReadMemory(span.base + offset, g_memory.chunk, length))
                continue;
            for (size_t i = 0; i + pattern.size() <= length && found < kMaxListed; ++i) {
                size_t j = 0;
                while (j < pattern.size() && (pattern[j] < 0 || g_memory.chunk[i + j] == pattern[j]))
                    ++j;
                if (j < pattern.size())
                    continue;
                uintptr_t address = span.base + offset + i;
                snprintf(line, sizeof(line), "0x%" PRIxPTR " ", address);
                text += line + Locate(address, modules) + "\n";
                ++found;
            }
        }
    }
    return WithPayload(text, payload);
}

} // namespace

bool HandleMemory(const std::string& command, const std::string& rest, std::vector<uint8_t>& payload, std::string& answer) {
    static const char* const kCommands[] = {"modules", "read", "write", "find", "next", "results", "pattern"};
    if (std::find_if(std::begin(kCommands), std::end(kCommands), [&](const char* name) { return command == name; }) == std::end(kCommands))
        return false;
    if (!GetConfig().remoteMemory) {
        answer = "error memory access is off: set config.remoteMemory in example/config.cpp and rebuild the library";
        return true;
    }
    if (command == "modules")
        answer = Modules(payload);
    else if (command == "read")
        answer = Read(rest, payload);
    else if (command == "write")
        answer = Write(rest);
    else if (command == "find")
        answer = Find(rest);
    else if (command == "next")
        answer = Next(rest);
    else if (command == "results")
        answer = Results(rest, payload);
    else
        answer = Pattern(rest, payload);
    return true;
}

} // namespace uih::detail::remote
