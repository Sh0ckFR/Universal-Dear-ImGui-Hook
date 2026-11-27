// Objects of the game's engine for the assistant, with Config::remoteMemory:
// Unity games, whose scripts run on Mono or IL2CPP, through the C API that
// these runtimes export, resolved at run time; the library links neither.
//   engine info                              -> ok <engine> <runtime> <version> <module>, or ok none
//   engine assemblies                        -> ok <bytes>, then lines: <assembly> <classes>
//   engine classes <filter|*> [assembly]     -> ok <bytes>, then lines: <assembly> <class>, at most 1000
//   engine class <class>                     -> ok <bytes>, then the class, its fields and its methods
//   engine get <class> <field> [object]      -> ok <bytes>, then: <type> <value>
//   engine set <class> <field> <object|-> <value> -> ok <bytes>, then: <type> <value>, as read back
//   engine instances <class> [count]         -> ok <bytes>, then lines: <address>
//   engine dump [game|all|packages]          -> ok <bytes>, then the path of the C++ SDK, what it holds
// and with Config::aiAdvancedFeatures:
//   engine call <class> <method> <object|-> [JSON array] -> ok <bytes>, then: <type> <value>
// Classes are Namespace.Name, Outer/Inner for a nested class, optionally
// after <assembly>:. Fields of structs are reached with a dot: position.x.
// A name with spaces, such as a blueprint's on Unreal, goes between double
// quotes. On Unreal (src/unreal.cpp), `engine get <class> * <object>` reads
// every property of the object, a line each; `engine dump` is src/sdk.cpp.
//
// The commands run on the server thread, attached to the runtime for their
// duration. Instances are found by a scan of the writable memory for the
// header of the class's objects (its vtable on Mono, its class on IL2CPP),
// each candidate checked against the layout of the class: it runs no code of
// the game, from any thread, where FindObjectsOfType() needs Unity's main
// thread. Calls run on the game's main thread: the thread of its window on
// Windows (src/windows/input.cpp), the thread that reads its X events on Linux
// (src/linux/events.cpp).
#include "internal.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cinttypes>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iterator>

namespace uih::detail::remote {
namespace {

constexpr size_t kMaxListed = 1000;
constexpr size_t kMaxInstances = 1000;
constexpr size_t kMaxString = 1024;
constexpr size_t kChunk = 1 << 20;
constexpr int kMaxStructDepth = 3;
constexpr auto kCallTimeout = std::chrono::seconds(10);
constexpr size_t kHeader = 2 * sizeof(void*);

// MonoTypeEnum and Il2CppTypeEnum, which share their values.
enum TypeKind {
    kVoid = 0x01,
    kBoolean = 0x02,
    kChar = 0x03,
    kI1 = 0x04,
    kU1 = 0x05,
    kI2 = 0x06,
    kU2 = 0x07,
    kI4 = 0x08,
    kU4 = 0x09,
    kI8 = 0x0a,
    kU8 = 0x0b,
    kR4 = 0x0c,
    kR8 = 0x0d,
    kString = 0x0e,
    kPtr = 0x0f,
    kValueType = 0x11,
    kClass = 0x12,
    kArray = 0x14,
    kGenericInst = 0x15,
    kI = 0x18,
    kU = 0x19,
    kFnPtr = 0x1b,
    kObject = 0x1c,
    kSzArray = 0x1d,
};

constexpr uint32_t kFieldStatic = 0x10;
constexpr uint32_t kFieldLiteral = 0x40;
constexpr uint32_t kMethodStatic = 0x10;
constexpr uint32_t kMethodAbstract = 0x400;
constexpr uint32_t kTypeDefTable = 2;
constexpr uint32_t kTypeDefToken = 0x02000000;

using Handle = void*;

// The exported functions of Mono or IL2CPP. Their few differences of
// signature have members of their own.
struct Runtime {
    bool il2cpp = false;
    std::string module;
    Handle domain = nullptr;
    // Boehm's garbage collector, Unity's, allocates objects in granules of
    // two words; SGen aligns them to 8 bytes.
    size_t objectAlign = 2 * sizeof(void*);

    Handle (*getDomain)() = nullptr;
    Handle (*threadAttach)(Handle) = nullptr;
    void (*threadDetach)(Handle) = nullptr;
    // mono_domain_get(), il2cpp_thread_current(): null on a thread not attached.
    Handle (*attachedMarker)() = nullptr;
    Handle (*assemblyImage)(Handle) = nullptr;
    const char* (*imageName)(Handle) = nullptr;
    const char* (*className)(Handle) = nullptr;
    const char* (*classNamespace)(Handle) = nullptr;
    Handle (*classParent)(Handle) = nullptr;
    Handle (*classDeclaring)(Handle) = nullptr;
    Handle (*classFields)(Handle, void**) = nullptr;
    Handle (*classMethods)(Handle, void**) = nullptr;
    Handle (*classEnumBase)(Handle) = nullptr;
    Handle (*classFromType)(Handle) = nullptr;
    const char* (*fieldName)(Handle) = nullptr;
    Handle (*fieldType)(Handle) = nullptr;
    char* (*typeName)(Handle) = nullptr;
    int (*typeKind)(Handle) = nullptr;
    void (*freeMemory)(void*) = nullptr;
    const char* (*methodName)(Handle) = nullptr;
    uint32_t (*methodFlags)(Handle, uint32_t*) = nullptr;
    Handle (*objectClass)(Handle) = nullptr;
    Handle (*invoke)(Handle, Handle, void**, Handle*) = nullptr;
    // Optional: the write barrier of the incremental garbage collector.
    void (*writeBarrier)(Handle, void*, Handle) = nullptr;
    // Optional: the override of a virtual method for an object.
    Handle (*virtualMethod)(Handle, Handle) = nullptr;

    // Mono
    void (*monoAssemblyForeach)(void (*)(Handle, void*), void*) = nullptr;
    int (*monoTableRows)(Handle, int) = nullptr;
    Handle (*monoClassGet)(Handle, uint32_t) = nullptr;
    Handle (*monoClassVTable)(Handle, Handle) = nullptr;
    Handle (*monoMethodSignature)(Handle) = nullptr;
    Handle (*monoSignatureReturn)(Handle) = nullptr;
    uint32_t (*monoSignatureParamCount)(Handle) = nullptr;
    Handle (*monoSignatureParams)(Handle, void**) = nullptr;
    void (*monoMethodParamNames)(Handle, const char**) = nullptr;
    void (*monoStaticGet)(Handle, Handle, void*) = nullptr;
    void (*monoStaticSet)(Handle, Handle, void*) = nullptr;
    Handle (*monoStringNew)(Handle, const char*) = nullptr;
    int32_t (*monoIsSubclassOf)(Handle, Handle, int32_t) = nullptr;
    int32_t (*monoIsValueType)(Handle) = nullptr;
    int32_t (*monoIsEnum)(Handle) = nullptr;
    uint32_t (*monoFieldOffset)(Handle) = nullptr;
    uint32_t (*monoFieldFlags)(Handle) = nullptr;

    // IL2CPP
    Handle* (*il2cppAssemblies)(Handle, size_t*) = nullptr;
    size_t (*il2cppClassCount)(Handle) = nullptr;
    Handle (*il2cppClassAt)(Handle, size_t) = nullptr;
    Handle (*il2cppMethodReturn)(Handle) = nullptr;
    uint32_t (*il2cppMethodParamCount)(Handle) = nullptr;
    Handle (*il2cppMethodParam)(Handle, uint32_t) = nullptr;
    const char* (*il2cppMethodParamName)(Handle, uint32_t) = nullptr;
    void (*il2cppStaticGet)(Handle, void*) = nullptr;
    void (*il2cppStaticSet)(Handle, void*) = nullptr;
    Handle (*il2cppStringNew)(const char*) = nullptr;
    bool (*il2cppIsSubclassOf)(Handle, Handle, bool) = nullptr;
    bool (*il2cppIsValueType)(Handle) = nullptr;
    bool (*il2cppIsEnum)(Handle) = nullptr;
    size_t (*il2cppFieldOffset)(Handle) = nullptr;
    int (*il2cppFieldFlags)(Handle) = nullptr;

    // What the commands found, built again when assemblies load.
    struct ClassEntry {
        Handle klass;
        std::string assembly;
        std::string name;
    };
    std::vector<std::pair<std::string, size_t>> assemblies;
    std::vector<ClassEntry> classes;
    std::vector<Handle> images;
    size_t indexedAssemblies = SIZE_MAX;
    std::string version;
    bool versionKnown = false;

    bool IsSubclassOf(Handle klass, Handle parent) const {
        if (klass == parent)
            return true;
        return il2cpp ? il2cppIsSubclassOf(klass, parent, false) : monoIsSubclassOf(klass, parent, 0) != 0;
    }
    bool IsValueType(Handle klass) const { return il2cpp ? il2cppIsValueType(klass) : monoIsValueType(klass) != 0; }
    bool IsEnum(Handle klass) const { return il2cpp ? il2cppIsEnum(klass) : monoIsEnum(klass) != 0; }
    size_t FieldOffset(Handle field) const { return il2cpp ? il2cppFieldOffset(field) : monoFieldOffset(field); }
    uint32_t FieldFlags(Handle field) const { return il2cpp ? static_cast<uint32_t>(il2cppFieldFlags(field)) : monoFieldFlags(field); }
    std::string TypeName(Handle type) const {
        char* name = typeName(type);
        std::string text = name ? name : "?";
        if (name)
            freeMemory(name);
        return text;
    }
    Handle ReturnType(Handle method) const {
        return il2cpp ? il2cppMethodReturn(method) : monoSignatureReturn(monoMethodSignature(method));
    }
    // The types and names of the parameters of `method`.
    std::vector<std::pair<Handle, std::string>> Parameters(Handle method) const {
        std::vector<std::pair<Handle, std::string>> parameters;
        if (il2cpp) {
            uint32_t count = il2cppMethodParamCount(method);
            for (uint32_t i = 0; i < count; ++i) {
                const char* name = il2cppMethodParamName ? il2cppMethodParamName(method, i) : nullptr;
                parameters.emplace_back(il2cppMethodParam(method, i), name ? name : "");
            }
            return parameters;
        }
        Handle signature = monoMethodSignature(method);
        if (!signature)
            return parameters;
        uint32_t count = monoSignatureParamCount(signature);
        std::vector<const char*> names(count + 1, nullptr);
        if (monoMethodParamNames && count)
            monoMethodParamNames(method, names.data());
        void* iterator = nullptr;
        for (uint32_t i = 0; i < count; ++i) {
            Handle type = monoSignatureParams(signature, &iterator);
            parameters.emplace_back(type, names[i] ? names[i] : "");
        }
        return parameters;
    }
    // The header of the objects of `klass`: its vtable on Mono, itself on IL2CPP.
    Handle Header(Handle klass) const { return il2cpp ? klass : monoClassVTable(domain, klass); }
    void StaticGet(Handle klass, Handle field, void* out) const {
        if (il2cpp) {
            il2cppStaticGet(field, out);
            return;
        }
        if (Handle vtable = monoClassVTable(domain, klass))
            monoStaticGet(vtable, field, out);
    }
    void StaticSet(Handle klass, Handle field, void* value) const {
        if (il2cpp) {
            il2cppStaticSet(field, value);
            return;
        }
        if (Handle vtable = monoClassVTable(domain, klass))
            monoStaticSet(vtable, field, value);
    }
    Handle NewString(const char* text) const { return il2cpp ? il2cppStringNew(text) : monoStringNew(domain, text); }
};

std::mutex g_engineMutex;
Runtime* g_runtime = nullptr;

// Attaches the calling thread to the runtime, unless it already is, and
// detaches it at the end of the scope: the game's own threads stay attached.
class Attached {
public:
    explicit Attached(const Runtime& runtime) : runtime_(runtime) {
        if (!runtime.attachedMarker())
            thread_ = runtime.threadAttach(runtime.domain);
    }
    ~Attached() {
        if (thread_)
            runtime_.threadDetach(thread_);
    }
    Attached(const Attached&) = delete;
    Attached& operator=(const Attached&) = delete;

private:
    const Runtime& runtime_;
    Handle thread_ = nullptr;
};

template <typename Fn>
bool Resolve(void* module, Fn& function, const char* name) {
    function = reinterpret_cast<Fn>(ModuleSymbol(module, name));
    return function != nullptr;
}

bool ResolveMono(void* m, Runtime& r) {
    Resolve(m, r.writeBarrier, "mono_gc_wbarrier_set_field");
    Resolve(m, r.virtualMethod, "mono_object_get_virtual_method");
    Resolve(m, r.monoMethodParamNames, "mono_method_get_param_names");
    return Resolve(m, r.getDomain, "mono_get_root_domain") && Resolve(m, r.threadAttach, "mono_thread_attach") &&
           Resolve(m, r.threadDetach, "mono_thread_detach") && Resolve(m, r.attachedMarker, "mono_domain_get") &&
           Resolve(m, r.monoAssemblyForeach, "mono_assembly_foreach") && Resolve(m, r.assemblyImage, "mono_assembly_get_image") &&
           Resolve(m, r.imageName, "mono_image_get_name") && Resolve(m, r.monoTableRows, "mono_image_get_table_rows") &&
           Resolve(m, r.monoClassGet, "mono_class_get") && Resolve(m, r.className, "mono_class_get_name") &&
           Resolve(m, r.classNamespace, "mono_class_get_namespace") && Resolve(m, r.classParent, "mono_class_get_parent") &&
           Resolve(m, r.classDeclaring, "mono_class_get_nesting_type") && Resolve(m, r.classFields, "mono_class_get_fields") &&
           Resolve(m, r.classMethods, "mono_class_get_methods") && Resolve(m, r.classEnumBase, "mono_class_enum_basetype") &&
           Resolve(m, r.classFromType, "mono_class_from_mono_type") && Resolve(m, r.monoIsSubclassOf, "mono_class_is_subclass_of") &&
           Resolve(m, r.monoIsValueType, "mono_class_is_valuetype") && Resolve(m, r.monoIsEnum, "mono_class_is_enum") &&
           Resolve(m, r.monoClassVTable, "mono_class_vtable") && Resolve(m, r.fieldName, "mono_field_get_name") &&
           Resolve(m, r.fieldType, "mono_field_get_type") && Resolve(m, r.monoFieldOffset, "mono_field_get_offset") &&
           Resolve(m, r.monoFieldFlags, "mono_field_get_flags") && Resolve(m, r.monoStaticGet, "mono_field_static_get_value") &&
           Resolve(m, r.monoStaticSet, "mono_field_static_set_value") && Resolve(m, r.typeName, "mono_type_get_name") &&
           Resolve(m, r.typeKind, "mono_type_get_type") && Resolve(m, r.freeMemory, "mono_free") &&
           Resolve(m, r.methodName, "mono_method_get_name") && Resolve(m, r.methodFlags, "mono_method_get_flags") &&
           Resolve(m, r.monoMethodSignature, "mono_method_signature") && Resolve(m, r.monoSignatureReturn, "mono_signature_get_return_type") &&
           Resolve(m, r.monoSignatureParamCount, "mono_signature_get_param_count") &&
           Resolve(m, r.monoSignatureParams, "mono_signature_get_params") && Resolve(m, r.objectClass, "mono_object_get_class") &&
           Resolve(m, r.invoke, "mono_runtime_invoke") && Resolve(m, r.monoStringNew, "mono_string_new");
}

bool ResolveIl2cpp(void* m, Runtime& r) {
    r.il2cpp = true;
    Resolve(m, r.writeBarrier, "il2cpp_gc_wbarrier_set_field");
    Resolve(m, r.virtualMethod, "il2cpp_object_get_virtual_method");
    Resolve(m, r.il2cppMethodParamName, "il2cpp_method_get_param_name");
    return Resolve(m, r.getDomain, "il2cpp_domain_get") && Resolve(m, r.threadAttach, "il2cpp_thread_attach") &&
           Resolve(m, r.threadDetach, "il2cpp_thread_detach") && Resolve(m, r.attachedMarker, "il2cpp_thread_current") &&
           Resolve(m, r.il2cppAssemblies, "il2cpp_domain_get_assemblies") && Resolve(m, r.assemblyImage, "il2cpp_assembly_get_image") &&
           Resolve(m, r.imageName, "il2cpp_image_get_name") && Resolve(m, r.il2cppClassCount, "il2cpp_image_get_class_count") &&
           Resolve(m, r.il2cppClassAt, "il2cpp_image_get_class") && Resolve(m, r.className, "il2cpp_class_get_name") &&
           Resolve(m, r.classNamespace, "il2cpp_class_get_namespace") && Resolve(m, r.classParent, "il2cpp_class_get_parent") &&
           Resolve(m, r.classDeclaring, "il2cpp_class_get_declaring_type") && Resolve(m, r.classFields, "il2cpp_class_get_fields") &&
           Resolve(m, r.classMethods, "il2cpp_class_get_methods") && Resolve(m, r.classEnumBase, "il2cpp_class_enum_basetype") &&
           Resolve(m, r.classFromType, "il2cpp_class_from_type") && Resolve(m, r.il2cppIsSubclassOf, "il2cpp_class_is_subclass_of") &&
           Resolve(m, r.il2cppIsValueType, "il2cpp_class_is_valuetype") && Resolve(m, r.il2cppIsEnum, "il2cpp_class_is_enum") &&
           Resolve(m, r.fieldName, "il2cpp_field_get_name") && Resolve(m, r.fieldType, "il2cpp_field_get_type") &&
           Resolve(m, r.il2cppFieldOffset, "il2cpp_field_get_offset") && Resolve(m, r.il2cppFieldFlags, "il2cpp_field_get_flags") &&
           Resolve(m, r.il2cppStaticGet, "il2cpp_field_static_get_value") && Resolve(m, r.il2cppStaticSet, "il2cpp_field_static_set_value") &&
           Resolve(m, r.typeName, "il2cpp_type_get_name") && Resolve(m, r.typeKind, "il2cpp_type_get_type") &&
           Resolve(m, r.freeMemory, "il2cpp_free") && Resolve(m, r.methodName, "il2cpp_method_get_name") &&
           Resolve(m, r.methodFlags, "il2cpp_method_get_flags") && Resolve(m, r.il2cppMethodReturn, "il2cpp_method_get_return_type") &&
           Resolve(m, r.il2cppMethodParamCount, "il2cpp_method_get_param_count") && Resolve(m, r.il2cppMethodParam, "il2cpp_method_get_param") &&
           Resolve(m, r.objectClass, "il2cpp_object_get_class") && Resolve(m, r.invoke, "il2cpp_runtime_invoke") &&
           Resolve(m, r.il2cppStringNew, "il2cpp_string_new");
}

// The runtimes of Unity, by the file name of their library, and the Mono of
// Linux distributions, which embedding applications load.
const struct {
    const char* module;
    bool il2cpp;
} kUnityRuntimes[] = {
    {"GameAssembly.dll", true},       {"GameAssembly.so", true},     {"mono-2.0-bdwgc.dll", false},
    {"mono-2.0-sgen.dll", false},     {"mono.dll", false},           {"libmonobdwgc-2.0.so", false},
    {"libmonosgen-2.0.so", false},    {"libmonosgen-2.0.so.1", false}, {"libmono.so", false},
};

// The runtime of the game, found once it is loaded and its domain exists;
// null otherwise, with the reason in `error`. Holds g_engineMutex.
Runtime* FindRuntime(std::string& error) {
    if (g_runtime)
        return g_runtime;
    for (const auto& candidate : kUnityRuntimes) {
        void* module = FindModule(candidate.module);
        if (!module)
            continue;
        auto* runtime = new Runtime();
        runtime->module = candidate.module;
        if (strstr(candidate.module, "sgen"))
            runtime->objectAlign = 8;
        if (!(candidate.il2cpp ? ResolveIl2cpp(module, *runtime) : ResolveMono(module, *runtime))) {
            delete runtime;
            error = std::string("error ") + candidate.module + " does not export the functions of its runtime: the engine is not supported";
            return nullptr;
        }
        runtime->domain = runtime->getDomain();
        if (!runtime->domain) {
            delete runtime;
            error = std::string("error the runtime of ") + candidate.module + " is not started yet: try again once the game shows its first scene";
            return nullptr;
        }
        // Never freed: the commands of other threads may hold it.
        g_runtime = runtime;
        return runtime;
    }
    error = "ok none";
    return nullptr;
}

// Text --------------------------------------------------------------------

std::string Hex(uintptr_t value) {
    char text[24];
    snprintf(text, sizeof(text), "0x%" PRIxPTR, value);
    return text;
}

std::string Lower(std::string text) {
    for (char& c : text)
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return text;
}

std::string StripDll(std::string name) {
    if (name.size() > 4 && Lower(name.substr(name.size() - 4)) == ".dll")
        name.resize(name.size() - 4);
    return name;
}

// The next word of `text` from `at`, which moves past it and the spaces after.
std::string NextWord(const std::string& text, size_t& at) {
    while (at < text.size() && text[at] == ' ')
        ++at;
    size_t start = at;
    while (at < text.size() && text[at] != ' ')
        ++at;
    std::string word = text.substr(start, at - start);
    while (at < text.size() && text[at] == ' ')
        ++at;
    return word;
}

std::string Payload(const std::string& text, std::vector<uint8_t>& payload) {
    payload.assign(text.begin(), text.end());
    return "ok " + std::to_string(payload.size());
}

void AppendUtf8(std::string& out, uint32_t point) {
    if (point < 0x80) {
        out += static_cast<char>(point);
    } else if (point < 0x800) {
        out += static_cast<char>(0xc0 | point >> 6);
        out += static_cast<char>(0x80 | (point & 0x3f));
    } else if (point < 0x10000) {
        out += static_cast<char>(0xe0 | point >> 12);
        out += static_cast<char>(0x80 | (point >> 6 & 0x3f));
        out += static_cast<char>(0x80 | (point & 0x3f));
    } else {
        out += static_cast<char>(0xf0 | point >> 18);
        out += static_cast<char>(0x80 | (point >> 12 & 0x3f));
        out += static_cast<char>(0x80 | (point >> 6 & 0x3f));
        out += static_cast<char>(0x80 | (point & 0x3f));
    }
}

template <typename T>
bool ReadValue(uintptr_t address, T& value) {
    return ReadMemory(address, &value, sizeof(value));
}

// Classes -------------------------------------------------------------------

std::string FullName(const Runtime& runtime, Handle klass) {
    std::string name = runtime.className(klass) ? runtime.className(klass) : "?";
    if (Handle outer = runtime.classDeclaring(klass))
        return FullName(runtime, outer) + "/" + name;
    const char* space = runtime.classNamespace(klass);
    return space && *space ? std::string(space) + "." + name : name;
}

std::vector<Handle> Assemblies(const Runtime& runtime) {
    std::vector<Handle> assemblies;
    if (runtime.il2cpp) {
        size_t count = 0;
        Handle* list = runtime.il2cppAssemblies(runtime.domain, &count);
        for (size_t i = 0; list && i < count; ++i)
            assemblies.push_back(list[i]);
    } else {
        runtime.monoAssemblyForeach([](Handle assembly, void* context) { static_cast<std::vector<Handle>*>(context)->push_back(assembly); },
                                    &assemblies);
    }
    return assemblies;
}

// Lists the classes of every assembly, again when another assembly loaded.
void Index(Runtime& runtime) {
    std::vector<Handle> assemblies = Assemblies(runtime);
    if (assemblies.size() == runtime.indexedAssemblies)
        return;
    runtime.assemblies.clear();
    runtime.classes.clear();
    runtime.images.clear();
    for (Handle assembly : assemblies) {
        Handle image = runtime.assemblyImage(assembly);
        if (!image)
            continue;
        runtime.images.push_back(image);
        std::string name = StripDll(runtime.imageName(image) ? runtime.imageName(image) : "?");
        size_t before = runtime.classes.size();
        if (runtime.il2cpp) {
            size_t count = runtime.il2cppClassCount(image);
            for (size_t i = 0; i < count; ++i)
                if (Handle klass = runtime.il2cppClassAt(image, i))
                    runtime.classes.push_back({klass, name, FullName(runtime, klass)});
        } else {
            int rows = runtime.monoTableRows(image, kTypeDefTable);
            // The first row is <Module>.
            for (int row = 2; row <= rows; ++row)
                if (Handle klass = runtime.monoClassGet(image, kTypeDefToken | static_cast<uint32_t>(row)))
                    runtime.classes.push_back({klass, name, FullName(runtime, klass)});
        }
        runtime.assemblies.emplace_back(name, runtime.classes.size() - before);
    }
    std::sort(runtime.images.begin(), runtime.images.end());
    runtime.indexedAssemblies = assemblies.size();
}

// The class named `text`: <assembly>:Namespace.Name, Namespace.Name, or Name
// when no other class has it. Null with the reason in `error`.
Handle FindClass(const Runtime& runtime, const std::string& text, std::string& error) {
    std::string assembly;
    std::string name = text;
    size_t colon = text.find(':');
    if (colon != std::string::npos) {
        assembly = Lower(StripDll(text.substr(0, colon)));
        name = text.substr(colon + 1);
    }
    if (name.empty()) {
        error = "error give a class, such as Namespace.Name";
        return nullptr;
    }
    std::vector<const Runtime::ClassEntry*> exact;
    std::vector<const Runtime::ClassEntry*> loose;
    std::string lower = Lower(name);
    for (const Runtime::ClassEntry& entry : runtime.classes) {
        if (!assembly.empty() && Lower(entry.assembly) != assembly)
            continue;
        if (entry.name == name) {
            exact.push_back(&entry);
            continue;
        }
        std::string full = Lower(entry.name);
        size_t cut = full.find_last_of("./");
        if (full == lower || (cut != std::string::npos && full.compare(cut + 1, std::string::npos, lower) == 0))
            loose.push_back(&entry);
    }
    const std::vector<const Runtime::ClassEntry*>& found = exact.empty() ? loose : exact;
    if (found.size() == 1)
        return found[0]->klass;
    if (found.empty()) {
        error = "error no class named " + text + ": find it with engine classes";
        return nullptr;
    }
    error = "error several classes are named " + text + ":";
    for (size_t i = 0; i < found.size() && i < 10; ++i)
        error += " " + found[i]->assembly + ":" + found[i]->name;
    return nullptr;
}

struct Field {
    Handle field = nullptr;
    Handle type = nullptr;
    // The class that declares it.
    Handle owner = nullptr;
    size_t offset = 0;
    uint32_t flags = 0;
};

std::vector<Field> OwnFields(const Runtime& runtime, Handle klass) {
    std::vector<Field> fields;
    void* iterator = nullptr;
    while (Handle field = runtime.classFields(klass, &iterator))
        fields.push_back({field, runtime.fieldType(field), klass, runtime.FieldOffset(field), runtime.FieldFlags(field)});
    return fields;
}

// The class, then its parents.
std::vector<Handle> Lineage(const Runtime& runtime, Handle klass) {
    std::vector<Handle> lineage;
    for (Handle at = klass; at && lineage.size() < 64; at = runtime.classParent(at))
        lineage.push_back(at);
    return lineage;
}

bool FindField(const Runtime& runtime, Handle klass, const std::string& name, Field& out) {
    for (Handle at : Lineage(runtime, klass)) {
        for (const Field& field : OwnFields(runtime, at)) {
            if (name == runtime.fieldName(field.field)) {
                out = field;
                return true;
            }
        }
    }
    return false;
}

// The class of a value type, of an enum or a struct; null for other types.
Handle ValueClass(const Runtime& runtime, Handle type) {
    int kind = runtime.typeKind(type);
    if (kind != kValueType && kind != kGenericInst)
        return nullptr;
    Handle klass = runtime.classFromType(type);
    return klass && runtime.IsValueType(klass) ? klass : nullptr;
}

bool IsReference(const Runtime& runtime, Handle type) {
    int kind = runtime.typeKind(type);
    if (kind == kString || kind == kClass || kind == kObject || kind == kSzArray || kind == kArray)
        return true;
    return kind == kGenericInst && !ValueClass(runtime, type);
}

// The type with enums turned into their underlying type.
Handle Underlying(const Runtime& runtime, Handle type) {
    for (int i = 0; i < 4; ++i) {
        Handle klass = ValueClass(runtime, type);
        if (!klass || !runtime.IsEnum(klass))
            break;
        Handle base = runtime.classEnumBase(klass);
        if (!base)
            break;
        type = base;
    }
    return type;
}

// Bytes of a primitive or pointer type, 0 for the others.
size_t PrimitiveSize(int kind) {
    switch (kind) {
    case kBoolean:
    case kI1:
    case kU1:
        return 1;
    case kChar:
    case kI2:
    case kU2:
        return 2;
    case kI4:
    case kU4:
    case kR4:
        return 4;
    case kI8:
    case kU8:
    case kR8:
        return 8;
    case kI:
    case kU:
    case kPtr:
    case kFnPtr:
        return sizeof(void*);
    default:
        return 0;
    }
}

// Objects -------------------------------------------------------------------

// The class of the object at `address`, checked through its header without a
// call to the runtime; null when it does not look like an object.
Handle ObjectClass(const Runtime& runtime, uintptr_t address) {
    uintptr_t header = 0;
    if (!address || address % sizeof(void*) || !ReadValue(address, header) || !header || header % sizeof(void*))
        return nullptr;
    if (runtime.il2cpp) {
        // Il2CppClass starts with its image.
        uintptr_t image = 0;
        if (!ReadValue(header, image) || !std::binary_search(runtime.images.begin(), runtime.images.end(), reinterpret_cast<Handle>(image)))
            return nullptr;
        // Its monitor is null, or a pointer to the lock.
        uintptr_t monitor = 0;
        if (!ReadValue(address + sizeof(void*), monitor) || (monitor && (monitor % sizeof(void*) || !ReadValue(monitor, image))))
            return nullptr;
        return reinterpret_cast<Handle>(header);
    }
    // MonoVTable starts with its class, then its GC descriptor and its domain.
    uintptr_t vtable[3] = {};
    if (!ReadMemory(header, vtable, sizeof(vtable)) || !vtable[0] || reinterpret_cast<Handle>(vtable[2]) != runtime.domain)
        return nullptr;
    return reinterpret_cast<Handle>(vtable[0]);
}

std::string ReadString(uintptr_t address) {
    int32_t length = 0;
    if (!ReadValue(address + kHeader, length) || length < 0)
        return "?";
    size_t count = std::min<size_t>(static_cast<size_t>(length), kMaxString);
    std::vector<uint16_t> chars(count);
    if (count && !ReadMemory(address + kHeader + 4, chars.data(), count * 2))
        return "?";
    std::string text;
    for (size_t i = 0; i < count; ++i) {
        uint32_t point = chars[i];
        if (point >= 0xd800 && point < 0xdc00 && i + 1 < count && chars[i + 1] >= 0xdc00 && chars[i + 1] < 0xe000)
            point = 0x10000 + ((point - 0xd800) << 10) + (chars[++i] - 0xdc00);
        AppendUtf8(text, point);
    }
    std::string quoted = JsonQuote(text);
    if (static_cast<size_t>(length) > count)
        quoted += "... (" + std::to_string(length) + " characters)";
    return quoted;
}

std::string FormatReference(const Runtime& runtime, Handle type, uintptr_t object) {
    if (!object)
        return "null";
    Handle klass = ObjectClass(runtime, object);
    if (!klass)
        return Hex(object) + " (not an object)";
    if (runtime.typeKind(type) == kString)
        return ReadString(object);
    std::string text = Hex(object) + " (" + FullName(runtime, klass);
    int kind = runtime.typeKind(type);
    if (kind == kSzArray || kind == kArray) {
        uintptr_t length = 0;
        if (ReadValue(object + 3 * sizeof(void*), length))
            text += ", length " + std::to_string(length);
    }
    return text + ")";
}

// The value of `type` stored at `address`.
std::string FormatAt(const Runtime& runtime, Handle type, uintptr_t address, int depth = 0) {
    type = Underlying(runtime, type);
    int kind = runtime.typeKind(type);
    if (IsReference(runtime, type)) {
        uintptr_t object = 0;
        return ReadValue(address, object) ? FormatReference(runtime, type, object) : "?";
    }
    if (Handle klass = ValueClass(runtime, type)) {
        if (depth >= kMaxStructDepth)
            return "{...}";
        std::string text = "{";
        for (const Field& field : OwnFields(runtime, klass)) {
            if (field.flags & kFieldStatic)
                continue;
            if (text.size() > 1)
                text += ", ";
            text += std::string(runtime.fieldName(field.field)) + "=" +
                    FormatAt(runtime, field.type, address + field.offset - kHeader, depth + 1);
        }
        return text + "}";
    }
    size_t size = PrimitiveSize(kind);
    if (!size)
        return "?";
    uint8_t bytes[8] = {};
    if (!ReadMemory(address, bytes, size))
        return "?";
    char text[64];
    switch (kind) {
    case kBoolean:
        return bytes[0] ? "true" : "false";
    case kChar: {
        uint16_t c = 0;
        memcpy(&c, bytes, 2);
        std::string character;
        if (c >= 0x20)
            AppendUtf8(character, c);
        snprintf(text, sizeof(text), "%u", c);
        return character.empty() ? text : std::string(text) + " " + JsonQuote(character);
    }
    case kR4: {
        float value = 0;
        memcpy(&value, bytes, 4);
        snprintf(text, sizeof(text), "%.9g", value);
        return text;
    }
    case kR8: {
        double value = 0;
        memcpy(&value, bytes, 8);
        snprintf(text, sizeof(text), "%.17g", value);
        return text;
    }
    case kI:
    case kU:
    case kPtr:
    case kFnPtr: {
        uintptr_t value = 0;
        memcpy(&value, bytes, sizeof(value));
        return Hex(value);
    }
    default:
        break;
    }
    bool isSigned = kind == kI1 || kind == kI2 || kind == kI4 || kind == kI8;
    uint64_t raw = 0;
    memcpy(&raw, bytes, size);
    if (isSigned && size < 8 && (raw >> (size * 8 - 1) & 1))
        raw |= ~uint64_t{0} << (size * 8);
    if (isSigned)
        snprintf(text, sizeof(text), "%" PRId64, static_cast<int64_t>(raw));
    else
        snprintf(text, sizeof(text), "%" PRIu64, raw);
    return text;
}

// A value to write, decoded for `type`: the bytes of a primitive, or the
// object of a reference, a string created when given as "text".
struct Encoded {
    uint8_t bytes[8] = {};
    size_t size = 0;
    bool reference = false;
    Handle object = nullptr;
};

bool Encode(const Runtime& runtime, Handle type, const std::string& text, bool quoted, Encoded& out, std::string& error) {
    type = Underlying(runtime, type);
    int kind = runtime.typeKind(type);
    std::string typeName = runtime.TypeName(type);
    if (IsReference(runtime, type)) {
        out.reference = true;
        out.size = sizeof(void*);
        if (text == "null")
            return true;
        if (kind == kString && quoted) {
            out.object = runtime.NewString(text.c_str());
            return out.object != nullptr || (error = "error the runtime did not create the string", false);
        }
        char* end = nullptr;
        uintptr_t address = static_cast<uintptr_t>(strtoull(text.c_str(), &end, 16));
        Handle klass = end != text.c_str() && !*end ? ObjectClass(runtime, address) : nullptr;
        Handle expected = runtime.classFromType(type);
        if (!klass || (expected && kind != kObject && kind != kGenericInst && !runtime.IsSubclassOf(klass, expected))) {
            error = "error give " + typeName + " as the address of an object of its class, or null" + (kind == kString ? std::string(", or a \"string\"") : "");
            return false;
        }
        out.object = reinterpret_cast<Handle>(address);
        return true;
    }
    if (ValueClass(runtime, type)) {
        error = "error " + typeName + " is a struct: set its fields one at a time, such as position.x";
        return false;
    }
    out.size = PrimitiveSize(kind);
    if (!out.size) {
        error = "error the type " + typeName + " cannot be written";
        return false;
    }
    char* end = nullptr;
    const char* start = text.c_str();
    if (kind == kBoolean) {
        std::string lower = Lower(text);
        if (lower != "true" && lower != "false" && lower != "1" && lower != "0") {
            error = "error give true or false";
            return false;
        }
        out.bytes[0] = lower == "true" || lower == "1";
        return true;
    }
    if (kind == kR4 || kind == kR8) {
        double value = strtod(start, &end);
        if (end == start || *end) {
            error = "error give a number for " + typeName;
            return false;
        }
        if (kind == kR4) {
            float single = static_cast<float>(value);
            memcpy(out.bytes, &single, 4);
        } else {
            memcpy(out.bytes, &value, 8);
        }
        return true;
    }
    if (kind == kChar && quoted) {
        // The first UTF-16 unit of the text.
        uint32_t point = text.empty() ? 0 : static_cast<unsigned char>(text[0]);
        if (point >= 0x80 && text.size() >= 2)
            point = (point & 0x1f) << 6 | (static_cast<unsigned char>(text[1]) & 0x3f);
        memcpy(out.bytes, &point, 2);
        return true;
    }
    bool isSigned = kind == kI1 || kind == kI2 || kind == kI4 || kind == kI8 || kind == kI;
    uint64_t raw = isSigned ? static_cast<uint64_t>(strtoll(start, &end, 0)) : strtoull(start, &end, 0);
    if (end == start || *end) {
        error = "error give an integer for " + typeName;
        return false;
    }
    memcpy(out.bytes, &raw, out.size);
    return true;
}

// Field paths ----------------------------------------------------------------

// Where `path` (a field, then the fields of structs after dots) of `klass` is,
// for an object at `object`, or a static field when `object` is 0.
struct Location {
    Field field;
    Handle klass = nullptr;
    // The type of the last field, and its address in the object.
    Handle type = nullptr;
    uintptr_t address = 0;
    bool isStatic = false;
};

bool Locate(const Runtime& runtime, Handle klass, const std::string& path, uintptr_t object, Location& out, std::string& error) {
    size_t dot = path.find('.');
    std::string first = path.substr(0, dot);
    if (!FindField(runtime, klass, first, out.field)) {
        error = "error " + FullName(runtime, klass) + " has no field " + first + ": see engine class";
        return false;
    }
    out.klass = out.field.owner;
    out.type = out.field.type;
    out.isStatic = (out.field.flags & kFieldStatic) != 0;
    if (out.field.flags & kFieldLiteral) {
        error = "error " + first + " is a constant, compiled into the code that uses it";
        return false;
    }
    if (out.isStatic) {
        if (dot != std::string::npos) {
            error = "error read the static struct " + first + " whole";
            return false;
        }
        return true;
    }
    if (!object) {
        error = "error " + first + " is a field of each object: give the address of one, from engine instances";
        return false;
    }
    Handle actual = ObjectClass(runtime, object);
    if (!actual || !runtime.IsSubclassOf(actual, klass)) {
        error = "error " + Hex(object) + " is not an object of " + FullName(runtime, klass);
        return false;
    }
    out.address = object + out.field.offset;
    while (dot != std::string::npos) {
        size_t start = dot + 1;
        dot = path.find('.', start);
        std::string name = path.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        Handle structure = ValueClass(runtime, out.type);
        Field inner;
        if (!structure || runtime.IsEnum(structure) || !FindField(runtime, structure, name, inner) || (inner.flags & kFieldStatic)) {
            error = "error " + runtime.TypeName(out.type) + " has no field " + name;
            return false;
        }
        out.address += inner.offset - kHeader;
        out.type = inner.type;
    }
    return true;
}

// The value of a static field, read through the runtime.
std::string FormatStatic(const Runtime& runtime, const Location& location) {
    alignas(16) uint8_t value[256] = {};
    Handle structure = ValueClass(runtime, Underlying(runtime, location.type));
    if (structure) {
        // The size of the struct is not exported: the end of its last field bounds it.
        size_t end = 0;
        for (const Field& field : OwnFields(runtime, structure))
            if (!(field.flags & kFieldStatic))
                end = std::max(end, field.offset - kHeader + 8);
        if (end > sizeof(value))
            return "{...}";
    }
    runtime.StaticGet(location.klass, location.field.field, value);
    // Formatted from a copy: kHeader is added back for the fields of structs.
    return FormatAt(runtime, location.type, reinterpret_cast<uintptr_t>(value));
}

// Commands ------------------------------------------------------------------

std::string Info(Runtime& runtime) {
    if (!runtime.versionKnown) {
        runtime.versionKnown = true;
        // UnityPlayer holds the player, the executable before Unity 2017.2;
        // the version is a string such as 2022.3.10f1 or 6000.0.23f1 in it.
        std::vector<std::string> names = {"UnityPlayer.dll", "UnityPlayer.so"};
        std::vector<LoadedModule> modules = LoadedModules();
        for (const LoadedModule& module : modules)
            if (module.main)
                names.push_back(module.name);
        names.push_back(runtime.module);
        for (const std::string& name : names) {
            for (const LoadedModule& module : modules) {
                if (Lower(module.name) != Lower(name) || !runtime.version.empty())
                    continue;
                ForEachReadable(module.base, module.size, 32, [&](uintptr_t, const uint8_t* bytes, size_t length) {
                    for (size_t i = 1; i + 10 < length && runtime.version.empty(); ++i) {
                        const char* c = reinterpret_cast<const char*>(bytes) + i;
                        auto digit = [&](size_t at) { return i + at < length && isdigit(static_cast<unsigned char>(c[at])); };
                        if (!((c[0] == '2' && c[1] == '0') || c[0] == '6') || !digit(0) || !digit(1) || !digit(2) || !digit(3) || c[4] != '.' ||
                            isalnum(static_cast<unsigned char>(c[-1])) || c[-1] == '.')
                            continue;
                        size_t at = 5;
                        size_t digits = 0;
                        while (digit(at) && digits < 3)
                            ++at, ++digits;
                        if (!digits || i + at >= length || c[at] != '.')
                            continue;
                        ++at;
                        digits = 0;
                        while (digit(at) && digits < 3)
                            ++at, ++digits;
                        if (!digits || i + at >= length || !strchr("abfpx", c[at]) || !c[at])
                            continue;
                        ++at;
                        digits = 0;
                        while (digit(at) && digits < 3)
                            ++at, ++digits;
                        if (!digits || i + at >= length || isalnum(static_cast<unsigned char>(c[at])))
                            continue;
                        runtime.version.assign(c, at);
                    }
                    return runtime.version.empty();
                });
            }
        }
    }
    return std::string("ok unity ") + (runtime.il2cpp ? "il2cpp " : "mono ") + (runtime.version.empty() ? "-" : runtime.version) + " " + runtime.module;
}

std::string ListAssemblies(const Runtime& runtime, std::vector<uint8_t>& payload) {
    std::string text;
    for (const auto& assembly : runtime.assemblies)
        text += assembly.first + " " + std::to_string(assembly.second) + "\n";
    return Payload(text, payload);
}

std::string ListClasses(const Runtime& runtime, const std::string& rest, std::vector<uint8_t>& payload) {
    size_t at = 0;
    std::string filter = Lower(NextWord(rest, at));
    std::string assembly = Lower(StripDll(NextWord(rest, at)));
    if (filter == "*")
        filter.clear();
    std::string text;
    size_t listed = 0;
    size_t more = 0;
    for (const Runtime::ClassEntry& entry : runtime.classes) {
        if ((!assembly.empty() && Lower(entry.assembly) != assembly) || (!filter.empty() && Lower(entry.name).find(filter) == std::string::npos))
            continue;
        if (listed++ < EngineListLimit())
            text += entry.assembly + " " + entry.name + "\n";
        else
            ++more;
    }
    if (more)
        text += "+" + std::to_string(more) + " more\n";
    return Payload(text, payload);
}

std::string DescribeField(const Runtime& runtime, const Field& field) {
    std::string where = field.flags & kFieldLiteral ? "const" : field.flags & kFieldStatic ? "static" : Hex(field.offset);
    return where + " " + runtime.TypeName(field.type) + " " + runtime.fieldName(field.field);
}

std::string Describe(const Runtime& runtime, const std::string& rest, std::vector<uint8_t>& payload) {
    std::string error;
    Handle klass = FindClass(runtime, rest, error);
    if (!klass)
        return error;
    std::string kind = "class";
    if (runtime.IsValueType(klass))
        kind = runtime.IsEnum(klass) ? "enum" : "struct";
    std::string text = kind + " " + FullName(runtime, klass);
    std::vector<Handle> lineage = Lineage(runtime, klass);
    for (size_t i = 1; i < lineage.size(); ++i)
        text += (i == 1 ? " : " : ", ") + FullName(runtime, lineage[i]);
    for (const Runtime::ClassEntry& entry : runtime.classes)
        if (entry.klass == klass)
            text += " (" + entry.assembly + ")";
    text += "\nfields:\n";
    for (size_t i = 0; i < lineage.size(); ++i) {
        // An enum's values, and System.Object's and System.ValueType's nothing.
        for (const Field& field : OwnFields(runtime, lineage[i]))
            text += "  " + DescribeField(runtime, field) + (i ? "  (" + FullName(runtime, lineage[i]) + ")" : "") + "\n";
    }
    text += "methods:\n";
    void* iterator = nullptr;
    while (Handle method = runtime.classMethods(klass, &iterator)) {
        uint32_t implementation = 0;
        uint32_t flags = runtime.methodFlags(method, &implementation);
        Handle returned = runtime.ReturnType(method);
        text += std::string("  ") + (flags & kMethodStatic ? "static " : "") + (flags & kMethodAbstract ? "abstract " : "") +
                (returned ? runtime.TypeName(returned) : "?") + " " + runtime.methodName(method) + "(";
        bool first = true;
        for (const auto& parameter : runtime.Parameters(method)) {
            text += (first ? "" : ", ") + runtime.TypeName(parameter.first) + (parameter.second.empty() ? "" : " " + parameter.second);
            first = false;
        }
        text += ")\n";
    }
    if (lineage.size() > 1)
        text += "methods of " + FullName(runtime, lineage[1]) + ": engine class " + FullName(runtime, lineage[1]) + "\n";
    return Payload(text, payload);
}

uintptr_t ParseObject(const std::string& text) {
    if (text.empty() || text == "-")
        return 0;
    return static_cast<uintptr_t>(strtoull(text.c_str(), nullptr, 16));
}

// Every field of the object and the static ones, its class's first: lines
// <offset|static> <type> <name> <value>, separated by tabs.
std::string AllFields(const Runtime& runtime, Handle klass, uintptr_t object, std::vector<uint8_t>& payload) {
    std::string text;
    for (Handle level : Lineage(runtime, klass)) {
        for (const Field& field : OwnFields(runtime, level)) {
            Location location;
            std::string error;
            if (field.flags & kFieldLiteral || !Locate(runtime, klass, runtime.fieldName(field.field), object, location, error))
                continue;
            if (!location.isStatic && !object)
                continue;
            std::string value = location.isStatic ? FormatStatic(runtime, location) : FormatAt(runtime, location.type, location.address);
            std::replace(value.begin(), value.end(), '\t', ' ');
            std::replace(value.begin(), value.end(), '\n', ' ');
            text += (location.isStatic ? std::string("static") : Hex(field.offset)) + "\t" + runtime.TypeName(location.type) + "\t" +
                    runtime.fieldName(field.field) + "\t" + value + "\n";
        }
    }
    return Payload(text, payload);
}

std::string Get(const Runtime& runtime, const std::string& rest, std::vector<uint8_t>& payload) {
    size_t at = 0;
    std::string className = NextWord(rest, at);
    std::string path = NextWord(rest, at);
    uintptr_t object = ParseObject(NextWord(rest, at));
    std::string error;
    Handle klass = FindClass(runtime, className, error);
    if (klass && path == "*") {
        if (object && !ObjectClass(runtime, object))
            return "error " + Hex(object) + " is not an object";
        return AllFields(runtime, klass, object, payload);
    }
    Location location;
    if (!klass || !Locate(runtime, klass, path, object, location, error))
        return error;
    std::string value = location.isStatic ? FormatStatic(runtime, location) : FormatAt(runtime, location.type, location.address);
    return Payload(runtime.TypeName(location.type) + " " + value, payload);
}

std::string Set(const Runtime& runtime, const std::string& rest, std::vector<uint8_t>& payload) {
    size_t at = 0;
    std::string className = NextWord(rest, at);
    std::string path = NextWord(rest, at);
    uintptr_t object = ParseObject(NextWord(rest, at));
    std::string text = rest.substr(std::min(at, rest.size()));
    while (!text.empty() && text.back() == ' ')
        text.pop_back();
    bool quoted = false;
    if (!text.empty() && text[0] == '"') {
        std::vector<std::string> values;
        if (!ParseJsonArray("[" + text + "]", values) || values.size() != 1)
            return "error give the string in JSON, such as \"text\"";
        text = values[0];
        quoted = true;
    }
    if (className.empty() || path.empty() || (text.empty() && !quoted))
        return "error give the class, the field, the object or - for a static field, and the value";
    std::string error;
    Handle klass = FindClass(runtime, className, error);
    Location location;
    Encoded value;
    if (!klass || !Locate(runtime, klass, path, object, location, error) || !Encode(runtime, location.type, text, quoted, value, error))
        return error;
    if (location.isStatic) {
        runtime.StaticSet(location.klass, location.field.field, value.reference ? value.object : static_cast<void*>(value.bytes));
        return Payload(runtime.TypeName(location.type) + " " + FormatStatic(runtime, location), payload);
    }
    if (value.reference && runtime.writeBarrier)
        runtime.writeBarrier(reinterpret_cast<Handle>(object), reinterpret_cast<void*>(location.address), value.object);
    else if (!WriteMemory(location.address, value.reference ? static_cast<const void*>(&value.object) : value.bytes, value.size))
        return "error the field could not be written";
    return Payload(runtime.TypeName(location.type) + " " + FormatAt(runtime, location.type, location.address), payload);
}

// What a candidate object of a class must hold, from the layout of the class.
struct Check {
    size_t offset;
    enum { Reference, Boolean, Alive } kind;
};

std::vector<Check> ChecksOf(const Runtime& runtime, Handle klass) {
    std::vector<Check> checks;
    for (Handle at : Lineage(runtime, klass)) {
        bool unityObject = FullName(runtime, at) == "UnityEngine.Object";
        for (const Field& field : OwnFields(runtime, at)) {
            if (field.flags & kFieldStatic)
                continue;
            // A destroyed UnityEngine.Object has no native object any more.
            if (unityObject && std::string(runtime.fieldName(field.field)) == "m_CachedPtr")
                checks.push_back({field.offset, Check::Alive});
            else if (IsReference(runtime, field.type))
                checks.push_back({field.offset, Check::Reference});
            else if (runtime.typeKind(Underlying(runtime, field.type)) == kBoolean)
                checks.push_back({field.offset, Check::Boolean});
        }
    }
    return checks;
}

bool Passes(const Runtime& runtime, uintptr_t candidate, const std::vector<Check>& checks) {
    for (const Check& check : checks) {
        uintptr_t value = 0;
        if (check.kind == Check::Boolean) {
            uint8_t flag = 0;
            if (!ReadValue(candidate + check.offset, flag) || flag > 1)
                return false;
        } else if (!ReadValue(candidate + check.offset, value)) {
            return false;
        } else if (check.kind == Check::Alive ? !value : value && !ObjectClass(runtime, value)) {
            return false;
        }
    }
    return true;
}

} // namespace

void ForEachReadable(uintptr_t base, size_t size, size_t overlap, const std::function<bool(uintptr_t, const uint8_t*, size_t)>& visit) {
    std::vector<uint8_t> chunk;
    for (const MemoryRegion& region : MemoryRegions()) {
        uintptr_t start = std::max(region.base, base);
        uintptr_t end = std::min(region.base + region.size, base + size);
        for (uintptr_t at = start; at < end;) {
            size_t length = static_cast<size_t>(std::min<uintptr_t>(kChunk + overlap, end - at));
            chunk.resize(length);
            if (ReadMemory(at, chunk.data(), length) && !visit(at, chunk.data(), length))
                return;
            if (at + length >= end)
                break;
            at += length - std::min(overlap, length / 2);
        }
    }
}

namespace {

std::string Instances(const Runtime& runtime, const std::string& rest, std::vector<uint8_t>& payload) {
    size_t at = 0;
    std::string className = NextWord(rest, at);
    std::string countText = NextWord(rest, at);
    size_t limit = countText.empty() ? 100 : std::clamp<size_t>(strtoul(countText.c_str(), nullptr, 10), 1, kMaxInstances);
    std::string error;
    Handle klass = FindClass(runtime, className, error);
    if (!klass)
        return error;
    if (runtime.IsValueType(klass))
        return "error " + className + " is a value type: its values live inside other objects, not on their own";
    auto header = reinterpret_cast<uintptr_t>(runtime.Header(klass));
    if (!header)
        return "error the runtime has no objects of " + className + ": an abstract or generic class has none of its own";
    std::vector<Check> checks = ChecksOf(runtime, klass);
    std::vector<AddressRange> modules = ModuleMemory();
    // Out of the heap, which it would find copies of the objects in.
    static uint8_t* chunk = static_cast<uint8_t*>(AllocateScanMemory(kChunk));
    if (!chunk)
        return "error no memory for the scan";
    auto own = reinterpret_cast<uintptr_t>(chunk);
    std::vector<uintptr_t> found;
    for (const MemoryRegion& region : MemoryRegions()) {
        if (!region.writable || (region.base <= own && own < region.base + region.size) ||
            (InRanges(modules, region.base) && InRanges(modules, region.base + region.size - 1)))
            continue;
        for (size_t offset = 0; offset < region.size && found.size() < limit; offset += kChunk) {
            if (StopRequested())
                return "error the remote control is stopping";
            size_t length = std::min(kChunk, region.size - offset);
            uintptr_t base = region.base + offset;
            if (!ReadMemory(base, chunk, length))
                continue;
            // Regions start on a page: the offsets keep the alignment of objects.
            for (size_t i = 0; i + sizeof(uintptr_t) <= length && found.size() < limit; i += runtime.objectAlign) {
                uintptr_t word = 0;
                memcpy(&word, chunk + i, sizeof(word));
                if (word == header && !InRanges(modules, base + i) && ObjectClass(runtime, base + i) == klass &&
                    Passes(runtime, base + i, checks))
                    found.push_back(base + i);
            }
        }
    }
    std::string text;
    for (uintptr_t object : found)
        text += Hex(object) + "\n";
    return Payload(text, payload);
}

// Calls on the game's main thread ---------------------------------------------

std::mutex g_callMutex;
std::condition_variable g_callDone;
std::atomic<bool> g_callPending{false};
uint64_t g_callId = 0;
bool g_callFinished = false;
std::function<std::string()> g_call;
std::string g_callResult;
thread_local bool t_inCall = false;

// Runs `work` on the game's main thread and returns its answer.
std::string OnGameThread(std::function<std::string()> work) {
    std::unique_lock<std::mutex> lock(g_callMutex);
    uint64_t id = ++g_callId;
    g_call = std::move(work);
    g_callFinished = false;
    g_callResult.clear();
    g_callPending = true;
    lock.unlock();
    bool woken = WakeGameThread();
    lock.lock();
    auto deadline = std::chrono::steady_clock::now() + kCallTimeout;
    while (woken && !g_callFinished && !StopRequested() && std::chrono::steady_clock::now() < deadline)
        g_callDone.wait_for(lock, std::chrono::milliseconds(50));
    bool finished = g_callFinished && g_callId == id;
    g_callPending = false;
    g_call = nullptr;
    if (finished)
        return g_callResult;
    if (!woken)
        return "error the game has no window yet, whose thread runs the call";
    return StopRequested() ? "error the remote control is stopping"
                           : "error the game's main thread did not run the call within 10 seconds: it may be loading, or the call is still running";
}

std::string FormatException(const Runtime& runtime, Handle exception) {
    auto address = reinterpret_cast<uintptr_t>(exception);
    Handle klass = ObjectClass(runtime, address);
    if (!klass)
        return "error the method threw an exception";
    std::string text = "error the method threw " + FullName(runtime, klass);
    Field message;
    if (FindField(runtime, klass, "_message", message)) {
        uintptr_t string = 0;
        if (ReadValue(address + message.offset, string) && string)
            text += ": " + ReadString(string);
    }
    for (char& c : text)
        if (c == '\n' || c == '\r')
            c = ' ';
    return text;
}

std::string Call(const Runtime& runtime, const std::string& rest, std::vector<uint8_t>& payload) {
    size_t at = 0;
    std::string className = NextWord(rest, at);
    std::string methodName = NextWord(rest, at);
    std::string objectText = NextWord(rest, at);
    std::string arguments = rest.substr(std::min(at, rest.size()));
    std::vector<std::string> values;
    std::vector<bool> quoted;
    if (arguments.empty())
        arguments = "[]";
    if (!ParseJsonArray(arguments, values, &quoted))
        return "error give the arguments as a JSON array, such as [1, 2.5, \"text\", true]";
    std::string error;
    Handle klass = FindClass(runtime, className, error);
    if (!klass)
        return error;
    if (methodName.empty())
        return "error give the method";
    Handle method = nullptr;
    Handle owner = nullptr;
    std::vector<std::pair<Handle, std::string>> parameters;
    for (Handle at2 : Lineage(runtime, klass)) {
        void* iterator = nullptr;
        while (!method && (method = runtime.classMethods(at2, &iterator)) != nullptr) {
            parameters = runtime.Parameters(method);
            if (methodName != runtime.methodName(method) || parameters.size() != values.size())
                method = nullptr;
        }
        if (method) {
            owner = at2;
            break;
        }
    }
    if (!method)
        return "error " + FullName(runtime, klass) + " has no method " + methodName + " with " + std::to_string(values.size()) +
               " parameters: see engine class";
    uint32_t implementation = 0;
    bool isStatic = (runtime.methodFlags(method, &implementation) & kMethodStatic) != 0;
    uintptr_t object = isStatic ? 0 : ParseObject(objectText);
    if (!isStatic) {
        Handle actual = ObjectClass(runtime, object);
        if (!actual || !runtime.IsSubclassOf(actual, owner))
            return "error " + methodName + " is called on an object: give the address of an object of " + FullName(runtime, owner) +
                   ", from engine instances";
        if (runtime.IsValueType(owner))
            return "error the methods of structs cannot be called";
        // The invoke runs the method given, not its override in the class of the object.
        if (runtime.virtualMethod) {
            Handle actualMethod = nullptr;
            if (Guarded([&] { actualMethod = runtime.virtualMethod(reinterpret_cast<Handle>(object), method); }) && actualMethod)
                method = actualMethod;
        }
    }
    Handle returned = runtime.ReturnType(method);
    // Everything by value: the call may still run after the command gave up on it.
    const Runtime* shared = &runtime;
    std::string answer = OnGameThread([shared, method, object, returned, parameters, values, quoted]() -> std::string {
        const Runtime& rt = *shared;
        Attached attached(rt);
        std::vector<Encoded> encoded(values.size());
        std::vector<void*> slots(values.size());
        for (size_t i = 0; i < values.size(); ++i) {
            std::string reason;
            if (!Encode(rt, parameters[i].first, values[i], quoted[i], encoded[i], reason))
                return reason + " (argument " + std::to_string(i + 1) + ")";
            slots[i] = encoded[i].reference ? encoded[i].object : static_cast<void*>(encoded[i].bytes);
        }
        Handle exception = nullptr;
        Handle result = nullptr;
        bool ran = Guarded([&] { result = rt.invoke(method, reinterpret_cast<Handle>(object), slots.empty() ? nullptr : slots.data(), &exception); });
        if (!ran)
            return "error the call faulted, which removed the overlay";
        if (exception)
            return FormatException(rt, exception);
        std::string type = returned ? rt.TypeName(returned) : "?";
        if (!returned || rt.typeKind(returned) == kVoid)
            return type;
        auto address = reinterpret_cast<uintptr_t>(result);
        if (IsReference(rt, returned))
            return type + " " + FormatReference(rt, returned, address);
        // Values come back boxed.
        return type + " " + (address ? FormatAt(rt, returned, address + kHeader) : "?");
    });
    return answer.rfind("error ", 0) == 0 ? answer : Payload(answer, payload);
}

} // namespace

std::string RunOnGameThread(std::function<std::string()> work) { return OnGameThread(std::move(work)); }

size_t& EngineListLimit() {
    thread_local size_t limit = kMaxListed;
    return limit;
}

void RunGameThreadCalls() {
    if (!g_callPending.load(std::memory_order_acquire) || t_inCall)
        return;
    std::function<std::string()> call;
    uint64_t id = 0;
    {
        std::lock_guard<std::mutex> lock(g_callMutex);
        if (!g_callPending || g_callFinished || !g_call)
            return;
        call = std::move(g_call);
        g_call = nullptr;
        id = g_callId;
    }
    t_inCall = true;
    std::string result = call();
    t_inCall = false;
    std::lock_guard<std::mutex> lock(g_callMutex);
    if (g_callId != id || !g_callPending)
        return;
    g_callResult = std::move(result);
    g_callFinished = true;
    g_callDone.notify_all();
}

bool HandleEngine(const std::string& command, const std::string& rest, std::vector<uint8_t>& payload, std::string& answer) {
    if (command != "engine")
        return false;
    size_t at = 0;
    std::string sub = NextWord(rest, at);
    std::string arguments = rest.substr(std::min(at, rest.size()));
    const Config& config = GetConfig();
    if (!config.remoteMemory)
        answer = "error memory access is off: set config.remoteMemory in example/config.cpp and rebuild the library";
    else if (sub == "call" && !config.aiAdvancedFeatures)
        answer = "error the advanced features are off: set config.aiAdvancedFeatures in example/config.cpp and rebuild the library";
    else if (!MemoryAllowed())
        answer = "error the player paused the memory tools in the AI Advanced Features window of the overlay";
    if (!answer.empty())
        return true;
    static const char* const kCommands[] = {"info", "assemblies", "classes", "class", "get", "set", "instances", "call", "dump"};
    if (std::find_if(std::begin(kCommands), std::end(kCommands), [&](const char* name) { return sub == name; }) == std::end(kCommands)) {
        answer = "error unknown engine command: info, assemblies, classes, class, get, set, instances, call or dump";
        return true;
    }
    std::lock_guard<std::mutex> lock(g_engineMutex);
    std::string error;
    Runtime* runtime = FindRuntime(error);
    if (!runtime) {
        if (error == "ok none" && HandleUnreal(sub, arguments, payload, answer))
            return true;
        answer = sub == "info" || error.rfind("error ", 0) == 0 ? error
                                                                : "error the game runs no engine whose objects the library knows: Unity with Mono or IL2CPP, Unreal Engine 4 or 5";
        return true;
    }
    if (sub == "info") {
        answer = Info(*runtime);
        return true;
    }
    if (sub == "dump") {
        answer = "error engine dump writes the C++ SDK of Unreal Engine games: Unity games have their own tools for it, such as Il2CppDumper";
        return true;
    }
    Attached attached(*runtime);
    Index(*runtime);
    if (sub == "assemblies")
        answer = ListAssemblies(*runtime, payload);
    else if (sub == "classes")
        answer = ListClasses(*runtime, arguments, payload);
    else if (sub == "class")
        answer = Describe(*runtime, arguments, payload);
    else if (sub == "get")
        answer = Get(*runtime, arguments, payload);
    else if (sub == "set")
        answer = Set(*runtime, arguments, payload);
    else if (sub == "call")
        answer = Call(*runtime, arguments, payload);
    else
        answer = Instances(*runtime, arguments, payload);
    return true;
}

} // namespace uih::detail::remote
