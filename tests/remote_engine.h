// The engine commands of the remote control, src/engine.cpp, driven as
// remote/uih_mcp.py does by the Windows and the Linux test hosts, on the fake
// runtime of tests/fake_unity.cpp. Include it after remote_memory.h, for
// RemotePayload() and HexAddress().
#pragma once

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "fake_unreal.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

// The fake runtime, loaded by the host under the name of the real one.
struct FakeEngine {
    bool il2cpp = false;
    std::string module;
    const char* version = nullptr;
    void* (*object)(int) = nullptr;
    int (*staticCount)() = nullptr;
    uint64_t (*lastCallThread)() = nullptr;
    void (*attachMain)() = nullptr;
    int (*attachedThreads)() = nullptr;
    int (*attaches)() = nullptr;
};

inline uint64_t EngineThreadId() {
#ifdef _WIN32
    return GetCurrentThreadId();
#else
    return static_cast<uint64_t>(syscall(SYS_gettid));
#endif
}

// False when the library does not load or is not the fake runtime.
inline bool LoadFakeEngine(const std::string& path, FakeEngine& fake) {
#ifdef _WIN32
    void* library = LoadLibraryA(path.c_str());
    auto find = [&](const char* name) { return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(library), name)); };
#else
    void* library = dlopen(path.c_str(), RTLD_NOW);
    auto find = [&](const char* name) { return dlsym(library, name); };
#endif
    if (!library)
        return false;
    size_t slash = path.find_last_of("/\\");
    fake.module = slash == std::string::npos ? path : path.substr(slash + 1);
    fake.il2cpp = fake.module.rfind("GameAssembly", 0) == 0;
    fake.version = static_cast<const char*>(find("fake_engine_version"));
    fake.object = reinterpret_cast<void* (*)(int)>(find("fake_engine_object"));
    fake.staticCount = reinterpret_cast<int (*)()>(find("fake_engine_static_count"));
    fake.lastCallThread = reinterpret_cast<uint64_t (*)()>(find("fake_engine_last_call_thread"));
    fake.attachMain = reinterpret_cast<void (*)()>(find("fake_engine_attach_main"));
    fake.attachedThreads = reinterpret_cast<int (*)()>(find("fake_engine_attached_threads"));
    fake.attaches = reinterpret_cast<int (*)()>(find("fake_engine_attaches"));
    return fake.version && fake.object && fake.staticCount && fake.lastCallThread && fake.attachMain && fake.attachedThreads && fake.attaches;
}

inline std::string EngineHex(uintptr_t value) {
    char text[32];
    snprintf(text, sizeof(text), "0x%" PRIxPTR, value);
    return text;
}

// Empty when the engine commands answered as remote/uih_mcp.py expects, the
// failure otherwise. `mainThread` is the thread that pumps the host's window
// messages or reads its X events, which runs the calls.
template <typename Client>
std::string CheckRemoteEngine(Client& client, const FakeEngine& fake, bool advanced, uint64_t mainThread) {
    auto payload = [&](const std::string& command) {
        std::string answer = client.Request(command);
        std::string text = answer.rfind("ok ", 0) == 0 ? RemotePayload(client, answer) : answer;
        printf("%s:\n%s\n", command.c_str(), text.c_str());
        return text;
    };
    auto has = [](const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; };
    const size_t word = sizeof(void*);
    auto at = [&](size_t offset) { return "  " + EngineHex(offset) + " "; };
    auto alice = reinterpret_cast<uintptr_t>(fake.object(0));
    auto bob = reinterpret_cast<uintptr_t>(fake.object(1));
    auto boss = reinterpret_cast<uintptr_t>(fake.object(2));
    auto decoy = reinterpret_cast<uintptr_t>(fake.object(3));
    auto dead = reinterpret_cast<uintptr_t>(fake.object(4));
    // The fields of Game.Player, laid out after m_CachedPtr as tests/fake_unity.cpp does.
    const size_t health = 3 * word;
    const size_t position = (3 * word + 16 + 2 * word + 3) / 4 * 4;
    std::string runtime = fake.il2cpp ? "il2cpp" : "mono";

    std::string info = client.Request("engine info");
    printf("engine info: %s\n", info.c_str());
    // After "Unity version: ", which the version scan must skip.
    if (info != "ok unity " + runtime + " " + std::string(fake.version + 15) + " " + fake.module)
        return "engine info does not describe the fake runtime";
    if (client.Request("engine nothing").rfind("error unknown engine command", 0) != 0)
        return "an unknown engine command is not refused";

    std::string assemblies = payload("engine assemblies");
    if (!has(assemblies, "Assembly-CSharp 4\n") || !has(assemblies, "mscorlib 10\n") || !has(assemblies, "UnityEngine.CoreModule 3\n"))
        return "engine assemblies does not list the assemblies and their classes";
    std::string classes = payload("engine classes player");
    std::string core = payload("engine classes * UnityEngine.CoreModule");
    if (classes != "Assembly-CSharp Game.Player\nAssembly-CSharp Game.Player/Inventory\n" ||
        core != "UnityEngine.CoreModule UnityEngine.Object\nUnityEngine.CoreModule UnityEngine.Vector3\nUnityEngine.CoreModule UnityEngine.MonoBehaviour\n")
        return "engine classes does not filter the classes by name and assembly";

    std::string player = payload("engine class Player");
    if (!has(player, "class Game.Player : UnityEngine.MonoBehaviour, UnityEngine.Object, System.Object (Assembly-CSharp)\n") ||
        !has(player, "  const System.Int32 MaxHealth\n") || !has(player, "  static System.Int32 s_Count\n") ||
        !has(player, at(health) + "System.Int32 health\n") || !has(player, at(health + 4) + "System.Single speed\n") ||
        !has(player, at(health + 12) + "Game.Team team\n") || !has(player, at(position) + "UnityEngine.Vector3 position\n") ||
        !has(player, at(2 * word) + "System.IntPtr m_CachedPtr  (UnityEngine.Object)\n") ||
        !has(player, "  static System.Int32 Add(System.Int32 a, System.Int32 b)\n") || !has(player, "  System.Void Heal(System.Int32 amount)\n") ||
        !has(player, "  System.String GetName()\n"))
        return "engine class does not describe the fields and methods of the class";
    std::string team = payload("engine class Game.Team");
    if (!has(team, "enum Game.Team : System.Enum, System.ValueType, System.Object (Assembly-CSharp)\n") || !has(team, "  const Game.Team Blue\n"))
        return "engine class does not describe an enum";
    if (client.Request("engine class Nothing").rfind("error no class named Nothing", 0) != 0)
        return "an unknown class is not refused";

    // The methods and fields of every class: the one named so first, then those that start with the text.
    if (payload("engine search heal") != "function Assembly-CSharp Game.Player::Heal System.Void Heal(System.Int32 amount)\n"
                                         "property Assembly-CSharp Game.Player::health " + EngineHex(health) + " System.Int32\n"
                                         "property Assembly-CSharp Game.Player::MaxHealth const System.Int32\n"
                                         "function Assembly-CSharp Game.Player::get_Health System.Int32 get_Health()\n" ||
        payload("engine search HEAL functions") != "function Assembly-CSharp Game.Player::Heal System.Void Heal(System.Int32 amount)\n"
                                                  "function Assembly-CSharp Game.Player::get_Health System.Int32 get_Health()\n" ||
        payload("engine search Game.Player::s_ properties") != "property Assembly-CSharp Game.Player::s_Count static System.Int32\n"
                                                              "property Assembly-CSharp Game.Player::s_Local static Game.Player\n"
                                                              "property Assembly-CSharp Game.Player::s_Spawn static UnityEngine.Vector3\n" ||
        payload("engine search nothing_named_so") != "" || client.Request("engine search").rfind("error give a part of the name", 0) != 0)
        return "engine search does not find the methods and fields of the classes";

    if (payload("engine get Game.Player s_Count") != "System.Int32 7" || payload("engine get Game.Player s_Spawn") != "UnityEngine.Vector3 {x=5, y=6, z=7}" ||
        payload("engine get Game.Player s_Local") != "Game.Player " + EngineHex(alice) + " (Game.Player)")
        return "engine get does not read the static fields";
    if (client.Request("engine get Game.Player MaxHealth").rfind("error MaxHealth is a constant", 0) != 0 ||
        client.Request("engine get Game.Player health").rfind("error health is a field of each object", 0) != 0)
        return "engine get reads a constant, or an instance field without an object";
    if (payload("engine set Game.Player s_Count - 42") != "System.Int32 42" || fake.staticCount() != 42)
        return "engine set does not write a static field";

    std::string instances = payload("engine instances Game.Player");
    if (!has(instances, EngineHex(alice) + "\n") || !has(instances, EngineHex(bob) + "\n"))
        return "engine instances does not find the players";
    if (has(instances, EngineHex(boss) + "\n") || has(instances, EngineHex(decoy) + "\n") || has(instances, EngineHex(dead) + "\n"))
        return "engine instances finds a subclass, a memory block that is no object, or a destroyed object";
    if (!has(payload("engine instances Game.Boss"), EngineHex(boss) + "\n"))
        return "engine instances does not find the boss";

    std::string object = " " + EngineHex(alice);
    if (payload("engine get Game.Player health" + object) != "System.Int32 100" || payload("engine get Game.Player speed" + object) != "System.Single 4.5" ||
        payload("engine get Game.Player alive" + object) != "System.Boolean true" || payload("engine get Game.Player team" + object) != "Game.Team 2" ||
        payload("engine get Game.Player playerName" + object) != "System.String \"Alice\"" ||
        payload("engine get Game.Player target" + object) != "Game.Player " + EngineHex(bob) + " (Game.Player)" ||
        payload("engine get Game.Player position" + object) != "UnityEngine.Vector3 {x=1, y=2, z=3}" ||
        payload("engine get Game.Player position.y" + object) != "System.Single 2" ||
        payload("engine get Game.Player m_CachedPtr " + EngineHex(boss)).rfind("System.IntPtr 0x", 0) != 0 ||
        payload("engine get Player health " + EngineHex(boss)) != "System.Int32 500")
        return "engine get does not read the fields of an object";
    if (client.Request("engine get Game.Boss phase" + object).rfind("error Game.Boss has no field phase", 0) == 0 ||
        client.Request("engine get Game.Boss phase" + object).rfind("error " + EngineHex(alice) + " is not an object of Game.Boss", 0) != 0 ||
        client.Request("engine get Game.Player health " + EngineHex(reinterpret_cast<uintptr_t>(&fake))).rfind("error ", 0) != 0)
        return "engine get reads an object of another class, or memory that is not an object";

    int32_t value = 0;
    if (payload("engine set Game.Player health" + object + " 55") != "System.Int32 55" ||
        (memcpy(&value, reinterpret_cast<const void*>(alice + health), 4), value != 55))
        return "engine set does not write the field of an object";
    if (payload("engine set Game.Player alive" + object + " false") != "System.Boolean false" ||
        payload("engine set Game.Player position.x" + object + " 9.5") != "System.Single 9.5" ||
        payload("engine set Game.Player playerName" + object + " \"Carol \\u00e9\"") != "System.String \"Carol \xc3\xa9\"" ||
        payload("engine set Game.Player target" + object + " null") != "Game.Player null" ||
        payload("engine set Game.Player target" + object + " " + EngineHex(boss)) != "Game.Player " + EngineHex(boss) + " (Game.Boss)")
        return "engine set does not write booleans, fields of structs, strings or references";
    if (client.Request("engine set Game.Player health" + object + " lots").rfind("error give an integer", 0) != 0 ||
        client.Request("engine set Game.Player target" + object + " " + EngineHex(decoy + 8)).rfind("error give Game.Player as the address", 0) != 0 ||
        client.Request("engine set Game.Player position" + object + " 1").rfind("error UnityEngine.Vector3 is a struct", 0) != 0)
        return "engine set writes a value of the wrong type";

    if (!advanced) {
        if (client.Request("engine call Game.Player Add - [2, 3]").rfind("error the advanced features are off", 0) != 0)
            return "engine call answers while aiAdvancedFeatures is off";
    } else {
        if (payload("engine call Game.Player Add - [2, 3]") != "System.Int32 5")
            return "engine call does not call a static method";
        if (fake.lastCallThread() != mainThread)
            return "engine call did not run on the main thread of the host";
        if (payload("engine call Game.Player Heal" + object + " [10]") != "System.Void" || payload("engine call Game.Player get_Health" + object) != "System.Int32 65")
            return "engine call does not call a method of an object";
        if (payload("engine call Game.Player Rename" + object + " [\"Dave\"]") != "System.Void" ||
            payload("engine call Game.Player GetName" + object) != "System.String \"Dave\"")
            return "engine call does not pass or return strings";
        std::string failed = client.Request("engine call Game.Player Fail -");
        printf("engine call Game.Player Fail: %s\n", failed.c_str());
        if (failed != "error the method threw System.Exception: \"it failed on purpose\"")
            return "engine call does not report the exception of the method";
        if (client.Request("engine call Game.Player Heal - [1]").rfind("error Heal is called on an object", 0) != 0 ||
            client.Request("engine call Game.Player Add - [1]").rfind("error Game.Player has no method Add with 1 parameters", 0) != 0 ||
            client.Request("engine call Game.Player Add - [1, \"x\"]").rfind("error give an integer", 0) != 0 ||
            client.Request("engine call Game.Player Add - 1 2").rfind("error give the arguments as a JSON array", 0) != 0)
            return "engine call accepts a call without its object or with the wrong arguments";
    }
    printf("attached threads: %d, attaches: %d\n", fake.attachedThreads(), fake.attaches());
    if (fake.attachedThreads() != 1 || fake.attaches() < 3)
        return "the commands did not attach their thread to the runtime, or left it attached";
    return "";
}

// A real Mono runtime embedded by the host, as a Unity player embeds its own,
// running tests/mono/Game.cs.
struct RealMono {
    std::string module;
    uintptr_t alice = 0;
    uintptr_t bob = 0;
    uintptr_t boss = 0;
};

// Starts the runtime `library` on the calling thread, which stays attached to
// it, loads `assembly` and calls Game.Boot.Spawn(). `root` holds the lib and
// etc directories of the runtime when it does not know them, as Unity's.
inline bool StartRealMono(const std::string& library, const std::string& assembly, const std::string& root, RealMono& mono, std::string& error) {
#ifdef _WIN32
    // SGen keeps the copies of the objects it moved out of the nursery until it allocates there again.
    _putenv("MONO_GC_DEBUG=clear-at-gc");
    void* runtime = LoadLibraryA(library.c_str());
    auto find = [&](const char* name) { return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(runtime), name)); };
#else
    setenv("MONO_GC_DEBUG", "clear-at-gc", 1);
    void* runtime = dlopen(library.c_str(), RTLD_NOW | RTLD_GLOBAL);
    auto find = [&](const char* name) { return dlsym(runtime, name); };
#endif
    if (!runtime) {
        error = "the Mono runtime " + library + " does not load";
        return false;
    }
    size_t slash = library.find_last_of("/\\");
    mono.module = slash == std::string::npos ? library : library.substr(slash + 1);
    auto setDirs = reinterpret_cast<void (*)(const char*, const char*)>(find("mono_set_dirs"));
    auto init = reinterpret_cast<void* (*)(const char*, const char*)>(find("mono_jit_init_version"));
    auto open = reinterpret_cast<void* (*)(void*, const char*)>(find("mono_domain_assembly_open"));
    auto image = reinterpret_cast<void* (*)(void*)>(find("mono_assembly_get_image"));
    auto classFromName = reinterpret_cast<void* (*)(void*, const char*, const char*)>(find("mono_class_from_name"));
    auto methodFromName = reinterpret_cast<void* (*)(void*, const char*, int)>(find("mono_class_get_method_from_name"));
    auto invoke = reinterpret_cast<void* (*)(void*, void*, void**, void**)>(find("mono_runtime_invoke"));
    auto fieldFromName = reinterpret_cast<void* (*)(void*, const char*)>(find("mono_class_get_field_from_name"));
    auto vtable = reinterpret_cast<void* (*)(void*, void*)>(find("mono_class_vtable"));
    auto staticValue = reinterpret_cast<void (*)(void*, void*, void*)>(find("mono_field_static_get_value"));
    auto fieldValue = reinterpret_cast<void (*)(void*, void*, void*)>(find("mono_field_get_value"));
    if (!setDirs || !init || !open || !image || !classFromName || !methodFromName || !invoke || !fieldFromName || !vtable || !staticValue || !fieldValue) {
        error = library + " does not export the embedding API of Mono";
        return false;
    }
    if (!root.empty())
        setDirs((root + "/lib").c_str(), (root + "/etc").c_str());
    void* domain = init("uih_host", "v4.0.30319");
    void* game = domain ? open(domain, assembly.c_str()) : nullptr;
    void* gameImage = game ? image(game) : nullptr;
    void* boot = gameImage ? classFromName(gameImage, "Game", "Boot") : nullptr;
    void* player = gameImage ? classFromName(gameImage, "Game", "Player") : nullptr;
    void* spawn = boot ? methodFromName(boot, "Spawn", 0) : nullptr;
    if (!spawn || !player) {
        error = "Mono did not load Game.Boot.Spawn() from " + assembly;
        return false;
    }
    void* exception = nullptr;
    invoke(spawn, nullptr, nullptr, &exception);
    if (exception) {
        error = "Game.Boot.Spawn() threw an exception";
        return false;
    }
    staticValue(vtable(domain, player), fieldFromName(player, "s_Local"), &mono.alice);
    staticValue(vtable(domain, boot), fieldFromName(boot, "s_Boss"), &mono.boss);
    if (mono.alice)
        fieldValue(reinterpret_cast<void*>(mono.alice), fieldFromName(player, "target"), &mono.bob);
    if (!mono.alice || !mono.bob || !mono.boss) {
        error = "Game.Boot.Spawn() did not create the players";
        return false;
    }
    return true;
}

// The engine commands on the real runtime, whose layout of the objects the
// test does not know beforehand.
template <typename Client>
std::string CheckRealMono(Client& client, const RealMono& mono, bool advanced) {
    auto payload = [&](const std::string& command) {
        std::string answer = client.Request(command);
        std::string text = answer.rfind("ok ", 0) == 0 ? RemotePayload(client, answer) : answer;
        printf("%s:\n%s\n", command.c_str(), text.c_str());
        return text;
    };
    auto has = [](const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; };
    std::string info = client.Request("engine info");
    printf("engine info: %s\n", info.c_str());
    if (info.rfind("ok unity mono ", 0) != 0 || info.size() < mono.module.size() || info.compare(info.size() - mono.module.size(), std::string::npos, mono.module) != 0)
        return "engine info does not describe the Mono runtime";
    if (!has(payload("engine assemblies"), "Game 5\n") || payload("engine classes player Game") != "Game Game.Player\n")
        return "engine assemblies or classes does not list the assembly of the game";

    std::string player = payload("engine class Game.Player");
    size_t line = player.find(" System.Int32 health\n");
    size_t start = player.rfind("  0x", line);
    uintptr_t health = start == std::string::npos ? 0 : static_cast<uintptr_t>(strtoull(player.c_str() + start + 2, nullptr, 16));
    int32_t value = 0;
    if (!has(player, "class Game.Player : System.Object (Game)\n") || !has(player, "  const System.Int32 MaxHealth\n") ||
        !has(player, "  static System.Int32 s_Count\n") || !has(player, "  static System.Int32 Add(System.Int32 a, System.Int32 b)\n") ||
        !has(player, "  System.Void Heal(System.Int32 amount)\n") || line == std::string::npos || !health ||
        (memcpy(&value, reinterpret_cast<const void*>(mono.alice + health), 4), value != 100))
        return "engine class does not describe the fields of Game.Player at their offsets";

    if (payload("engine get Game.Player s_Count") != "System.Int32 2" ||
        payload("engine get Game.Player s_Local") != "Game.Player " + EngineHex(mono.alice) + " (Game.Player)")
        return "engine get does not read the static fields";
    std::string instances = payload("engine instances Game.Player");
    if (!has(instances, EngineHex(mono.alice) + "\n") || !has(instances, EngineHex(mono.bob) + "\n") || has(instances, EngineHex(mono.boss) + "\n") ||
        payload("engine instances Game.Boss") != EngineHex(mono.boss) + "\n")
        return "engine instances does not find the players and the boss apart";

    std::string object = " " + EngineHex(mono.alice);
    if (payload("engine get Game.Player health" + object) != "System.Int32 100" || payload("engine get Game.Player speed" + object) != "System.Single 4.5" ||
        payload("engine get Game.Player alive" + object) != "System.Boolean true" || payload("engine get Game.Player team" + object) != "Game.Team 2" ||
        payload("engine get Game.Player playerName" + object) != "System.String \"Alice\"" ||
        payload("engine get Game.Player target" + object) != "Game.Player " + EngineHex(mono.bob) + " (Game.Player)" ||
        payload("engine get Game.Player spawn" + object) != "Game.Vec3 {x=1, y=2, z=3}" || payload("engine get Game.Player spawn.y" + object) != "System.Single 2" ||
        payload("engine get Game.Player health " + EngineHex(mono.boss)) != "System.Int32 500" ||
        payload("engine get Game.Boss phase " + EngineHex(mono.boss)) != "System.Int32 1")
        return "engine get does not read the fields of the objects";
    if (payload("engine set Game.Player health" + object + " 55") != "System.Int32 55" ||
        (memcpy(&value, reinterpret_cast<const void*>(mono.alice + health), 4), value != 55) ||
        payload("engine set Game.Player playerName" + object + " \"Carol\"") != "System.String \"Carol\"" ||
        payload("engine set Game.Player spawn.z" + object + " -4") != "System.Single -4" ||
        payload("engine set Game.Player s_Count - 3") != "System.Int32 3")
        return "engine set does not write the fields";

    if (advanced) {
        if (payload("engine call Game.Player Add - [2, 3]") != "System.Int32 5" || payload("engine call Game.Player Heal" + object + " [10]") != "System.Void" ||
            payload("engine get Game.Player health" + object) != "System.Int32 65" ||
            payload("engine call Game.Player GetName" + object) != "System.String \"Carol\"")
            return "engine call does not call the methods";
        std::string failed = client.Request("engine call Game.Player Fail -");
        printf("engine call Game.Player Fail: %s\n", failed.c_str());
        if (failed != "error the method threw System.InvalidOperationException: \"it failed on purpose\"")
            return "engine call does not report the exception of the method";
        if (payload("engine call Game.Player Kind " + EngineHex(mono.boss)) != "System.String \"boss\"")
            return "engine call does not call the override of a virtual method";
        std::string thread = payload("engine call Game.Boot Thread -");
        if (thread.rfind("System.Int32 ", 0) != 0 || thread.substr(13) != payload("engine get Game.Boot s_MainThread").substr(13))
            return "engine call did not run on the main thread of the host";
    }
    return "";
}

// The text of a file, empty when it cannot be read.
inline std::string ReadTextFile(const std::string& path) {
    std::string text;
    FILE* file = fopen(path.c_str(), "rb");
    if (!file)
        return text;
    char buffer[65536];
    size_t count;
    while ((count = fread(buffer, 1, sizeof(buffer), file)) > 0)
        text.append(buffer, count);
    fclose(file);
    return text;
}

// The engine commands on the objects of tests/fake_unreal.cpp, whose layout
// src/unreal.cpp must find by itself. With `sdkCopy`, the SDK of every
// package is copied there, for the program that tests it (tests/sdk).
template <typename Client>
std::string CheckRemoteUnreal(Client& client, const FakeUnreal& fake, bool advanced, const std::string& sdkCopy = "") {
    auto payload = [&](const std::string& command) {
        std::string answer = client.Request(command);
        std::string text = answer.rfind("ok ", 0) == 0 ? RemotePayload(client, answer) : answer;
        printf("%s:\n%s\n", command.c_str(), text.c_str());
        return text;
    };
    auto has = [](const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; };
    bool five = fake.generation == 5;
    std::string info = client.Request("engine info");
    printf("engine info: %s\n", info.c_str());
    if (info.rfind(five ? "ok unreal ue5 5.8 uih_host" : "ok unreal ue4 4.22 uih_host", 0) != 0)
        return "engine info does not describe the Unreal Engine game";
    std::string packages = payload("engine assemblies");
    if (!has(packages, "/Game/Blueprints/BP_Hero 1\n") || !has(packages, "/Script/Engine 3\n/Script/Game 2\n") ||
        payload("engine classes player") != "/Script/Game GamePlayer\n" || payload("engine classes * Game") != "/Script/Game ETeam\n/Script/Game GamePlayer\n" ||
        payload("engine classes * /Script/Engine") != "/Script/Engine Actor\n/Script/Engine KismetMathLibrary\n/Script/Engine Pawn\n" ||
        payload("engine classes hero") != "/Game/Blueprints/BP_Hero BP_Hero_C\n")
        return "engine assemblies or classes does not list the packages and their classes";
    std::string real = five ? "double" : "float";
    if (payload("engine class GamePlayer") != "class GamePlayer : Pawn : Actor : Object (/Script/Game)\n"
                                              "properties:\n"
                                              "  0x100 int32 Health\n"
                                              "  0x104 float Speed\n"
                                              "  0x108 bool bAlive\n"
                                              "  0x109 ETeam Team\n"
                                              "  0x110 FString PlayerName\n"
                                              "  0x120 GamePlayer* Target\n"
                                              "  0x128 Vector Location\n"
                                              "  0x148 TArray<int32> Inventory\n"
                                              "  0x158 int32 \"Health Max\"\n"
                                              "  0x80 float BaseEyeHeight  (Pawn)\n"
                                              "  0x58 bool bHidden (bit 0x2)  (Actor)\n"
                                              "  0x60 TArray<FName> Tags  (Actor)\n"
                                              "functions:\n"
                                              "  bool Heal(int32 Amount)\n"
                                              "  int32 GetStats(int32 Bonus, out int32 Total, out float Ratio)\n"
                                              "  void Boost(ref int32 Value)\n"
                                              "  Vector GetLocation()\n"
                                              "  void SetLocation(Vector NewLocation)\n"
                                              "  void Rename(FString NewName)\n"
                                              "functions of Actor: engine class Actor\n" ||
        payload("engine class Vector") != "struct Vector (/Script/CoreUObject)\nproperties:\n  0x0 " + real + " X\n  0x" + (five ? "8 " : "4 ") + real +
                                             " Y\n  0x" + (five ? "10 " : "8 ") + real + " Z\n" ||
        payload("engine class Game:ETeam") != "enum ETeam (/Script/Game)\nvalues:\n  0 Red\n  1 Blue\n  2 Green\n  3 ETeam_MAX\n" ||
        !has(payload("engine class KismetMathLibrary"), "functions:\n  static int32 Add_IntInt(int32 A, int32 B)\n") ||
        client.Request("engine class Nothing") != "error no class, struct or enum is named Nothing: see engine classes")
        return "engine class does not describe the classes, structs and enums";
    std::string hero = payload("engine class BP_Hero_C");
    if (hero.rfind("class BP_Hero_C : GamePlayer : Pawn : Actor : Object (/Game/Blueprints/BP_Hero)\nproperties:\n  0x15c float Mana\n", 0) != 0 ||
        !has(hero, "functions:\n  float \"Get Health Ratio\"()\nfunctions of GamePlayer: engine class GamePlayer\n"))
        return "engine class does not describe a blueprint class, or does not quote the names with spaces";

    // The functions and properties of every class: the one named so first,
    // then those that start with the text, then those that contain it.
    std::string healers = "function /Script/Game GamePlayer::Heal bool Heal(int32 Amount)\n"
                          "property /Script/Game GamePlayer::Health 0x100 int32\n"
                          "property /Script/Game \"GamePlayer::Health Max\" 0x158 int32\n"
                          "function /Game/Blueprints/BP_Hero \"BP_Hero_C::Get Health Ratio\" float \"Get Health Ratio\"()\n";
    if (payload("engine search heal") != healers || payload("engine search Heal all") != healers ||
        payload("engine search HEAL functions") != "function /Script/Game GamePlayer::Heal bool Heal(int32 Amount)\n"
                                                  "function /Game/Blueprints/BP_Hero \"BP_Hero_C::Get Health Ratio\" float \"Get Health Ratio\"()\n" ||
        payload("engine search health max properties") != "property /Script/Game \"GamePlayer::Health Max\" 0x158 int32\n" ||
        payload("engine search \"Health Max\"") != "property /Script/Game \"GamePlayer::Health Max\" 0x158 int32\n" ||
        payload("engine search hero::") != "property /Game/Blueprints/BP_Hero BP_Hero_C::Mana 0x15c float\n"
                                           "function /Game/Blueprints/BP_Hero \"BP_Hero_C::Get Health Ratio\" float \"Get Health Ratio\"()\n" ||
        payload("engine search bhidden") != "property /Script/Engine Actor::bHidden 0x58 bool (bit 0x2)\n" ||
        payload("engine search add_int functions") != "function /Script/Engine KismetMathLibrary::Add_IntInt static int32 Add_IntInt(int32 A, int32 B)\n" ||
        payload("engine search nothing_named_so") != "" || client.Request("engine search").rfind("error give a part of the name", 0) != 0)
        return "engine search does not find the functions and properties of the classes";

    std::string players = EngineHex(fake.alice) + " GamePlayer GamePlayer_0\n" + EngineHex(fake.bob) + " GamePlayer GamePlayer_1\n" + EngineHex(fake.carol) +
                          " GamePlayer GamePlayer_2\n" + EngineHex(fake.dave) + " BP_Hero_C BP_Hero_C_0\n";
    if (payload("engine instances GamePlayer") != players || payload("engine instances Pawn") != EngineHex(fake.pawn) + " Pawn Pawn_0\n" + players ||
        payload("engine instances GamePlayer 1") != EngineHex(fake.alice) + " GamePlayer GamePlayer_0\n" ||
        client.Request("engine instances Vector") != "error Vector is a struct: its values live inside objects, not on their own")
        return "engine instances does not list the objects of the class and its subclasses, without the default object";

    std::string alice = " " + EngineHex(fake.alice);
    if (payload("engine get GamePlayer Health" + alice) != "int32 100" || payload("engine get GamePlayer Speed" + alice) != "float 4.5" ||
        payload("engine get GamePlayer bAlive" + alice) != "bool true" || payload("engine get GamePlayer Team" + alice) != "ETeam 2" ||
        payload("engine get GamePlayer PlayerName" + alice) != "FString \"Alice\"" ||
        payload("engine get GamePlayer Target" + alice) != "GamePlayer* " + EngineHex(fake.bob) + " (GamePlayer GamePlayer_1)" ||
        payload("engine get GamePlayer Location" + alice) != "Vector {X=1, Y=2, Z=3}" || payload("engine get GamePlayer Location.Y" + alice) != real + " 2" ||
        payload("engine get GamePlayer Inventory" + alice) != "TArray<int32> [3, 1, 4]" ||
        payload("engine get Pawn BaseEyeHeight" + alice) != "float 64" || payload("engine get GamePlayer bHidden" + alice) != "bool true" ||
        payload("engine get Actor Tags" + alice) != "TArray<FName> [\"Hero\", \"Player\"]" ||
        payload("engine get GamePlayer \"Health Max\"" + alice) != "int32 200")
        return "engine get does not read the properties of the objects";
    std::string all = payload("engine get GamePlayer *" + alice);
    if (all.rfind("0x100\tint32\tHealth\t100\n0x104\tfloat\tSpeed\t4.5\n", 0) != 0 || !has(all, "\n0x158\tint32\tHealth Max\t200\n") ||
        !has(all, "\n0x58\tbool\tbHidden\ttrue\n"))
        return "engine get * does not read every property of the object";
    if (client.Request("engine get GamePlayer Health") != "error give the object: properties of Unreal classes belong to objects, which engine instances lists" ||
        client.Request("engine get GamePlayer Health " + EngineHex(fake.pawn)) !=
            "error " + EngineHex(fake.pawn) + " is not an object of GamePlayer: see engine instances GamePlayer" ||
        client.Request("engine get GamePlayer Mana" + alice) != "error GamePlayer has no property Mana: see engine class GamePlayer")
        return "engine get does not refuse what it cannot read";

    uint8_t hidden = 0;
    int32_t health = 0;
    if (payload("engine set GamePlayer Health" + alice + " 55") != "int32 55" ||
        (memcpy(&health, reinterpret_cast<const void*>(fake.alice + 0x100), 4), health != 55) ||
        payload("engine set GamePlayer Speed" + alice + " 6.25") != "float 6.25" || payload("engine set GamePlayer bHidden" + alice + " false") != "bool false" ||
        (memcpy(&hidden, reinterpret_cast<const void*>(fake.alice + 0x58), 1), hidden != 0x01) ||
        payload("engine set GamePlayer Location.Z" + alice + " -4") != real + " -4" || payload("engine set GamePlayer Team" + alice + " Blue") != "ETeam 1" ||
        payload("engine set GamePlayer Location" + alice + " {\"X\": 7, \"Y\": 8, \"Z\": 9}") != "Vector {X=7, Y=8, Z=9}" ||
        payload("engine set GamePlayer \"Health Max\"" + alice + " 250") != "int32 250" ||
        payload("engine set GamePlayer Target" + alice + " null") != "GamePlayer* null" ||
        payload("engine set GamePlayer Target" + alice + " " + EngineHex(fake.carol)) != "GamePlayer* " + EngineHex(fake.carol) + " (GamePlayer GamePlayer_2)")
        return "engine set does not write the properties";
    if (client.Request("engine set GamePlayer Target" + alice + " " + EngineHex(fake.pawn)) !=
            "error give null or the address of an object of GamePlayer, from engine instances" ||
        client.Request("engine set GamePlayer PlayerName" + alice + " \"Eve\"") !=
            "error the library cannot write a FString property yet: numbers, enums, bools, names, structs and object references, and strings as arguments of a call")
        return "engine set does not refuse what it cannot write";

    if (!advanced) {
        std::string call = client.Request("engine call GamePlayer Heal" + alice + " [5]");
        printf("engine call: %s\n", call.c_str());
        if (call.rfind("error the advanced features are off", 0) != 0)
            return "engine call answers while aiAdvancedFeatures is off";
    } else {
        std::string dave = " " + EngineHex(fake.dave);
        int callsBefore = FakeUnrealCalls();
        if (payload("engine call GamePlayer Heal" + alice + " [5]") != "bool true\n" || payload("engine get GamePlayer Health" + alice) != "int32 60" ||
            payload("engine call GamePlayer GetStats" + alice + " [10]") != "int32 60\nout Total = int32 70\nout Ratio = float 0.6\n" ||
            payload("engine call GamePlayer Boost" + alice + " [21]") != "void\nout Value = int32 42\n" ||
            payload("engine call GamePlayer GetLocation" + alice) != "Vector {X=7, Y=8, Z=9}\n" ||
            payload("engine call GamePlayer SetLocation" + alice + " [{\"X\": 1, \"Y\": 2.5, \"Z\": 3}]") != "void\n" ||
            payload("engine get GamePlayer Location" + alice) != "Vector {X=1, Y=2.5, Z=3}" ||
            payload("engine call GamePlayer Rename" + alice + " [\"Eve\"]") != "void\n" || payload("engine get GamePlayer PlayerName" + alice) != "FString \"Eve\"" ||
            payload("engine call BP_Hero_C \"Get Health Ratio\"" + dave) != "float 0.45\n" ||
            payload("engine call KismetMathLibrary Add_IntInt - [2, 3]") != "int32 5\n")
            return "engine call does not call the functions of the objects through ProcessEvent";
        // The check of ProcessEvent, Add_IntInt(20, 22), before the first call.
        printf("ProcessEvent calls: %d, on thread %llu, the window's %llu\n", FakeUnrealCalls() - callsBefore,
               static_cast<unsigned long long>(FakeUnrealCallThread()), static_cast<unsigned long long>(fake.mainThread));
        if (FakeUnrealCalls() - callsBefore != 9)
            return "engine call did not check ProcessEvent once before the first call";
        if (FakeUnrealCallThread() != fake.mainThread)
            return "engine call did not run on the thread of the game's window";
        if (client.Request("engine call GamePlayer Heal" + alice + " []") != "error Heal takes 1 arguments, 0 given: bool Heal(int32 Amount)" ||
            client.Request("engine call GamePlayer Heal - [1]") != "error Heal is called on an object: give the address of an object of GamePlayer, from engine instances" ||
            client.Request("engine call GamePlayer Fly" + alice) != "error GamePlayer has no function Fly: see engine class GamePlayer" ||
            client.Request("engine call Vector X -") != "error Vector is a struct: only the functions of classes are called")
            return "engine call does not refuse what it cannot call";
    }

    // The SDK: of the game by default, /Game/ and /Script/uih_host here, with
    // the parents of the blueprint class and what it uses; of every package with all.
    std::string game = payload("engine dump");
    std::string gamePath = game.substr(0, game.find('\n'));
    std::string version = five ? "-SDK-5.8-" : "-SDK-4.22-";
    std::string gameSdk = ReadTextFile(gamePath);
    if (!has(gamePath, "uih_host" + version) || !has(game, "\n5 classes, 1 structs, 1 enums, 8 functions, ") || gameSdk.empty() ||
        !has(gameSdk, "class ABP_Hero_C : public AGamePlayer {") || !has(gameSdk, "    float Get_Health_Ratio();\n") ||
        !has(gameSdk, "fn(\"Get Health Ratio\", ") || has(gameSdk, "UKismetMathLibrary {"))
        return "engine dump does not write the SDK of the game";
    std::string every = payload("engine dump all");
    std::string path = every.substr(0, every.find('\n'));
    std::string sdk = ReadTextFile(path);
    printf("SDK: %s, %zu bytes\n", path.c_str(), sdk.size());
    if (sdk.empty() || !has(sdk, "enum class ETeam : uint8_t {\n    Red = 0,\n    Blue = 1,\n    Green = 2,\n    ETeam_MAX = 3,\n};") ||
        !has(sdk, "    static int32_t Add_IntInt(int32_t A, int32_t B);\n") ||
        !has(sdk, "    int32_t GetStats(int32_t Bonus, int32_t& Total, float& Ratio);\n") ||
        !has(sdk, "    void Boost(int32_t& Value);\n") || !has(sdk, "    int32_t& Health_Max() {") ||
        !has(sdk, std::string("struct alignas(") + (five ? "8" : "4") + ") FVector {") ||
        !has(sdk, "constexpr int kProcessEvent = " + std::to_string(fake.processEvent) + ";"))
        return "engine dump all does not write the SDK of every package";
    if (client.Request("engine dump /Script/Nothing").rfind("error no class, struct or enum is in /Script/Nothing", 0) != 0)
        return "engine dump does not refuse a scope without types";
    if (!sdkCopy.empty()) {
        FILE* copy = fopen(sdkCopy.c_str(), "wb");
        bool written = copy && fwrite(sdk.data(), 1, sdk.size(), copy) == sdk.size();
        if (copy)
            fclose(copy);
        if (!written)
            return "the SDK could not be copied to " + sdkCopy;
    }
    return "";
}
