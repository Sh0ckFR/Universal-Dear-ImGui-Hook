// A fake Unity scripting runtime for the engine commands of the remote
// control (src/engine.cpp): it exports the C API of Mono, or of IL2CPP when
// built with UIH_FAKE_IL2CPP, over a small object model laid out as the real
// runtimes lay out theirs. The test hosts load it under the name of the real
// library, mono-2.0-bdwgc.dll or GameAssembly.dll (libmonobdwgc-2.0.so,
// GameAssembly.so on Linux), and check the commands with tests/remote_engine.h.
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <new>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define EXPORT extern "C" __declspec(dllexport)
#else
#include <sys/syscall.h>
#include <unistd.h>
#define EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace {

enum Kind {
    kVoid = 0x01,
    kBoolean = 0x02,
    kI4 = 0x08,
    kR4 = 0x0c,
    kString = 0x0e,
    kValueType = 0x11,
    kClass = 0x12,
    kI = 0x18,
};

constexpr uint32_t kStatic = 0x10;
constexpr uint32_t kLiteral = 0x40;
constexpr size_t kHeader = 2 * sizeof(void*);

struct Class;
struct Image;

struct Type {
    int kind;
    Class* klass;
};

struct Domain {
    int unused;
};

// As MonoVTable starts.
struct VTable {
    Class* klass;
    void* gcDescriptor;
    Domain* domain;
};

struct Field {
    const char* name;
    Type* type;
    Class* parent;
    size_t offset;
    uint32_t flags;
};

using Invoker = void* (*)(void* object, void** params, void** exception);

// Fixed arrays in the arena below: the metadata stays in the library, which
// the scan for instances skips, as it skips the modules of a real runtime.
template <typename T, size_t N>
struct List {
    T items[N];
    size_t count;
    void push_back(T item) { items[count++] = item; }
    size_t size() const { return count; }
    T operator[](size_t i) const { return items[i]; }
    T* begin() { return items; }
    T* end() { return items + count; }
};

struct Method {
    const char* name;
    uint32_t flags;
    Type* returnType;
    List<Type*, 4> params;
    List<const char*, 4> names;
    Class* klass;
    Invoker call;
};

// As Il2CppClass starts: with its image.
struct Class {
    Image* image;
    const char* name;
    const char* space;
    Class* parent;
    Class* declaring;
    List<Field*, 16> fields;
    List<Method*, 8> methods;
    Type type;
    bool valueType;
    bool isEnum;
    Type* enumBase;
    VTable* vtable;
    uint8_t* staticData;
    size_t staticSize;
    size_t size;
};

struct Image {
    const char* name;
    List<Class*, 16> classes;
};

struct Assembly {
    Image* image;
};

alignas(16) unsigned char g_arena[64 * 1024];
size_t g_arenaUsed = 0;

template <typename T>
T* Make() {
    void* memory = g_arena + g_arenaUsed;
    g_arenaUsed += (sizeof(T) + 15) / 16 * 16;
    return new (memory) T();
}

Domain g_domain;
List<Assembly*, 4> g_assemblies;
alignas(16) uint8_t g_staticData[4][64];
size_t g_staticUsed = 0;
std::atomic<int> g_attached{0};
std::atomic<int> g_attaches{0};
std::atomic<uint64_t> g_lastCallThread{0};
thread_local bool t_attached = false;
thread_local int t_thread = 0;

Class* g_string;
Class* g_int32;
Class* g_exception;
Class* g_player;
Class* g_vector3;
void* g_objects[5];

uint64_t ThreadId() {
#ifdef _WIN32
    return GetCurrentThreadId();
#else
    return static_cast<uint64_t>(syscall(SYS_gettid));
#endif
}

Image* NewImage(const char* name) {
    auto* image = Make<Image>();
    image->name = name;
    auto* assembly = Make<Assembly>();
    assembly->image = image;
    g_assemblies.push_back(assembly);
    return image;
}

Class* NewClass(Image* image, const char* space, const char* name, Class* parent, bool valueType = false) {
    auto* klass = Make<Class>();
    klass->image = image;
    klass->name = name;
    klass->space = space;
    klass->parent = parent;
    klass->type = {valueType ? kValueType : kClass, klass};
    klass->valueType = valueType;
    klass->size = parent && !valueType ? parent->size : kHeader;
    if (!valueType) {
        klass->vtable = Make<VTable>();
        *klass->vtable = {klass, nullptr, &g_domain};
    }
    image->classes.push_back(klass);
    return klass;
}

size_t SizeOf(const Type* type) {
    switch (type->kind) {
    case kBoolean:
        return 1;
    case kI4:
    case kR4:
        return 4;
    case kValueType:
        return type->klass->isEnum ? 4 : type->klass->size - kHeader;
    default:
        return sizeof(void*);
    }
}

Field* AddField(Class* klass, const char* name, Type* type, uint32_t flags = 0) {
    auto* field = Make<Field>();
    *field = {name, type, klass, 0, flags};
    size_t size = SizeOf(type);
    size_t align = type->kind == kValueType ? 4 : size;
    if (flags & kLiteral) {
    } else if (flags & kStatic) {
        klass->staticSize = (klass->staticSize + align - 1) / align * align;
        field->offset = klass->staticSize;
        klass->staticSize += size;
    } else {
        klass->size = (klass->size + align - 1) / align * align;
        field->offset = klass->size;
        klass->size += size;
    }
    klass->fields.push_back(field);
    return field;
}

void AddMethod(Class* klass, const char* name, uint32_t flags, Type* returnType, std::initializer_list<Type*> params,
               std::initializer_list<const char*> names, Invoker call) {
    auto* method = Make<Method>();
    method->name = name;
    method->flags = flags;
    method->returnType = returnType;
    for (Type* type : params)
        method->params.push_back(type);
    for (const char* parameter : names)
        method->names.push_back(parameter);
    method->klass = klass;
    method->call = call;
    klass->methods.push_back(method);
}

void* Header(Class* klass) {
#ifdef UIH_FAKE_IL2CPP
    return klass;
#else
    return klass->vtable;
#endif
}

void* NewObject(Class* klass) {
    // Rounded to 16 bytes, as the garbage collector allocates.
    void* object = calloc(1, (klass->size + 15) / 16 * 16);
    *static_cast<void**>(object) = Header(klass);
    return object;
}

Class* ClassOf(void* object) {
#ifdef UIH_FAKE_IL2CPP
    return *static_cast<Class**>(object);
#else
    return (*static_cast<VTable**>(object))->klass;
#endif
}

// From UTF-8, up to three bytes a character.
void* NewString(const char* text) {
    std::vector<uint16_t> units;
    for (const auto* c = reinterpret_cast<const unsigned char*>(text); *c;) {
        uint32_t point = *c++;
        int extra = point >= 0xe0 ? 2 : point >= 0xc0 ? 1 : 0;
        point &= extra == 2 ? 0x0f : extra == 1 ? 0x1f : 0x7f;
        for (; extra > 0 && (*c & 0xc0) == 0x80; --extra)
            point = point << 6 | (*c++ & 0x3f);
        units.push_back(static_cast<uint16_t>(point));
    }
    auto* object = static_cast<uint8_t*>(calloc(1, kHeader + 4 + 2 * units.size() + 2));
    *reinterpret_cast<void**>(object) = Header(g_string);
    int32_t count = static_cast<int32_t>(units.size());
    memcpy(object + kHeader, &count, 4);
    memcpy(object + kHeader + 4, units.data(), 2 * units.size());
    return object;
}

std::string ReadString(void* string) {
    if (!string)
        return "";
    auto* bytes = static_cast<uint8_t*>(string);
    int32_t length = 0;
    memcpy(&length, bytes + kHeader, 4);
    std::string text;
    for (int32_t i = 0; i < length; ++i) {
        uint16_t c = 0;
        memcpy(&c, bytes + kHeader + 4 + 2 * i, 2);
        text += static_cast<char>(c);
    }
    return text;
}

Field* FieldNamed(Class* klass, const char* name) {
    for (Class* at = klass; at; at = at->parent)
        for (Field* field : at->fields)
            if (strcmp(field->name, name) == 0)
                return field;
    return nullptr;
}

template <typename T>
T& At(void* object, const char* field) {
    return *reinterpret_cast<T*>(static_cast<uint8_t*>(object) + FieldNamed(ClassOf(object), field)->offset);
}

void* Box(int32_t value) {
    void* object = NewObject(g_int32);
    memcpy(static_cast<uint8_t*>(object) + kHeader, &value, 4);
    return object;
}

void* Add(void*, void** params, void**) { return Box(*static_cast<int32_t*>(params[0]) + *static_cast<int32_t*>(params[1])); }

void* Heal(void* object, void** params, void**) {
    At<int32_t>(object, "health") += *static_cast<int32_t*>(params[0]);
    return nullptr;
}

void* GetName(void* object, void**, void**) { return At<void*>(object, "playerName"); }

void* Rename(void* object, void** params, void**) {
    At<void*>(object, "playerName") = params[0];
    return nullptr;
}

void* Fail(void*, void**, void** exception) {
    void* thrown = NewObject(g_exception);
    At<void*>(thrown, "_message") = NewString("it failed on purpose");
    *exception = thrown;
    return nullptr;
}

void* GetHealth(void* object, void**, void**) { return Box(At<int32_t>(object, "health")); }

void* NewPlayer(Class* klass, const char* name, int32_t health, bool alive) {
    void* player = NewObject(klass);
    At<void*>(player, "m_CachedPtr") = alive ? malloc(16) : nullptr;
    At<int32_t>(player, "health") = health;
    At<float>(player, "speed") = 4.5f;
    At<bool>(player, "alive") = true;
    At<int32_t>(player, "team") = 2;
    At<void*>(player, "playerName") = NewString(name);
    float position[3] = {1.0f, 2.0f, 3.0f};
    memcpy(&At<float>(player, "position"), position, sizeof(position));
    return player;
}

struct Model {
    Model() {
        Image* corlib = NewImage("mscorlib.dll");
        Class* object = NewClass(corlib, "System", "Object", nullptr);
        Class* valueType = NewClass(corlib, "System", "ValueType", object);
        Class* enumClass = NewClass(corlib, "System", "Enum", valueType);
        g_int32 = NewClass(corlib, "System", "Int32", valueType, true);
        g_int32->type.kind = kI4;
        AddField(g_int32, "m_value", &g_int32->type);
        Class* single = NewClass(corlib, "System", "Single", valueType, true);
        single->type.kind = kR4;
        Class* boolean = NewClass(corlib, "System", "Boolean", valueType, true);
        boolean->type.kind = kBoolean;
        Class* intPtr = NewClass(corlib, "System", "IntPtr", valueType, true);
        intPtr->type.kind = kI;
        Class* voidClass = NewClass(corlib, "System", "Void", valueType, true);
        voidClass->type.kind = kVoid;
        g_string = NewClass(corlib, "System", "String", object);
        g_string->type.kind = kString;
        AddField(g_string, "_stringLength", &g_int32->type);
        g_exception = NewClass(corlib, "System", "Exception", object);
        AddField(g_exception, "_message", &g_string->type);

        Image* core = NewImage("UnityEngine.CoreModule.dll");
        Class* unityObject = NewClass(core, "UnityEngine", "Object", object);
        AddField(unityObject, "m_CachedPtr", &intPtr->type);
        g_vector3 = NewClass(core, "UnityEngine", "Vector3", valueType, true);
        AddField(g_vector3, "x", &single->type);
        AddField(g_vector3, "y", &single->type);
        AddField(g_vector3, "z", &single->type);
        Class* behaviour = NewClass(core, "UnityEngine", "MonoBehaviour", unityObject);

        Image* game = NewImage("Assembly-CSharp.dll");
        Class* team = NewClass(game, "Game", "Team", enumClass, true);
        team->isEnum = true;
        team->enumBase = &g_int32->type;
        AddField(team, "value__", &g_int32->type);
        AddField(team, "Red", &team->type, kStatic | kLiteral);
        AddField(team, "Blue", &team->type, kStatic | kLiteral);
        g_player = NewClass(game, "Game", "Player", behaviour);
        g_player->staticData = g_staticData[g_staticUsed++];
        AddField(g_player, "MaxHealth", &g_int32->type, kStatic | kLiteral);
        AddField(g_player, "s_Count", &g_int32->type, kStatic);
        AddField(g_player, "s_Local", &g_player->type, kStatic);
        AddField(g_player, "s_Spawn", &g_vector3->type, kStatic);
        AddField(g_player, "health", &g_int32->type);
        AddField(g_player, "speed", &single->type);
        AddField(g_player, "alive", &boolean->type);
        AddField(g_player, "team", &team->type);
        AddField(g_player, "playerName", &g_string->type);
        AddField(g_player, "target", &g_player->type);
        AddField(g_player, "position", &g_vector3->type);
        AddMethod(g_player, "Add", kStatic, &g_int32->type, {&g_int32->type, &g_int32->type}, {"a", "b"}, Add);
        AddMethod(g_player, "Heal", 0, &voidClass->type, {&g_int32->type}, {"amount"}, Heal);
        AddMethod(g_player, "GetName", 0, &g_string->type, {}, {}, GetName);
        AddMethod(g_player, "Rename", 0, &voidClass->type, {&g_string->type}, {"name"}, Rename);
        AddMethod(g_player, "Fail", kStatic, &voidClass->type, {}, {}, Fail);
        AddMethod(g_player, "get_Health", 0, &g_int32->type, {}, {}, GetHealth);
        Class* inventory = NewClass(game, "", "Inventory", object);
        inventory->declaring = g_player;
        Class* boss = NewClass(game, "Game", "Boss", g_player);
        AddField(boss, "phase", &g_int32->type);

        void* alice = NewPlayer(g_player, "Alice", 100, true);
        void* bob = NewPlayer(g_player, "Bob", 80, true);
        At<void*>(alice, "target") = bob;
        void* theBoss = NewPlayer(boss, "Boss", 500, true);
        // The header of a player, but a name that is no string: not an object.
        void* decoy = NewPlayer(g_player, "Decoy", 1, true);
        At<void*>(decoy, "playerName") = calloc(1, 64);
        // Destroyed: its native object is gone.
        void* dead = NewPlayer(g_player, "Dead", 0, false);
        void* objects[] = {alice, bob, theBoss, decoy, dead};
        memcpy(g_objects, objects, sizeof(objects));
        int32_t count = 7;
        memcpy(g_player->staticData + FieldNamed(g_player, "s_Count")->offset, &count, 4);
        memcpy(g_player->staticData + FieldNamed(g_player, "s_Local")->offset, &alice, sizeof(void*));
        float spawn[3] = {5.0f, 6.0f, 7.0f};
        memcpy(g_player->staticData + FieldNamed(g_player, "s_Spawn")->offset, spawn, sizeof(spawn));
    }
};

Model& TheModel() {
    static Model model;
    return model;
}

void* Attach() {
    TheModel();
    if (!t_attached) {
        t_attached = true;
        ++g_attached;
    }
    ++g_attaches;
    return &t_thread;
}

void Detach(void* thread) {
    if (thread == &t_thread && t_attached) {
        t_attached = false;
        --g_attached;
    }
}

template <typename T, size_t N>
T* Next(List<T*, N>& list, void** iterator) {
    auto index = reinterpret_cast<uintptr_t>(*iterator);
    if (index >= list.size())
        return nullptr;
    *iterator = reinterpret_cast<void*>(index + 1);
    return list[index];
}

char* TypeName(Type* type) {
    std::string name = type->klass->space && *type->klass->space ? std::string(type->klass->space) + "." + type->klass->name : type->klass->name;
    char* copy = static_cast<char*>(malloc(name.size() + 1));
    memcpy(copy, name.c_str(), name.size() + 1);
    return copy;
}

bool IsSubclass(Class* klass, Class* parent) {
    for (Class* at = klass; at; at = at->parent)
        if (at == parent)
            return true;
    return false;
}

size_t ValueSize(Field* field) { return field->type->kind == kString || field->type->kind == kClass ? sizeof(void*) : SizeOf(field->type); }

void StaticGet(Field* field, void* out) { memcpy(out, field->parent->staticData + field->offset, ValueSize(field)); }

void StaticSet(Field* field, void* value) {
    // References are given as the object itself, values by their address.
    if (field->type->kind == kString || field->type->kind == kClass)
        memcpy(field->parent->staticData + field->offset, &value, sizeof(void*));
    else
        memcpy(field->parent->staticData + field->offset, value, ValueSize(field));
}

void* Invoke(Method* method, void* object, void** params, void** exception) {
    g_lastCallThread = ThreadId();
    if (exception)
        *exception = nullptr;
    if (!t_attached) {
        void* thrown = NewObject(g_exception);
        At<void*>(thrown, "_message") = NewString("the thread is not attached");
        *exception = thrown;
        return nullptr;
    }
    return method->call(object, params, exception);
}

} // namespace

// The state of the model, for the hosts.
EXPORT const char fake_engine_version[] = "Unity version: 2022.3.10f1";
EXPORT void* fake_engine_object(int index) {
    TheModel();
    return index >= 0 && index < 5 ? g_objects[index] : nullptr;
}
EXPORT int fake_engine_static_count() {
    TheModel();
    int32_t count = 0;
    memcpy(&count, g_player->staticData + FieldNamed(g_player, "s_Count")->offset, 4);
    return count;
}
EXPORT uint64_t fake_engine_last_call_thread() { return g_lastCallThread; }
// The game's main thread is attached to the runtime, as Unity's is.
EXPORT void fake_engine_attach_main() { Attach(); }
EXPORT int fake_engine_attached_threads() { return g_attached; }
EXPORT int fake_engine_attaches() { return g_attaches; }

#ifdef UIH_FAKE_IL2CPP

EXPORT void* il2cpp_domain_get() { return TheModel(), &g_domain; }
EXPORT void* il2cpp_thread_attach(void*) { return Attach(); }
EXPORT void il2cpp_thread_detach(void* thread) { Detach(thread); }
EXPORT void* il2cpp_thread_current() { return t_attached ? &t_thread : nullptr; }
EXPORT Assembly** il2cpp_domain_get_assemblies(void*, size_t* count) {
    *count = g_assemblies.size();
    return g_assemblies.items;
}
EXPORT Image* il2cpp_assembly_get_image(Assembly* assembly) { return assembly->image; }
EXPORT const char* il2cpp_image_get_name(Image* image) { return image->name; }
EXPORT size_t il2cpp_image_get_class_count(Image* image) { return image->classes.size(); }
EXPORT Class* il2cpp_image_get_class(Image* image, size_t index) { return index < image->classes.size() ? image->classes[index] : nullptr; }
EXPORT const char* il2cpp_class_get_name(Class* klass) { return klass->name; }
EXPORT const char* il2cpp_class_get_namespace(Class* klass) { return klass->space; }
EXPORT Class* il2cpp_class_get_parent(Class* klass) { return klass->parent; }
EXPORT Class* il2cpp_class_get_declaring_type(Class* klass) { return klass->declaring; }
EXPORT Field* il2cpp_class_get_fields(Class* klass, void** iterator) { return Next(klass->fields, iterator); }
EXPORT Method* il2cpp_class_get_methods(Class* klass, void** iterator) { return Next(klass->methods, iterator); }
EXPORT Type* il2cpp_class_enum_basetype(Class* klass) { return klass->enumBase; }
EXPORT Class* il2cpp_class_from_type(Type* type) { return type->klass; }
EXPORT bool il2cpp_class_is_subclass_of(Class* klass, Class* parent, bool) { return IsSubclass(klass, parent); }
EXPORT bool il2cpp_class_is_valuetype(Class* klass) { return klass->valueType; }
EXPORT bool il2cpp_class_is_enum(Class* klass) { return klass->isEnum; }
EXPORT const char* il2cpp_field_get_name(Field* field) { return field->name; }
EXPORT Type* il2cpp_field_get_type(Field* field) { return field->type; }
EXPORT size_t il2cpp_field_get_offset(Field* field) { return field->offset; }
EXPORT int il2cpp_field_get_flags(Field* field) { return static_cast<int>(field->flags); }
EXPORT void il2cpp_field_static_get_value(Field* field, void* out) { StaticGet(field, out); }
EXPORT void il2cpp_field_static_set_value(Field* field, void* value) { StaticSet(field, value); }
EXPORT char* il2cpp_type_get_name(Type* type) { return TypeName(type); }
EXPORT int il2cpp_type_get_type(Type* type) { return type->kind; }
EXPORT void il2cpp_free(void* memory) { free(memory); }
EXPORT const char* il2cpp_method_get_name(Method* method) { return method->name; }
EXPORT uint32_t il2cpp_method_get_flags(Method* method, uint32_t* implementation) {
    if (implementation)
        *implementation = 0;
    return method->flags;
}
EXPORT Type* il2cpp_method_get_return_type(Method* method) { return method->returnType; }
EXPORT uint32_t il2cpp_method_get_param_count(Method* method) { return static_cast<uint32_t>(method->params.size()); }
EXPORT Type* il2cpp_method_get_param(Method* method, uint32_t index) { return index < method->params.size() ? method->params[index] : nullptr; }
EXPORT const char* il2cpp_method_get_param_name(Method* method, uint32_t index) { return index < method->names.size() ? method->names[index] : nullptr; }
EXPORT Class* il2cpp_object_get_class(void* object) { return ClassOf(object); }
EXPORT void* il2cpp_runtime_invoke(Method* method, void* object, void** params, void** exception) { return Invoke(method, object, params, exception); }
EXPORT void* il2cpp_string_new(const char* text) { return NewString(text); }
EXPORT void il2cpp_gc_wbarrier_set_field(void*, void** target, void* value) { *target = value; }

#else

EXPORT Domain* mono_get_root_domain() { return TheModel(), &g_domain; }
EXPORT void* mono_thread_attach(Domain*) { return Attach(); }
EXPORT void mono_thread_detach(void* thread) { Detach(thread); }
EXPORT Domain* mono_domain_get() { return t_attached ? &g_domain : nullptr; }
EXPORT void mono_assembly_foreach(void (*callback)(void*, void*), void* context) {
    for (Assembly* assembly : g_assemblies)
        callback(assembly, context);
}
EXPORT Image* mono_assembly_get_image(Assembly* assembly) { return assembly->image; }
// Without the extension, as Mono names its images.
EXPORT const char* mono_image_get_name(Image* image) {
    static thread_local std::string name;
    name = image->name;
    name.resize(name.size() - 4);
    return name.c_str();
}
// The first row of the TypeDef table is <Module>.
EXPORT int mono_image_get_table_rows(Image* image, int table) { return table == 2 ? static_cast<int>(image->classes.size()) + 1 : 0; }
EXPORT Class* mono_class_get(Image* image, uint32_t token) {
    uint32_t row = token & 0xffffff;
    return (token >> 24) == 2 && row >= 2 && row - 2 < image->classes.size() ? image->classes[row - 2] : nullptr;
}
EXPORT const char* mono_class_get_name(Class* klass) { return klass->name; }
EXPORT const char* mono_class_get_namespace(Class* klass) { return klass->space; }
EXPORT Class* mono_class_get_parent(Class* klass) { return klass->parent; }
EXPORT Class* mono_class_get_nesting_type(Class* klass) { return klass->declaring; }
EXPORT Field* mono_class_get_fields(Class* klass, void** iterator) { return Next(klass->fields, iterator); }
EXPORT Method* mono_class_get_methods(Class* klass, void** iterator) { return Next(klass->methods, iterator); }
EXPORT Type* mono_class_enum_basetype(Class* klass) { return klass->enumBase; }
EXPORT Class* mono_class_from_mono_type(Type* type) { return type->klass; }
EXPORT int32_t mono_class_is_subclass_of(Class* klass, Class* parent, int32_t) { return IsSubclass(klass, parent); }
EXPORT int32_t mono_class_is_valuetype(Class* klass) { return klass->valueType; }
EXPORT int32_t mono_class_is_enum(Class* klass) { return klass->isEnum; }
EXPORT VTable* mono_class_vtable(Domain*, Class* klass) { return klass->vtable; }
EXPORT const char* mono_field_get_name(Field* field) { return field->name; }
EXPORT Type* mono_field_get_type(Field* field) { return field->type; }
EXPORT uint32_t mono_field_get_offset(Field* field) { return static_cast<uint32_t>(field->offset); }
EXPORT uint32_t mono_field_get_flags(Field* field) { return field->flags; }
EXPORT void mono_field_static_get_value(VTable*, Field* field, void* out) { StaticGet(field, out); }
EXPORT void mono_field_static_set_value(VTable*, Field* field, void* value) { StaticSet(field, value); }
EXPORT char* mono_type_get_name(Type* type) { return TypeName(type); }
EXPORT int mono_type_get_type(Type* type) { return type->kind; }
EXPORT void mono_free(void* memory) { free(memory); }
EXPORT const char* mono_method_get_name(Method* method) { return method->name; }
EXPORT uint32_t mono_method_get_flags(Method* method, uint32_t* implementation) {
    if (implementation)
        *implementation = 0;
    return method->flags;
}
// The method stands for its signature.
EXPORT Method* mono_method_signature(Method* method) { return method; }
EXPORT Type* mono_signature_get_return_type(Method* signature) { return signature->returnType; }
EXPORT uint32_t mono_signature_get_param_count(Method* signature) { return static_cast<uint32_t>(signature->params.size()); }
EXPORT Type* mono_signature_get_params(Method* signature, void** iterator) { return Next(signature->params, iterator); }
EXPORT void mono_method_get_param_names(Method* method, const char** names) {
    for (size_t i = 0; i < method->names.size(); ++i)
        names[i] = method->names[i];
}
EXPORT Class* mono_object_get_class(void* object) { return ClassOf(object); }
EXPORT void* mono_runtime_invoke(Method* method, void* object, void** params, void** exception) { return Invoke(method, object, params, exception); }
EXPORT void* mono_string_new(Domain*, const char* text) { return NewString(text); }
EXPORT void mono_gc_wbarrier_set_field(void*, void* target, void* value) { memcpy(target, &value, sizeof(value)); }

#endif
