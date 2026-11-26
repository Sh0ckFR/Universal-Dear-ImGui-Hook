// The advanced commands of the remote control that the Windows and the Linux
// test hosts check the same way: wait, pointer chains and pointer scans, the
// watch list and its frozen values, and the instruction watch, on values of
// the host itself. Include after remote_memory.h and host.h.
#pragma once

#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#ifndef _WIN32
#include <unistd.h>
#endif

// Behind a pointer stored in the executable, as game objects are.
struct RemoteObject {
    int64_t padding;
    int32_t value;
};
inline RemoteObject* volatile g_remoteObject = nullptr;
inline volatile int32_t g_remoteFrozen = 0;
inline volatile int32_t g_remoteWritten = 0;

#ifdef _WIN32
#define UIH_NOINLINE __declspec(noinline)
#else
#define UIH_NOINLINE __attribute__((noinline))
#endif

// The instruction the instruction watch must find.
UIH_NOINLINE inline void WriteWatched(int32_t value) { g_remoteWritten = value; }

inline void Pause(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// The executable's name and base, from the modules command.
template <typename Client>
bool HostModule(Client& client, std::string& name, uintptr_t& base) {
    std::string modules = RemotePayload(client, client.Request("modules"));
    size_t line = modules.find("uih_host");
    if (line == std::string::npos)
        return false;
    size_t start = modules.rfind('\n', line);
    start = start == std::string::npos ? 0 : start + 1;
    base = static_cast<uintptr_t>(strtoull(modules.c_str() + start, nullptr, 16));
    name = modules.substr(line, modules.find('\n', line) - line);
    return base != 0;
}

// Empty when the commands worked, the reason otherwise. The host renders
// frames on another thread meanwhile.
template <typename Client>
std::string CheckRemoteAdvanced(Client& client) {
    // Wait: unchanged while the host clears to the same color, changed once it flashes.
    std::string still = client.Request("wait 400 1");
    std::thread flash([] {
        Pause(300);
        g_flashClear = true;
    });
    std::string changed = client.Request("wait 5000 1");
    flash.join();
    g_flashClear = false;
    printf("wait, still: %s\nwait, flash after 300 ms: %s\n", still.c_str(), changed.c_str());
    if (still.rfind("ok unchanged ", 0) != 0)
        return "wait saw a change on a still screen";
    if (changed.rfind("ok changed ", 0) != 0 || atoi(changed.c_str() + 11) < 250)
        return "wait did not see the screen change, or not when it did";

    std::string name;
    uintptr_t base = 0;
    if (!HostModule(client, name, base))
        return "the module list does not show the host";
    static RemoteObject object{0, 0};
    object.value = 4711;
    g_remoteObject = &object;
    char chain[300];
    snprintf(chain, sizeof(chain), "[%s+%" PRIxPTR "]+8", name.c_str(), reinterpret_cast<uintptr_t>(&g_remoteObject) - base);
    std::string pointer = RemotePayload(client, client.Request(std::string("pointer ") + chain));
    std::string read = client.Request(std::string("read ") + chain + " 4");
    int32_t value = 0;
    bool readBack = read == "ok 4" && client.Read(reinterpret_cast<uint8_t*>(&value), sizeof(value));
    printf("pointer %s:\n%sread through the chain: %s, value %d\n", chain, pointer.c_str(), read.c_str(), value);
    if (pointer.find("address " + HexAddress(&object.value) + " ") == std::string::npos)
        return "the pointer chain does not resolve to the value behind the pointer";
    if (!readBack || value != 4711)
        return "read does not follow a pointer chain";

    std::string pointers = RemotePayload(client, client.Request("pointers " + HexAddress(&object.value) + " 40"));
    printf("pointers to %s:\n%s", HexAddress(&object.value).c_str(), pointers.substr(0, 600).c_str());
    size_t hit = pointers.find(HexAddress(&g_remoteObject) + " ");
    if (hit == std::string::npos || pointers.compare(pointers.find(' ', hit + 1) + 1, name.size(), name) != 0 ||
        pointers.find(" +0x8\n", hit) != pointers.find('\n', hit) - 5)
        return "the pointer scan does not find the pointer of the executable, at module+offset with its offset";

    // The watch list: a frozen value is written back every frame.
    g_remoteFrozen = 5;
    std::string added = client.Request("watch add i32 " + HexAddress(&g_remoteFrozen) + " health");
    std::string id = added.size() > 3 ? added.substr(3) : "";
    std::string list = RemotePayload(client, client.Request("watch list"));
    std::string frozen = client.Request("freeze " + id + " 4242");
    g_remoteFrozen = 1;
    Pause(300);
    int32_t afterFreeze = g_remoteFrozen;
    std::string unfrozen = client.Request("unfreeze " + id);
    g_remoteFrozen = 7;
    Pause(200);
    int32_t afterUnfreeze = g_remoteFrozen;
    std::string removed = client.Request("watch remove " + id);
    std::string empty = RemotePayload(client, client.Request("watch list"));
    std::string unknown = client.Request("freeze 999");
    printf("watch add: %s\nwatch list: %sfreeze: %s, value %d\nunfreeze: %s, value %d\nwatch remove: %s, list now %zu bytes\nfreeze 999: %s\n",
           added.c_str(), list.c_str(), frozen.c_str(), afterFreeze, unfrozen.c_str(), afterUnfreeze, removed.c_str(), empty.size(), unknown.c_str());
    if (added.rfind("ok ", 0) != 0 || list.find(" i32 " + HexAddress(&g_remoteFrozen) + " ") == std::string::npos ||
        list.find(" 5 frozen=0 health\n") == std::string::npos)
        return "the watch list does not show the value of the host";
    if (frozen != "ok" || afterFreeze != 4242)
        return "the frozen value was not written back";
    if (unfrozen != "ok" || afterUnfreeze != 7)
        return "the value stayed frozen";
    if (removed != "ok" || !empty.empty() || unknown.rfind("error no value 999", 0) != 0)
        return "the watch list does not remove its values";

    // The chat with Claude, as remote/uih_chat.py drives it.
    std::string polled = RemotePayload(client, client.Request("chat poll"));
    std::string working = client.Request("chat status looking at the game");
    std::string said = client.Request("chat say Hello\\nplayer \\\\o/");
    std::string noted = client.Request("chat note Claude Code stopped");
    std::string idle = client.Request("chat status");
    std::string unknownChat = client.Request("chat shout hello");
    std::string emptySay = client.Request("chat say");
    printf("chat poll: %zu bytes\nchat status: %s, chat say: %s, chat note: %s, chat status: %s\nchat shout: %s\nchat say: %s\n", polled.size(),
           working.c_str(), said.c_str(), noted.c_str(), idle.c_str(), unknownChat.c_str(), emptySay.c_str());
    if (!polled.empty() || working != "ok" || said != "ok" || noted != "ok" || idle != "ok")
        return "the chat commands do not answer";
    if (unknownChat.rfind("error use chat poll", 0) != 0 || emptySay.rfind("error give the text", 0) != 0)
        return "the chat takes a wrong command";

    // The instruction watch: debug registers on Windows, perf events of the kernel on Linux.
    std::string started = client.Request("writes start " + HexAddress(&g_remoteWritten) + " 4");
#ifndef _WIN32
    if (started.rfind("error the kernel does not let the game watch its memory", 0) == 0) {
        // Only where the kernel refuses perf events to unprivileged
        // processes, or a container blocks them: the CI lets them through.
        int paranoid = 2;
        if (FILE* file = fopen("/proc/sys/kernel/perf_event_paranoid", "r")) {
            if (fscanf(file, "%d", &paranoid) != 1)
                paranoid = 2;
            fclose(file);
        }
        bool container = access("/.dockerenv", F_OK) == 0 || access("/run/.containerenv", F_OK) == 0;
        printf("writes start: %s\n", started.c_str());
        if (paranoid <= 2 && !container)
            return "the instruction watch was refused while kernel.perf_event_paranoid allows it";
        return "";
    }
#endif
    for (int i = 1; i <= 5; ++i)
        WriteWatched(i);
    std::string hits = RemotePayload(client, client.Request("writes results"));
    std::string stopped = client.Request("writes stop");
    WriteWatched(6);
    std::string after = RemotePayload(client, client.Request("writes results"));
    printf("writes start: %s\nwrites results:\n%swrites stop: %s\n", started.c_str(), hits.c_str(), stopped.c_str());
    size_t line = hits.find(" " + name + "+0x");
    if (started != "ok" || line == std::string::npos)
        return "the instruction watch did not find the instruction of the host that writes the value";
    if (atoi(hits.c_str() + hits.find(' ', line + 1) + 1) != 5)
        return "the instruction watch did not count the 5 writes";
    if (stopped != "ok" || after != hits)
        return "the instruction watch went on after writes stop";
    std::string misaligned = client.Request("writes start " + HexAddress(reinterpret_cast<const volatile char*>(&g_remoteWritten) + 1) + " 4");
    if (misaligned.rfind("error the address must be a multiple of the size", 0) != 0)
        return "the instruction watch took a misaligned address";
    return "";
}
