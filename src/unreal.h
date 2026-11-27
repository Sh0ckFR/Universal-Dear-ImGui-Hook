// The reflection of Unreal Engine games, shared by the engine commands of
// src/unreal.cpp and the C++ SDK that src/sdk.cpp writes from it. Everything
// is read from the game's memory with the layouts that src/unreal.cpp finds
// at run time: see the comment at its top.
#pragma once

#include "internal.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace uih::detail::remote::unreal {

constexpr size_t P = sizeof(void*);

// EObjectFlags
constexpr int32_t kClassDefaultObject = 0x10;
constexpr int32_t kArchetypeObject = 0x20;
// EPropertyFlags
constexpr uint64_t kParm = 0x80;
constexpr uint64_t kOutParm = 0x100;
constexpr uint64_t kReturnParm = 0x400;
constexpr uint64_t kConstParm = 0x2;
constexpr uint64_t kReferenceParm = 0x8000000;
// EFunctionFlags
constexpr uint32_t kFuncNative = 0x400;
constexpr uint32_t kFuncStatic = 0x2000;

// A global of the engine, for the SDK: the module it is in, and its offset there.
struct Global {
    std::string module;
    bool main = false;
    uintptr_t offset = 0;
};

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
    // Where `names` was found: the address of the pointer to it when
    // `namesIndirect`, the table itself otherwise.
    uintptr_t namesAt = 0;
    bool namesIndirect = false;
    // Objects: the chunk table of FChunkedFixedUObjectArray, or the items of
    // FFixedUObjectArray, and where its number of elements is, if known.
    uintptr_t objects = 0;
    // The pointer to `objects`, in the data of a core module.
    uintptr_t objectsAt = 0;
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
    uintptr_t objectClass = 0;
    uintptr_t vectorStruct = 0;
    uintptr_t transformStruct = 0;
    // /Script/CoreUObject
    uintptr_t package = 0;

    // Found when first needed, for the calls, the enums and the SDK.
    bool reflected = false;
    std::string reflectionFailure;
    // FProperty::PropertyFlags
    size_t propertyFlagsOffset = 0;
    // UStruct::PropertiesSize, then MinAlignment.
    size_t propertiesSizeOffset = 0;
    // UFunction: FunctionFlags, NumParms, ParmsSize, ReturnValueOffset.
    size_t functionFlagsOffset = 0;
    size_t numParmsOffset = 0;
    size_t parmsSizeOffset = 0;
    size_t returnValueOffset = 0;
    // UEnum::Names, a TArray of TPair<FName, int64>.
    size_t enumNamesOffset = 0;
    size_t enumEntrySize = 0;
    // UObject::ProcessEvent: its index in the virtual table, and the
    // function the default object of Object has there.
    bool processEventSearched = false;
    int processEventIndex = -1;
    uintptr_t processEventAddress = 0;
    // The entries of the virtual table whose code tests FUNC_Native, in order:
    // the check of the first call keeps the first that returns its sum.
    std::vector<std::pair<int, uintptr_t>> processEventCandidates;
    std::string processEventFailure;
    // 1 once a call of KismetMathLibrary:Add_IntInt returned its sum, -1 when it did not.
    int processEventVerified = 0;
    uintptr_t defaultObject = 0;

    std::unordered_map<uint32_t, std::string> nameCache;
    std::unordered_map<std::string, uint32_t> nameIndex;
    bool nameIndexComplete = false;
    std::unordered_map<uintptr_t, uintptr_t> defaultObjects;
};

// The game, once it was recognized and its objects found; null when it is
// not an Unreal Engine game. `error` is set when it is one whose objects the
// library did not find.
Unreal* FindUnreal(std::string& error);
// The functions, enum values and the virtual table: empty when found, the reason otherwise.
std::string DiscoverReflection(Unreal& ue);
std::string DiscoverProcessEvent(Unreal& ue);

// Memory
uintptr_t Ptr(uintptr_t address);
int32_t Int(uintptr_t address, int32_t fallback = -1);
// The module that holds `address`, for the SDK.
bool GlobalOf(const Unreal& ue, uintptr_t address, Global& global);

// Names and objects
std::string NameAt(Unreal& ue, uintptr_t address);
std::string ObjectName(Unreal& ue, uintptr_t object);
uintptr_t ClassOf(const Unreal& ue, uintptr_t object);
uintptr_t OuterOf(const Unreal& ue, uintptr_t object);
uintptr_t SuperOf(const Unreal& ue, uintptr_t type);
bool IsObject(const Unreal& ue, uintptr_t address);
bool IsA(const Unreal& ue, uintptr_t type, uintptr_t ancestor);
std::string PackageOf(Unreal& ue, uintptr_t object);
int32_t ObjectCount(const Unreal& ue);
uintptr_t ObjectAt(const Unreal& ue, int32_t index);
// The default object of a class, null when it has none.
uintptr_t DefaultObject(Unreal& ue, uintptr_t klass);

// Types
struct Property {
    uintptr_t address = 0;
    std::string name;
    // Its class, such as IntProperty.
    std::string kind;
    int32_t offset = 0;
    int32_t size = 0;
    int32_t arrayDim = 1;
    uint64_t flags = 0;
    uintptr_t owner = 0;
};

struct TypeEntry {
    uintptr_t object = 0;
    std::string name;
    std::string package;
    // class, struct or enum
    const char* kind = "";
};

struct EnumValue {
    std::string name;
    int64_t value = 0;
};

Property Describe(Unreal& ue, uintptr_t address, uintptr_t owner);
// The properties a struct, class or function declares itself, in order.
std::vector<Property> OwnProperties(Unreal& ue, uintptr_t type);
// The parameters of a function, its return value included: the properties with CPF_Parm.
std::vector<Property> Parameters(Unreal& ue, uintptr_t function);
std::vector<uintptr_t> Functions(Unreal& ue, uintptr_t type);
// FObjectPropertyBase::PropertyClass, FStructProperty::Struct, ... at `index`.
uintptr_t Extra(const Unreal& ue, const Property& property, size_t index = 0);
// The element of an array or set.
uintptr_t Inner(Unreal& ue, const Property& property, size_t index = 0);
// The enum of an EnumProperty or ByteProperty, null when it has none.
uintptr_t EnumOf(Unreal& ue, const Property& property);
std::string TypeName(Unreal& ue, const Property& property, int depth = 0);
// The types of the game, sorted by package and name; kept for a few seconds.
const std::vector<TypeEntry>& Types(Unreal& ue, bool refresh = false);
bool FindType(Unreal& ue, const std::string& text, TypeEntry& found, std::string& error);
bool FindProperty(Unreal& ue, uintptr_t type, const std::string& name, Property& found);
uintptr_t FindFunction(Unreal& ue, uintptr_t type, const std::string& name);
std::vector<EnumValue> EnumValues(Unreal& ue, uintptr_t enumeration);
uint32_t FunctionFlags(const Unreal& ue, uintptr_t function);
int32_t ParmsSize(const Unreal& ue, uintptr_t function);
// UStruct::PropertiesSize and MinAlignment.
int32_t StructSize(const Unreal& ue, uintptr_t type);
int32_t StructAlignment(const Unreal& ue, uintptr_t type);
// The byte offset in the value and the mask of a BoolProperty.
void BoolLayout(const Unreal& ue, const Property& property, uint8_t& byteOffset, uint8_t& mask);

// Text
std::string Lower(std::string text);
// A name of the protocol: as it is, or quoted when it has spaces or quotes.
std::string QuoteName(const std::string& name);

// The SDK, src/sdk.cpp: writes the header of `scope` and answers with its path.
std::string DumpSdk(Unreal& ue, const std::string& scope, std::vector<uint8_t>& payload);

} // namespace uih::detail::remote::unreal
