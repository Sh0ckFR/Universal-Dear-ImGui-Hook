// The objects of an Unreal Engine game in the memory of the test host: see
// tests/fake_unreal.h. The layouts follow Epic's engine headers for 5.8
// (generation 5) and 4.22 (generation 4); src/unreal.cpp must find every
// offset by itself.
#include "fake_unreal.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr size_t P = sizeof(void*);

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
    uintptr_t Property(uintptr_t owner, const std::string& kind, const std::string& text, int32_t at, int32_t size) {
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
        Put<uint64_t>(property + arrayDim + 8, 0x0010000000000005ull);
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

    uintptr_t Function(uintptr_t owner, const std::string& text) {
        uintptr_t function = Type(functionClass, owner, text, 0);
        Append(owner, function, false);
        return function;
    }
};

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
    // Vector: X, Y, Z, doubles since 5.0.
    uintptr_t vector = b.Type(scriptStruct, core, "Vector", 0);
    const char* real = generation == 5 ? "DoubleProperty" : "FloatProperty";
    int32_t realSize = generation == 5 ? 8 : 4;
    for (int i = 0; i < 3; ++i)
        b.Property(vector, real, std::string(1, static_cast<char>('X' + i)), i * realSize, realSize);
    uintptr_t transform = b.Type(scriptStruct, core, "Transform", 0);
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

    uintptr_t game = b.Object(packageClass, 0, "/Script/Game", 0x100);
    uintptr_t team = b.Type(enumClass, game, "ETeam", 0);
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
    uintptr_t heal = b.Function(player, "Heal");
    b.Property(heal, "IntProperty", "Amount", 0, 4);
    b.Property(heal, "BoolProperty", "ReturnValue", 4, 1);

    // The objects of the level.
    auto makePlayer = [&](const char* text, uint32_t number, int32_t health, int32_t flags) {
        uintptr_t instance = b.Object(player, game, "GamePlayer", 0x200, number, flags);
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

    SetUtf16(g_scriptPackage, "/Script/CoreUObject");
    SetUtf16(g_branch, generation == 5 ? "++UE5+Release-5.8" : "++UE4+Release-4.22");
    return g_keep[0] != nullptr;
}
