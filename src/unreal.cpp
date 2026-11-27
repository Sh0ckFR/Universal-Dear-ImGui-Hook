// Objects of Unreal Engine 4 and 5 games for the engine commands of
// src/engine.cpp, which hands them the commands when the game runs no Unity
// runtime. Unreal games export nothing to find their objects by, so the
// library looks for them in the data of the executable (or of CoreUObject and
// Core in a modular build), from the layouts of Epic's public engine headers:
//   - the names, FNamePool (4.23 and later): a block table whose first block
//     starts with the entries None and ByteProperty, or TNameEntryArray
//     (earlier), a chunk table of entries;
//   - the objects, GUObjectArray: a chunk table of FUObjectItem whose objects
//     hold their own index at the place of UObjectBase::InternalIndex.
// The offsets that changed between versions (the size of FUObjectItem and the
// place of its object, UObjectBase::OuterPrivate, UStruct::SuperStruct and
// ChildProperties, FField, FProperty) are not assumed: each candidate is
// checked against objects every game has, such as the classes Class, Struct
// and Field and the structs Vector and Transform of /Script/CoreUObject.
// The layouts the calls and the SDK need are found the same way, when first
// needed: UFunction::NumParms, ParmsSize and ReturnValueOffset must match the
// parameters of the functions, UEnum::Names ends with the _MAX value of each
// enum, and UObject::ProcessEvent is the virtual function of the objects
// whose code tests FUNC_Native in UFunction::FunctionFlags (src/code.cpp).
// Nothing runs the game's code, from any thread, except engine call, on the
// game's main thread.
//   engine info                       -> ok unreal ue4|ue5 <version|-> <module>
//   engine assemblies                 -> lines <package> <types>
//   engine classes <filter|*> [pkg]   -> lines <package> <class>
//   engine class <class>              -> the class, its properties with their offset, its functions; the values of an enum
//   engine get <class> <prop|*> <obj> -> <type> <value>, or every property with *
//   engine set <class> <prop> <obj> <value> -> <type> <value>, as read back
//   engine instances <class> [count]  -> lines <address> <class> <name>
//   engine call <class> <function> <obj|-> [JSON array] -> <type> <value>, then the out parameters
//   engine dump [scope]               -> the path of the C++ SDK, src/sdk.cpp
// Names with spaces, frequent in blueprints, are given between double quotes.
#include "unreal.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <unordered_map>

namespace uih::detail::remote::unreal {
namespace {

constexpr size_t kItemsPerChunk = 64 * 1024;
constexpr size_t kMaxInstances = 1000;
// The size of a block of FNamePool, and of a chunk of TNameEntryArray.
constexpr size_t kNameBlock = 2 * 65536;
constexpr size_t kLegacyChunk = 16384;

Unreal g_unreal;

// The types of the game, listed again after a while: blueprints load with the levels.
struct TypeCache {
    std::vector<TypeEntry> types;
    std::chrono::steady_clock::time_point at;
    bool valid = false;
};
TypeCache g_types;

// Memory ------------------------------------------------------------------

template <typename T>
bool Read(uintptr_t address, T& value) {
    return address && ReadMemory(address, &value, sizeof(T));
}

// The readable memory that no module maps, sorted, where the engine allocates.
struct Heap {
    std::vector<AddressRange> regions;
    std::vector<AddressRange> modules;

    Heap() : modules(ModuleMemory()) {
        for (const MemoryRegion& region : MemoryRegions())
            regions.push_back({region.base, region.size});
        std::sort(regions.begin(), regions.end(), [](const AddressRange& a, const AddressRange& b) { return a.base < b.base; });
    }
    bool Contains(uintptr_t address) const { return address && address % P == 0 && InRanges(regions, address) && !InRanges(modules, address); }
};

// Text --------------------------------------------------------------------

std::string Hex(uintptr_t value) {
    char text[32];
    snprintf(text, sizeof(text), "0x%" PRIxPTR, value);
    return text;
}

// The next word of a command: up to a space, or between double quotes, in
// which \" and \\ stand for a quote and a backslash.
std::string NextWord(const std::string& text, size_t& at) {
    while (at < text.size() && text[at] == ' ')
        ++at;
    if (at < text.size() && text[at] == '"') {
        std::string word;
        for (++at; at < text.size() && text[at] != '"'; ++at) {
            if (text[at] == '\\' && at + 1 < text.size())
                ++at;
            word += text[at];
        }
        if (at < text.size())
            ++at;
        return word;
    }
    size_t start = at;
    while (at < text.size() && text[at] != ' ')
        ++at;
    return text.substr(start, at - start);
}

std::string Rest(const std::string& text, size_t at) {
    std::string rest = text.substr(std::min(at, text.size()));
    rest.erase(0, rest.find_first_not_of(' '));
    return rest;
}

std::string Payload(const std::string& text, std::vector<uint8_t>& payload) {
    payload.assign(text.begin(), text.end());
    return "ok " + std::to_string(payload.size());
}

void AppendUtf8(std::string& out, uint32_t point) {
    if (point < 0x80) {
        out += static_cast<char>(point);
    } else if (point < 0x800) {
        out += static_cast<char>(0xc0 | (point >> 6));
        out += static_cast<char>(0x80 | (point & 0x3f));
    } else if (point < 0x10000) {
        out += static_cast<char>(0xe0 | (point >> 12));
        out += static_cast<char>(0x80 | ((point >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (point & 0x3f));
    } else {
        out += static_cast<char>(0xf0 | (point >> 18));
        out += static_cast<char>(0x80 | ((point >> 12) & 0x3f));
        out += static_cast<char>(0x80 | ((point >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (point & 0x3f));
    }
}

std::string FromUtf16(const uint16_t* units, size_t count) {
    std::string out;
    for (size_t i = 0; i < count; ++i) {
        uint32_t point = units[i];
        if (point >= 0xd800 && point < 0xdc00 && i + 1 < count && units[i + 1] >= 0xdc00 && units[i + 1] < 0xe000)
            point = 0x10000 + ((point - 0xd800) << 10) + (units[++i] - 0xdc00);
        AppendUtf8(out, point);
    }
    return out;
}

std::u16string ToUtf16(const std::string& text) {
    std::u16string out;
    for (size_t i = 0; i < text.size();) {
        auto c = static_cast<unsigned char>(text[i]);
        uint32_t point = c;
        size_t length = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
        if (length > 1) {
            point = c & (0xff >> (length + 1));
            for (size_t k = 1; k < length && i + k < text.size(); ++k)
                point = (point << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3f);
        }
        i += length;
        if (point >= 0x10000) {
            out += static_cast<char16_t>(0xd800 + ((point - 0x10000) >> 10));
            out += static_cast<char16_t>(0xdc00 + ((point - 0x10000) & 0x3ff));
        } else {
            out += static_cast<char16_t>(point);
        }
    }
    return out;
}

std::vector<uint8_t> Utf16Bytes(const char* text) {
    std::vector<uint8_t> bytes;
    for (const char* c = text; *c; ++c) {
        bytes.push_back(static_cast<uint8_t>(*c));
        bytes.push_back(0);
    }
    return bytes;
}

// The shortest text that reads back as the same number.
template <typename T>
std::string FormatReal(T value) {
    char text[64];
    for (int precision = 6; precision <= 17; ++precision) {
        snprintf(text, sizeof(text), "%.*g", precision, static_cast<double>(value));
        if (static_cast<T>(strtod(text, nullptr)) == value)
            break;
    }
    return text;
}

bool EndsWith(const std::string& text, const char* end) {
    size_t length = strlen(end);
    return text.size() >= length && text.compare(text.size() - length, length, end) == 0;
}

// Detection ---------------------------------------------------------------

// The executable, or CoreUObject and Core in a modular build, such as the
// editor's UnrealEditor-CoreUObject.dll.
void Detect(Unreal& ue) {
    ue.checked = true;
    std::vector<LoadedModule> modules = LoadedModules();
    for (const LoadedModule& module : modules) {
        std::string name = Lower(module.name);
        if (module.main || name.find("-coreuobject") != std::string::npos || name.find("-core.") != std::string::npos)
            ue.cores.push_back(module);
    }
    // The package of the engine's base classes, which the code that registers
    // them names in UTF-16, and the branch the engine was built from, such as
    // ++UE5+Release-5.3; built here, so that the library holds neither.
    std::vector<uint8_t> marker = Utf16Bytes("/Script/CoreUObject");
    std::vector<uint8_t> branch = Utf16Bytes("++UE");
    for (const LoadedModule& module : ue.cores) {
        ForEachReadable(module.base, module.size, 128, [&](uintptr_t, const uint8_t* bytes, size_t length) {
            const uint8_t* end = bytes + length;
            if (!ue.present && std::search(bytes, end, marker.begin(), marker.end()) != end) {
                ue.present = true;
                ue.module = module.name;
            }
            for (const uint8_t* at = bytes; ue.version == "-" && (at = std::search(at, end, branch.begin(), branch.end())) != end; at += 2) {
                // ++UE5+Release-5.3: the generation, then the version after the dash.
                std::string text;
                for (const uint8_t* c = at; c + 1 < end && c[1] == 0 && c[0] >= 0x20 && c[0] < 0x7f && text.size() < 64; c += 2)
                    text += static_cast<char>(c[0]);
                size_t dash = text.find("+Release-");
                if (text.size() < 6 || !isdigit(static_cast<unsigned char>(text[4])) || dash == std::string::npos)
                    continue;
                size_t start = dash + 9;
                size_t stop = start;
                while (stop < text.size() && (isdigit(static_cast<unsigned char>(text[stop])) || text[stop] == '.'))
                    ++stop;
                if (stop > start && isdigit(static_cast<unsigned char>(text[start])))
                    ue.version = text.substr(start, stop - start);
            }
            return !ue.present || ue.version == "-";
        });
    }
}

// Names -------------------------------------------------------------------

bool IsPoolStart(const uint8_t* b) {
    auto header = [&](size_t at) { return static_cast<uint16_t>(b[at] | (b[at + 1] << 8)); };
    return (header(0) & 1) == 0 && (header(0) >> 6) == 4 && memcmp(b + 2, "None", 4) == 0 && (header(6) & 1) == 0 && (header(6) >> 6) == 12 &&
           memcmp(b + 8, "ByteProperty", 12) == 0;
}

bool IsLegacyEntry(uintptr_t entry, size_t textOffset, const char* text) {
    char bytes[16] = {};
    size_t length = strlen(text) + 1;
    return ReadMemory(entry + textOffset, bytes, length) && memcmp(bytes, text, length) == 0;
}

// The text of a FNamePool entry: its header, then its characters.
std::string PoolEntry(const uint8_t* entry, size_t available) {
    uint16_t header = static_cast<uint16_t>(entry[0] | (entry[1] << 8));
    size_t length = header >> 6;
    if (header & 1) {
        if (2 + 2 * length > available)
            return "";
        std::vector<uint16_t> units(length);
        memcpy(units.data(), entry + 2, 2 * length);
        return FromUtf16(units.data(), length);
    }
    return 2 + length > available ? "" : std::string(reinterpret_cast<const char*>(entry + 2), length);
}

std::string NameEntry(Unreal& ue, uint32_t index) {
    auto cached = ue.nameCache.find(index);
    if (cached != ue.nameCache.end())
        return cached->second;
    std::string name;
    if (ue.namePool) {
        uintptr_t block = Ptr(ue.names + (index >> 16) * P);
        uintptr_t entry = block ? block + (index & 0xffff) * 2 : 0;
        uint16_t header = 0;
        if (Read(entry, header)) {
            size_t length = header >> 6;
            if (header & 1) {
                std::vector<uint16_t> units(length);
                if (length && ReadMemory(entry + 2, units.data(), length * 2))
                    name = FromUtf16(units.data(), length);
            } else {
                name.resize(length);
                if (length && !ReadMemory(entry + 2, &name[0], length))
                    name.clear();
            }
        }
    } else {
        uintptr_t chunk = Ptr(ue.names + (index / kLegacyChunk) * P);
        uintptr_t entry = chunk ? Ptr(chunk + (index % kLegacyChunk) * P) : 0;
        int32_t entryIndex = 0;
        if (entry && Read(entry + ue.legacyIndex, entryIndex)) {
            if (entryIndex & 1) {
                uint16_t units[256] = {};
                if (ReadMemory(entry + ue.legacyText, units, sizeof(units)) || ReadMemory(entry + ue.legacyText, units, 64)) {
                    size_t length = 0;
                    while (length < 256 && units[length])
                        ++length;
                    name = FromUtf16(units, length);
                }
            } else {
                char text[256] = {};
                if (ReadMemory(entry + ue.legacyText, text, sizeof(text)) || ReadMemory(entry + ue.legacyText, text, 32))
                    name.assign(text, strnlen(text, sizeof(text)));
            }
        }
    }
    if (ue.nameCache.size() < 1000000)
        ue.nameCache[index] = name;
    return name;
}

// The index of every name, by its text in lower case, for the FName arguments.
void IndexNames(Unreal& ue) {
    if (ue.nameIndexComplete)
        return;
    ue.nameIndexComplete = true;
    if (ue.namePool) {
        // FNameEntryAllocator: CurrentBlock and CurrentByteCursor right before Blocks.
        uint32_t cursor[2] = {};
        if (!ReadMemory(ue.names - 8, cursor, sizeof(cursor)) || cursor[0] >= 8192)
            return;
        std::vector<uint8_t> bytes(kNameBlock);
        for (uint32_t block = 0; block <= cursor[0] && !StopRequested(); ++block) {
            uintptr_t base = Ptr(ue.names + block * P);
            size_t end = block == cursor[0] ? std::min<size_t>(cursor[1], kNameBlock) : kNameBlock;
            if (!base || !ReadMemory(base, bytes.data(), end))
                continue;
            for (size_t at = 0; at + 2 <= end;) {
                uint16_t header = static_cast<uint16_t>(bytes[at] | (bytes[at + 1] << 8));
                size_t length = header >> 6;
                if (!length)
                    break;
                std::string text = Lower(PoolEntry(bytes.data() + at, end - at));
                ue.nameIndex.emplace(text, static_cast<uint32_t>(block << 16 | at / 2));
                at += (2 + length * (header & 1 ? 2 : 1) + 1) / 2 * 2;
            }
        }
        return;
    }
    for (size_t chunk = 0; chunk < 1024 && !StopRequested(); ++chunk) {
        uintptr_t table = Ptr(ue.names + chunk * P);
        if (!table)
            break;
        std::vector<uintptr_t> entries(kLegacyChunk);
        if (!ReadMemory(table, entries.data(), entries.size() * P))
            break;
        for (size_t i = 0; i < kLegacyChunk; ++i)
            if (entries[i])
                ue.nameIndex.emplace(Lower(NameEntry(ue, static_cast<uint32_t>(chunk * kLegacyChunk + i))), static_cast<uint32_t>(chunk * kLegacyChunk + i));
    }
}

// The FName of `text`, ComparisonIndex and Number, when the game has it.
bool FindName(Unreal& ue, const std::string& text, uint32_t parts[2]) {
    IndexNames(ue);
    auto found = ue.nameIndex.find(Lower(text));
    if (found != ue.nameIndex.end()) {
        parts[0] = found->second;
        parts[1] = 0;
        return true;
    }
    // Name_3 is Name with the number 4.
    size_t underscore = text.rfind('_');
    if (underscore == std::string::npos || underscore + 1 >= text.size() || text[underscore + 1] == '0' ||
        text.find_first_not_of("0123456789", underscore + 1) != std::string::npos)
        return false;
    found = ue.nameIndex.find(Lower(text.substr(0, underscore)));
    if (found == ue.nameIndex.end())
        return false;
    parts[0] = found->second;
    parts[1] = static_cast<uint32_t>(strtoul(text.c_str() + underscore + 1, nullptr, 10) + 1);
    return true;
}

// Objects -----------------------------------------------------------------

// Calls `visit` with the index and the address of each live object, until it returns false.
void ForEachObject(const Unreal& ue, const std::function<bool(int32_t, uintptr_t)>& visit) {
    int32_t count = ObjectCount(ue);
    std::vector<uint8_t> items;
    constexpr size_t kBatch = 4096;
    for (size_t chunk = 0; chunk * kItemsPerChunk < static_cast<size_t>(count); ++chunk) {
        uintptr_t base = ue.flat ? (chunk ? 0 : ue.objects) : Ptr(ue.objects + chunk * P);
        if (!base)
            return;
        size_t inChunk = ue.flat ? static_cast<size_t>(count) : std::min(kItemsPerChunk, static_cast<size_t>(count) - chunk * kItemsPerChunk);
        for (size_t first = 0; first < inChunk; first += kBatch) {
            if (StopRequested())
                return;
            size_t batch = std::min(kBatch, inChunk - first);
            items.resize(batch * ue.itemSize);
            if (!ReadMemory(base + first * ue.itemSize, items.data(), items.size()))
                return;
            for (size_t i = 0; i < batch; ++i) {
                uintptr_t object = 0;
                memcpy(&object, items.data() + i * ue.itemSize + ue.itemObject, sizeof(object));
                if (object && !visit(static_cast<int32_t>(chunk * kItemsPerChunk + first + i), object))
                    return;
            }
        }
    }
}

// Discovery ---------------------------------------------------------------

// Items whose first 16 objects hold their own index; sets the item layout.
bool ValidItems(Unreal& ue, const Heap& heap, uintptr_t items) {
    uint8_t buffer[16 * 48];
    if (!heap.Contains(items) || !ReadMemory(items, buffer, sizeof(buffer)))
        return false;
    // The object, then the flags, cluster and serial number; or the flags first since 5.x; and a stat id in some builds.
    for (size_t size = 2 * P; size <= 48; size += P) {
        for (size_t object : {size_t{0}, size_t{8}}) {
            if (object + P > size || 16 * size > sizeof(buffer))
                continue;
            bool valid = true;
            for (int32_t i = 0; i < 16 && valid; ++i) {
                uintptr_t candidate = 0;
                memcpy(&candidate, buffer + i * size + object, sizeof(candidate));
                valid = heap.Contains(candidate) && Int(candidate + ue.indexOffset) == i;
            }
            if (valid) {
                ue.itemSize = size;
                ue.itemObject = object;
                return true;
            }
        }
    }
    return false;
}

// The words of the writable data of the core modules: `visit(address, value)` until it returns false.
void ForEachCoreWord(const Unreal& ue, const std::function<bool(uintptr_t, uintptr_t)>& visit) {
    bool going = true;
    for (const MemoryRegion& region : MemoryRegions()) {
        if (!region.writable || !going)
            continue;
        for (const LoadedModule& module : ue.cores) {
            uintptr_t start = std::max(region.base, module.base);
            uintptr_t end = std::min(region.base + region.size, module.base + module.size);
            if (start >= end || !going)
                continue;
            ForEachReadable(start, end - start, 0, [&](uintptr_t base, const uint8_t* bytes, size_t length) {
                for (size_t i = 0; i + P <= length && going; i += P) {
                    uintptr_t value = 0;
                    memcpy(&value, bytes + i, sizeof(value));
                    if (value)
                        going = visit(base + i, value);
                }
                return going && !StopRequested();
            });
        }
    }
}

bool FindNames(Unreal& ue, const Heap& heap) {
    ForEachCoreWord(ue, [&](uintptr_t address, uintptr_t value) {
        uint8_t bytes[20];
        if (heap.Contains(value) && ReadMemory(value, bytes, sizeof(bytes)) && IsPoolStart(bytes)) {
            ue.namePool = true;
            ue.names = address;
            ue.namesAt = address;
            return false;
        }
        return true;
    });
    if (ue.names)
        return true;
    // TNameEntryArray: chunks of entry pointers, Index and HashNext before the text.
    ForEachCoreWord(ue, [&](uintptr_t address, uintptr_t value) {
        for (uintptr_t table : {value, address}) {
            uintptr_t chunk = table == address ? value : Ptr(table);
            if (!heap.Contains(chunk))
                continue;
            uintptr_t first = Ptr(chunk);
            uintptr_t second = Ptr(chunk + P);
            if (!heap.Contains(first) || !heap.Contains(second))
                continue;
            for (size_t text : {2 * P, P + 4}) {
                if (IsLegacyEntry(first, text, "None") && IsLegacyEntry(second, text, "ByteProperty")) {
                    ue.names = table;
                    ue.namesAt = address;
                    ue.namesIndirect = table == value;
                    ue.legacyText = text;
                    ue.legacyIndex = text == 2 * P ? 0 : P;
                    return false;
                }
            }
        }
        return true;
    });
    return ue.names != 0;
}

bool FindObjects(Unreal& ue, const Heap& heap) {
    uintptr_t found = 0;
    ForEachCoreWord(ue, [&](uintptr_t address, uintptr_t value) {
        if (!heap.Contains(value))
            return true;
        uintptr_t chunk = Ptr(value);
        for (bool flat : {false, true}) {
            uintptr_t items = flat ? value : chunk;
            if (ValidItems(ue, heap, items)) {
                ue.objects = value;
                ue.flat = flat;
                if (NameAt(ue, Ptr(items + ue.itemObject) + ue.nameOffset) == "/Script/CoreUObject") {
                    found = address;
                    return false;
                }
                ue.objects = 0;
            }
        }
        return true;
    });
    if (!found)
        return false;
    ue.objectsAt = found;
    // The number of objects, among the counters after the table: within the allocated chunks.
    size_t chunks = 0;
    while (!ue.flat && chunks < 4096 && Ptr(ue.objects + chunks * P))
        ++chunks;
    for (size_t at = P; at <= P + 4 * 4; at += 4) {
        int32_t value = Int(found + at, 0);
        bool fits = ue.flat ? value > 0 && value <= Int(found + P, 0) && at == P + 4
                            : value > 0 && static_cast<size_t>(value) <= chunks * kItemsPerChunk && static_cast<size_t>(value) > (chunks - 1) * kItemsPerChunk;
        if (fits) {
            ue.numElements = found + at;
            break;
        }
    }
    return true;
}

// The offsets of the layout, checked against the objects of /Script/CoreUObject.
bool FindLayout(Unreal& ue, const Heap& heap) {
    uintptr_t package = ObjectAt(ue, 0);
    // UObjectBase::OuterPrivate follows the name, which a hash index may follow.
    std::unordered_map<std::string, uintptr_t> core;
    for (size_t outer : {ue.nameOffset + 8, (ue.nameOffset + 12 + P - 1) / P * P}) {
        ue.outerOffset = outer;
        core.clear();
        ForEachObject(ue, [&](int32_t, uintptr_t object) {
            uintptr_t type = ClassOf(ue, object);
            if (type == object && ObjectName(ue, object) == "Class")
                ue.classClass = object;
            if (OuterOf(ue, object) == package) {
                std::string name = ObjectName(ue, object);
                if (name == "Object" || name == "Field" || name == "Struct" || name == "Class" || name == "ScriptStruct" || name == "Function" ||
                    name == "Enum" || name == "Vector" || name == "Transform")
                    core.emplace(name, object);
            }
            return core.size() < 9 || !ue.classClass;
        });
        if (ue.classClass && core.size() == 9 && core["Class"] == ue.classClass)
            break;
    }
    if (core.size() != 9 || !ue.classClass || core["Class"] != ue.classClass)
        return false;
    ue.package = package;
    ue.objectClass = core["Object"];
    ue.structClass = core["Struct"];
    ue.scriptStructClass = core["ScriptStruct"];
    ue.functionClass = core["Function"];
    ue.enumClass = core["Enum"];
    uintptr_t vector = ue.vectorStruct = core["Vector"];
    uintptr_t transform = ue.transformStruct = core["Transform"];
    ue.fieldNext = ue.outerOffset + P;
    // UStruct::SuperStruct: Struct derives from Field, Class and ScriptStruct from Struct.
    size_t structStart = ue.outerOffset + 2 * P;
    for (size_t at = structStart; at < structStart + 0x40 && !ue.superOffset; at += P)
        if (Ptr(ue.structClass + at) == core["Field"] && Ptr(ue.classClass + at) == ue.structClass && Ptr(ue.scriptStructClass + at) == ue.structClass)
            ue.superOffset = at;
    if (!ue.superOffset)
        return false;
    // The properties of Vector: X, Y and Z, UProperty objects in Children
    // before 4.25, FField in ChildProperties since.
    uintptr_t x = 0;
    for (size_t at = ue.superOffset + P; at < ue.superOffset + 0x30 && !x; at += P) {
        uintptr_t first = Ptr(vector + at);
        if (!heap.Contains(first))
            continue;
        if (IsObject(ue, first) && ObjectName(ue, first) == "X") {
            uintptr_t y = Ptr(first + ue.fieldNext);
            if (ObjectName(ue, y) == "Y" && ObjectName(ue, Ptr(y + ue.fieldNext)) == "Z") {
                ue.childrenOffset = at;
                x = first;
            }
            continue;
        }
        for (size_t next : {3 * P, 4 * P}) {
            for (size_t name : {next + P, next + 2 * P}) {
                uintptr_t y = Ptr(first + next);
                if (!x && NameAt(ue, first + name) == "X" && heap.Contains(y) && NameAt(ue, y + name) == "Y" && NameAt(ue, Ptr(y + next) + name) == "Z") {
                    ue.childPropertiesOffset = at;
                    ue.childrenOffset = at - P;
                    ue.fieldNextOffset = next;
                    ue.fieldNameOffset = name;
                    x = first;
                }
            }
        }
    }
    if (!x)
        return false;
    auto nextProperty = [&](uintptr_t property) { return Ptr(property + (ue.childPropertiesOffset ? ue.fieldNextOffset : ue.fieldNext)); };
    uintptr_t y = nextProperty(x);
    uintptr_t z = nextProperty(y);
    // ArrayDim 1 and ElementSize 4 (float) or 8 (double), then Offset_Internal 0, size, twice the size.
    size_t base = ue.childPropertiesOffset ? ue.fieldNameOffset + 8 : ue.outerOffset + 2 * P;
    for (size_t at = base; at < base + 0x20 && !ue.elementSizeOffset; at += 4) {
        int32_t size = Int(x + at + 4);
        if (Int(x + at) == 1 && Int(y + at) == 1 && Int(z + at) == 1 && (size == 4 || size == 8) && Int(y + at + 4) == size && Int(z + at + 4) == size) {
            for (size_t offset = at + 8; offset < at + 0x30 && !ue.offsetOffset; offset += 4)
                if (Int(x + offset) == 0 && Int(y + offset) == size && Int(z + offset) == 2 * size)
                    ue.offsetOffset = offset;
            if (ue.offsetOffset) {
                ue.elementSizeOffset = at + 4;
                ue.doubles = size == 8;
            }
        }
    }
    if (!ue.offsetOffset)
        return false;
    // FStructProperty::Struct, the first member after FProperty: Transform's Translation is a Vector.
    uintptr_t translation = Ptr(transform + (ue.childPropertiesOffset ? ue.childPropertiesOffset : ue.childrenOffset));
    for (int i = 0; translation && i < 16; ++i, translation = nextProperty(translation)) {
        std::string name = ue.childPropertiesOffset ? NameAt(ue, translation + ue.fieldNameOffset) : ObjectName(ue, translation);
        if (name != "Translation")
            continue;
        size_t start = (ue.offsetOffset + 4 + P - 1) / P * P;
        for (size_t at = start; at < start + 0x60 && !ue.extraOffset; at += P)
            if (Ptr(translation + at) == vector)
                ue.extraOffset = at;
        break;
    }
    return ue.extraOffset != 0;
}

bool IsPowerOfTwo(int32_t value) { return value > 0 && (value & (value - 1)) == 0; }

// UStruct::PropertiesSize and MinAlignment, after the lists of children: 3
// and 12 reals for Vector and Transform, which is aligned on 16 bytes.
bool FindStructSize(Unreal& ue) {
    int32_t real = ue.doubles ? 8 : 4;
    size_t last = std::max(ue.childrenOffset, ue.childPropertiesOffset) + P;
    for (size_t at = last; at < last + 0x20; at += 4) {
        if (Int(ue.vectorStruct + at) == 3 * real && Int(ue.transformStruct + at) == 12 * real && Int(ue.transformStruct + at + 4) == 16 &&
            IsPowerOfTwo(Int(ue.vectorStruct + at + 4))) {
            ue.propertiesSizeOffset = at;
            return true;
        }
    }
    return false;
}

uint16_t Word(uintptr_t address) {
    uint16_t value = 0;
    return Read(address, value) ? value : 0;
}

// NumParms, ParmsSize and ReturnValueOffset of UFunction, the counters after
// FunctionFlags, must be those of the parameters of every function sampled.
bool FindFunctionLayout(Unreal& ue) {
    struct Sample {
        uintptr_t function;
        uint8_t count;
        uint32_t end;
        uint16_t returned;
    };
    std::vector<Sample> samples;
    std::unordered_map<uintptr_t, bool> isFunction;
    ForEachObject(ue, [&](int32_t, uintptr_t object) {
        uintptr_t klass = ClassOf(ue, object);
        auto known = isFunction.find(klass);
        if (known == isFunction.end())
            known = isFunction.emplace(klass, IsA(ue, klass, ue.functionClass)).first;
        if (!known->second)
            return true;
        std::vector<Property> parameters = Parameters(ue, object);
        if (parameters.empty() || parameters.size() > 255)
            return true;
        Sample sample{object, static_cast<uint8_t>(parameters.size()), 0, 0xffff};
        for (const Property& parameter : parameters) {
            sample.end = std::max(sample.end, static_cast<uint32_t>(parameter.offset + parameter.size * std::max(parameter.arrayDim, 1)));
            if (parameter.flags & kReturnParm)
                sample.returned = static_cast<uint16_t>(parameter.offset);
        }
        samples.push_back(sample);
        return samples.size() < 64;
    });
    if (samples.size() < 3)
        return false;
    size_t start = ue.propertiesSizeOffset + 8;
    for (size_t at = start; at < start + 0x100; ++at) {
        size_t size = (at + 2) & ~size_t{1};
        bool valid = true;
        for (size_t i = 0; i < samples.size() && valid; ++i) {
            const Sample& sample = samples[i];
            uint8_t count = 0;
            uint16_t parms = Word(sample.function + size);
            valid = Read(sample.function + at, count) && count == sample.count && parms >= sample.end && parms < sample.end + 16 &&
                    Word(sample.function + size + 2) == sample.returned;
        }
        if (valid) {
            ue.numParmsOffset = at;
            ue.parmsSizeOffset = size;
            ue.returnValueOffset = size + 2;
            // FunctionFlags, then RepOffset in some versions.
            ue.functionFlagsOffset = (at - 4) & ~size_t{3};
            return true;
        }
    }
    return false;
}

// UEnum::Names after UField and the CppType string: TPair<FName, int64>,
// whose last one is the <Enum>_MAX the engine adds to every enum.
bool FindEnumLayout(Unreal& ue, const Heap& heap) {
    std::vector<uintptr_t> enums;
    std::unordered_map<uintptr_t, bool> isEnum;
    ForEachObject(ue, [&](int32_t, uintptr_t object) {
        uintptr_t klass = ClassOf(ue, object);
        auto known = isEnum.find(klass);
        if (known == isEnum.end())
            known = isEnum.emplace(klass, IsA(ue, klass, ue.enumClass)).first;
        if (known->second)
            enums.push_back(object);
        return enums.size() < 64;
    });
    if (enums.empty())
        return true;
    size_t start = ue.fieldNext + P;
    for (size_t at = start; at < start + 0x40; at += P) {
        for (size_t entry : {size_t{16}, size_t{12}, size_t{24}}) {
            size_t valid = 0;
            size_t empty = 0;
            size_t ended = 0;
            for (uintptr_t enumeration : enums) {
                uintptr_t data = Ptr(enumeration + at);
                int32_t count = Int(enumeration + at + P, 0);
                int32_t capacity = Int(enumeration + at + P + 4, 0);
                // An enum without values, such as one a blueprint declares and the game never loaded.
                if (!data && !count) {
                    ++empty;
                    continue;
                }
                if (!heap.Contains(data & ~(P - 1)) || count <= 0 || count > capacity || capacity > 65536)
                    break;
                ++valid;
                if (EndsWith(NameAt(ue, data + (count - 1) * entry), "_MAX"))
                    ++ended;
            }
            if (valid && valid + empty == enums.size() && ended * 4 >= valid * 3) {
                ue.enumNamesOffset = at;
                ue.enumEntrySize = entry;
                return true;
            }
        }
    }
    return false;
}

// Types -------------------------------------------------------------------

bool IsPropertyKind(const std::string& kind) { return kind.size() > 8 && kind.compare(kind.size() - 8, 8, "Property") == 0; }

std::vector<TypeEntry> ListTypesNow(Unreal& ue) {
    std::vector<TypeEntry> types;
    std::unordered_map<uintptr_t, const char*> kinds;
    ForEachObject(ue, [&](int32_t, uintptr_t object) {
        uintptr_t type = ClassOf(ue, object);
        auto known = kinds.find(type);
        if (known == kinds.end()) {
            const char* kind = IsA(ue, type, ue.classClass)          ? "class"
                               : IsA(ue, type, ue.scriptStructClass) ? "struct"
                               : IsA(ue, type, ue.enumClass)         ? "enum"
                                                                     : nullptr;
            known = kinds.emplace(type, kind).first;
        }
        if (known->second)
            types.push_back({object, ObjectName(ue, object), PackageOf(ue, object), known->second});
        return true;
    });
    std::sort(types.begin(), types.end(), [](const TypeEntry& a, const TypeEntry& b) { return a.package != b.package ? a.package < b.package : a.name < b.name; });
    return types;
}

// A package by its path, /Script/Engine, or its last part, Engine.
bool PackageMatches(const std::string& package, const std::string& wanted) {
    std::string lower = Lower(package);
    std::string target = Lower(wanted);
    return target.empty() || lower == target ||
           (lower.size() > target.size() && lower.compare(lower.size() - target.size(), target.size(), target) == 0 &&
            lower[lower.size() - target.size() - 1] == '/');
}

// Values ------------------------------------------------------------------

std::string FormatObject(Unreal& ue, uintptr_t object) {
    if (!object)
        return "null";
    if (!IsObject(ue, object))
        return Hex(object) + " (not an object)";
    return Hex(object) + " (" + ObjectName(ue, ClassOf(ue, object)) + " " + ObjectName(ue, object) + ")";
}

// The properties of a struct and of its parents, the parents' first.
std::vector<Property> AllProperties(Unreal& ue, uintptr_t type) {
    std::vector<Property> fields;
    for (int level = 0; type && level < 16; ++level, type = SuperOf(ue, type)) {
        std::vector<Property> own = OwnProperties(ue, type);
        fields.insert(fields.begin(), own.begin(), own.end());
    }
    return fields;
}

std::string FormatValue(Unreal& ue, const Property& property, uintptr_t address, int depth = 0) {
    const std::string& kind = property.kind;
    auto number = [&](auto value) -> std::string {
        if (!Read(address, value))
            return "?";
        if constexpr (std::is_floating_point_v<decltype(value)>)
            return FormatReal(value);
        else
            return std::to_string(value);
    };
    if (kind == "IntProperty")
        return number(int32_t{});
    if (kind == "Int8Property")
        return number(int8_t{});
    if (kind == "Int16Property")
        return number(int16_t{});
    if (kind == "Int64Property")
        return number(int64_t{});
    if (kind == "UInt16Property")
        return number(uint16_t{});
    if (kind == "UInt32Property")
        return number(uint32_t{});
    if (kind == "UInt64Property")
        return number(uint64_t{});
    if (kind == "FloatProperty")
        return number(float{});
    if (kind == "DoubleProperty")
        return number(double{});
    if (kind == "ByteProperty") {
        uint8_t value = 0;
        return Read(address, value) ? std::to_string(value) : "?";
    }
    if (kind == "EnumProperty") {
        uint64_t raw = 0;
        int32_t size = std::clamp(property.size, 1, 8);
        if (!ReadMemory(address, &raw, static_cast<size_t>(size)))
            return "?";
        // Sign-extended from its size.
        int shift = 64 - 8 * size;
        return std::to_string(static_cast<int64_t>(raw << shift) >> shift);
    }
    if (kind == "BoolProperty") {
        uint8_t offset = 0;
        uint8_t mask = 0;
        uint8_t byte = 0;
        BoolLayout(ue, property, offset, mask);
        if (!mask || !Read(address + offset, byte))
            return "?";
        return byte & mask ? "true" : "false";
    }
    if (kind == "NameProperty")
        return JsonQuote(NameAt(ue, address));
    if (kind == "StrProperty") {
        uintptr_t data = Ptr(address);
        int32_t count = Int(address + P, 0);
        if (!data || count <= 0)
            return "\"\"";
        std::vector<uint16_t> units(static_cast<size_t>(std::min(count, 4096)));
        if (!ReadMemory(data, units.data(), units.size() * 2))
            return "?";
        size_t length = units.size();
        while (length && !units[length - 1])
            --length;
        return JsonQuote(FromUtf16(units.data(), length));
    }
    if (kind == "ObjectProperty" || kind == "ObjectPtrProperty" || kind == "ClassProperty" || kind == "ClassPtrProperty")
        return FormatObject(ue, Ptr(address));
    if (kind == "WeakObjectProperty") {
        int32_t index = Int(address, -1);
        return index < 0 ? "null" : FormatObject(ue, ObjectAt(ue, index));
    }
    if (kind == "StructProperty") {
        if (depth >= 3)
            return "{...}";
        std::string text = "{";
        std::vector<Property> fields = AllProperties(ue, Extra(ue, property));
        for (size_t i = 0; i < fields.size() && i < 32; ++i)
            text += (i ? ", " : "") + fields[i].name + "=" + FormatValue(ue, fields[i], address + fields[i].offset, depth + 1);
        return text + "}";
    }
    if (kind == "ArrayProperty") {
        uintptr_t data = Ptr(address);
        int32_t count = Int(address + P, 0);
        uintptr_t inner = Inner(ue, property);
        if (count <= 0)
            return "[]";
        if (!inner || depth >= 3)
            return "[" + std::to_string(count) + " elements]";
        Property element = Describe(ue, inner, 0);
        std::string text = "[";
        for (int32_t i = 0; i < count && i < 32; ++i)
            text += (i ? ", " : "") + FormatValue(ue, element, data + static_cast<uintptr_t>(i) * static_cast<uintptr_t>(element.size), depth + 1);
        return text + (count > 32 ? ", ... " + std::to_string(count) + " elements]" : "]");
    }
    return "<" + std::to_string(property.size) + " bytes at " + Hex(address) + ">";
}

// The strings of the arguments of a call, which their FString point to.
using Strings = std::deque<std::u16string>;

// Writes `text` as the value of `property` at `address`. The strings of
// arguments point to `strings`; without it, strings are refused, since the
// game would free them with its own allocator.
bool Encode(Unreal& ue, const Property& property, uintptr_t address, const std::string& text, std::string& error, Strings* strings = nullptr,
            int depth = 0) {
    const std::string& kind = property.kind;
    char* end = nullptr;
    auto integer = [&](size_t size, bool isSigned) {
        errno = 0;
        int64_t value = isSigned ? strtoll(text.c_str(), &end, 0) : static_cast<int64_t>(strtoull(text.c_str(), &end, 0));
        bool parsed = !text.empty() && !*end && !errno;
        // The enums also take the names of their values.
        uintptr_t enumeration = parsed ? 0 : EnumOf(ue, property);
        if (enumeration) {
            for (const EnumValue& known : EnumValues(ue, enumeration)) {
                if (Lower(known.name) == Lower(text) || Lower(ObjectName(ue, enumeration) + "::" + known.name) == Lower(text)) {
                    value = known.value;
                    parsed = true;
                }
            }
        }
        if (!parsed) {
            error = "error " + text + " is not an integer" + (enumeration ? std::string(" or a value of ") + ObjectName(ue, enumeration) : "");
            return false;
        }
        if (size < 8) {
            int64_t low = isSigned ? -(int64_t{1} << (8 * size - 1)) : 0;
            int64_t high = isSigned ? (int64_t{1} << (8 * size - 1)) - 1 : (int64_t{1} << (8 * size)) - 1;
            if (value < low || value > high) {
                error = "error " + text + " does not fit in " + std::to_string(size) + " bytes";
                return false;
            }
        }
        return WriteMemory(address, &value, size);
    };
    if (kind == "IntProperty")
        return integer(4, true);
    if (kind == "Int8Property")
        return integer(1, true);
    if (kind == "Int16Property")
        return integer(2, true);
    if (kind == "Int64Property")
        return integer(8, true);
    if (kind == "UInt16Property")
        return integer(2, false);
    if (kind == "UInt32Property")
        return integer(4, false);
    if (kind == "UInt64Property")
        return integer(8, false);
    if (kind == "ByteProperty")
        return integer(1, false);
    if (kind == "EnumProperty")
        return integer(static_cast<size_t>(std::clamp(property.size, 1, 8)), true);
    if (kind == "FloatProperty" || kind == "DoubleProperty") {
        double value = strtod(text.c_str(), &end);
        if (text.empty() || *end || !std::isfinite(value)) {
            error = "error " + text + " is not a number";
            return false;
        }
        float single = static_cast<float>(value);
        return kind == "FloatProperty" ? WriteMemory(address, &single, 4) : WriteMemory(address, &value, 8);
    }
    if (kind == "BoolProperty") {
        std::string lower = Lower(text);
        if (lower != "true" && lower != "false" && lower != "1" && lower != "0") {
            error = "error a bool is true or false";
            return false;
        }
        uint8_t offset = 0;
        uint8_t mask = 0;
        uint8_t byte = 0;
        BoolLayout(ue, property, offset, mask);
        if (!mask || !Read(address + offset, byte))
            return false;
        byte = static_cast<uint8_t>(lower == "true" || lower == "1" ? byte | mask : byte & ~mask);
        return WriteMemory(address + offset, &byte, 1);
    }
    if (kind == "ObjectProperty" || kind == "ObjectPtrProperty" || kind == "ClassProperty" || kind == "ClassPtrProperty") {
        bool isClass = kind == "ClassProperty" || kind == "ClassPtrProperty";
        uintptr_t object = text == "null" ? 0 : static_cast<uintptr_t>(strtoull(text.c_str(), &end, 16));
        // A class must derive from FClassProperty::MetaClass, an object be one of PropertyClass.
        uintptr_t wanted = Extra(ue, property, isClass ? 1 : 0);
        bool fits = text == "null" ||
                    (!text.empty() && !*end && IsObject(ue, object) && (isClass ? IsA(ue, object, wanted) : IsA(ue, ClassOf(ue, object), wanted)));
        if (!fits) {
            error = "error give null or the address of " + std::string(isClass ? "a class derived from " : "an object of ") + ObjectName(ue, wanted) +
                    ", from engine instances";
            return false;
        }
        return WriteMemory(address, &object, sizeof(object));
    }
    if (kind == "NameProperty") {
        uint32_t parts[2] = {};
        if (!FindName(ue, text, parts)) {
            error = "error the game has no name " + text + ": a name argument must be one the game already uses";
            return false;
        }
        return WriteMemory(address, parts, sizeof(parts));
    }
    if (kind == "StrProperty" && strings) {
        strings->push_back(ToUtf16(text));
        std::u16string& units = strings->back();
        units.push_back(0);
        uintptr_t data = reinterpret_cast<uintptr_t>(units.data());
        int32_t count[2] = {static_cast<int32_t>(units.size()), static_cast<int32_t>(units.size())};
        return WriteMemory(address, &data, sizeof(data)) && WriteMemory(address + P, count, sizeof(count));
    }
    if (kind == "StructProperty" && depth < 4) {
        // A JSON object of its members, such as {"X": 1, "Y": 2, "Z": 3}; those left out keep their value.
        if (text.empty() || text[0] != '{') {
            error = "error give a " + TypeName(ue, property) + " as a JSON object of its members, such as {\"X\": 1, \"Y\": 2}";
            return false;
        }
        for (const Property& member : AllProperties(ue, Extra(ue, property))) {
            std::string value = ToolArgument(text, member.name.c_str());
            if (value.empty() && text.find("\"" + member.name + "\"") == std::string::npos)
                continue;
            if (!Encode(ue, member, address + member.offset, value, error, strings, depth + 1)) {
                error += " (member " + member.name + ")";
                return false;
            }
        }
        return true;
    }
    error = "error the library cannot write a " + TypeName(ue, property) + (kind == "StrProperty" ? " property" : "") +
            " yet: numbers, enums, bools, names, structs and object references" + (kind == "StrProperty" ? ", and strings as arguments of a call" : "");
    return false;
}

// Commands ----------------------------------------------------------------

std::string Info(const Unreal& ue) {
    bool five = ue.version[0] == '5' || (ue.version == "-" && ue.discovered && ue.doubles);
    return std::string("ok unreal ") + (five ? "ue5 " : "ue4 ") + ue.version + " " + ue.module;
}

std::string ListPackages(Unreal& ue, std::vector<uint8_t>& payload) {
    std::vector<std::pair<std::string, size_t>> packages;
    for (const TypeEntry& type : Types(ue, true)) {
        if (packages.empty() || packages.back().first != type.package)
            packages.push_back({type.package, 0});
        ++packages.back().second;
    }
    std::string text;
    for (const auto& package : packages)
        text += package.first + " " + std::to_string(package.second) + "\n";
    return Payload(text, payload);
}

std::string ListTypes(Unreal& ue, const std::string& rest, std::vector<uint8_t>& payload) {
    size_t at = 0;
    std::string filter = Lower(NextWord(rest, at));
    std::string package = NextWord(rest, at);
    if (filter.empty())
        return "error give a part of the name, or *";
    std::string text;
    size_t listed = 0;
    size_t more = 0;
    size_t limit = EngineListLimit();
    for (const TypeEntry& type : Types(ue, true)) {
        if (!PackageMatches(type.package, package) || (filter != "*" && Lower(type.name).find(filter) == std::string::npos))
            continue;
        if (listed < limit) {
            text += type.package + " " + type.name + "\n";
            ++listed;
        } else {
            ++more;
        }
    }
    if (more)
        text += "+" + std::to_string(more) + " more\n";
    return Payload(text, payload);
}

// Whether the function takes a value for the parameter, and whether it gives one back.
bool IsInput(const Property& parameter) { return !(parameter.flags & kReturnParm) && (!(parameter.flags & kOutParm) || (parameter.flags & kReferenceParm)); }
bool IsOutput(const Property& parameter) { return (parameter.flags & kOutParm) && !(parameter.flags & kConstParm) && !(parameter.flags & kReturnParm); }

std::string FunctionSignature(Unreal& ue, uintptr_t function) {
    std::string returned = "void";
    std::string parameters;
    for (const Property& parameter : Parameters(ue, function)) {
        if (parameter.flags & kReturnParm) {
            returned = TypeName(ue, parameter);
            continue;
        }
        const char* mode = !IsOutput(parameter) ? "" : IsInput(parameter) ? "ref " : "out ";
        parameters += (parameters.empty() ? "" : ", ") + std::string(mode) + TypeName(ue, parameter) + " " + QuoteName(parameter.name);
    }
    bool isStatic = ue.functionFlagsOffset && (FunctionFlags(ue, function) & kFuncStatic);
    return (isStatic ? "static " : "") + returned + " " + QuoteName(ObjectName(ue, function)) + "(" + parameters + ")";
}

int32_t PropertyOffset(Unreal& ue, const Property& property, std::string& suffix) {
    int32_t offset = property.offset;
    if (property.kind == "BoolProperty") {
        uint8_t byte = 0;
        uint8_t mask = 0;
        BoolLayout(ue, property, byte, mask);
        offset += byte;
        if (mask != 0xff) {
            char text[16];
            snprintf(text, sizeof(text), " (bit 0x%x)", mask);
            suffix = text;
        }
    }
    return offset;
}

std::string PropertyLine(Unreal& ue, const Property& property) {
    std::string suffix;
    char head[32];
    snprintf(head, sizeof(head), "  0x%x ", static_cast<unsigned>(PropertyOffset(ue, property, suffix)));
    return head + TypeName(ue, property) + " " + QuoteName(property.name) + suffix;
}

std::string DescribeType(Unreal& ue, const std::string& rest, std::vector<uint8_t>& payload) {
    size_t at = 0;
    TypeEntry type;
    std::string error;
    if (!FindType(ue, NextWord(rest, at), type, error))
        return error;
    DiscoverReflection(ue);
    std::string text = std::string(type.kind) + " " + type.name;
    if (strcmp(type.kind, "enum") == 0) {
        text += " (" + type.package + ")\n";
        std::vector<EnumValue> values = EnumValues(ue, type.object);
        if (!values.empty())
            text += "values:\n";
        for (const EnumValue& value : values)
            text += "  " + std::to_string(value.value) + " " + QuoteName(value.name) + "\n";
        return Payload(text, payload);
    }
    std::vector<uintptr_t> lineage;
    for (uintptr_t level = type.object; level && lineage.size() < 64; level = SuperOf(ue, level))
        lineage.push_back(level);
    for (size_t i = 1; i < lineage.size(); ++i)
        text += " : " + ObjectName(ue, lineage[i]);
    text += " (" + type.package + ")\nproperties:\n";
    for (size_t i = 0; i < lineage.size(); ++i)
        for (const Property& property : OwnProperties(ue, lineage[i]))
            text += PropertyLine(ue, property) + (i ? "  (" + ObjectName(ue, lineage[i]) + ")" : "") + "\n";
    if (strcmp(type.kind, "class") == 0) {
        text += "functions:\n";
        for (uintptr_t function : Functions(ue, type.object))
            text += "  " + FunctionSignature(ue, function) + "\n";
        for (size_t i = 1; i < lineage.size(); ++i)
            if (!Functions(ue, lineage[i]).empty())
                text += "functions of " + ObjectName(ue, lineage[i]) + ": engine class " + ObjectName(ue, lineage[i]) + "\n";
    }
    return Payload(text, payload);
}

uintptr_t ParseObject(const std::string& text) {
    char* end = nullptr;
    uintptr_t value = static_cast<uintptr_t>(strtoull(text.c_str(), &end, 16));
    return !text.empty() && end && !*end ? value : 0;
}

bool CheckObject(Unreal& ue, const TypeEntry& type, const std::string& objectText, uintptr_t& object, std::string& error) {
    object = ParseObject(objectText);
    if (objectText.empty() || objectText == "-") {
        error = "error give the object: properties of Unreal classes belong to objects, which engine instances lists";
        return false;
    }
    if (!IsObject(ue, object) || !IsA(ue, ClassOf(ue, object), type.object)) {
        error = "error " + objectText + " is not an object of " + type.name + ": see engine instances " + type.name;
        return false;
    }
    return true;
}

// The property at `path` (a.b for the member b of the struct a) in the object.
bool Locate(Unreal& ue, const TypeEntry& type, const std::string& path, const std::string& objectText, Property& property, uintptr_t& address,
            std::string& error) {
    uintptr_t object = 0;
    if (!CheckObject(ue, type, objectText, object, error))
        return false;
    uintptr_t owner = type.object;
    address = object;
    size_t start = 0;
    while (start <= path.size()) {
        size_t dot = path.find('.', start);
        std::string name = path.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        if (!FindProperty(ue, owner, name, property)) {
            error = "error " + ObjectName(ue, owner) + " has no property " + QuoteName(name) + ": see engine class " + ObjectName(ue, owner);
            return false;
        }
        address += static_cast<uintptr_t>(property.offset);
        if (dot == std::string::npos)
            return true;
        if (property.kind != "StructProperty") {
            error = "error " + name + " is not a struct: only the members of a struct are reached with a dot";
            return false;
        }
        owner = Extra(ue, property);
        start = dot + 1;
    }
    return true;
}

// Every property of the object, its class's first: lines <offset> <type> <name> <value>, separated by tabs.
std::string AllValues(Unreal& ue, const TypeEntry& type, const std::string& objectText, std::vector<uint8_t>& payload) {
    uintptr_t object = 0;
    std::string error;
    if (!CheckObject(ue, type, objectText, object, error))
        return error;
    std::string text;
    for (uintptr_t level = type.object; level; level = SuperOf(ue, level)) {
        for (const Property& property : OwnProperties(ue, level)) {
            std::string suffix;
            char offset[16];
            snprintf(offset, sizeof(offset), "0x%x", static_cast<unsigned>(PropertyOffset(ue, property, suffix)));
            std::string value = FormatValue(ue, property, object + property.offset);
            std::replace(value.begin(), value.end(), '\t', ' ');
            std::replace(value.begin(), value.end(), '\n', ' ');
            text += std::string(offset) + "\t" + TypeName(ue, property) + "\t" + property.name + "\t" + value + "\n";
        }
    }
    return Payload(text, payload);
}

std::string GetValue(Unreal& ue, const std::string& rest, std::vector<uint8_t>& payload) {
    size_t at = 0;
    std::string typeName = NextWord(rest, at);
    std::string path = NextWord(rest, at);
    std::string objectText = NextWord(rest, at);
    TypeEntry type;
    Property property;
    uintptr_t address = 0;
    std::string error;
    if (!FindType(ue, typeName, type, error))
        return error;
    if (path == "*")
        return AllValues(ue, type, objectText, payload);
    if (!Locate(ue, type, path, objectText, property, address, error))
        return error;
    return Payload(TypeName(ue, property) + " " + FormatValue(ue, property, address), payload);
}

std::string SetValue(Unreal& ue, const std::string& rest, std::vector<uint8_t>& payload) {
    size_t at = 0;
    std::string typeName = NextWord(rest, at);
    std::string path = NextWord(rest, at);
    std::string objectText = NextWord(rest, at);
    std::string value = Rest(rest, at);
    TypeEntry type;
    Property property;
    uintptr_t address = 0;
    std::string error;
    if (!FindType(ue, typeName, type, error) || !Locate(ue, type, path, objectText, property, address, error))
        return error;
    if (value.empty())
        return "error give the value";
    // A name or an enum may come as a JSON string.
    std::vector<std::string> values;
    std::vector<bool> quoted;
    if (value[0] == '"' && ParseJsonArray("[" + value + "]", values, &quoted) && values.size() == 1)
        value = values[0];
    if (!Encode(ue, property, address, value, error))
        return error.empty() ? "error the write failed" : error;
    return Payload(TypeName(ue, property) + " " + FormatValue(ue, property, address), payload);
}

std::string Instances(Unreal& ue, const std::string& rest, std::vector<uint8_t>& payload) {
    size_t at = 0;
    std::string typeName = NextWord(rest, at);
    std::string countText = NextWord(rest, at);
    size_t limit = countText.empty() ? 100 : std::clamp<size_t>(strtoul(countText.c_str(), nullptr, 10), 1, kMaxInstances);
    TypeEntry type;
    std::string error;
    if (!FindType(ue, typeName, type, error))
        return error;
    if (strcmp(type.kind, "class") != 0)
        return "error " + type.name + " is a " + type.kind + ": its values live inside objects, not on their own";
    std::unordered_map<uintptr_t, bool> derived;
    std::string text;
    size_t found = 0;
    ForEachObject(ue, [&](int32_t, uintptr_t object) {
        uintptr_t klass = ClassOf(ue, object);
        auto known = derived.find(klass);
        if (known == derived.end())
            known = derived.emplace(klass, IsA(ue, klass, type.object)).first;
        if (known->second && !(Int(object + ue.flagsOffset, 0) & (kClassDefaultObject | kArchetypeObject))) {
            text += Hex(object) + " " + ObjectName(ue, klass) + " " + ObjectName(ue, object) + "\n";
            ++found;
        }
        return found < limit;
    });
    return Payload(text, payload);
}

// Calls ------------------------------------------------------------------

#if defined(_WIN32) && (defined(_M_IX86) || defined(__i386__))
// A virtual function of 32-bit Windows takes its object in ecx: __fastcall
// passes the first two arguments in ecx and edx, and the callee cleans the stack, as __thiscall does.
using ProcessEventFn = void(__fastcall*)(uintptr_t object, uintptr_t unused, uintptr_t function, void* parms);
void CallProcessEvent(uintptr_t code, uintptr_t object, uintptr_t function, void* parms) { reinterpret_cast<ProcessEventFn>(code)(object, 0, function, parms); }
#else
using ProcessEventFn = void (*)(uintptr_t object, uintptr_t function, void* parms);
void CallProcessEvent(uintptr_t code, uintptr_t object, uintptr_t function, void* parms) { reinterpret_cast<ProcessEventFn>(code)(object, function, parms); }
#endif

// A call through UObject::ProcessEvent, prepared on the server thread and
// run on the game's: everything by value, since it may run after the command gave up.
struct PreparedCall {
    uintptr_t object = 0;
    uintptr_t function = 0;
    std::vector<uint8_t> parms;
};

// Runs the calls in order on the game thread: the bytes of the parameters of
// each, as the call left them, or an error.
std::string RunCalls(int index, std::vector<PreparedCall> calls, std::shared_ptr<Strings> strings) {
    return RunOnGameThread([index, calls, strings]() mutable -> std::string {
        std::string out = "ok\n";
        for (PreparedCall& call : calls) {
            uintptr_t vtable = Ptr(call.object);
            uintptr_t code = vtable ? Ptr(vtable + static_cast<uintptr_t>(index) * P) : 0;
            if (!code)
                return "error the object has no virtual table: it may have been destroyed";
            void* parms = call.parms.empty() ? nullptr : call.parms.data();
            if (!GuardedCall([&] { CallProcessEvent(code, call.object, call.function, parms); }))
                return "error the call faulted in the game's code";
            uint32_t size = static_cast<uint32_t>(call.parms.size());
            out.append(reinterpret_cast<const char*>(&size), sizeof(size));
            out.append(reinterpret_cast<const char*>(call.parms.data()), call.parms.size());
        }
        return out;
    });
}

// The parameters of the calls, as RunCalls() answered them.
bool ReadCallResults(const std::string& answer, std::vector<PreparedCall>& calls) {
    size_t at = 3;
    for (PreparedCall& call : calls) {
        uint32_t size = 0;
        if (at + sizeof(size) > answer.size())
            return false;
        memcpy(&size, answer.data() + at, sizeof(size));
        at += sizeof(size);
        if (size != call.parms.size() || at + size > answer.size())
            return false;
        if (size)
            memcpy(call.parms.data(), answer.data() + at, size);
        at += size;
    }
    return true;
}

// KismetMathLibrary:Add_IntInt(20, 22), on the default object of the library,
// to make sure that the function found is ProcessEvent before the first call.
bool PrepareCheck(Unreal& ue, PreparedCall& check, size_t& resultOffset) {
    TypeEntry library;
    std::string error;
    uintptr_t add = 0;
    if (!FindType(ue, "Engine:KismetMathLibrary", library, error) || !(add = FindFunction(ue, library.object, "Add_IntInt")))
        return false;
    check.object = DefaultObject(ue, library.object);
    check.function = add;
    int32_t size = ParmsSize(ue, add);
    if (!check.object || size <= 0 || size > 64)
        return false;
    check.parms.assign(static_cast<size_t>(size), 0);
    int32_t values[2] = {20, 22};
    int found = 0;
    for (const Property& parameter : Parameters(ue, add)) {
        if (parameter.offset < 0 || parameter.offset + 4 > size)
            return false;
        if (parameter.flags & kReturnParm)
            resultOffset = static_cast<size_t>(parameter.offset);
        else if (found < 2)
            memcpy(check.parms.data() + parameter.offset, &values[found++], 4);
    }
    return found == 2 && resultOffset;
}

std::string Call(Unreal& ue, const std::string& rest, std::vector<uint8_t>& payload) {
    size_t at = 0;
    std::string typeName = NextWord(rest, at);
    std::string functionName = NextWord(rest, at);
    std::string objectText = NextWord(rest, at);
    std::string arguments = Rest(rest, at);
    std::vector<std::string> values;
    if (arguments.empty())
        arguments = "[]";
    if (!ParseJsonArray(arguments, values))
        return "error give the arguments as a JSON array, such as [1, 2.5, \"text\", true, {\"X\": 1, \"Y\": 2, \"Z\": 3}]";
    TypeEntry type;
    std::string error;
    if (!FindType(ue, typeName, type, error))
        return error;
    if (strcmp(type.kind, "class") != 0)
        return "error " + type.name + " is a " + type.kind + ": only the functions of classes are called";
    if (functionName.empty())
        return "error give the function";
    std::string failure = DiscoverProcessEvent(ue);
    if (!failure.empty())
        return failure;
    uintptr_t function = FindFunction(ue, type.object, functionName);
    if (!function)
        return "error " + type.name + " has no function " + QuoteName(functionName) + ": see engine class " + type.name;
    uintptr_t owner = OuterOf(ue, function);
    bool isStatic = (FunctionFlags(ue, function) & kFuncStatic) != 0;
    uintptr_t object = ParseObject(objectText);
    if ((objectText.empty() || objectText == "-") && isStatic) {
        object = DefaultObject(ue, owner);
        if (!object)
            return "error " + ObjectName(ue, owner) + " has no default object to call its static function on";
    } else if (!IsObject(ue, object) || !IsA(ue, ClassOf(ue, object), owner)) {
        return "error " + QuoteName(functionName) + " is called on an object: give the address of an object of " + ObjectName(ue, owner) +
               ", from engine instances";
    }
    std::vector<Property> parameters = Parameters(ue, function);
    std::vector<const Property*> inputs;
    int32_t size = ParmsSize(ue, function);
    for (const Property& parameter : parameters) {
        if (IsInput(parameter))
            inputs.push_back(&parameter);
        if (parameter.offset < 0 || parameter.offset + parameter.size * std::max(parameter.arrayDim, 1) > size)
            return "error the parameters of " + QuoteName(functionName) + " do not fit in its ParmsSize";
    }
    if (inputs.size() != values.size())
        return "error " + QuoteName(functionName) + " takes " + std::to_string(inputs.size()) + " arguments, " + std::to_string(values.size()) +
               " given: " + FunctionSignature(ue, function);
    PreparedCall call;
    call.object = object;
    call.function = function;
    call.parms.assign(static_cast<size_t>(std::max(size, 0)), 0);
    auto strings = std::make_shared<Strings>();
    for (size_t i = 0; i < inputs.size(); ++i) {
        std::string reason;
        if (!Encode(ue, *inputs[i], reinterpret_cast<uintptr_t>(call.parms.data()) + inputs[i]->offset, values[i], reason, strings.get()))
            return (reason.empty() ? "error the argument does not fit" : reason) + " (argument " + std::to_string(i + 1) + ", " + QuoteName(inputs[i]->name) +
                   ")";
    }
    if (ue.processEventVerified < 0)
        return ue.processEventFailure;
    // Before the first call, one whose result is known: the sum of KismetMathLibrary:Add_IntInt.
    PreparedCall check;
    size_t checkResult = 0;
    std::string answer;
    if (ue.processEventVerified == 0 && PrepareCheck(ue, check, checkResult)) {
        // Each candidate in turn: one that faults or returns another sum is not ProcessEvent.
        std::string tried;
        for (const std::pair<int, uintptr_t>& candidate : ue.processEventCandidates) {
            std::vector<PreparedCall> checked{check};
            answer = RunCalls(candidate.first, checked, strings);
            int32_t sum = 0;
            bool ok = answer.rfind("ok\n", 0) == 0 && ReadCallResults(answer, checked);
            if (ok)
                memcpy(&sum, checked[0].parms.data() + checkResult, 4);
            if (ok && sum == 42) {
                ue.processEventIndex = candidate.first;
                ue.processEventAddress = candidate.second;
                ue.processEventVerified = 1;
                break;
            }
            // The game thread did not run it: no answer about this candidate.
            if (answer.rfind("error ", 0) == 0 && answer.rfind("error the call faulted", 0) != 0)
                return answer;
            tried += (tried.empty() ? "" : ", ") + std::to_string(candidate.first) + (ok ? " returned " + std::to_string(sum) : " faulted");
        }
        if (ue.processEventVerified != 1) {
            ue.processEventVerified = -1;
            ue.processEventFailure = "error the library did not find how this game calls its functions: KismetMathLibrary:Add_IntInt(20, 22) did not "
                                     "return 42 through the virtual functions that test FUNC_Native (" + tried + ")";
            return ue.processEventFailure;
        }
    }
    std::vector<PreparedCall> calls{call};
    answer = RunCalls(ue.processEventIndex, calls, strings);
    if (answer.rfind("ok\n", 0) != 0)
        return answer;
    if (!ReadCallResults(answer, calls))
        return "error the call did not answer";
    uintptr_t parms = reinterpret_cast<uintptr_t>(calls[0].parms.data());
    std::string text = "void";
    std::string outputs;
    for (const Property& parameter : parameters) {
        if (parameter.flags & kReturnParm)
            text = TypeName(ue, parameter) + " " + FormatValue(ue, parameter, parms + parameter.offset);
        else if (IsOutput(parameter))
            outputs += "out " + QuoteName(parameter.name) + " = " + TypeName(ue, parameter) + " " + FormatValue(ue, parameter, parms + parameter.offset) + "\n";
    }
    return Payload(text + "\n" + outputs, payload);
}

} // namespace

// Shared with src/sdk.cpp ---------------------------------------------------

uintptr_t Ptr(uintptr_t address) {
    uintptr_t value = 0;
    return Read(address, value) ? value : 0;
}

int32_t Int(uintptr_t address, int32_t fallback) {
    int32_t value = fallback;
    return Read(address, value) ? value : fallback;
}

bool GlobalOf(const Unreal& ue, uintptr_t address, Global& global) {
    for (const LoadedModule& module : LoadedModules()) {
        if (address >= module.base && address - module.base < module.size) {
            global.module = module.name;
            global.main = module.main;
            global.offset = address - module.base;
            return true;
        }
    }
    (void)ue;
    return false;
}

std::string Lower(std::string text) {
    for (char& c : text)
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return text;
}

std::string QuoteName(const std::string& name) {
    if (!name.empty() && name.find(' ') == std::string::npos && name[0] != '"')
        return name;
    std::string out = "\"";
    for (char c : name) {
        if (c == '"' || c == '\\')
            out += '\\';
        out += c;
    }
    return out + "\"";
}

// The text of the FName at `address`: ComparisonIndex, then Number.
std::string NameAt(Unreal& ue, uintptr_t address) {
    uint32_t parts[2] = {};
    if (!ReadMemory(address, parts, sizeof(parts)))
        return "";
    std::string name = NameEntry(ue, parts[0]);
    if (parts[1] && !name.empty())
        name += "_" + std::to_string(parts[1] - 1);
    return name;
}

std::string ObjectName(Unreal& ue, uintptr_t object) { return NameAt(ue, object + ue.nameOffset); }
uintptr_t ClassOf(const Unreal& ue, uintptr_t object) { return Ptr(object + ue.classOffset); }
uintptr_t OuterOf(const Unreal& ue, uintptr_t object) { return Ptr(object + ue.outerOffset); }
uintptr_t SuperOf(const Unreal& ue, uintptr_t type) { return Ptr(type + ue.superOffset); }

int32_t ObjectCount(const Unreal& ue) {
    int32_t count = ue.numElements ? Int(ue.numElements, 0) : 0;
    return count > 0 ? count : INT_MAX;
}

uintptr_t ObjectAt(const Unreal& ue, int32_t index) {
    if (index < 0 || index >= ObjectCount(ue))
        return 0;
    uintptr_t base = ue.flat ? ue.objects : Ptr(ue.objects + (index / kItemsPerChunk) * P);
    size_t at = ue.flat ? static_cast<size_t>(index) : index % kItemsPerChunk;
    return base ? Ptr(base + at * ue.itemSize + ue.itemObject) : 0;
}

// An object the array holds, at its own index.
bool IsObject(const Unreal& ue, uintptr_t address) {
    if (!address || address % P)
        return false;
    int32_t index = Int(address + ue.indexOffset);
    return index >= 0 && ObjectAt(ue, index) == address;
}

bool IsA(const Unreal& ue, uintptr_t type, uintptr_t ancestor) {
    for (int depth = 0; type && depth < 64; ++depth, type = SuperOf(ue, type))
        if (type == ancestor)
            return true;
    return false;
}

std::string PackageOf(Unreal& ue, uintptr_t object) {
    uintptr_t outer = object;
    for (int depth = 0; depth < 32; ++depth) {
        uintptr_t next = OuterOf(ue, outer);
        if (!next)
            break;
        outer = next;
    }
    return ObjectName(ue, outer);
}

uintptr_t DefaultObject(Unreal& ue, uintptr_t klass) {
    auto known = ue.defaultObjects.find(klass);
    if (known != ue.defaultObjects.end() && known->second && ClassOf(ue, known->second) == klass)
        return known->second;
    uintptr_t found = 0;
    ForEachObject(ue, [&](int32_t, uintptr_t object) {
        if (ClassOf(ue, object) == klass && (Int(object + ue.flagsOffset, 0) & kClassDefaultObject))
            found = object;
        return !found;
    });
    ue.defaultObjects[klass] = found;
    return found;
}

Unreal* FindUnreal(std::string& error) {
    Unreal& ue = g_unreal;
    if (!ue.checked)
        Detect(ue);
    if (!ue.present)
        return nullptr;
    if (!ue.discovered && ue.failure.empty()) {
        Heap heap;
        if (!FindNames(ue, heap))
            ue.failure = "error the library did not find the names of this Unreal Engine game (FNamePool or GNames)";
        else if (!FindObjects(ue, heap))
            ue.failure = "error the library did not find the objects of this Unreal Engine game (GUObjectArray)";
        else if (!FindLayout(ue, heap))
            ue.failure = "error the library did not recognize the layout of the classes of this Unreal Engine game";
        else
            ue.discovered = true;
        if (!ue.discovered)
            ue.nameCache.clear();
    }
    error = ue.failure;
    return &ue;
}

std::string DiscoverReflection(Unreal& ue) {
    if (ue.reflected)
        return ue.reflectionFailure;
    ue.reflected = true;
    // EPropertyFlags follows ArrayDim and ElementSize in every version.
    ue.propertyFlagsOffset = ue.elementSizeOffset + 4;
    Heap heap;
    if (!FindStructSize(ue))
        ue.reflectionFailure = "error the library did not find the size of the structs of this game (UStruct::PropertiesSize)";
    else if (!FindFunctionLayout(ue))
        ue.reflectionFailure = "error the library did not find the parameters of the functions of this game (UFunction::ParmsSize)";
    else if (!FindEnumLayout(ue, heap))
        ue.reflectionFailure = "error the library did not find the values of the enums of this game (UEnum::Names)";
    return ue.reflectionFailure;
}

std::string DiscoverProcessEvent(Unreal& ue) {
    if (ue.processEventSearched)
        return ue.processEventFailure;
    ue.processEventSearched = true;
    std::string failure = DiscoverReflection(ue);
    if (!failure.empty())
        return ue.processEventFailure = failure;
    // The default object of Object has the virtual table of UObject itself.
    ue.defaultObject = DefaultObject(ue, ue.objectClass);
    uintptr_t object = ue.defaultObject ? ue.defaultObject : ue.package;
    uintptr_t vtable = Ptr(object);
    // Other virtual functions may test the same flag: each is a candidate,
    // which the check of the first call confirms or rules out.
    for (int i = 0; vtable && i < 0x200 && ue.processEventCandidates.size() < 8; ++i) {
        uintptr_t entry = Ptr(vtable + static_cast<uintptr_t>(i) * P);
        if (!entry || !IsCode(entry, 1))
            break;
        if (CodeTestsBits(entry, static_cast<uint32_t>(ue.functionFlagsOffset), kFuncNative, 0x600))
            ue.processEventCandidates.push_back({i, entry});
    }
    if (ue.processEventCandidates.empty())
        return ue.processEventFailure = "error the library did not find UObject::ProcessEvent, which calls the functions of the objects, in their virtual table";
    ue.processEventIndex = ue.processEventCandidates.front().first;
    ue.processEventAddress = ue.processEventCandidates.front().second;
    return "";
}

Property Describe(Unreal& ue, uintptr_t address, uintptr_t owner) {
    Property property;
    property.address = address;
    property.owner = owner;
    if (ue.childPropertiesOffset) {
        property.name = NameAt(ue, address + ue.fieldNameOffset);
        property.kind = NameAt(ue, Ptr(address + ue.fieldClassOffset));
    } else {
        property.name = ObjectName(ue, address);
        property.kind = ObjectName(ue, ClassOf(ue, address));
    }
    property.offset = Int(address + ue.offsetOffset, 0);
    property.size = Int(address + ue.elementSizeOffset, 0);
    property.arrayDim = Int(address + ue.elementSizeOffset - 4, 1);
    if (ue.propertyFlagsOffset) {
        uint64_t flags = 0;
        if (Read(address + ue.propertyFlagsOffset, flags))
            property.flags = flags;
    }
    return property;
}

std::vector<Property> OwnProperties(Unreal& ue, uintptr_t type) {
    std::vector<Property> properties;
    if (ue.childPropertiesOffset) {
        uintptr_t field = Ptr(type + ue.childPropertiesOffset);
        for (int i = 0; field && i < 4096; ++i, field = Ptr(field + ue.fieldNextOffset))
            properties.push_back(Describe(ue, field, type));
    } else {
        uintptr_t field = Ptr(type + ue.childrenOffset);
        for (int i = 0; field && i < 4096; ++i, field = Ptr(field + ue.fieldNext)) {
            Property property = Describe(ue, field, type);
            if (IsPropertyKind(property.kind))
                properties.push_back(property);
        }
    }
    return properties;
}

std::vector<Property> Parameters(Unreal& ue, uintptr_t function) {
    std::vector<Property> parameters;
    for (Property& property : OwnProperties(ue, function))
        if (property.flags & kParm)
            parameters.push_back(std::move(property));
    return parameters;
}

std::vector<uintptr_t> Functions(Unreal& ue, uintptr_t type) {
    std::vector<uintptr_t> functions;
    uintptr_t field = Ptr(type + ue.childrenOffset);
    for (int i = 0; field && i < 4096; ++i, field = Ptr(field + ue.fieldNext))
        if (IsA(ue, ClassOf(ue, field), ue.functionClass))
            functions.push_back(field);
    return functions;
}

uintptr_t Extra(const Unreal& ue, const Property& property, size_t index) { return Ptr(property.address + ue.extraOffset + index * P); }

// FArrayProperty::Inner, after ArrayFlags since 5.x: the first member that is a property.
uintptr_t Inner(Unreal& ue, const Property& property, size_t index) {
    for (size_t at = index; at < index + 2; ++at) {
        uintptr_t inner = Extra(ue, property, at);
        if (inner && IsPropertyKind(Describe(ue, inner, 0).kind))
            return inner;
    }
    return 0;
}

uintptr_t EnumOf(Unreal& ue, const Property& property) {
    if (property.kind == "EnumProperty")
        return Extra(ue, property, 1);
    if (property.kind == "ByteProperty")
        return Extra(ue, property, 0);
    (void)ue;
    return 0;
}

std::string TypeName(Unreal& ue, const Property& property, int depth) {
    static const std::pair<const char*, const char*> kSimple[] = {
        {"IntProperty", "int32"},     {"Int8Property", "int8"},     {"Int16Property", "int16"},   {"Int64Property", "int64"},
        {"UInt16Property", "uint16"}, {"UInt32Property", "uint32"}, {"UInt64Property", "uint64"}, {"FloatProperty", "float"},
        {"DoubleProperty", "double"}, {"BoolProperty", "bool"},     {"NameProperty", "FName"},    {"StrProperty", "FString"},
        {"TextProperty", "FText"},
    };
    for (const auto& simple : kSimple)
        if (property.kind == simple.first)
            return simple.second;
    const std::string& kind = property.kind;
    auto named = [&](size_t index) {
        uintptr_t object = Extra(ue, property, index);
        return object ? ObjectName(ue, object) : std::string("?");
    };
    auto inner = [&](uintptr_t address) { return address && depth < 4 ? TypeName(ue, Describe(ue, address, 0), depth + 1) : std::string("?"); };
    if (kind == "ObjectProperty" || kind == "ObjectPtrProperty")
        return named(0) + "*";
    if (kind == "ClassProperty" || kind == "ClassPtrProperty")
        return "TSubclassOf<" + named(1) + ">";
    if (kind == "WeakObjectProperty")
        return "TWeakObjectPtr<" + named(0) + ">";
    if (kind == "LazyObjectProperty")
        return "TLazyObjectPtr<" + named(0) + ">";
    if (kind == "SoftObjectProperty")
        return "TSoftObjectPtr<" + named(0) + ">";
    if (kind == "SoftClassProperty")
        return "TSoftClassPtr<" + named(1) + ">";
    if (kind == "InterfaceProperty")
        return "TScriptInterface<" + named(0) + ">";
    if (kind == "StructProperty")
        return named(0);
    if (kind == "EnumProperty")
        return named(1);
    if (kind == "ByteProperty")
        return Extra(ue, property) ? named(0) : "uint8";
    if (kind == "ArrayProperty")
        return "TArray<" + inner(Inner(ue, property)) + ">";
    if (kind == "SetProperty")
        return "TSet<" + inner(Inner(ue, property)) + ">";
    if (kind == "MapProperty")
        return "TMap<" + inner(Extra(ue, property, 0)) + ", " + inner(Extra(ue, property, 1)) + ">";
    return kind;
}

const std::vector<TypeEntry>& Types(Unreal& ue, bool refresh) {
    auto now = std::chrono::steady_clock::now();
    if (refresh || !g_types.valid || now - g_types.at > std::chrono::seconds(10)) {
        g_types.types = ListTypesNow(ue);
        g_types.at = now;
        g_types.valid = true;
    }
    return g_types.types;
}

// A class, struct or enum by its name, or <package>:<name>.
bool FindType(Unreal& ue, const std::string& text, TypeEntry& found, std::string& error) {
    if (text.empty()) {
        error = "error give the class";
        return false;
    }
    size_t colon = text.rfind(':');
    std::string package = colon == std::string::npos ? "" : text.substr(0, colon);
    std::string name = colon == std::string::npos ? text : text.substr(colon + 1);
    std::vector<TypeEntry> matches;
    // A type loaded since the list was made: the list is made again, at most once a second.
    for (int pass = 0; pass < 2 && matches.empty(); ++pass) {
        if (pass && std::chrono::steady_clock::now() - g_types.at < std::chrono::seconds(1))
            break;
        const std::vector<TypeEntry>& types = Types(ue, pass == 1);
        for (bool exact : {true, false}) {
            for (const TypeEntry& type : types) {
                bool same = exact ? type.name == name : Lower(type.name) == Lower(name);
                if (same && PackageMatches(type.package, package))
                    matches.push_back(type);
            }
            if (!matches.empty())
                break;
        }
    }
    if (matches.size() == 1) {
        found = matches[0];
        return true;
    }
    if (matches.empty()) {
        error = "error no class, struct or enum is named " + text + ": see engine classes";
        return false;
    }
    error = "error several types are named " + name + ", give the package:";
    for (size_t i = 0; i < matches.size() && i < 20; ++i)
        error += " " + matches[i].package + ":" + matches[i].name;
    return false;
}

// The property of `type` or of its parents.
bool FindProperty(Unreal& ue, uintptr_t type, const std::string& name, Property& found) {
    for (bool exact : {true, false})
        for (uintptr_t level = type; level; level = SuperOf(ue, level))
            for (const Property& property : OwnProperties(ue, level))
                if (exact ? property.name == name : Lower(property.name) == Lower(name)) {
                    found = property;
                    return true;
                }
    return false;
}

uintptr_t FindFunction(Unreal& ue, uintptr_t type, const std::string& name) {
    for (bool exact : {true, false})
        for (uintptr_t level = type; level; level = SuperOf(ue, level))
            for (uintptr_t function : Functions(ue, level)) {
                std::string own = ObjectName(ue, function);
                if (exact ? own == name : Lower(own) == Lower(name))
                    return function;
            }
    return 0;
}

std::vector<EnumValue> EnumValues(Unreal& ue, uintptr_t enumeration) {
    std::vector<EnumValue> values;
    if (!ue.enumNamesOffset)
        return values;
    uintptr_t data = Ptr(enumeration + ue.enumNamesOffset);
    int32_t count = Int(enumeration + ue.enumNamesOffset + P, 0);
    for (int32_t i = 0; data && i < count && i < 65536; ++i) {
        uintptr_t entry = data + static_cast<uintptr_t>(i) * ue.enumEntrySize;
        EnumValue value;
        value.name = NameAt(ue, entry);
        // EnumClass and Namespaced enums prefix their values: ETeam::Red.
        size_t scope = value.name.rfind("::");
        if (scope != std::string::npos)
            value.name = value.name.substr(scope + 2);
        if (!Read(entry + ue.enumEntrySize - 8, value.value))
            break;
        values.push_back(value);
    }
    return values;
}

uint32_t FunctionFlags(const Unreal& ue, uintptr_t function) {
    return ue.functionFlagsOffset ? static_cast<uint32_t>(Int(function + ue.functionFlagsOffset, 0)) : 0;
}

int32_t ParmsSize(const Unreal& ue, uintptr_t function) { return ue.parmsSizeOffset ? Word(function + ue.parmsSizeOffset) : 0; }

int32_t StructSize(const Unreal& ue, uintptr_t type) { return ue.propertiesSizeOffset ? Int(type + ue.propertiesSizeOffset, 0) : 0; }

int32_t StructAlignment(const Unreal& ue, uintptr_t type) {
    int32_t alignment = ue.propertiesSizeOffset ? Int(type + ue.propertiesSizeOffset + 4, 0) : 0;
    return IsPowerOfTwo(alignment) && alignment <= 16 ? alignment : 1;
}

void BoolLayout(const Unreal& ue, const Property& property, uint8_t& byteOffset, uint8_t& mask) {
    // FBoolProperty: FieldSize, ByteOffset, ByteMask, FieldMask.
    uint8_t layout[4] = {};
    byteOffset = 0;
    mask = 0;
    if (ReadMemory(property.address + ue.extraOffset, layout, sizeof(layout))) {
        byteOffset = layout[1];
        mask = layout[3];
    }
}

} // namespace uih::detail::remote::unreal

namespace uih::detail::remote {

bool HandleUnreal(const std::string& sub, const std::string& arguments, std::vector<uint8_t>& payload, std::string& answer) {
    using namespace unreal;
    std::string error;
    Unreal* ue = FindUnreal(error);
    if (!ue)
        return false;
    if (sub == "info") {
        answer = Info(*ue);
        return true;
    }
    if (!error.empty()) {
        answer = error;
        return true;
    }
    if (sub == "assemblies")
        answer = ListPackages(*ue, payload);
    else if (sub == "classes")
        answer = ListTypes(*ue, arguments, payload);
    else if (sub == "class")
        answer = DescribeType(*ue, arguments, payload);
    else if (sub == "get")
        answer = GetValue(*ue, arguments, payload);
    else if (sub == "set")
        answer = SetValue(*ue, arguments, payload);
    else if (sub == "instances")
        answer = Instances(*ue, arguments, payload);
    else if (sub == "call")
        answer = Call(*ue, arguments, payload);
    else
        answer = DumpSdk(*ue, arguments, payload);
    return true;
}

} // namespace uih::detail::remote
