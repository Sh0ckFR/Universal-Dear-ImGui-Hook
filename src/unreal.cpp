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
// Nothing runs the game's code, from any thread.
//   engine info                       -> ok unreal ue4|ue5 <version|-> <module>
//   engine assemblies                 -> lines <package> <types>
//   engine classes <filter|*> [pkg]   -> lines <package> <class>
//   engine class <class>              -> the class, its properties with their offset, its functions
//   engine get <class> <prop> <obj>   -> <type> <value>
//   engine set <class> <prop> <obj> <value> -> <type> <value>, as read back
//   engine instances <class> [count]  -> lines <address> <class> <name>
#include "internal.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cinttypes>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <unordered_map>

namespace uih::detail::remote {
namespace {

constexpr size_t P = sizeof(void*);
constexpr size_t kItemsPerChunk = 64 * 1024;
constexpr size_t kMaxListed = 1000;
constexpr size_t kMaxInstances = 1000;
// EObjectFlags
constexpr int32_t kClassDefaultObject = 0x10;
constexpr int32_t kArchetypeObject = 0x20;

struct Unreal {
    bool checked = false;
    bool present = false;
    std::string module;
    std::string version = "-";
    // The modules whose data holds the names and the objects.
    std::vector<LoadedModule> cores;

    bool discovered = false;
    std::string failure;
    // Names: &FNamePool::Blocks[0], or the TNameEntryArray.
    bool namePool = false;
    uintptr_t names = 0;
    size_t legacyText = 0;
    size_t legacyIndex = 0;
    // Objects: the chunk table of FChunkedFixedUObjectArray, or the items of
    // FFixedUObjectArray, and where its number of elements is, if known.
    uintptr_t objects = 0;
    bool flat = false;
    uintptr_t numElements = 0;
    size_t itemSize = 0;
    size_t itemObject = 0;
    // UObjectBase
    size_t flagsOffset = P;
    size_t indexOffset = P + 4;
    size_t classOffset = P + 8;
    size_t nameOffset = 2 * P + 8;
    size_t outerOffset = 0;
    // UField::Next, UStruct
    size_t fieldNext = 0;
    size_t superOffset = 0;
    size_t childrenOffset = 0;
    // 0 before 4.25, whose properties are UObjects in the Children list.
    size_t childPropertiesOffset = 0;
    // FField
    size_t fieldClassOffset = P;
    size_t fieldNextOffset = 0;
    size_t fieldNameOffset = 0;
    // FProperty or UProperty, from its start: ElementSize, Offset_Internal,
    // and the members of its subclasses, such as FStructProperty::Struct.
    size_t elementSizeOffset = 0;
    size_t offsetOffset = 0;
    size_t extraOffset = 0;
    bool doubles = false;
    // The classes of the classes.
    uintptr_t classClass = 0;
    uintptr_t structClass = 0;
    uintptr_t scriptStructClass = 0;
    uintptr_t functionClass = 0;
    uintptr_t enumClass = 0;
    std::unordered_map<uint32_t, std::string> nameCache;
};

Unreal g_unreal;

// Memory ------------------------------------------------------------------

template <typename T>
bool Read(uintptr_t address, T& value) {
    return address && ReadMemory(address, &value, sizeof(T));
}

uintptr_t Ptr(uintptr_t address) {
    uintptr_t value = 0;
    return Read(address, value) ? value : 0;
}

int32_t Int(uintptr_t address, int32_t fallback = -1) {
    int32_t value = fallback;
    return Read(address, value) ? value : fallback;
}

// The readable memory outside the modules, sorted, where the engine allocates.
struct Heap {
    std::vector<MemoryRegion> regions;
    std::vector<LoadedModule> modules;

    Heap() : regions(MemoryRegions()), modules(SortedModules()) {
        std::sort(regions.begin(), regions.end(), [](const MemoryRegion& a, const MemoryRegion& b) { return a.base < b.base; });
    }
    bool Contains(uintptr_t address) const {
        if (!address || address % P)
            return false;
        auto region = std::upper_bound(regions.begin(), regions.end(), address, [](uintptr_t value, const MemoryRegion& r) { return value < r.base; });
        if (region == regions.begin() || address - std::prev(region)->base >= std::prev(region)->size)
            return false;
        auto module = std::upper_bound(modules.begin(), modules.end(), address, [](uintptr_t value, const LoadedModule& m) { return value < m.base; });
        return module == modules.begin() || address - std::prev(module)->base >= std::prev(module)->size;
    }
};

// Text --------------------------------------------------------------------

std::string Hex(uintptr_t value) {
    char text[32];
    snprintf(text, sizeof(text), "0x%" PRIxPTR, value);
    return text;
}

std::string Lower(std::string text) {
    for (char& c : text)
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return text;
}

std::string NextWord(const std::string& text, size_t& at) {
    while (at < text.size() && text[at] == ' ')
        ++at;
    size_t start = at;
    while (at < text.size() && text[at] != ' ')
        ++at;
    return text.substr(start, at - start);
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
        uintptr_t chunk = Ptr(ue.names + (index / 16384) * P);
        uintptr_t entry = chunk ? Ptr(chunk + (index % 16384) * P) : 0;
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

// Objects -----------------------------------------------------------------

std::string ObjectName(Unreal& ue, uintptr_t object) { return NameAt(ue, object + ue.nameOffset); }
uintptr_t ClassOf(const Unreal& ue, uintptr_t object) { return Ptr(object + ue.classOffset); }
uintptr_t OuterOf(const Unreal& ue, uintptr_t object) { return Ptr(object + ue.outerOffset); }
uintptr_t SuperOf(const Unreal& ue, uintptr_t type) { return Ptr(type + ue.superOffset); }

int32_t ObjectCount(const Unreal& ue) {
    int32_t count = ue.numElements ? Int(ue.numElements, 0) : 0;
    return count > 0 ? count : INT_MAX;
}

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
    ue.structClass = core["Struct"];
    ue.scriptStructClass = core["ScriptStruct"];
    ue.functionClass = core["Function"];
    ue.enumClass = core["Enum"];
    uintptr_t vector = core["Vector"];
    uintptr_t transform = core["Transform"];
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

// Types -------------------------------------------------------------------

struct Property {
    uintptr_t address = 0;
    std::string name;
    // Its class, such as IntProperty.
    std::string kind;
    int32_t offset = 0;
    int32_t size = 0;
    uintptr_t owner = 0;
};

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
    return property;
}

bool IsPropertyKind(const std::string& kind) { return kind.size() > 8 && kind.compare(kind.size() - 8, 8, "Property") == 0; }

// The properties a struct, class or function declares itself, in order.
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

std::vector<uintptr_t> Functions(Unreal& ue, uintptr_t type) {
    std::vector<uintptr_t> functions;
    uintptr_t field = Ptr(type + ue.childrenOffset);
    for (int i = 0; field && i < 4096; ++i, field = Ptr(field + ue.fieldNext))
        if (IsA(ue, ClassOf(ue, field), ue.functionClass))
            functions.push_back(field);
    return functions;
}

// An object, struct or enum the property refers to: FObjectPropertyBase::PropertyClass, FStructProperty::Struct...
uintptr_t Extra(const Unreal& ue, const Property& property, size_t index = 0) { return Ptr(property.address + ue.extraOffset + index * P); }

// FArrayProperty::Inner, after ArrayFlags since 5.x: the first member that is a property.
uintptr_t Inner(Unreal& ue, const Property& property, size_t index = 0) {
    for (size_t at = index; at < index + 2; ++at) {
        uintptr_t inner = Extra(ue, property, at);
        if (inner && IsPropertyKind(Describe(ue, inner, 0).kind))
            return inner;
    }
    return 0;
}

std::string TypeName(Unreal& ue, const Property& property, int depth = 0) {
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

struct TypeEntry {
    uintptr_t object;
    std::string name;
    std::string package;
    // class, struct or enum
    const char* kind;
};

std::vector<TypeEntry> Types(Unreal& ue) {
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
    std::vector<TypeEntry> types = Types(ue);
    for (bool exact : {true, false}) {
        for (const TypeEntry& type : types) {
            bool same = exact ? type.name == name : Lower(type.name) == Lower(name);
            if (same && PackageMatches(type.package, package))
                matches.push_back(type);
        }
        if (!matches.empty())
            break;
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
    for (int depth = 0; type && depth < 64; ++depth, type = SuperOf(ue, type))
        for (const Property& property : OwnProperties(ue, type))
            if (property.name == name || Lower(property.name) == Lower(name)) {
                found = property;
                return true;
            }
    return false;
}

// Values ------------------------------------------------------------------

std::string QuoteJson(const std::string& text) { return JsonQuote(text); }

std::string FormatObject(Unreal& ue, uintptr_t object) {
    if (!object)
        return "null";
    if (!IsObject(ue, object))
        return Hex(object) + " (not an object)";
    return Hex(object) + " (" + ObjectName(ue, ClassOf(ue, object)) + " " + ObjectName(ue, object) + ")";
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
        uint8_t layout[4] = {};
        uint8_t byte = 0;
        if (!ReadMemory(property.address + ue.extraOffset, layout, sizeof(layout)) || !Read(address + layout[1], byte))
            return "?";
        return byte & layout[3] ? "true" : "false";
    }
    if (kind == "NameProperty")
        return QuoteJson(NameAt(ue, address));
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
        return QuoteJson(FromUtf16(units.data(), length));
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
        uintptr_t type = Extra(ue, property);
        std::vector<Property> fields;
        for (int level = 0; type && level < 16; ++level, type = SuperOf(ue, type)) {
            std::vector<Property> own = OwnProperties(ue, type);
            fields.insert(fields.begin(), own.begin(), own.end());
        }
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

// Writes `text` as the value of `property` at `address`.
bool Encode(Unreal& ue, const Property& property, uintptr_t address, const std::string& text, std::string& error) {
    const std::string& kind = property.kind;
    char* end = nullptr;
    auto integer = [&](size_t size, bool isSigned) {
        errno = 0;
        int64_t value = isSigned ? strtoll(text.c_str(), &end, 0) : static_cast<int64_t>(strtoull(text.c_str(), &end, 0));
        if (text.empty() || *end || errno) {
            error = "error " + text + " is not an integer";
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
        uint8_t layout[4] = {};
        uint8_t byte = 0;
        if (!ReadMemory(property.address + ue.extraOffset, layout, sizeof(layout)) || !Read(address + layout[1], byte))
            return false;
        byte = static_cast<uint8_t>(lower == "true" || lower == "1" ? byte | layout[3] : byte & ~layout[3]);
        return WriteMemory(address + layout[1], &byte, 1);
    }
    if (kind == "ObjectProperty" || kind == "ObjectPtrProperty") {
        uintptr_t object = text == "null" ? 0 : static_cast<uintptr_t>(strtoull(text.c_str(), &end, 16));
        if (text != "null" && (text.empty() || *end || !IsObject(ue, object) || !IsA(ue, ClassOf(ue, object), Extra(ue, property)))) {
            error = "error give null or the address of an object of " + ObjectName(ue, Extra(ue, property)) + ", from engine instances";
            return false;
        }
        return WriteMemory(address, &object, sizeof(object));
    }
    error = "error the library cannot write a " + TypeName(ue, property) + " yet: numbers, enums, bools and object references";
    return false;
}

// Commands ----------------------------------------------------------------

std::string Info(const Unreal& ue) {
    bool five = ue.version[0] == '5' || (ue.version == "-" && ue.discovered && ue.doubles);
    return std::string("ok unreal ") + (five ? "ue5 " : "ue4 ") + ue.version + " " + ue.module;
}

std::string ListPackages(Unreal& ue, std::vector<uint8_t>& payload) {
    std::vector<std::pair<std::string, size_t>> packages;
    for (const TypeEntry& type : Types(ue)) {
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
    for (const TypeEntry& type : Types(ue)) {
        if (!PackageMatches(type.package, package) || (filter != "*" && Lower(type.name).find(filter) == std::string::npos))
            continue;
        if (listed < kMaxListed) {
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

std::string FunctionSignature(Unreal& ue, uintptr_t function) {
    std::string returned = "void";
    std::string parameters;
    for (const Property& parameter : OwnProperties(ue, function)) {
        if (parameter.name == "ReturnValue") {
            returned = TypeName(ue, parameter);
            continue;
        }
        parameters += (parameters.empty() ? "" : ", ") + TypeName(ue, parameter) + " " + parameter.name;
    }
    return returned + " " + ObjectName(ue, function) + "(" + parameters + ")";
}

std::string PropertyLine(Unreal& ue, const Property& property) {
    int32_t offset = property.offset;
    std::string suffix;
    if (property.kind == "BoolProperty") {
        uint8_t layout[4] = {};
        if (ReadMemory(property.address + ue.extraOffset, layout, sizeof(layout))) {
            offset += layout[1];
            if (layout[3] != 0xff) {
                char mask[16];
                snprintf(mask, sizeof(mask), " (bit 0x%x)", layout[3]);
                suffix = mask;
            }
        }
    }
    char head[32];
    snprintf(head, sizeof(head), "  0x%x ", static_cast<unsigned>(offset));
    return head + TypeName(ue, property) + " " + property.name + suffix;
}

std::string DescribeType(Unreal& ue, const std::string& rest, std::vector<uint8_t>& payload) {
    size_t at = 0;
    TypeEntry type;
    std::string error;
    if (!FindType(ue, NextWord(rest, at), type, error))
        return error;
    std::string text = std::string(type.kind) + " " + type.name;
    if (strcmp(type.kind, "enum") == 0)
        return Payload(text + " (" + type.package + ")\n", payload);
    std::vector<uintptr_t> lineage;
    for (uintptr_t level = type.object; level && lineage.size() < 64; level = SuperOf(ue, level))
        lineage.push_back(level);
    for (size_t i = 1; i < lineage.size(); ++i)
        text += (i == 1 ? " : " : " : ") + ObjectName(ue, lineage[i]);
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

// The property at `path` (a.b for the member b of the struct a) in the object.
bool Locate(Unreal& ue, const TypeEntry& type, const std::string& path, const std::string& objectText, Property& property, uintptr_t& address,
            std::string& error) {
    uintptr_t object = ParseObject(objectText);
    if (objectText.empty() || objectText == "-") {
        error = "error give the object: properties of Unreal classes belong to objects, which engine instances lists";
        return false;
    }
    if (!IsObject(ue, object) || !IsA(ue, ClassOf(ue, object), type.object)) {
        error = "error " + objectText + " is not an object of " + type.name + ": see engine instances " + type.name;
        return false;
    }
    uintptr_t owner = type.object;
    address = object;
    size_t start = 0;
    while (start <= path.size()) {
        size_t dot = path.find('.', start);
        std::string name = path.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        if (!FindProperty(ue, owner, name, property)) {
            error = "error " + ObjectName(ue, owner) + " has no property " + name + ": see engine class " + ObjectName(ue, owner);
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

std::string GetValue(Unreal& ue, const std::string& rest, std::vector<uint8_t>& payload) {
    size_t at = 0;
    std::string typeName = NextWord(rest, at);
    std::string path = NextWord(rest, at);
    std::string objectText = NextWord(rest, at);
    TypeEntry type;
    Property property;
    uintptr_t address = 0;
    std::string error;
    if (!FindType(ue, typeName, type, error) || !Locate(ue, type, path, objectText, property, address, error))
        return error;
    return Payload(TypeName(ue, property) + " " + FormatValue(ue, property, address), payload);
}

std::string SetValue(Unreal& ue, const std::string& rest, std::vector<uint8_t>& payload) {
    size_t at = 0;
    std::string typeName = NextWord(rest, at);
    std::string path = NextWord(rest, at);
    std::string objectText = NextWord(rest, at);
    std::string value = rest.substr(std::min(at, rest.size()));
    value.erase(0, value.find_first_not_of(' '));
    TypeEntry type;
    Property property;
    uintptr_t address = 0;
    std::string error;
    if (!FindType(ue, typeName, type, error) || !Locate(ue, type, path, objectText, property, address, error))
        return error;
    if (value.empty())
        return "error give the value";
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

} // namespace

bool HandleUnreal(const std::string& sub, const std::string& arguments, std::vector<uint8_t>& payload, std::string& answer) {
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
    else
        answer = "error engine call does not support Unreal Engine games yet: the library does not call their functions";
    return true;
}

} // namespace uih::detail::remote
