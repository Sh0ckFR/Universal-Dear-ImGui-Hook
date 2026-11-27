// The C++ SDK of an Unreal Engine game, `engine dump [scope]`: one header,
// written to the temporary directory, that another DLL includes to use the
// game's classes and call its functions, as a trainer does. It needs neither
// this library nor Unreal's headers:
//   - a small runtime (kRuntime below, copied as it is) finds the name table,
//     the object array and UObject::ProcessEvent from the offsets recorded at
//     dump time, checks them, and fails with a message when the game changed;
//   - each class is a C++ class without data members: its properties are
//     accessors that read at the offsets of the game, found by name at run
//     time, and its functions fill the parameters and call ProcessEvent;
//   - structs used by value have their size and accessors at fixed offsets,
//     enums are enum class with their values.
// The default scope is the game: /Script/<game> and the /Game/ packages of
// its blueprints, with the parents of their classes and the structs and
// enums they use by value, which keeps the header small enough to compile
// quickly. Other classes they refer to are declared without members.
#include "unreal.h"

#include <algorithm>
#include <cctype>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

#ifndef _WIN32
#include <cstdlib>
#endif

namespace uih::detail::remote::unreal {
namespace {

// The runtime of the SDK, after its recorded layout: valid C++17 for MSVC,
// GCC and Clang, on Windows and Linux. Split in pieces, which MSVC needs for
// long string literals.
const char* const kRuntime[] = {
    R"SDK(
// ===========================================================================
// Runtime of the SDK
// ===========================================================================

namespace uesdk {

class Object;

// An index in the engine's name table and a number: Name_3 is Name with 4.
struct FName {
    int32_t ComparisonIndex = 0;
    int32_t Number = 0;
    std::string ToString() const;
    bool IsNone() const { return ComparisonIndex == 0 && Number == 0; }
    // The FName of a name the game already uses; None when it has none.
    static FName Find(const std::string& text);
    bool operator==(const FName& other) const { return ComparisonIndex == other.ComparisonIndex && Number == other.Number; }
    bool operator!=(const FName& other) const { return !(*this == other); }
};

// The elements of an array of the game. The engine's allocator owns them:
// read and write the elements, never resize the array.
template <typename T>
struct TArray {
    T* Data = nullptr;
    int32_t Num = 0;
    int32_t Max = 0;
    int32_t Count() const { return Num; }
    bool IsValidIndex(int32_t index) const { return Data && index >= 0 && index < Num; }
    T& operator[](int32_t index) { return Data[index]; }
    const T& operator[](int32_t index) const { return Data[index]; }
    T* begin() { return Data; }
    T* end() { return Data ? Data + Num : nullptr; }
    const T* begin() const { return Data; }
    const T* end() const { return Data ? Data + Num : nullptr; }
};

// UTF-16 text, its null included. One made here points to text that you
// keep alive while the game uses it, such as a literal: FString(u"Hello").
struct FString : TArray<char16_t> {
    FString() = default;
    FString(const char16_t* text) {
        if (!text)
            return;
        int32_t length = 0;
        while (text[length])
            ++length;
        Data = const_cast<char16_t*>(text);
        Num = Max = length + 1;
    }
    std::u16string ToU16() const {
        if (!Data || Num <= 0)
            return std::u16string();
        std::u16string text(Data, static_cast<size_t>(Num));
        while (!text.empty() && text.back() == 0)
            text.pop_back();
        return text;
    }
    // UTF-8
    std::string ToString() const;
};

// A value whose type the SDK does not describe, such as FText or a delegate: its bytes.
template <size_t N>
struct alignas(N % 8 == 0 ? 8 : N % 4 == 0 ? 4 : N % 2 == 0 ? 2 : 1) Opaque {
    uint8_t Bytes[N];
};

// A class derived from T: UClass*.
template <typename T>
struct TSubclassOf {
    Object* Class = nullptr;
    Object* Get() const { return Class; }
};

// A weak reference: the index of the object in the object array.
template <typename T>
struct TWeakObjectPtr {
    int32_t ObjectIndex = -1;
    int32_t ObjectSerialNumber = 0;
    T* Get() const;
};

// A bool that is one bit of a byte, as most bool properties of classes are.
class BitRef {
public:
    BitRef(uint8_t* byte, uint8_t mask) : byte_(byte), mask_(mask) {}
    operator bool() const { return (*byte_ & mask_) != 0; }
    BitRef& operator=(bool value) {
        *byte_ = static_cast<uint8_t>(value ? *byte_ | mask_ : *byte_ & ~mask_);
        return *this;
    }

private:
    uint8_t* byte_;
    uint8_t mask_;
};

// Finds the engine in the game from the recorded offsets and checks it:
// call it once the game has started, or WaitForInit() from a DLL injected at
// launch. False, with LastError(), until the engine is up, or when the game
// is not the build the SDK was dumped from.
inline bool Init();
// Init() again every 100 ms until it succeeds, the game is found to be
// another build, or `milliseconds` passed.
inline bool WaitForInit(unsigned milliseconds);
inline bool Ready();
// Why the last Init(), lookup or call failed.
inline std::string LastError();

// A UObject. The SDK never creates one: pointers to it come from the game.
class Object {
public:
    Object() = delete;
    Object(const Object&) = delete;
    Object& operator=(const Object&) = delete;

    int32_t GetIndex() const;
    uint32_t GetFlags() const;
    Object* GetClass() const;
    Object* GetOuter() const;
    FName GetFName() const;
    std::string GetName() const;
    // Class /Package/Path.Object
    std::string GetFullName() const;
    // UStruct::SuperStruct: the parent of a class or struct.
    Object* GetSuper() const;
    bool IsA(const Object* klass) const;
    template <typename T>
    bool IsA() const {
        return IsA(T::StaticClass());
    }
    template <typename T>
    T* Cast() {
        return IsA<T>() ? static_cast<T*>(this) : nullptr;
    }
    // A class default object or an archetype, not an object of the level.
    bool IsDefaultObject() const;
    // Calls `function`, a UFunction of the object's class, with its parameters,
    // through UObject::ProcessEvent: on the game thread only, see GameThread.
    // False, with LastError(), when it could not, or when the call faulted
    // (caught with MSVC, which builds the game's code with the same handlers).
    bool ProcessEvent(Object* function, void* parms);
};

inline int32_t ObjectCount();
inline Object* ObjectAt(int32_t index);
// A class, struct or enum by its name, such as Actor, optionally in a package such as /Script/Engine.
inline Object* FindType(const std::string& name, const std::string& package = "");
// An object by its name, optionally of a class.
inline Object* FindObject(const std::string& name, const Object* klass = nullptr);
template <typename T>
T* FindObject(const std::string& name) {
    return static_cast<T*>(FindObject(name, T::StaticClass()));
}
// The objects of a class and of its subclasses, without the default objects.
inline std::vector<Object*> Instances(const Object* klass, bool defaults = false);
template <typename T>
std::vector<T*> Instances() {
    std::vector<T*> objects;
    for (Object* object : Instances(T::StaticClass()))
        objects.push_back(static_cast<T*>(object));
    return objects;
}
template <typename T>
T* FirstInstance() {
    std::vector<T*> objects = Instances<T>();
    return objects.empty() ? nullptr : objects.front();
}
// The default object of a class, the one its static functions are called on.
inline Object* DefaultObject(const Object* klass);

// The game thread, where the functions of the objects must be called.
// Properties can be read and written from any thread, as memory.
namespace GameThread {
// Runs `work` on the thread of the game's window, Unreal's game thread, in
// its next message, and returns at once; false when the game has no window
// yet. Safe from the render thread of an overlay menu. Windows only.
inline bool Post(std::function<void()> work);
// Posts `work` and waits until it ran, at most `milliseconds`; runs it at
// once on the game thread. Never from the render thread: the game thread may
// be waiting for it, and both would wait.
inline bool Run(std::function<void()> work, unsigned milliseconds = 5000);
// True on the game thread.
inline bool IsCurrent();
} // namespace GameThread
// Removes what GameThread installed: call it before your DLL unloads.
inline void Shutdown();

namespace detail {

constexpr size_t P = sizeof(void*);

template <typename T>
T Read(uintptr_t address) {
    T value;
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(T));
    return value;
}

// A read that fails instead of faulting, for the checks of Init().
inline bool SafeRead(uintptr_t address, void* out, size_t size) {
    if (!address)
        return false;
#ifdef _WIN32
    SIZE_T done = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(address), out, size, &done) && done == size;
#else
    static int memory = open("/proc/self/mem", O_RDONLY | O_CLOEXEC);
    return memory >= 0 && pread(memory, out, size, static_cast<off_t>(address)) == static_cast<ssize_t>(size);
#endif
}

template <typename T>
bool SafeRead(uintptr_t address, T& value) {
    return SafeRead(address, &value, sizeof(T));
}

inline std::string Utf8(const char16_t* units, size_t count) {
    std::string out;
    for (size_t i = 0; i < count; ++i) {
        uint32_t point = units[i];
        if (point >= 0xd800 && point < 0xdc00 && i + 1 < count && units[i + 1] >= 0xdc00 && units[i + 1] < 0xe000)
            point = 0x10000 + ((point - 0xd800) << 10) + (units[++i] - 0xdc00);
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
    return out;
}

inline std::string Lower(std::string text) {
    for (char& c : text)
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return text;
}

struct State {
    std::mutex mutex;
    bool ready = false;
    // The game is another build: Init() stops trying.
    bool outdated = false;
    std::string error;
    uintptr_t names = 0;
    uintptr_t objects = 0;
    uintptr_t numElements = 0;
    Object* package = nullptr;
    Object* classClass = nullptr;
    Object* structClass = nullptr;
    Object* enumClass = nullptr;
    Object* functionClass = nullptr;
    std::mutex nameMutex;
    std::unordered_map<int32_t, std::string> nameCache;
    std::unordered_map<std::string, FName> nameIndex;
    std::mutex typeMutex;
    std::unordered_multimap<std::string, Object*> types;
    std::chrono::steady_clock::time_point typesAt;
};

inline State& GetState() {
    static State state;
    return state;
}

inline void SetError(const std::string& error) {
    State& state = GetState();
    std::lock_guard<std::mutex> lock(state.nameMutex);
    state.error = error;
}

inline bool Ready() { return GetState().ready; }
)SDK",
    R"SDK(
// Modules -------------------------------------------------------------------

#ifdef _WIN32
inline uintptr_t ModuleBase(const char* name, bool main) {
    if (main)
        return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    return reinterpret_cast<uintptr_t>(GetModuleHandleA(name));
}

// The time stamp the linker wrote in the module, which a new build changes.
inline uint32_t ModuleStamp(uintptr_t base) {
    int32_t header = 0;
    uint32_t stamp = 0;
    return SafeRead(base + 0x3c, header) && SafeRead(base + static_cast<uintptr_t>(header) + 8, stamp) ? stamp : 0;
}
#else
struct ModuleQuery {
    const char* name;
    bool main;
    uintptr_t base;
    bool first;
};

inline int FindModuleBase(dl_phdr_info* info, size_t, void* context) {
    auto* query = static_cast<ModuleQuery*>(context);
    bool executable = query->first && (!info->dlpi_name || !info->dlpi_name[0]);
    query->first = false;
    const char* path = info->dlpi_name ? info->dlpi_name : "";
    const char* slash = strrchr(path, '/');
    const char* file = slash ? slash + 1 : path;
    if (query->main ? !executable : strcmp(file, query->name) != 0)
        return 0;
    uintptr_t low = UINTPTR_MAX;
    for (int i = 0; i < info->dlpi_phnum; ++i)
        if (info->dlpi_phdr[i].p_type == PT_LOAD)
            low = std::min<uintptr_t>(low, info->dlpi_addr + info->dlpi_phdr[i].p_vaddr);
    query->base = low == UINTPTR_MAX ? 0 : low;
    return 1;
}

inline uintptr_t ModuleBase(const char* name, bool main) {
    ModuleQuery query{name, main, 0, true};
    dl_iterate_phdr(FindModuleBase, &query);
    return query.base;
}

inline uint32_t ModuleStamp(uintptr_t) { return 0; }
#endif

// Names ---------------------------------------------------------------------

inline std::string NameText(int32_t index) {
    State& state = GetState();
    {
        std::lock_guard<std::mutex> lock(state.nameMutex);
        auto known = state.nameCache.find(index);
        if (known != state.nameCache.end())
            return known->second;
    }
    std::string text;
    if (layout::kNamePool) {
        uintptr_t block = Read<uintptr_t>(state.names + (static_cast<uint32_t>(index) >> 16) * P);
        if (block) {
            uintptr_t entry = block + (static_cast<uint32_t>(index) & 0xffff) * 2;
            uint16_t header = Read<uint16_t>(entry);
            size_t length = header >> 6;
            if (header & 1) {
                std::vector<char16_t> units(length);
                std::memcpy(units.data(), reinterpret_cast<const void*>(entry + 2), length * 2);
                text = Utf8(units.data(), length);
            } else {
                text.assign(reinterpret_cast<const char*>(entry + 2), length);
            }
        }
    } else {
        uintptr_t chunk = Read<uintptr_t>(state.names + (static_cast<uint32_t>(index) / 16384) * P);
        uintptr_t entry = chunk ? Read<uintptr_t>(chunk + (static_cast<uint32_t>(index) % 16384) * P) : 0;
        if (entry) {
            if (Read<int32_t>(entry + layout::kLegacyIndex) & 1) {
                auto units = reinterpret_cast<const char16_t*>(entry + layout::kLegacyText);
                size_t length = 0;
                while (length < 1024 && units[length])
                    ++length;
                text = Utf8(units, length);
            } else {
                auto chars = reinterpret_cast<const char*>(entry + layout::kLegacyText);
                text.assign(chars, strnlen(chars, 1024));
            }
        }
    }
    std::lock_guard<std::mutex> lock(state.nameMutex);
    if (state.nameCache.size() < 1000000)
        state.nameCache[index] = text;
    return text;
}

// Objects -------------------------------------------------------------------

inline uintptr_t Address(const void* object) { return reinterpret_cast<uintptr_t>(object); }

inline int32_t Count() {
    State& state = GetState();
    if (state.numElements)
        return Read<int32_t>(state.numElements);
    // Without the counter: every item of the allocated chunks.
    uintptr_t chunks = Read<uintptr_t>(state.objects);
    if (layout::kFlat)
        return 0;
    int32_t count = 0;
    while (count < 0x4000000 && Read<uintptr_t>(chunks + static_cast<uintptr_t>(count / 65536) * P))
        count += 65536;
    return count;
}

inline Object* At(int32_t index) {
    State& state = GetState();
    if (index < 0 || index >= Count())
        return nullptr;
    uintptr_t table = Read<uintptr_t>(state.objects);
    uintptr_t items = layout::kFlat ? table : Read<uintptr_t>(table + static_cast<uintptr_t>(index / 65536) * P);
    size_t at = layout::kFlat ? static_cast<size_t>(index) : static_cast<size_t>(index % 65536);
    return items ? Read<Object*>(items + at * layout::kItemSize + layout::kItemObject) : nullptr;
}

inline Object* Super(const Object* type) { return Read<Object*>(Address(type) + layout::kSuper); }
inline Object* ClassOf(const Object* object) { return Read<Object*>(Address(object) + layout::kClass); }
inline Object* OuterOf(const Object* object) { return Read<Object*>(Address(object) + layout::kOuter); }
inline std::string NameOf(const Object* object) { return object->GetName(); }

inline bool Derives(const Object* type, const Object* ancestor) {
    for (int depth = 0; type && depth < 64; ++depth, type = Super(type))
        if (type == ancestor)
            return true;
    return false;
}

inline std::string PackageOf(const Object* object) {
    const Object* outer = object;
    for (int depth = 0; depth < 32 && OuterOf(outer); ++depth)
        outer = OuterOf(outer);
    return outer->GetName();
}

// The classes, structs and enums by name.
inline void IndexTypes() {
    State& state = GetState();
    state.types.clear();
    for (int32_t i = 0, count = Count(); i < count; ++i) {
        Object* object = At(i);
        if (!object)
            continue;
        Object* klass = ClassOf(object);
        if ((Derives(klass, state.structClass) && !Derives(klass, state.functionClass)) || Derives(klass, state.enumClass))
            state.types.emplace(object->GetName(), object);
    }
    state.typesAt = std::chrono::steady_clock::now();
}

// The properties of a struct, class or function: FField since 4.25, UProperty before.
template <typename Visit>
void ForEachField(const Object* type, Visit&& visit) {
    if (layout::kChildProperties) {
        for (uintptr_t field = Read<uintptr_t>(Address(type) + layout::kChildProperties); field; field = Read<uintptr_t>(field + layout::kFFieldNext)) {
            FName name = Read<FName>(field + layout::kFFieldName);
            std::string text = NameText(name.ComparisonIndex);
            if (name.Number)
                text += "_" + std::to_string(name.Number - 1);
            if (!visit(field, text))
                return;
        }
        return;
    }
    for (uintptr_t field = Read<uintptr_t>(Address(type) + layout::kChildren); field; field = Read<uintptr_t>(field + layout::kFieldNext))
        if (!visit(field, reinterpret_cast<const Object*>(field)->GetName()))
            return;
}

// A property of the type or of its parents.
inline uintptr_t FindProperty(const Object* type, const char* name) {
    for (; type; type = Super(type)) {
        uintptr_t found = 0;
        ForEachField(type, [&](uintptr_t field, const std::string& text) {
            if (text == name)
                found = field;
            return !found;
        });
        if (found)
            return found;
    }
    return 0;
}

inline Object* FindFunction(const Object* type, const char* name) {
    for (; type; type = Super(type))
        for (uintptr_t field = Read<uintptr_t>(Address(type) + layout::kChildren); field; field = Read<uintptr_t>(field + layout::kFieldNext))
            if (reinterpret_cast<const Object*>(field)->GetName() == name)
                return reinterpret_cast<Object*>(field);
    return nullptr;
}

// The offset of a property in a class, found by its name the first time: a
// blueprint can move it when the game is patched. The offset recorded in the
// SDK serves until Init() succeeds.
class MemberRef {
public:
    constexpr MemberRef(const char* name, int32_t offset) : name_(name), recorded_(offset) {}
    int32_t Get(const Object* klass) {
        int32_t offset = resolved_.load(std::memory_order_acquire);
        if (offset != kUnresolved)
            return offset;
        if (!klass || !Ready())
            return recorded_;
        uintptr_t property = FindProperty(klass, name_);
        if (property) {
            offset = Read<int32_t>(property + layout::kOffset);
        } else {
            SetError(std::string("the property ") + name_ + " of " + klass->GetName() + " is gone: the game changed, dump the SDK again");
            offset = recorded_;
        }
        resolved_.store(offset, std::memory_order_release);
        return offset;
    }

private:
    static constexpr int32_t kUnresolved = INT32_MIN;
    const char* name_;
    int32_t recorded_;
    std::atomic<int32_t> resolved_{kUnresolved};
};

template <typename T>
T& At(void* self, int32_t offset) {
    return *reinterpret_cast<T*>(static_cast<uint8_t*>(self) + offset);
}

// A class, struct or enum, found by its package and name the first time.
class TypeRef {
public:
    constexpr TypeRef(const char* package, const char* name) : package_(package), name_(name) {}
    Object* Get() {
        Object* type = type_.load(std::memory_order_acquire);
        if (type || !Ready())
            return type;
        type = FindType(name_, package_);
        if (type)
            type_.store(type, std::memory_order_release);
        return type;
    }

private:
    const char* package_;
    const char* name_;
    std::atomic<Object*> type_{nullptr};
};
)SDK",
    R"SDK(
// Functions -----------------------------------------------------------------

// A parameter of a function, as the SDK was dumped: its name and size.
struct Param {
    const char* name;
    int32_t offset;
    int32_t size;
};

// A function of a class and the offsets of its parameters, found by their
// names the first time, and checked against the sizes of the SDK.
class FunctionRef {
public:
    constexpr FunctionRef(const char* name, const Param* params, int32_t* offsets, int count)
        : name_(name), params_(params), offsets_(offsets), count_(count) {}

    bool Resolve(const Object* klass) {
        int state = state_.load(std::memory_order_acquire);
        if (state)
            return state > 0;
        if (!klass || !Ready()) {
            SetError("call uesdk::Init() first");
            return false;
        }
        bool expected = false;
        while (!busy_.compare_exchange_weak(expected, true, std::memory_order_acquire))
            expected = false;
        state = state_.load(std::memory_order_acquire);
        if (!state) {
            state = Find(klass) ? 1 : -1;
            state_.store(state, std::memory_order_release);
        }
        busy_.store(false, std::memory_order_release);
        return state > 0;
    }
    Object* Function() const { return function_; }
    int32_t ParmsSize() const { return parmsSize_; }
    int32_t Offset(int index) const { return offsets_[index]; }
    int32_t Size(int index) const { return params_[index].size; }
    const char* Name() const { return name_; }
    // The default object of the class that declares the function, for its static functions.
    Object* Default() {
        Object* object = default_.load(std::memory_order_acquire);
        if (!object && function_ && (object = DefaultObject(OuterOf(function_))) != nullptr)
            default_.store(object, std::memory_order_release);
        return object;
    }

private:
    bool Find(const Object* klass) {
        function_ = FindFunction(klass, name_);
        if (!function_) {
            SetError(std::string("the function ") + name_ + " of " + klass->GetName() + " is gone: the game changed, dump the SDK again");
            return false;
        }
        parmsSize_ = Read<uint16_t>(Address(function_) + layout::kParmsSize);
        for (int i = 0; i < count_; ++i) {
            uintptr_t property = 0;
            ForEachField(function_, [&](uintptr_t field, const std::string& text) {
                if (text == params_[i].name)
                    property = field;
                return !property;
            });
            int32_t size = property ? Read<int32_t>(property + layout::kElementSize) * Read<int32_t>(property + layout::kElementSize - 4) : -1;
            int32_t offset = property ? Read<int32_t>(property + layout::kOffset) : -1;
            if (size != params_[i].size || offset < 0 || offset + size > parmsSize_) {
                SetError(std::string("the parameter ") + params_[i].name + " of " + name_ + " changed: dump the SDK again");
                return false;
            }
            offsets_[i] = offset;
        }
        return true;
    }

    const char* name_;
    const Param* params_;
    int32_t* offsets_;
    int count_;
    std::atomic<int> state_{0};
    std::atomic<bool> busy_{false};
    std::atomic<Object*> default_{nullptr};
    Object* function_ = nullptr;
    int32_t parmsSize_ = 0;
};

// The parameters of one call, in a buffer of the function's ParmsSize,
// aligned as the engine aligns it for its vector types, such as FTransform.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
class Frame {
public:
    Frame(FunctionRef& function, const Object* klass) : function_(function) {
        ready_ = function.Resolve(klass);
        if (!ready_)
            return;
        size_t size = static_cast<size_t>(function.ParmsSize());
        if (size > sizeof(local_)) {
            heap_.resize(size);
            data_ = heap_.data();
        }
        std::memset(data_, 0, std::max<size_t>(size, 1));
    }
    bool Ready() const { return ready_; }
    template <typename T>
    void Set(int index, const T& value) {
        if (ready_)
            std::memcpy(data_ + function_.Offset(index), &value, std::min<size_t>(sizeof(T), static_cast<size_t>(function_.Size(index))));
    }
    template <typename T>
    void Get(int index, T& value) const {
        if (ready_)
            std::memcpy(&value, data_ + function_.Offset(index), std::min<size_t>(sizeof(T), static_cast<size_t>(function_.Size(index))));
    }
    bool Call(Object* self) {
        if (!ready_ || !self) {
            if (ready_)
                SetError(std::string("no object to call ") + function_.Name() + " on");
            return false;
        }
        if (layout::kProcessEvent < 0) {
            SetError("the SDK was dumped without UObject::ProcessEvent: functions cannot be called");
            return false;
        }
        return self->ProcessEvent(function_.Function(), data_);
    }
    bool CallStatic() { return Call(function_.Default()); }

private:
    FunctionRef& function_;
    bool ready_ = false;
    alignas(16) uint8_t local_[512];
    uint8_t* data_ = local_;
    std::vector<uint8_t> heap_;
};
#ifdef _MSC_VER
#pragma warning(pop)
#endif

// Initialization ------------------------------------------------------------

inline bool Fail(State& state, const std::string& error, bool outdated = false) {
    {
        std::lock_guard<std::mutex> lock(state.nameMutex);
        state.error = error;
    }
    state.outdated = state.outdated || outdated;
    return false;
}

inline bool StartsPool(uintptr_t block) {
    uint8_t b[20] = {};
    if (!SafeRead(block, b, sizeof(b)))
        return false;
    uint16_t none = static_cast<uint16_t>(b[0] | (b[1] << 8));
    uint16_t byteProperty = static_cast<uint16_t>(b[6] | (b[7] << 8));
    return (none >> 6) == 4 && std::memcmp(b + 2, "None", 4) == 0 && (byteProperty >> 6) == 12 && std::memcmp(b + 8, "ByteProperty", 12) == 0;
}

inline bool LegacyEntry(uintptr_t table, int index, const char* text) {
    uintptr_t chunk = 0;
    uintptr_t entry = 0;
    char bytes[16] = {};
    size_t length = strlen(text) + 1;
    return SafeRead(table, chunk) && SafeRead(chunk + index * P, entry) && SafeRead(entry + layout::kLegacyText, bytes, length) &&
           std::memcmp(bytes, text, length) == 0;
}

inline bool InitLocked(State& state) {
    if (state.ready)
        return true;
    if (state.outdated)
        return false;
    for (const layout::ModuleCheck& check : layout::kModules) {
        uintptr_t base = ModuleBase(check.name, check.main);
        if (!base)
            return Fail(state, std::string(check.name) + " is not loaded yet");
        uint32_t stamp = ModuleStamp(base);
        if (check.stamp && stamp && stamp != check.stamp)
            return Fail(state, std::string(check.name) + " is not the build this SDK was dumped from: the game was updated, dump the SDK again", true);
    }
    uintptr_t namesBase = ModuleBase(layout::kNamesModule, layout::kNamesMain);
    uintptr_t objectsBase = ModuleBase(layout::kObjectsModule, layout::kObjectsMain);
    uintptr_t names = namesBase + layout::kNames;
    if (layout::kNamesIndirect && !SafeRead(namesBase + layout::kNames, names))
        return Fail(state, "the name table cannot be read");
    uintptr_t block = 0;
    bool namesValid = layout::kNamePool ? SafeRead(names, block) && StartsPool(block)
                                        : LegacyEntry(names, 0, "None") && LegacyEntry(names, 1, "ByteProperty");
    if (!namesValid)
        return Fail(state, "the name table is not ready yet, or not where the SDK expects it");
    state.names = names;
    state.objects = objectsBase + layout::kObjects;
    state.numElements = layout::kNumElements ? objectsBase + layout::kNumElements : 0;
    uintptr_t table = 0;
    int32_t count = 0;
    if (!SafeRead(state.objects, table) || !table || (state.numElements && (!SafeRead(state.numElements, count) || count < 16)))
        return Fail(state, "the object array is not ready yet");
    uintptr_t items = table;
    uintptr_t first = 0;
    if ((!layout::kFlat && !SafeRead(table, items)) || !SafeRead(items + layout::kItemObject, first) || !first)
        return Fail(state, "the object array is not ready yet");
    state.package = reinterpret_cast<Object*>(first);
    if (state.package->GetName() != "/Script/CoreUObject")
        return Fail(state, "the object array is not where the SDK expects it: the game was updated, dump the SDK again", true);
    Object* objectClass = nullptr;
    Object* defaults = nullptr;
    for (int32_t i = 0, total = Count(); i < total && (!state.classClass || !state.structClass || !state.enumClass || !state.functionClass || !objectClass || !defaults); ++i) {
        Object* object = At(i);
        if (!object || OuterOf(object) != state.package)
            continue;
        std::string name = object->GetName();
        if (name == "Class" && ClassOf(object) == object)
            state.classClass = object;
        else if (name == "Struct")
            state.structClass = object;
        else if (name == "Enum")
            state.enumClass = object;
        else if (name == "Function")
            state.functionClass = object;
        else if (name == "Object")
            objectClass = object;
        else if (name == "Default__Object")
            defaults = object;
    }
    if (!state.classClass || !state.structClass || !state.enumClass || !state.functionClass || !objectClass)
        return Fail(state, "the classes of /Script/CoreUObject are not loaded yet");
    if (layout::kProcessEvent >= 0) {
        uintptr_t expected = ModuleBase(layout::kProcessEventModule, layout::kProcessEventMain) + layout::kProcessEventFunction;
        uintptr_t vtable = 0;
        uintptr_t actual = 0;
        if (!defaults || !SafeRead(Address(defaults), vtable) || !SafeRead(vtable + layout::kProcessEvent * P, actual))
            return Fail(state, "the default object of Object is not there yet");
        if (actual != expected)
            return Fail(state, "UObject::ProcessEvent is not where the SDK expects it: the game was updated, dump the SDK again", true);
    }
    std::lock_guard<std::mutex> lock(state.typeMutex);
    IndexTypes();
    state.ready = true;
    return true;
}

} // namespace detail
)SDK",
    R"SDK(
// Definitions ---------------------------------------------------------------

inline bool Init() {
    detail::State& state = detail::GetState();
    std::lock_guard<std::mutex> lock(state.mutex);
    return detail::InitLocked(state);
}

inline bool WaitForInit(unsigned milliseconds) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (!Init()) {
        if (detail::GetState().outdated || std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return true;
}

inline bool Ready() { return detail::Ready(); }

inline std::string LastError() {
    detail::State& state = detail::GetState();
    std::lock_guard<std::mutex> lock(state.nameMutex);
    return state.error;
}

inline std::string FName::ToString() const {
    // The name table, found by Init() before it checks the objects.
    if (!detail::GetState().names)
        return "";
    std::string text = detail::NameText(ComparisonIndex);
    if (Number)
        text += "_" + std::to_string(Number - 1);
    return text;
}

inline FName FName::Find(const std::string& text) {
    detail::State& state = detail::GetState();
    if (!Ready())
        return FName();
    std::string wanted = detail::Lower(text);
    {
        std::lock_guard<std::mutex> lock(state.nameMutex);
        auto known = state.nameIndex.find(wanted);
        if (known != state.nameIndex.end())
            return known->second;
    }
    // The names of the objects, and of their properties: those the game uses.
    FName found;
    for (int32_t i = 0, count = detail::Count(); i < count && found.IsNone(); ++i) {
        Object* object = detail::At(i);
        if (object && detail::Lower(object->GetName()) == wanted)
            found = object->GetFName();
    }
    std::lock_guard<std::mutex> lock(state.nameMutex);
    state.nameIndex[wanted] = found;
    return found;
}

inline std::string FString::ToString() const {
    std::u16string text = ToU16();
    return detail::Utf8(text.data(), text.size());
}

template <typename T>
T* TWeakObjectPtr<T>::Get() const {
    return static_cast<T*>(ObjectAt(ObjectIndex));
}

inline int32_t Object::GetIndex() const { return detail::Read<int32_t>(detail::Address(this) + layout::kIndex); }
inline uint32_t Object::GetFlags() const { return detail::Read<uint32_t>(detail::Address(this) + layout::kFlags); }
inline Object* Object::GetClass() const { return detail::ClassOf(this); }
inline Object* Object::GetOuter() const { return detail::OuterOf(this); }
inline FName Object::GetFName() const { return detail::Read<FName>(detail::Address(this) + layout::kName); }
inline std::string Object::GetName() const { return GetFName().ToString(); }
inline Object* Object::GetSuper() const { return detail::Super(this); }
inline bool Object::IsDefaultObject() const { return (GetFlags() & 0x30) != 0; }

inline std::string Object::GetFullName() const {
    std::string path = GetName();
    for (const Object* outer = GetOuter(); outer; outer = outer->GetOuter())
        path = outer->GetName() + (outer->GetOuter() ? ":" : ".") + path;
    return GetClass()->GetName() + " " + path;
}

inline bool Object::IsA(const Object* klass) const { return klass && detail::Derives(GetClass(), klass); }

namespace detail {
inline void CallProcessEvent(uintptr_t code, Object* self, Object* function, void* parms) {
#if defined(_WIN32) && (defined(_M_IX86) || defined(__i386__))
    // A virtual function of 32-bit Windows takes its object in ecx, as __fastcall does.
    reinterpret_cast<void(__fastcall*)(Object*, void*, Object*, void*)>(code)(self, nullptr, function, parms);
#else
    reinterpret_cast<void (*)(Object*, Object*, void*)>(code)(self, function, parms);
#endif
}

// False when the call faulted: a fault in the game's code stops the call, not the game.
inline bool GuardedProcessEvent(uintptr_t code, Object* self, Object* function, void* parms) {
#ifdef _MSC_VER
    __try {
        CallProcessEvent(code, self, function, parms);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    CallProcessEvent(code, self, function, parms);
#endif
    return true;
}
} // namespace detail

inline bool Object::ProcessEvent(Object* function, void* parms) {
    if (layout::kProcessEvent < 0 || !function) {
        detail::SetError("UObject::ProcessEvent is unknown, or the function is");
        return false;
    }
    uintptr_t vtable = 0;
    uintptr_t code = 0;
    if (!detail::SafeRead(detail::Address(this), vtable) || !detail::SafeRead(vtable + layout::kProcessEvent * detail::P, code) || !code) {
        detail::SetError("the object is gone: " + std::to_string(detail::Address(this)));
        return false;
    }
    if (!detail::GuardedProcessEvent(code, this, function, parms)) {
        detail::SetError("the call of " + function->GetName() + " faulted in the game's code");
        return false;
    }
    return true;
}

inline int32_t ObjectCount() { return Ready() ? detail::Count() : 0; }
inline Object* ObjectAt(int32_t index) { return Ready() ? detail::At(index) : nullptr; }

inline Object* FindType(const std::string& name, const std::string& package) {
    if (!Ready())
        return nullptr;
    detail::State& state = detail::GetState();
    std::lock_guard<std::mutex> lock(state.typeMutex);
    for (int pass = 0; pass < 2; ++pass) {
        auto range = state.types.equal_range(name);
        for (auto it = range.first; it != range.second; ++it)
            if (package.empty() || detail::PackageOf(it->second) == package)
                return it->second;
        // Loaded since: the blueprints come with their level. At most once a second.
        if (pass || std::chrono::steady_clock::now() - state.typesAt < std::chrono::seconds(1))
            break;
        detail::IndexTypes();
    }
    detail::SetError("no class, struct or enum " + package + (package.empty() ? "" : ".") + name + " is loaded");
    return nullptr;
}

inline Object* FindObject(const std::string& name, const Object* klass) {
    for (int32_t i = 0, count = ObjectCount(); i < count; ++i) {
        Object* object = detail::At(i);
        if (object && (!klass || object->IsA(klass)) && object->GetName() == name)
            return object;
    }
    return nullptr;
}

inline std::vector<Object*> Instances(const Object* klass, bool defaults) {
    std::vector<Object*> objects;
    if (!klass)
        return objects;
    std::unordered_map<const Object*, bool> derived;
    for (int32_t i = 0, count = ObjectCount(); i < count; ++i) {
        Object* object = detail::At(i);
        if (!object)
            continue;
        Object* type = object->GetClass();
        auto known = derived.find(type);
        if (known == derived.end())
            known = derived.emplace(type, detail::Derives(type, klass)).first;
        if (known->second && (defaults || !object->IsDefaultObject()))
            objects.push_back(object);
    }
    return objects;
}

inline Object* DefaultObject(const Object* klass) {
    for (int32_t i = 0, count = ObjectCount(); klass && i < count; ++i) {
        Object* object = detail::At(i);
        if (object && object->GetClass() == klass && (object->GetFlags() & 0x10))
            return object;
    }
    return nullptr;
}
)SDK",
    R"SDK(
// The game thread -----------------------------------------------------------

namespace detail {
#ifdef _WIN32
struct GameThreadState {
    std::mutex mutex;
    std::deque<std::function<void()>> queue;
    HWND window = nullptr;
    DWORD thread = 0;
    HHOOK hook = nullptr;
    // The calls of the hook procedure in progress, which Shutdown() waits for.
    std::atomic<int> running{0};
};

inline GameThreadState& GameThreadData() {
    static GameThreadState state;
    return state;
}

inline void RunPosted() {
    GameThreadState& state = GameThreadData();
    std::deque<std::function<void()>> work;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        work.swap(state.queue);
    }
    for (std::function<void()>& item : work)
        item();
}

// Called by the system on the window's thread for each message it takes from its queue.
inline LRESULT CALLBACK GameThreadHook(int code, WPARAM wParam, LPARAM lParam) {
    GameThreadState& state = GameThreadData();
    ++state.running;
    if (code == HC_ACTION)
        RunPosted();
    LRESULT result = CallNextHookEx(nullptr, code, wParam, lParam);
    --state.running;
    return result;
}

// The game's window: Unreal's UnrealWindow, or another visible window of the process.
inline BOOL CALLBACK FindGameWindow(HWND window, LPARAM context) {
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    if (process != GetCurrentProcessId() || !IsWindowVisible(window))
        return TRUE;
    wchar_t name[64] = {};
    GetClassNameW(window, name, 64);
    bool unreal = wcscmp(name, L"UnrealWindow") == 0;
    HWND* found = reinterpret_cast<HWND*>(context);
    if (!*found || unreal)
        *found = window;
    return !unreal;
}

inline bool Hooked() {
    GameThreadState& state = GameThreadData();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.hook && IsWindow(state.window))
        return true;
    HWND window = nullptr;
    EnumWindows(FindGameWindow, reinterpret_cast<LPARAM>(&window));
    if (!window)
        return false;
    if (state.hook)
        UnhookWindowsHookEx(state.hook);
    state.window = window;
    state.thread = GetWindowThreadProcessId(window, nullptr);
    state.hook = SetWindowsHookExW(WH_GETMESSAGE, GameThreadHook, nullptr, state.thread);
    return state.hook != nullptr;
}
#endif
} // namespace detail

namespace GameThread {
inline bool Post(std::function<void()> work) {
#ifdef _WIN32
    if (!detail::Hooked()) {
        detail::SetError("the game has no window yet, whose thread is the game thread");
        return false;
    }
    detail::GameThreadState& state = detail::GameThreadData();
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.queue.push_back(std::move(work));
    }
    PostMessageW(state.window, WM_NULL, 0, 0);
    return true;
#else
    (void)work;
    detail::SetError("GameThread is Windows only: on Linux, call the functions from the game thread");
    return false;
#endif
}

inline bool IsCurrent() {
#ifdef _WIN32
    return detail::Hooked() && GetCurrentThreadId() == detail::GameThreadData().thread;
#else
    return false;
#endif
}

inline bool Run(std::function<void()> work, unsigned milliseconds) {
    if (IsCurrent()) {
        work();
        return true;
    }
    auto done = std::make_shared<std::atomic<bool>>(false);
    if (!Post([work, done] {
            work();
            done->store(true);
        }))
        return false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (!done->load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (!done->load())
        detail::SetError("the game thread did not run the work in time: it may be loading; the work still runs when it can");
    return done->load();
}
} // namespace GameThread

// Then the hook procedure is no longer called, nor running: the DLL can unload.
inline void Shutdown() {
#ifdef _WIN32
    detail::GameThreadState& state = detail::GameThreadData();
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (state.hook)
            UnhookWindowsHookEx(state.hook);
        state.hook = nullptr;
        state.queue.clear();
    }
    // A message the game thread was taking when the hook went may still be in it.
    for (int i = 0; i < 1000 && (state.running.load() || i < 20); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
#endif
}

} // namespace uesdk
)SDK",
};

// Windows macros that would replace names of the game, such as a function
// GetObject or a property min: undefined over the generated classes.
const char* const kMacros[] = {"min", "max", "GetObject", "GetClassName", "GetMessage", "PostMessage", "SendMessage", "DrawText", "LoadImage",
                               "CreateWindow", "DeleteFile", "CopyFile", "MoveFile", "GetCurrentTime", "Yield", "IN", "OUT", "OPTIONAL", "DELETE",
                               "ERROR", "TRUE", "FALSE", "CONST", "VOID", "interface", "small", "near", "far", "IGNORE", "INFINITE", "TEXT",
                               "RGB", "PlaySound", "GetUserName", "GetComputerName", "SetPort", "GetProp", "SetProp", "RemoveProp", "LoadString",
                               "FindText", "ReplaceText", "DrawState", "GetCommandLine", "CreateEvent", "CreateMutex", "CreateFile", "FindResource",
                               "GetModuleHandle", "CreateFont", "GetTextMetrics", "ReportEvent", "StartDoc", "GetMonitorInfo", "UpdateResource",
                               "GetFileAttributes", "SetFileAttributes", "CreateDirectory", "RemoveDirectory", "GetEnvironmentVariable", "SetEnvironmentVariable",
                               "MessageBox", "GetObjectType", "Rectangle", "Polygon", "Ellipse", "LineTo", "TextOut", "GetCharWidth", "EnumFonts",
                               "OpenEvent", "GetTempPath", "GetVersion", "GetProfileString", "SetWindowText", "GetWindowText", "CALLBACK", "PASCAL",
                               "WINAPI", "APIENTRY", "BOOL", "BYTE", "WORD", "DWORD", "UINT", "INT", "FLOAT", "LONG", "CHAR", "HANDLE", "NULL",
                               "PURE", "THIS", "EXTERN_C", "DEFINE_GUID", "IsMaximized", "IsMinimized", "Status", "None", "Bool", "Always", "Success",
                               "DestroyAll", "Unsorted", "AnyKey", "ControlMask", "FocusIn", "FocusOut", "KeyPress", "KeyRelease"};

const char* const kKeywords[] = {
    "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor", "bool", "break", "case", "catch", "char", "char8_t", "char16_t", "char32_t",
    "class", "compl", "concept", "const", "consteval", "constexpr", "constinit", "const_cast", "continue", "co_await", "co_return", "co_yield", "decltype",
    "default", "delete", "do", "double", "dynamic_cast", "else", "enum", "explicit", "export", "extern", "false", "float", "for", "friend", "goto", "if",
    "inline", "int", "long", "mutable", "namespace", "new", "noexcept", "not", "not_eq", "nullptr", "operator", "or", "or_eq", "private", "protected",
    "public", "register", "reinterpret_cast", "requires", "return", "short", "signed", "sizeof", "static", "static_assert", "static_cast", "struct",
    "switch", "template", "this", "thread_local", "throw", "true", "try", "typedef", "typeid", "typename", "union", "unsigned", "using", "virtual",
    "void", "volatile", "wchar_t", "while", "xor", "xor_eq", "uesdk", "SDK", "detail", "result", "frame", "fn", "kParams", "offsets", "std",
    "int8_t", "int16_t", "int32_t", "int64_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t", "size_t", "uintptr_t", "assert"};

// The members of uesdk::Object, that the classes must not hide.
const char* const kObjectMembers[] = {"StaticClass", "GetIndex", "GetFlags", "GetClass", "GetOuter", "GetFName", "GetName", "GetFullName",
                                      "GetSuper", "IsA", "Cast", "IsDefaultObject", "ProcessEvent", "Object", "StaticStruct", "uesdk_raw_"};

bool Listed(const char* const* list, size_t count, const std::string& name) {
    for (size_t i = 0; i < count; ++i)
        if (name == list[i])
            return true;
    return false;
}

template <size_t N>
bool Listed(const char* const (&list)[N], const std::string& name) {
    return Listed(list, N, name);
}

// A C++ identifier for an Unreal name, which may have spaces, '?' and other
// characters: those become _, and a name C++ or windows.h already uses gets a _.
std::string Identifier(const std::string& name) {
    std::string out;
    for (char c : name) {
        auto byte = static_cast<unsigned char>(c);
        out += byte < 0x80 && (isalnum(byte) || c == '_') ? c : '_';
    }
    if (out.empty() || isdigit(static_cast<unsigned char>(out[0])))
        out = "_" + out;
    if (Listed(kKeywords, out) || Listed(kMacros, out) || Listed(kObjectMembers, out))
        out += "_";
    return out;
}

std::string Unique(std::set<std::string>& used, std::string name) {
    if (used.insert(name).second)
        return name;
    for (int i = 2;; ++i) {
        std::string candidate = name + "_" + std::to_string(i);
        if (used.insert(candidate).second)
            return candidate;
    }
}

// A C++ string literal of the text.
std::string Literal(const std::string& text) {
    std::string out = "\"";
    for (char c : text) {
        auto byte = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (byte < 0x20 || byte >= 0x7f || c == '?') {
            // Octal, three digits: no hexadecimal escape swallows the next character, nor a trigraph forms.
            char escape[8];
            snprintf(escape, sizeof(escape), "\\%03o", byte);
            out += escape;
        } else {
            out += c;
        }
    }
    return out + "\"";
}

// A text that stays inside a // comment.
std::string Comment(const std::string& text) {
    std::string out;
    for (char c : text)
        out += c == '\n' || c == '\r' ? ' ' : c;
    return out;
}

std::string HexText(uint64_t value) {
    char text[32];
    snprintf(text, sizeof(text), "0x%" PRIx64, value);
    return text;
}

// The temporary directory, with its separator.
std::string TempDirectory() {
#ifdef _WIN32
    wchar_t path[MAX_PATH + 1] = {};
    DWORD length = GetTempPathW(MAX_PATH + 1, path);
    if (!length || length > MAX_PATH)
        return "";
    char utf8[4 * MAX_PATH] = {};
    return WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8, sizeof(utf8), nullptr, nullptr) ? utf8 : "";
#else
    const char* directory = getenv("TMPDIR");
    std::string path = directory && *directory ? directory : "/tmp";
    return path.back() == '/' ? path : path + "/";
#endif
}

// Writes the whole file: false when it could not.
bool WriteText(const std::string& path, const std::string& text) {
#ifdef _WIN32
    wchar_t wide[2 * MAX_PATH] = {};
    if (!MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wide, 2 * MAX_PATH))
        return false;
    HANDLE file = CreateFileW(wide, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    bool done = ::WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) && written == text.size();
    return CloseHandle(file) && done;
#else
    FILE* file = fopen(path.c_str(), "wb");
    if (!file)
        return false;
    bool done = fwrite(text.data(), 1, text.size(), file) == text.size();
    return fclose(file) == 0 && done;
#endif
}

// The local time, as text: `long` for the header, short for the name of the file.
void LocalTime(std::string& full, std::string& compact) {
    int year, month, day, hour, minute, second;
#ifdef _WIN32
    SYSTEMTIME now{};
    GetLocalTime(&now);
    year = now.wYear, month = now.wMonth, day = now.wDay, hour = now.wHour, minute = now.wMinute, second = now.wSecond;
#else
    time_t seconds = time(nullptr);
    tm local{};
    localtime_r(&seconds, &local);
    year = local.tm_year + 1900, month = local.tm_mon + 1, day = local.tm_mday, hour = local.tm_hour, minute = local.tm_min, second = local.tm_sec;
#endif
    char text[64];
    snprintf(text, sizeof(text), "%04d-%02d-%02d %02d:%02d:%02d", year, month, day, hour, minute, second);
    full = text;
    snprintf(text, sizeof(text), "%04d%02d%02d-%02d%02d%02d", year, month, day, hour, minute, second);
    compact = text;
}

// The time stamp of a module, as the SDK checks it: IMAGE_FILE_HEADER::TimeDateStamp.
uint32_t ModuleStamp(const std::string& name) {
#ifdef _WIN32
    for (const LoadedModule& module : LoadedModules()) {
        if (module.name != name)
            continue;
        int32_t header = Int(module.base + 0x3c, 0);
        return static_cast<uint32_t>(Int(module.base + static_cast<uintptr_t>(header) + 8, 0));
    }
#else
    (void)name;
#endif
    return 0;
}

// The generator -------------------------------------------------------------

enum class Mode { Input, Output, InOut, Return };

struct CppType {
    std::string name;
    // Passed as const T& rather than by value.
    bool byReference = false;
};

class Generator {
public:
    Generator(Unreal& ue) : ue_(ue) {}

    std::string Run(const std::string& scopeText, std::vector<uint8_t>& payload);

private:
    struct Type {
        TypeEntry entry;
        std::string cpp;
        bool included = false;
        int32_t size = 0;
        int32_t alignment = 1;
        // Enums: the size of their underlying type, 0 until a property uses them.
        int32_t underlying = 0;
        std::vector<EnumValue> values;
    };

    bool InScope(const TypeEntry& type) const;
    Type& Know(uintptr_t object);
    void Include(uintptr_t object);
    void IncludeUses(const Property& property, int depth = 0);
    CppType TypeOf(const Property& property, int depth = 0);
    std::string IntegerOfSize(int32_t size, bool isSigned) const;
    std::string EnumUnderlying(const Type& type) const;
    void WriteEnum(std::string& out, Type& type);
    void WriteStruct(std::string& out, Type& type);
    void WriteClass(std::string& out, Type& type, std::string& definitions);
    void WriteAccessor(std::string& out, const Property& property, std::set<std::string>& used, bool inClass);
    void WriteFunction(std::string& declarations, std::string& definitions, Type& owner, uintptr_t function, std::set<std::string>& used);
    std::string Layout();

    Unreal& ue_;
    std::vector<std::string> scope_;
    bool all_ = false;
    std::string gameModule_;
    uintptr_t actorClass_ = 0;
    std::map<uintptr_t, Type> types_;
    std::set<std::string> typeNames_;
    std::vector<uintptr_t> order_;
    size_t functions_ = 0;
    std::vector<std::string> warnings_;
};

bool Generator::InScope(const TypeEntry& type) const {
    if (all_)
        return true;
    for (const std::string& item : scope_) {
        std::string package = Lower(type.package);
        std::string wanted = Lower(item);
        // A package, a folder of packages such as /Game/Characters, or the last part of a /Script/ package.
        if (package == wanted || (package.size() > wanted.size() && package.compare(0, wanted.size(), wanted) == 0 && package[wanted.size()] == '/') ||
            (wanted[0] != '/' && package == "/script/" + wanted))
            return true;
    }
    return false;
}

Generator::Type& Generator::Know(uintptr_t object) {
    auto known = types_.find(object);
    if (known != types_.end())
        return known->second;
    Type type;
    type.entry.object = object;
    type.entry.name = ObjectName(ue_, object);
    type.entry.package = PackageOf(ue_, object);
    uintptr_t klass = ClassOf(ue_, object);
    type.entry.kind = IsA(ue_, klass, ue_.classClass) ? "class" : IsA(ue_, klass, ue_.scriptStructClass) ? "struct" : "enum";
    std::string kind = type.entry.kind;
    std::string prefix = kind == "struct" ? "F" : kind == "class" ? (actorClass_ && IsA(ue_, object, actorClass_) ? "A" : "U") : "";
    type.cpp = Unique(typeNames_, Identifier(prefix + type.entry.name));
    if (kind == "struct") {
        type.size = StructSize(ue_, object);
        type.alignment = StructAlignment(ue_, object);
        // Without PropertiesSize: the end of its last member.
        if (type.size <= 0)
            for (const Property& member : OwnProperties(ue_, object))
                type.size = std::max(type.size, member.offset + member.size * std::max(member.arrayDim, 1));
        type.size = std::max(type.size, 1);
        if (type.size % type.alignment)
            type.alignment = 1;
    } else if (kind == "enum") {
        type.values = EnumValues(ue_, object);
    }
    return types_.emplace(object, type).first->second;
}

// A type with its members, its parents and what it uses by value.
void Generator::Include(uintptr_t object) {
    if (!object)
        return;
    Type& type = Know(object);
    if (type.included)
        return;
    type.included = true;
    std::string kind = type.entry.kind;
    if (kind == "enum") {
        order_.push_back(object);
        return;
    }
    // The parents first: C++ needs a class's base before the class.
    if (kind == "class")
        Include(SuperOf(ue_, object));
    for (uintptr_t level = object; level; level = kind == "struct" ? SuperOf(ue_, level) : 0)
        for (const Property& property : OwnProperties(ue_, level))
            IncludeUses(property);
    if (kind == "class")
        for (uintptr_t function : Functions(ue_, object))
            for (const Property& parameter : Parameters(ue_, function))
                IncludeUses(parameter);
    order_.push_back(object);
}

void Generator::IncludeUses(const Property& property, int depth) {
    const std::string& kind = property.kind;
    if (kind == "StructProperty")
        Include(Extra(ue_, property));
    else if (kind == "EnumProperty" || kind == "ByteProperty") {
        uintptr_t enumeration = EnumOf(ue_, property);
        if (enumeration && IsObject(ue_, enumeration)) {
            Include(enumeration);
            Type& type = Know(enumeration);
            if (!type.underlying || kind == "EnumProperty")
                type.underlying = kind == "EnumProperty" ? property.size : std::max(type.underlying, 1);
        }
    } else if ((kind == "ArrayProperty" || kind == "SetProperty") && depth < 3) {
        uintptr_t inner = Inner(ue_, property);
        if (inner)
            IncludeUses(Describe(ue_, inner, 0), depth + 1);
    } else if ((kind == "ObjectProperty" || kind == "ObjectPtrProperty" || kind == "WeakObjectProperty" || kind == "ClassProperty" ||
                kind == "ClassPtrProperty") && depth < 3) {
        uintptr_t klass = Extra(ue_, property, kind == "ClassProperty" || kind == "ClassPtrProperty" ? 1 : 0);
        if (klass && IsObject(ue_, klass))
            Know(klass);
    }
}

std::string Generator::IntegerOfSize(int32_t size, bool isSigned) const {
    switch (size) {
    case 1:
        return isSigned ? "int8_t" : "uint8_t";
    case 2:
        return isSigned ? "int16_t" : "uint16_t";
    case 4:
        return isSigned ? "int32_t" : "uint32_t";
    case 8:
        return isSigned ? "int64_t" : "uint64_t";
    default:
        return "uesdk::Opaque<" + std::to_string(size) + ">";
    }
}

// The underlying type of an enum: the size its properties have, wide enough for its values.
std::string Generator::EnumUnderlying(const Type& type) const {
    int32_t size = type.underlying ? type.underlying : 1;
    bool isSigned = false;
    for (const EnumValue& value : type.values)
        isSigned = isSigned || value.value < 0;
    return IntegerOfSize(size, isSigned);
}

CppType Generator::TypeOf(const Property& property, int depth) {
    const std::string& kind = property.kind;
    static const std::pair<const char*, const char*> kSimple[] = {
        {"Int8Property", "int8_t"},     {"Int16Property", "int16_t"},   {"IntProperty", "int32_t"},    {"Int64Property", "int64_t"},
        {"UInt16Property", "uint16_t"}, {"UInt32Property", "uint32_t"}, {"UInt64Property", "uint64_t"}, {"FloatProperty", "float"},
        {"DoubleProperty", "double"},   {"BoolProperty", "bool"}};
    static const std::unordered_map<std::string, int32_t> kSizes = {{"int8_t", 1}, {"int16_t", 2}, {"int32_t", 4}, {"int64_t", 8}, {"uint16_t", 2},
                                                                     {"uint32_t", 4}, {"uint64_t", 8}, {"float", 4}, {"double", 8}, {"bool", 1}};
    auto opaque = [&] { return CppType{"uesdk::Opaque<" + std::to_string(std::max(property.size, 1)) + ">", true}; };
    for (const auto& simple : kSimple)
        if (kind == simple.first)
            return kSizes.at(simple.second) == property.size ? CppType{simple.second, false} : opaque();
    auto className = [&](size_t index) {
        uintptr_t klass = Extra(ue_, property, index);
        if (!klass || !IsObject(ue_, klass))
            return std::string("uesdk::Object");
        return "SDK::" + Know(klass).cpp;
    };
    if (kind == "ByteProperty" || kind == "EnumProperty") {
        uintptr_t enumeration = EnumOf(ue_, property);
        auto known = enumeration ? types_.find(enumeration) : types_.end();
        if (known != types_.end() && known->second.included && (known->second.underlying ? known->second.underlying : 1) == property.size)
            return {"SDK::" + known->second.cpp, false};
        return {IntegerOfSize(property.size, kind == "EnumProperty"), property.size > 8};
    }
    if (kind == "NameProperty" && property.size == 8)
        return {"uesdk::FName", true};
    if (kind == "StrProperty" && property.size == static_cast<int32_t>(P + 8))
        return {"uesdk::FString", true};
    if ((kind == "ObjectProperty" || kind == "ObjectPtrProperty") && property.size == static_cast<int32_t>(P))
        return {className(0) + "*", false};
    if ((kind == "ClassProperty" || kind == "ClassPtrProperty") && property.size == static_cast<int32_t>(P))
        return {"uesdk::TSubclassOf<" + className(1) + ">", false};
    if (kind == "WeakObjectProperty" && property.size == 8)
        return {"uesdk::TWeakObjectPtr<" + className(0) + ">", false};
    if (kind == "StructProperty") {
        uintptr_t structure = Extra(ue_, property);
        auto known = structure ? types_.find(structure) : types_.end();
        if (known != types_.end() && known->second.included && known->second.size == property.size)
            return {"SDK::" + known->second.cpp, true};
        return opaque();
    }
    if (kind == "ArrayProperty" && property.size == static_cast<int32_t>(P + 8) && depth < 3) {
        uintptr_t inner = Inner(ue_, property);
        std::string element = inner ? TypeOf(Describe(ue_, inner, 0), depth + 1).name : "uint8_t";
        return {"uesdk::TArray<" + element + ">", true};
    }
    return opaque();
}

void Generator::WriteEnum(std::string& out, Type& type) {
    std::string underlying = EnumUnderlying(type);
    int32_t size = type.underlying ? type.underlying : 1;
    out += "// " + type.entry.package + "." + type.entry.name + "\nenum class " + type.cpp + " : " + underlying + " {\n";
    std::set<std::string> used;
    for (size_t i = 0; i < type.values.size(); ++i) {
        const EnumValue& value = type.values[i];
        // A value that does not fit the type, such as the _MAX of a full uint8 enum, is left out.
        bool fits = size >= 8 || (value.value < 0 ? value.value >= -(int64_t{1} << (8 * size - 1)) : value.value < (int64_t{1} << (8 * size)));
        std::string name = Unique(used, Identifier(value.name));
        if (fits)
            out += "    " + name + " = " + std::to_string(value.value) + (value.value > INT32_MAX || value.value < INT32_MIN ? "LL" : "") + ",\n";
    }
    out += "};\n\n";
}

// A property: a method returning a reference to its value, a BitRef for a
// bit of a byte, a reference to an array for a fixed-size array. In a class,
// the offset is found by the name of the property at run time.
void Generator::WriteAccessor(std::string& out, const Property& property, std::set<std::string>& used, bool inClass) {
    std::string name = Unique(used, Identifier(property.name));
    CppType type = TypeOf(property);
    std::string literal = Literal(property.name);
    std::string offset = inClass ? "m.Get(StaticClass())" : HexText(static_cast<uint64_t>(property.offset));
    std::string member = inClass ? "static uesdk::detail::MemberRef m(" + literal + ", " + HexText(static_cast<uint64_t>(property.offset)) + "); " : "";
    std::string self = inClass ? "this" : "uesdk_raw_";
    std::string comment = "  // " + HexText(static_cast<uint64_t>(property.offset)) + " " + Comment(TypeName(ue_, property)) + " " + Comment(property.name);
    uint8_t byte = 0;
    uint8_t mask = 0;
    if (property.kind == "BoolProperty")
        BoolLayout(ue_, property, byte, mask);
    if (property.kind == "BoolProperty" && mask != 0xff && property.arrayDim <= 1) {
        out += "    uesdk::BitRef " + name + "() { " + member + "return {&uesdk::detail::At<uint8_t>(" + self + ", " + offset + " + " +
               std::to_string(byte) + "), " + HexText(mask) + "}; }" + comment + "\n";
        return;
    }
    if (property.arrayDim > 1) {
        std::string array = type.name + "[" + std::to_string(property.arrayDim) + "]";
        out += "    " + type.name + " (&" + name + "())[" + std::to_string(property.arrayDim) + "] { " + member + "return uesdk::detail::At<" + array + ">(" +
               self + ", " + offset + "); }" + comment + "\n";
        return;
    }
    out += "    " + type.name + "& " + name + "() { " + member + "return uesdk::detail::At<" + type.name + ">(" + self + ", " + offset + "); }" + comment + "\n";
}

void Generator::WriteStruct(std::string& out, Type& type) {
    out += "// " + type.entry.package + "." + type.entry.name + ", " + HexText(static_cast<uint64_t>(type.size)) + " bytes";
    uintptr_t parent = SuperOf(ue_, type.entry.object);
    if (parent)
        out += ", derived from " + ObjectName(ue_, parent) + " (its members included)";
    out += "\nstruct " + std::string(type.alignment > 1 ? "alignas(" + std::to_string(type.alignment) + ") " : "") + type.cpp + " {\n";
    out += "    uint8_t uesdk_raw_[" + HexText(static_cast<uint64_t>(type.size)) + "];\n";
    out += "    static uesdk::Object* StaticStruct() { static uesdk::detail::TypeRef t(" + Literal(type.entry.package) + ", " + Literal(type.entry.name) +
           "); return t.Get(); }\n";
    std::set<std::string> used = {type.cpp};
    std::vector<Property> members;
    for (uintptr_t level = type.entry.object; level; level = SuperOf(ue_, level)) {
        std::vector<Property> own = OwnProperties(ue_, level);
        members.insert(members.begin(), own.begin(), own.end());
    }
    for (const Property& member : members)
        if (member.offset >= 0 && member.offset + member.size * std::max(member.arrayDim, 1) <= type.size)
            WriteAccessor(out, member, used, false);
    out += "};\nstatic_assert(sizeof(" + type.cpp + ") == " + HexText(static_cast<uint64_t>(type.size)) + ", \"" + type.cpp + "\");\n\n";
}

void Generator::WriteFunction(std::string& declarations, std::string& definitions, Type& owner, uintptr_t function, std::set<std::string>& used) {
    std::string original = ObjectName(ue_, function);
    std::string name = Unique(used, Identifier(original));
    uint32_t flags = FunctionFlags(ue_, function);
    bool isStatic = (flags & kFuncStatic) != 0;
    std::vector<Property> parameters = Parameters(ue_, function);
    std::string returned = "void";
    int returnIndex = -1;
    std::string signature;
    std::string params;
    std::string sets;
    std::string gets;
    std::set<std::string> names = {"result", "frame", "fn", "kParams", "offsets", name};
    for (size_t i = 0; i < parameters.size(); ++i) {
        const Property& parameter = parameters[i];
        CppType type = TypeOf(parameter);
        int32_t size = parameter.size * std::max(parameter.arrayDim, 1);
        params += std::string(params.empty() ? "" : ", ") + "{" + Literal(parameter.name) + ", " + HexText(static_cast<uint64_t>(parameter.offset)) + ", " +
                  HexText(static_cast<uint64_t>(size)) + "}";
        if (parameter.flags & kReturnParm) {
            returned = type.name;
            returnIndex = static_cast<int>(i);
            continue;
        }
        bool output = (parameter.flags & kOutParm) && !(parameter.flags & kConstParm);
        bool input = !(parameter.flags & kOutParm) || (parameter.flags & kReferenceParm);
        std::string argument = Unique(names, Identifier(parameter.name));
        std::string declared = output ? type.name + "& " : type.byReference ? "const " + type.name + "& " : type.name + " ";
        signature += (signature.empty() ? "" : ", ") + declared + argument;
        if (input)
            sets += "    frame.Set(" + std::to_string(i) + ", " + argument + ");\n";
        if (output)
            gets += "        frame.Get(" + std::to_string(i) + ", " + argument + ");\n";
    }
    if (returnIndex >= 0)
        gets += "        frame.Get(" + std::to_string(returnIndex) + ", result);\n";
    std::string what = flags & kFuncNative ? "native" : "blueprint";
    declarations += "    // " + Comment(original) + ": " + what + (isStatic ? ", static" : "") + ", ParmsSize " +
                    HexText(static_cast<uint64_t>(ParmsSize(ue_, function))) + "\n";
    declarations += std::string("    ") + (isStatic ? "static " : "") + returned + " " + name + "(" + signature + ");\n";
    definitions += "// " + owner.entry.name + ":" + Comment(original) + "\ninline " + returned + " " + owner.cpp + "::" + name + "(" + signature + ") {\n";
    size_t count = parameters.size();
    if (count) {
        definitions += "    static const uesdk::detail::Param kParams[] = {" + params + "};\n";
        definitions += "    static int32_t offsets[" + std::to_string(count) + "];\n";
    }
    definitions += "    static uesdk::detail::FunctionRef fn(" + Literal(original) + ", " + (count ? "kParams, offsets, " : "nullptr, nullptr, ") +
                   std::to_string(count) + ");\n";
    definitions += "    uesdk::detail::Frame frame(fn, StaticClass());\n" + sets;
    if (returned != "void")
        definitions += "    " + returned + " result{};\n";
    std::string call = std::string("frame.") + (isStatic ? "CallStatic()" : "Call(this)");
    definitions += gets.empty() ? "    " + call + ";\n" : "    if (" + call + ") {\n" + gets + "    }\n";
    if (returned != "void")
        definitions += "    return result;\n";
    definitions += "}\n\n";
    ++functions_;
}

void Generator::WriteClass(std::string& out, Type& type, std::string& definitions) {
    uintptr_t parent = SuperOf(ue_, type.entry.object);
    std::string base = parent ? Know(parent).cpp : "uesdk::Object";
    out += "// " + type.entry.package + "." + type.entry.name + "\nclass " + type.cpp + " : public " + base + " {\npublic:\n";
    out += "    static uesdk::Object* StaticClass() { static uesdk::detail::TypeRef t(" + Literal(type.entry.package) + ", " + Literal(type.entry.name) +
           "); return t.Get(); }\n";
    std::set<std::string> used = {type.cpp};
    for (const Property& property : OwnProperties(ue_, type.entry.object))
        WriteAccessor(out, property, used, true);
    for (uintptr_t function : Functions(ue_, type.entry.object)) {
        std::string functionName = ObjectName(ue_, function);
        if (functionName.size() > 19 && functionName.compare(functionName.size() - 19, 19, "__DelegateSignature") == 0)
            continue;
        WriteFunction(out, definitions, type, function, used);
    }
    out += "};\n\n";
}

std::string Generator::Layout() {
    Global names;
    Global objects;
    Global processEvent;
    GlobalOf(ue_, ue_.namesAt, names);
    GlobalOf(ue_, ue_.objectsAt, objects);
    bool hasProcessEvent = ue_.processEventIndex >= 0 && GlobalOf(ue_, ue_.processEventAddress, processEvent);
    Global count;
    bool hasCount = ue_.numElements && GlobalOf(ue_, ue_.numElements, count) && count.module == objects.module;
    auto size = [](size_t value) { return HexText(value); };
    std::string out = "namespace uesdk {\n// The layout of the engine, as found when the SDK was dumped.\nnamespace layout {\n";
    out += "struct ModuleCheck {\n    const char* name;\n    bool main;\n    uint32_t stamp;\n};\n";
    std::set<std::string> modules;
    std::string checks;
    for (const Global* global : {&names, &objects, &processEvent}) {
        if (global->module.empty() || !modules.insert(global->module).second)
            continue;
        checks += std::string(checks.empty() ? "" : ", ") + "{" + Literal(global->module) + ", " + (global->main ? "true" : "false") + ", " +
                  HexText(ModuleStamp(global->module)) + "}";
    }
    if (checks.empty())
        checks = "{\"\", true, 0}";
    out += "constexpr ModuleCheck kModules[] = {" + checks + "};\n";
    out += "constexpr const char* kNamesModule = " + Literal(names.module) + ";\nconstexpr bool kNamesMain = " + (names.main ? "true" : "false") + ";\n";
    out += "constexpr uintptr_t kNames = " + size(names.offset) + ";\nconstexpr bool kNamesIndirect = " + (ue_.namesIndirect ? "true" : "false") + ";\n";
    out += "constexpr bool kNamePool = " + std::string(ue_.namePool ? "true" : "false") + ";\nconstexpr size_t kLegacyText = " + size(ue_.legacyText) +
           ";\nconstexpr size_t kLegacyIndex = " + size(ue_.legacyIndex) + ";\n";
    out += "constexpr const char* kObjectsModule = " + Literal(objects.module) + ";\nconstexpr bool kObjectsMain = " + (objects.main ? "true" : "false") + ";\n";
    out += "constexpr uintptr_t kObjects = " + size(objects.offset) + ";\nconstexpr uintptr_t kNumElements = " + size(hasCount ? count.offset : 0) +
           ";\nconstexpr bool kFlat = " + (ue_.flat ? "true" : "false") + ";\n";
    out += "constexpr size_t kItemSize = " + size(ue_.itemSize) + ";\nconstexpr size_t kItemObject = " + size(ue_.itemObject) + ";\n";
    out += "// UObjectBase\nconstexpr size_t kFlags = " + size(ue_.flagsOffset) + ";\nconstexpr size_t kIndex = " + size(ue_.indexOffset) +
           ";\nconstexpr size_t kClass = " + size(ue_.classOffset) + ";\nconstexpr size_t kName = " + size(ue_.nameOffset) + ";\nconstexpr size_t kOuter = " +
           size(ue_.outerOffset) + ";\n";
    out += "// UField, UStruct\nconstexpr size_t kFieldNext = " + size(ue_.fieldNext) + ";\nconstexpr size_t kSuper = " + size(ue_.superOffset) +
           ";\nconstexpr size_t kChildren = " + size(ue_.childrenOffset) + ";\nconstexpr size_t kChildProperties = " + size(ue_.childPropertiesOffset) + ";\n";
    out += "// FField (4.25 and later), FProperty or UProperty, UFunction\nconstexpr size_t kFFieldNext = " + size(ue_.fieldNextOffset) +
           ";\nconstexpr size_t kFFieldName = " + size(ue_.fieldNameOffset) + ";\nconstexpr size_t kElementSize = " + size(ue_.elementSizeOffset) +
           ";\nconstexpr size_t kOffset = " + size(ue_.offsetOffset) + ";\nconstexpr size_t kParmsSize = " + size(ue_.parmsSizeOffset) + ";\n";
    out += "// UObject::ProcessEvent: its index in the virtual table, and the function there\nconstexpr int kProcessEvent = " +
           std::to_string(hasProcessEvent ? ue_.processEventIndex : -1) + ";\nconstexpr const char* kProcessEventModule = " + Literal(processEvent.module) +
           ";\nconstexpr bool kProcessEventMain = " + (processEvent.main ? "true" : "false") + ";\nconstexpr uintptr_t kProcessEventFunction = " +
           size(processEvent.offset) + ";\n";
    out += "} // namespace layout\n} // namespace uesdk\n";
    if (!hasProcessEvent)
        warnings_.push_back("warning: " + (ue_.processEventFailure.empty() ? std::string("UObject::ProcessEvent was not found") : ue_.processEventFailure.substr(6)) +
                            ": the functions of the SDK will not call");
    return out;
}

std::string Generator::Run(const std::string& scopeText, std::vector<uint8_t>& payload) {
    std::string failure = DiscoverReflection(ue_);
    if (!failure.empty())
        return failure;
    DiscoverProcessEvent(ue_);
    // The game's module, such as TheKillingAntidote in TheKillingAntidote-Win64-Shipping.exe.
    std::string game = ue_.module.substr(0, ue_.module.find_first_of("-."));
    std::string scopeName = scopeText.empty() || Lower(scopeText) == "game" ? "game" : Lower(scopeText) == "all" ? "all" : scopeText;
    all_ = scopeName == "all";
    if (scopeName == "game") {
        scope_ = {"/Game", "/Script/" + game};
    } else if (!all_) {
        size_t start = 0;
        while (start <= scopeText.size()) {
            size_t comma = scopeText.find(',', start);
            std::string item = scopeText.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            item.erase(0, item.find_first_not_of(' '));
            item.erase(item.find_last_not_of(" /") + 1);
            if (!item.empty())
                scope_.push_back(item);
            if (comma == std::string::npos)
                break;
            start = comma + 1;
        }
    }
    TypeEntry actor;
    std::string error;
    if (FindType(ue_, "Engine:Actor", actor, error))
        actorClass_ = actor.object;
    std::vector<TypeEntry> types = Types(ue_, true);
    size_t selected = 0;
    for (const TypeEntry& type : types) {
        if (!InScope(type))
            continue;
        Include(type.object);
        ++selected;
    }
    if (!selected)
        return "error no class, struct or enum is in " + scopeName + ": see engine assemblies for the packages; the scope is game, all, or packages and folders such as /Script/Engine,/Game/Characters";
    // Enums first, then structs, then classes, each after what it needs.
    std::string enums;
    std::string structs;
    std::string classes;
    std::string definitions;
    std::string declarations;
    size_t counts[3] = {};
    for (uintptr_t object : order_) {
        Type& type = types_[object];
        std::string kind = type.entry.kind;
        if (kind == "enum") {
            WriteEnum(enums, type);
            ++counts[2];
        } else if (kind == "struct") {
            WriteStruct(structs, type);
            ++counts[1];
        } else {
            WriteClass(classes, type, definitions);
            ++counts[0];
        }
    }
    for (const auto& known : types_) {
        const Type& type = known.second;
        if (type.entry.kind == std::string("class"))
            declarations += "class " + type.cpp + ";\n";
        else if (type.entry.kind == std::string("struct") && type.included)
            declarations += "struct " + type.cpp + ";\n";
    }
    std::string version = ue_.version != "-" ? ue_.version : ue_.doubles ? "5" : "4";
    std::string date;
    std::string stamp;
    LocalTime(date, stamp);
    std::string file = Identifier(game) + "-SDK-" + version + "-" + stamp + (scopeName == "game" ? "" : scopeName == "all" ? "-all" : "-custom") + ".hpp";
    std::string scopeDescription = all_ ? "every package" : scopeName == "game" ? "the game: /Script/" + game + " and the /Game/ packages" : scopeText;

    std::string text;
    text += "// C++ SDK of " + Comment(game) + " (" + Comment(ue_.module) + "), Unreal Engine " + version + ", dumped on " + date + "\n";
    text += "// by Universal-Dear-ImGui-Hook (engine dump). Scope: " + Comment(scopeDescription) +
            ", with the parents of its classes and the structs and enums they use by value: " + std::to_string(counts[0]) + " classes, " +
            std::to_string(counts[1]) + " structs, " + std::to_string(counts[2]) + " enums, " + std::to_string(functions_) + " functions.\n";
    text += R"SDK(//
// One header, without Unreal's headers or this library: include it in a DLL
// loaded into the game, as a trainer does. C++17, MSVC, GCC or Clang.
//
//   #include "THIS_FILE"
//   if (!uesdk::WaitForInit(60000))      // from a DLL injected at launch; Init() once the game runs
//       return log(uesdk::LastError());
//   for (SDK::ACharacter* character : uesdk::Instances<SDK::ACharacter>())
//       character->bHidden() = false;    // properties: references to the game's memory
//   uesdk::GameThread::Post([] {         // functions: on the game thread
//       if (auto* pawn = uesdk::FirstInstance<SDK::APawn>())
//           pawn->K2_DestroyActor();
//   });
//
// Classes have no data members: each property is a method that returns a
// reference to the value in the object, at the offset that the game has,
// found by the name of the property when first used, so that a patch of a
// blueprint does not break it. Inheritance gives IsA<T>() and Cast<T>().
// Structs used by value, such as FVector, have their size and members; other
// types the SDK does not describe are uesdk::Opaque<size>. Each function is a
// method that fills its parameters and calls UObject::ProcessEvent: inputs by
// value, outputs as references, and the return value. Static functions are
// called on the default object of their class. Names that are not C++, such
// as "Get Health Ratio", become identifiers (Get_Health_Ratio); the original
// name follows in a comment.
//
// Threads: read and write properties from any thread, as memory. Call the
// functions on the game thread only, the thread of the game's window: from
// another one, such as the render thread of an overlay menu (the render
// callback of Universal-Dear-ImGui-Hook), hand them to GameThread::Post().
// Never wait for the game thread from the render thread: the game thread
// waits for the render thread at the end of its frames.
//
// Init() checks that the executable is the build it was dumped from, and
// that the name table, the object array and UObject::ProcessEvent are where
// they were: after an update of the game, it fails with LastError(); dump the
// SDK again. Call Shutdown() before your DLL unloads.
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// GameThread: the window of the game and its messages.
#ifdef _MSC_VER
#pragma comment(lib, "user32.lib")
#endif
#else
#include <fcntl.h>
#include <link.h>
#include <unistd.h>
#endif

)SDK";
    size_t marker = text.find("THIS_FILE");
    text.replace(marker, 9, file);
    text += Layout();
    for (const char* piece : kRuntime)
        text += piece;
    text += "\n// ===========================================================================\n// The game\n// "
            "===========================================================================\n\n";
    for (const char* macro : kMacros)
        text += std::string("#pragma push_macro(\"") + macro + "\")\n#undef " + macro + "\n";
    text += "\nnamespace SDK {\n\nusing uesdk::FName;\nusing uesdk::FString;\nusing uesdk::TArray;\n\n";
    text += declarations + "\n" + enums + structs + classes + definitions + "} // namespace SDK\n\n";
    for (const char* macro : kMacros)
        text += std::string("#pragma pop_macro(\"") + macro + "\")\n";

    std::string directory = TempDirectory();
    if (directory.empty())
        return "error the library did not find the temporary directory";
    std::string path = directory + file;
    if (!WriteText(path, text))
        return "error the library could not write " + path;
    std::string answer = path + "\n" + std::to_string(counts[0]) + " classes, " + std::to_string(counts[1]) + " structs, " + std::to_string(counts[2]) +
                         " enums, " + std::to_string(functions_) + " functions, " + std::to_string(text.size()) + " bytes\n";
    for (const std::string& warning : warnings_)
        answer += warning + "\n";
    payload.assign(answer.begin(), answer.end());
    return "ok " + std::to_string(payload.size());
}

} // namespace

std::string DumpSdk(Unreal& ue, const std::string& scope, std::vector<uint8_t>& payload) {
    std::string text = scope;
    text.erase(0, text.find_first_not_of(' '));
    text.erase(text.find_last_not_of(' ') + 1);
    Generator generator(ue);
    return generator.Run(text, payload);
}

} // namespace uih::detail::remote::unreal
