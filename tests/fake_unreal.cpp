// The objects of an Unreal Engine game in the memory of the test host: see
// tests/fake_unreal.h. The layouts follow Epic's engine headers for 5.8
// (generation 5) and 4.22 (generation 4); src/unreal.cpp must find every
// offset by itself, UObject::ProcessEvent included: the objects share a
// virtual table whose entry 0x44 or 0x4f tests FUNC_Native in the function
// it calls, as the engine's does, after entries that test other bits.
#include "fake_unreal.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/syscall.h>
#include <unistd.h>
#endif

#if defined(_MSC_VER)
#define FAKE_NOINLINE __declspec(noinline)
#else
#define FAKE_NOINLINE __attribute__((noinline))
#endif

namespace {

constexpr size_t P = sizeof(void*);

// The offsets of the Builder below, as constants: UStruct::PropertiesSize
// after the lists of children, then UFunction::FunctionFlags further, as in
// the engine, and the native function the fake calls, after the counters.
constexpr size_t kNameAt = 2 * P + 8;
constexpr size_t OuterAt(int generation) { return generation == 5 ? (kNameAt + 12 + P - 1) / P * P : kNameAt + 8; }
constexpr size_t PropertiesSizeAt(int generation) {
    return OuterAt(generation) + 2 * P + 2 * P + P + (generation == 5 ? P : 0) + P;
}
constexpr size_t FunctionFlagsAt(int generation) { return PropertiesSizeAt(generation) + 8 + 0x50; }
constexpr size_t HandlerAt(int generation) { return (FunctionFlagsAt(generation) + 0x10 + P - 1) / P * P; }

// EFunctionFlags and EPropertyFlags
constexpr uint32_t kNative = 0x400;
constexpr uint32_t kStatic = 0x2000;
constexpr uint32_t kCallable = 0x04020000;
constexpr uint64_t kParm = 0x80;
constexpr uint64_t kOut = 0x100;
constexpr uint64_t kReturn = 0x400 | 0x100;
constexpr uint64_t kReference = 0x8000000;

uint64_t ThreadId() {
#ifdef _WIN32
    return GetCurrentThreadId();
#else
    return static_cast<uint64_t>(syscall(SYS_gettid));
#endif
}

std::atomic<uint64_t> g_callThread{0};
std::atomic<int> g_nativeCalls{0};
std::atomic<int> g_scriptCalls{0};
int g_generation = 0;

using Handler = void (*)(uintptr_t self, uint8_t* parms);

FAKE_NOINLINE void RunNative(uintptr_t self, uintptr_t function, void* parms, size_t handlerAt) {
    g_callThread = ThreadId();
    ++g_nativeCalls;
    Handler handler;
    memcpy(&handler, reinterpret_cast<const void*>(function + handlerAt), sizeof(handler));
    if (handler)
        handler(self, static_cast<uint8_t*>(parms));
}

// A blueprint function: its bytecode, here a C++ function as well.
FAKE_NOINLINE void RunScript(uintptr_t self, uintptr_t function, void* parms, size_t handlerAt) {
    g_callThread = ThreadId();
    ++g_scriptCalls;
    Handler handler;
    memcpy(&handler, reinterpret_cast<const void*>(function + handlerAt), sizeof(handler));
    if (handler)
        handler(self, static_cast<uint8_t*>(parms));
}

// UFunction up to FunctionFlags, at an offset the compiler writes in the instruction.
template <size_t Flags>
struct FunctionView {
    uint8_t before[Flags];
    uint32_t flags;
};

// UObject::ProcessEvent(UFunction*, void* Parms), a virtual function: the
// object comes in ecx on 32-bit Windows, as __fastcall passes its first argument.
#if defined(_WIN32) && (defined(_M_IX86) || defined(__i386__))
template <int Generation>
void __fastcall ProcessEvent(uintptr_t self, uintptr_t, uintptr_t function, void* parms) {
#else
template <int Generation>
void ProcessEvent(uintptr_t self, uintptr_t function, void* parms) {
#endif
    if (reinterpret_cast<const FunctionView<FunctionFlagsAt(Generation)>*>(function)->flags & kNative)
        RunNative(self, function, parms, HandlerAt(Generation));
    else
        RunScript(self, function, parms, HandlerAt(Generation));
}

// Virtual functions before ProcessEvent that test other bits of the
// function, or the same bit elsewhere: the library must not take them for it.
template <int Generation>
void TestsStatic(uintptr_t self, uintptr_t function, void* parms) {
    if (reinterpret_cast<const FunctionView<FunctionFlagsAt(Generation)>*>(function)->flags & kStatic)
        RunScript(self, function, parms, 0);
}

template <int Generation>
void TestsElsewhere(uintptr_t self, uintptr_t function, void* parms) {
    if (reinterpret_cast<const FunctionView<FunctionFlagsAt(Generation) + 8>*>(function)->flags & kNative)
        RunScript(self, function, parms, 0);
}

void Nothing() {}

// The virtual table of UObject in the engine: ProcessEvent at 0x44 in 4.22, 0x4f in 5.8.
uintptr_t g_vtable[0x60];

template <int Generation>
int BuildVirtualTable() {
    int processEvent = Generation == 5 ? 0x4f : 0x44;
    for (uintptr_t& entry : g_vtable)
        entry = reinterpret_cast<uintptr_t>(&Nothing);
    g_vtable[3] = reinterpret_cast<uintptr_t>(&TestsStatic<Generation>);
    g_vtable[processEvent - 2] = reinterpret_cast<uintptr_t>(&TestsElsewhere<Generation>);
    g_vtable[processEvent] = reinterpret_cast<uintptr_t>(&ProcessEvent<Generation>);
    return processEvent;
}

// FUObjectArray, whose FChunkedFixedUObjectArray follows four counters.
struct ObjectArray {
    int32_t firstGCIndex;
    int32_t lastNonGCIndex;
    int32_t maxObjectsNotConsideredByGC;
    bool openForDisregardForGC;
    // 5.8: Objects, NumElements, MaxElements, NumChunks, MaxChunks, PreAllocatedObjects.
    // 4.22: Objects, PreAllocatedObjects, MaxElements, NumElements, MaxChunks, NumChunks.
    void* objects;
    union {
        struct {
            int32_t numElements, maxElements, numChunks, maxChunks;
            void* preAllocated;
        } five;
        struct {
            void* preAllocated;
            int32_t maxElements, numElements, maxChunks, numChunks;
        } four;
    };
};

// FNameEntryAllocator of FNamePool: the lock, the cursor, then the blocks.
struct NamePool {
    uint64_t lock;
    uint32_t currentBlock;
    uint32_t currentByteCursor;
    uint8_t* blocks[8192];
};

ObjectArray GUObjectArray;
NamePool NamePoolData;
// 4.22: TNameEntryArray, allocated on first use.
void* GNames;
// Kept, so that the globals stay as they are written.
void* volatile g_keep[3] = {&GUObjectArray, &NamePoolData, &GNames};

// What the code that registers the classes names in UTF-16, and the branch of the engine.
volatile char16_t g_scriptPackage[32];
volatile char16_t g_branch[32];

struct Builder {
    int generation;
    // UObjectBase
    size_t flags = P, index = P + 4, klass = P + 8, name = 2 * P + 8, outer = 0;
    size_t fieldNext = 0, superStruct = 0, children = 0, childProperties = 0;
    // FField
    size_t fieldClass = P, fieldOwner = 2 * P, fieldNextField = 3 * P, fieldName = 4 * P;
    // FProperty or UProperty
    size_t arrayDim = 0, offset = 0, extra = 0;
    // FUObjectItem
    size_t itemSize = 0, itemObject = 0;
    // UStruct, UFunction, UEnum
    size_t propertiesSize = 0, functionFlags = 0, handler = 0, enumNames = 0;

    uint8_t* items = nullptr;
    int32_t count = 0;
    uint8_t* block = nullptr;
    size_t cursor = 0;
    void** legacyChunk = nullptr;
    uint32_t legacyCount = 0;
    std::vector<std::pair<std::string, uint32_t>> names;
    uintptr_t classClass = 0;
    uintptr_t functionClass = 0;
    uintptr_t package = 0;

    explicit Builder(int g) : generation(g) {
        // After the name, and the hash index of 5.x.
        outer = generation == 5 ? (name + 12 + P - 1) / P * P : name + 8;
        fieldNext = outer + P;
        size_t fieldSize = fieldNext + P;
        // FStructBaseChain, then SuperStruct, Children and, since 4.25, ChildProperties.
        superStruct = fieldSize + 2 * P;
        children = superStruct + P;
        childProperties = generation == 5 ? children + P : 0;
        propertiesSize = (generation == 5 ? childProperties : children) + P;
        functionFlags = FunctionFlagsAt(generation);
        handler = HandlerAt(generation);
        // After UField and the CppType string.
        enumNames = fieldSize + P + 8;
        if (generation == 5) {
            arrayDim = (fieldName + 8 + 4 + P - 1) / P * P;
            offset = arrayDim + 0x14;
            // PropertyLinkNext, NextRef, DestructorLinkNext, PostConstructLinkNext.
            extra = (offset + 4 + P - 1) / P * P + 4 * P;
            itemSize = sizeof(int64_t) + P + 8 > 24 ? 32 : 24;
            itemObject = 8;
        } else {
            arrayDim = fieldSize;
            offset = arrayDim + 0x14;
            // RepNotifyFunc, then the four links.
            extra = (offset + 4 + P - 1) / P * P + 8 + 4 * P;
            itemSize = 4 * P;
            itemObject = 0;
        }
    }

    template <typename T>
    static void Put(uintptr_t address, T value) {
        memcpy(reinterpret_cast<void*>(address), &value, sizeof(T));
    }
    template <typename T>
    static T Get(uintptr_t address) {
        T value;
        memcpy(&value, reinterpret_cast<const void*>(address), sizeof(T));
        return value;
    }

    // The FName index of `text`.
    uint32_t Name(const std::string& text) {
        for (const auto& known : names)
            if (known.first == text)
                return known.second;
        uint32_t id;
        if (generation == 5) {
            if (!block) {
                block = static_cast<uint8_t*>(calloc(1, 1 << 18));
                NamePoolData.blocks[0] = block;
            }
            id = static_cast<uint32_t>(cursor / 2);
            uint16_t header = static_cast<uint16_t>(text.size() << 6 | ((text.size() * 7) & 0x1f) << 1);
            memcpy(block + cursor, &header, 2);
            memcpy(block + cursor + 2, text.data(), text.size());
            cursor += (2 + text.size() + 1) / 2 * 2;
            NamePoolData.currentByteCursor = static_cast<uint32_t>(cursor);
        } else {
            if (!legacyChunk) {
                void** table = static_cast<void**>(calloc(128 + 2, P));
                legacyChunk = static_cast<void**>(calloc(16384, P));
                table[0] = legacyChunk;
                GNames = table;
            }
            id = legacyCount++;
            // Index, HashNext, then the text.
            uint8_t* entry = static_cast<uint8_t*>(calloc(1, 2 * P + text.size() + 1));
            int32_t index = static_cast<int32_t>(id << 1);
            memcpy(entry, &index, 4);
            memcpy(entry + 2 * P, text.data(), text.size());
            legacyChunk[id] = entry;
        }
        names.push_back({text, id});
        return id;
    }

    void SetName(uintptr_t at, const std::string& text, uint32_t number = 0) {
        Put<uint32_t>(at, Name(text));
        Put<uint32_t>(at + 4, number);
    }

    uintptr_t Object(uintptr_t type, uintptr_t outerObject, const std::string& text, size_t size, uint32_t number = 0, int32_t objectFlags = 0) {
        auto object = reinterpret_cast<uintptr_t>(calloc(1, size < 0x200 ? 0x200 : size));
        Put<uintptr_t>(object, reinterpret_cast<uintptr_t>(g_vtable));
        Put<int32_t>(object + flags, objectFlags);
        Put<int32_t>(object + index, count);
        Put<uintptr_t>(object + klass, type ? type : object);
        SetName(object + name, text, number);
        if (generation == 5)
            Put<int32_t>(object + name + 8, -1);
        Put<uintptr_t>(object + outer, outerObject);
        memcpy(items + count * itemSize + itemObject, &object, P);
        ++count;
        if (generation == 5)
            GUObjectArray.five.numElements = count;
        else
            GUObjectArray.four.numElements = count;
        return object;
    }

    uintptr_t Type(uintptr_t type, uintptr_t outerObject, const std::string& text, uintptr_t parent) {
        uintptr_t object = Object(type, outerObject, text, 0x200);
        Put<uintptr_t>(object + superStruct, parent);
        return object;
    }

    // FFieldClass for 5.8, a UClass for 4.22.
    std::vector<std::pair<std::string, uintptr_t>> kinds;
    uintptr_t Kind(const std::string& text) {
        for (const auto& known : kinds)
            if (known.first == text)
                return known.second;
        uintptr_t kind;
        if (generation == 5) {
            kind = reinterpret_cast<uintptr_t>(calloc(1, 0x40));
            SetName(kind, text);
        } else {
            kind = Type(classClass, package, text, 0);
        }
        kinds.push_back({text, kind});
        return kind;
    }

    // A property of `owner`, appended to its list unless `owner` is 0 (the inner property of a container).
    uintptr_t Property(uintptr_t owner, const std::string& kind, const std::string& text, int32_t at, int32_t size, uint64_t propertyFlags = 0x0010000000000005ull) {
        uintptr_t property;
        if (generation == 5) {
            property = reinterpret_cast<uintptr_t>(calloc(1, 0x100));
            Put<uintptr_t>(property + fieldClass, Kind(kind));
            Put<uintptr_t>(property + fieldOwner, owner | 1);
            SetName(property + fieldName, text);
        } else {
            property = Object(Kind(kind), owner ? owner : package, text, 0x100);
        }
        Put<int32_t>(property + arrayDim, 1);
        Put<int32_t>(property + arrayDim + 4, size);
        Put<uint64_t>(property + arrayDim + 8, propertyFlags);
        Put<int32_t>(property + offset, at);
        if (owner)
            Append(owner, property, generation == 5);
        return property;
    }

    void Append(uintptr_t owner, uintptr_t field, bool isFField) {
        size_t head = isFField ? childProperties : children;
        size_t next = isFField ? fieldNextField : fieldNext;
        uintptr_t last = Get<uintptr_t>(owner + head);
        if (!last) {
            Put<uintptr_t>(owner + head, field);
            return;
        }
        while (Get<uintptr_t>(last + next))
            last = Get<uintptr_t>(last + next);
        Put<uintptr_t>(last + next, field);
    }

    // A function of `owner`, which ProcessEvent runs with `run`.
    uintptr_t Function(uintptr_t owner, const std::string& text, uint32_t functionFlagsValue = kNative | kCallable, Handler run = nullptr) {
        uintptr_t function = Type(functionClass, owner, text, 0);
        Put<uint32_t>(function + functionFlags, functionFlagsValue);
        // NumParms, ParmsSize, ReturnValueOffset.
        Put<uint8_t>(function + functionFlags + 4, 0);
        Put<uint16_t>(function + functionFlags + 6, 0);
        Put<uint16_t>(function + functionFlags + 8, 0xffff);
        Put<Handler>(function + handler, run);
        Append(owner, function, false);
        return function;
    }

    // A parameter of a function: CPF_Parm and `more`.
    uintptr_t Param(uintptr_t function, const std::string& kind, const std::string& text, int32_t at, int32_t size, uint64_t more = 0) {
        uintptr_t property = Property(function, kind, text, at, size, kParm | more);
        Put<uint8_t>(function + functionFlags + 4, static_cast<uint8_t>(Get<uint8_t>(function + functionFlags + 4) + 1));
        uint16_t parms = Get<uint16_t>(function + functionFlags + 6);
        uint16_t end = static_cast<uint16_t>((at + size + 3) & ~3);
        Put<uint16_t>(function + functionFlags + 6, parms > end ? parms : end);
        if (more & 0x400)
            Put<uint16_t>(function + functionFlags + 8, static_cast<uint16_t>(at));
        return property;
    }

    // UStruct::PropertiesSize and MinAlignment.
    void Size(uintptr_t type, int32_t size, int32_t alignment) {
        Put<int32_t>(type + propertiesSize, size);
        Put<int32_t>(type + propertiesSize + 4, alignment);
    }

    // UEnum::Names: TPair<FName, int64>, then the <Enum>_MAX the engine adds.
    void Values(uintptr_t enumeration, const std::string& prefix, const std::vector<std::string>& values) {
        size_t count = values.size() + 1;
        auto data = reinterpret_cast<uintptr_t>(calloc(count, 16));
        for (size_t i = 0; i < count; ++i) {
            std::string text = i < values.size() ? values[i] : prefix + "_MAX";
            SetName(data + i * 16, prefix + "::" + text);
            Put<int64_t>(data + i * 16 + 8, static_cast<int64_t>(i));
        }
        Put<uintptr_t>(enumeration + enumNames, data);
        Put<int32_t>(enumeration + enumNames + P, static_cast<int32_t>(count));
        Put<int32_t>(enumeration + enumNames + P + 4, static_cast<int32_t>(count));
    }
};

// The functions of the game, as ProcessEvent runs them.
template <typename T>
T Value(uintptr_t address) {
    T value;
    memcpy(&value, reinterpret_cast<const void*>(address), sizeof(T));
    return value;
}

template <typename T>
void Store(uintptr_t address, T value) {
    memcpy(reinterpret_cast<void*>(address), &value, sizeof(T));
}

uintptr_t Parms(uint8_t* parms) { return reinterpret_cast<uintptr_t>(parms); }
size_t RealSize() { return g_generation == 5 ? 8 : 4; }

// bool Heal(int32 Amount): adds to Health, true while it is above 0.
void Heal(uintptr_t self, uint8_t* parms) {
    int32_t health = Value<int32_t>(self + 0x100) + Value<int32_t>(Parms(parms));
    Store<int32_t>(self + 0x100, health);
    parms[4] = health > 0;
}

// int32 GetStats(int32 Bonus, out int32 Total, out float Ratio): Health, Health + Bonus, Health / 100.
void GetStats(uintptr_t self, uint8_t* parms) {
    int32_t health = Value<int32_t>(self + 0x100);
    Store<int32_t>(Parms(parms) + 4, health + Value<int32_t>(Parms(parms)));
    Store<float>(Parms(parms) + 8, static_cast<float>(health) / 100.0f);
    Store<int32_t>(Parms(parms) + 12, health);
}

// void Boost(ref int32 Value): doubles it.
void Boost(uintptr_t, uint8_t* parms) { Store<int32_t>(Parms(parms), 2 * Value<int32_t>(Parms(parms))); }

// Vector GetLocation(), void SetLocation(Vector NewLocation): Location.
void GetLocation(uintptr_t self, uint8_t* parms) { memcpy(parms, reinterpret_cast<const void*>(self + 0x128), 3 * RealSize()); }
void SetLocation(uintptr_t self, uint8_t* parms) { memcpy(reinterpret_cast<void*>(self + 0x128), parms, 3 * RealSize()); }

// void Rename(FString NewName): PlayerName, a copy, as the engine copies the string.
void Rename(uintptr_t self, uint8_t* parms) {
    uintptr_t data = Value<uintptr_t>(Parms(parms));
    int32_t count = Value<int32_t>(Parms(parms) + P);
    auto* copy = static_cast<uint16_t*>(calloc(static_cast<size_t>(count > 0 ? count : 1), 2));
    if (data && count > 0)
        memcpy(copy, reinterpret_cast<const void*>(data), static_cast<size_t>(count) * 2);
    Store<uintptr_t>(self + 0x110, reinterpret_cast<uintptr_t>(copy));
    Store<int32_t>(self + 0x110 + P, count);
    Store<int32_t>(self + 0x110 + P + 4, count);
}

// float "Get Health Ratio"(), a blueprint function: Health / "Health Max".
void HealthRatio(uintptr_t self, uint8_t* parms) {
    Store<float>(Parms(parms), static_cast<float>(Value<int32_t>(self + 0x100)) / static_cast<float>(Value<int32_t>(self + 0x158)));
}

// static int32 KismetMathLibrary::Add_IntInt(int32 A, int32 B)
void AddIntInt(uintptr_t, uint8_t* parms) { Store<int32_t>(Parms(parms) + 8, Value<int32_t>(Parms(parms)) + Value<int32_t>(Parms(parms) + 4)); }

uintptr_t Utf16(const char* text) {
    size_t length = strlen(text);
    auto* units = static_cast<uint16_t*>(calloc(length + 1, 2));
    for (size_t i = 0; i < length; ++i)
        units[i] = static_cast<uint8_t>(text[i]);
    return reinterpret_cast<uintptr_t>(units);
}

void SetUtf16(volatile char16_t* out, const char* text) {
    size_t i = 0;
    for (; text[i]; ++i)
        out[i] = static_cast<char16_t>(text[i]);
    out[i] = 0;
}

} // namespace

bool StartFakeUnreal(int generation, FakeUnreal& fake) {
    if (generation != 4 && generation != 5)
        return false;
    Builder b(generation);
    fake.generation = generation;
    g_generation = generation;
    fake.processEvent = generation == 5 ? BuildVirtualTable<5>() : BuildVirtualTable<4>();
    fake.mainThread = ThreadId();
    b.items = static_cast<uint8_t*>(calloc(65536, b.itemSize));
    void** chunks = static_cast<void**>(calloc(33, P));
    chunks[0] = b.items;
    GUObjectArray.objects = chunks;
    if (generation == 5) {
        GUObjectArray.five.maxElements = 33 * 65536;
        GUObjectArray.five.numChunks = 1;
        GUObjectArray.five.maxChunks = 33;
    } else {
        GUObjectArray.four.maxElements = 33 * 65536;
        GUObjectArray.four.numChunks = 1;
        GUObjectArray.four.maxChunks = 33;
    }
    // The first names of every game.
    b.Name("None");
    b.Name("ByteProperty");
    b.Name("IntProperty");
    b.Name("BoolProperty");

    using B = Builder;
    uintptr_t core = b.package = b.Object(0, 0, "/Script/CoreUObject", 0x100);
    uintptr_t classClass = b.classClass = b.Type(0, core, "Class", 0);
    uintptr_t object = b.Type(classClass, core, "Object", 0);
    uintptr_t field = b.Type(classClass, core, "Field", object);
    uintptr_t strukt = b.Type(classClass, core, "Struct", field);
    B::Put<uintptr_t>(classClass + b.superStruct, strukt);
    uintptr_t scriptStruct = b.Type(classClass, core, "ScriptStruct", strukt);
    uintptr_t function = b.functionClass = b.Type(classClass, core, "Function", strukt);
    uintptr_t enumClass = b.Type(classClass, core, "Enum", field);
    uintptr_t packageClass = b.Type(classClass, core, "Package", object);
    B::Put<uintptr_t>(core + b.klass, packageClass);
    // The default object of Object, whose virtual table is UObject's.
    b.Object(object, core, "Default__Object", 0x200, 0, 0x10);
    // The default object of Enum, without values, as the engine has it.
    b.Object(enumClass, core, "Default__Enum", 0x200, 0, 0x10);
    // Vector: X, Y, Z, doubles since 5.0.
    uintptr_t vector = b.Type(scriptStruct, core, "Vector", 0);
    const char* real = generation == 5 ? "DoubleProperty" : "FloatProperty";
    int32_t realSize = generation == 5 ? 8 : 4;
    for (int i = 0; i < 3; ++i)
        b.Property(vector, real, std::string(1, static_cast<char>('X' + i)), i * realSize, realSize);
    b.Size(vector, 3 * realSize, realSize);
    uintptr_t transform = b.Type(scriptStruct, core, "Transform", 0);
    b.Size(transform, 12 * realSize, 16);
    uintptr_t translation = b.Property(transform, "StructProperty", "Translation", 4 * realSize, 3 * realSize);
    B::Put<uintptr_t>(translation + b.extra, vector);
    uintptr_t scale = b.Property(transform, "StructProperty", "Scale3D", 8 * realSize, 3 * realSize);
    B::Put<uintptr_t>(scale + b.extra, vector);

    uintptr_t engine = b.Object(packageClass, 0, "/Script/Engine", 0x100);
    uintptr_t actor = b.Type(classClass, engine, "Actor", object);
    uintptr_t hidden = b.Property(actor, "BoolProperty", "bHidden", 0x58, 1);
    uint8_t bit[4] = {1, 0, 0x02, 0x02};
    memcpy(reinterpret_cast<void*>(hidden + b.extra), bit, 4);
    uintptr_t tags = b.Property(actor, "ArrayProperty", "Tags", 0x60, 2 * 4 + static_cast<int32_t>(P));
    uintptr_t tag = b.Property(0, "NameProperty", "Tags", 0, 8);
    // 5.x puts ArrayFlags before Inner.
    B::Put<uintptr_t>(tags + b.extra + (generation == 5 ? P : 0), tag);
    b.Function(actor, "K2_DestroyActor");
    uintptr_t pawn = b.Type(classClass, engine, "Pawn", actor);
    b.Property(pawn, "FloatProperty", "BaseEyeHeight", 0x80, 4);
    // Static functions, called on the default object of their class.
    uintptr_t math = b.Type(classClass, engine, "KismetMathLibrary", object);
    uintptr_t add = b.Function(math, "Add_IntInt", kNative | kStatic | kCallable, AddIntInt);
    b.Param(add, "IntProperty", "A", 0, 4);
    b.Param(add, "IntProperty", "B", 4, 4);
    b.Param(add, "IntProperty", "ReturnValue", 8, 4, kReturn);
    b.Object(math, engine, "Default__KismetMathLibrary", 0x200, 0, 0x10);

    uintptr_t game = b.Object(packageClass, 0, "/Script/Game", 0x100);
    uintptr_t team = b.Type(enumClass, game, "ETeam", 0);
    b.Values(team, "ETeam", {"Red", "Blue", "Green"});
    uintptr_t player = b.Type(classClass, game, "GamePlayer", pawn);
    b.Property(player, "IntProperty", "Health", 0x100, 4);
    b.Property(player, "FloatProperty", "Speed", 0x104, 4);
    uintptr_t alive = b.Property(player, "BoolProperty", "bAlive", 0x108, 1);
    uint8_t native[4] = {1, 0, 0xff, 0xff};
    memcpy(reinterpret_cast<void*>(alive + b.extra), native, 4);
    uintptr_t teamProperty = b.Property(player, "EnumProperty", "Team", 0x109, 1);
    B::Put<uintptr_t>(teamProperty + b.extra, b.Property(0, "ByteProperty", "UnderlyingType", 0, 1));
    B::Put<uintptr_t>(teamProperty + b.extra + P, team);
    b.Property(player, "StrProperty", "PlayerName", 0x110, static_cast<int32_t>(P) + 8);
    uintptr_t target = b.Property(player, "ObjectProperty", "Target", 0x120, static_cast<int32_t>(P));
    B::Put<uintptr_t>(target + b.extra, player);
    uintptr_t location = b.Property(player, "StructProperty", "Location", 0x128, 3 * realSize);
    B::Put<uintptr_t>(location + b.extra, vector);
    uintptr_t inventory = b.Property(player, "ArrayProperty", "Inventory", 0x148, static_cast<int32_t>(P) + 8);
    B::Put<uintptr_t>(inventory + b.extra + (generation == 5 ? P : 0), b.Property(0, "IntProperty", "Inventory", 0, 4));
    b.Property(player, "IntProperty", "Health Max", 0x158, 4);
    uintptr_t heal = b.Function(player, "Heal", kNative | kCallable, Heal);
    b.Param(heal, "IntProperty", "Amount", 0, 4);
    uintptr_t healed = b.Param(heal, "BoolProperty", "ReturnValue", 4, 1, kReturn);
    memcpy(reinterpret_cast<void*>(healed + b.extra), native, 4);
    uintptr_t stats = b.Function(player, "GetStats", kNative | kCallable, GetStats);
    b.Param(stats, "IntProperty", "Bonus", 0, 4);
    b.Param(stats, "IntProperty", "Total", 4, 4, kOut);
    b.Param(stats, "FloatProperty", "Ratio", 8, 4, kOut);
    b.Param(stats, "IntProperty", "ReturnValue", 12, 4, kReturn);
    uintptr_t boost = b.Function(player, "Boost", kNative | kCallable, Boost);
    b.Param(boost, "IntProperty", "Value", 0, 4, kOut | kReference);
    uintptr_t getLocation = b.Function(player, "GetLocation", kNative | kCallable, GetLocation);
    B::Put<uintptr_t>(b.Param(getLocation, "StructProperty", "ReturnValue", 0, 3 * realSize, kReturn) + b.extra, vector);
    uintptr_t setLocation = b.Function(player, "SetLocation", kNative | kCallable, SetLocation);
    B::Put<uintptr_t>(b.Param(setLocation, "StructProperty", "NewLocation", 0, 3 * realSize) + b.extra, vector);
    uintptr_t rename = b.Function(player, "Rename", kNative | kCallable, Rename);
    b.Param(rename, "StrProperty", "NewName", 0, static_cast<int32_t>(P) + 8);

    // A blueprint class, in its package of /Game/, with a function whose name has spaces and a local variable.
    uintptr_t blueprints = b.Object(packageClass, 0, "/Game/Blueprints/BP_Hero", 0x100);
    uintptr_t hero = b.Type(classClass, blueprints, "BP_Hero_C", player);
    b.Property(hero, "FloatProperty", "Mana", 0x15c, 4);
    uintptr_t ratio = b.Function(hero, "Get Health Ratio", kCallable, HealthRatio);
    b.Param(ratio, "FloatProperty", "ReturnValue", 0, 4, kReturn);
    b.Property(ratio, "IntProperty", "Temp", 4, 4, 0);

    // The objects of the level.
    auto makePlayer = [&](const char* text, uint32_t number, int32_t health, int32_t flags, uintptr_t klass = 0) {
        uintptr_t instance = klass ? b.Object(klass, blueprints, "BP_Hero_C", 0x200, number, flags) : b.Object(player, game, "GamePlayer", 0x200, number, flags);
        B::Put<int32_t>(instance + 0x158, 200);
        B::Put<int32_t>(instance + 0x100, health);
        B::Put<float>(instance + 0x104, 4.5f);
        B::Put<uint8_t>(instance + 0x108, 1);
        B::Put<uint8_t>(instance + 0x109, 2);
        B::Put<uintptr_t>(instance + 0x110, Utf16(text));
        B::Put<int32_t>(instance + 0x110 + P, static_cast<int32_t>(strlen(text) + 1));
        B::Put<int32_t>(instance + 0x110 + P + 4, static_cast<int32_t>(strlen(text) + 1));
        for (int i = 0; i < 3; ++i) {
            if (generation == 5)
                B::Put<double>(instance + 0x128 + i * 8, 1.0 + i);
            else
                B::Put<float>(instance + 0x128 + i * 4, 1.0f + i);
        }
        auto* items = static_cast<int32_t*>(calloc(4, 4));
        items[0] = 3, items[1] = 1, items[2] = 4;
        B::Put<uintptr_t>(instance + 0x148, reinterpret_cast<uintptr_t>(items));
        B::Put<int32_t>(instance + 0x148 + P, 3);
        B::Put<int32_t>(instance + 0x148 + P + 4, 4);
        B::Put<float>(instance + 0x80, 64.0f);
        // bHidden, with the bit before it set too.
        B::Put<uint8_t>(instance + 0x58, 0x03);
        auto* names = static_cast<uint32_t*>(calloc(4, 4));
        names[0] = b.Name("Hero"), names[2] = b.Name("Player");
        B::Put<uintptr_t>(instance + 0x60, reinterpret_cast<uintptr_t>(names));
        B::Put<int32_t>(instance + 0x60 + P, 2);
        B::Put<int32_t>(instance + 0x60 + P + 4, 2);
        return instance;
    };
    fake.defaults = makePlayer("Default", 0, 0, 0x10);
    // B::Put on the default object's name: Default__GamePlayer.
    b.SetName(fake.defaults + b.name, "Default__GamePlayer");
    fake.pawn = b.Object(pawn, game, "Pawn", 0x200, 1);
    fake.alice = makePlayer("Alice", 1, 100, 0);
    fake.bob = makePlayer("Bob", 2, 80, 0);
    fake.carol = makePlayer("Carol", 3, 60, 0);
    B::Put<uintptr_t>(fake.alice + 0x120, fake.bob);
    fake.dave = makePlayer("Dave", 1, 90, 0, hero);
    B::Put<float>(fake.dave + 0x15c, 50.0f);

    SetUtf16(g_scriptPackage, "/Script/CoreUObject");
    SetUtf16(g_branch, generation == 5 ? "++UE5+Release-5.8" : "++UE4+Release-4.22");
    return g_keep[0] != nullptr;
}

uint64_t FakeUnrealCallThread() { return g_callThread; }

int FakeUnrealCalls() { return g_nativeCalls + g_scriptCalls; }
