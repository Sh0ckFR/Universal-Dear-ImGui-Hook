// The memory commands of the remote control, driven as remote/uih_mcp.py does
// by the Windows and the Linux test hosts, on values of the host itself.
#pragma once

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// Values the commands find and change.
inline volatile int32_t g_remoteValue = 0;
inline volatile float g_remoteFloat = 0.0f;
inline volatile uint8_t g_remotePattern[12] = {0x5a, 0xc3, 0x17, 0xe9, 0x00, 0x42, 0x99, 0x3c, 0x7d, 0x01, 0xfe, 0x66};

// Reads the text that follows an answer "ok <bytes>".
template <typename Client>
std::string RemotePayload(Client& client, const std::string& answer) {
    if (answer.rfind("ok ", 0) != 0)
        return "";
    std::string text(strtoul(answer.c_str() + 3, nullptr, 10), '\0');
    return client.Read(reinterpret_cast<uint8_t*>(text.data()), text.size()) ? text : "";
}

inline std::string HexAddress(const volatile void* address) {
    char text[32];
    snprintf(text, sizeof(text), "0x%" PRIxPTR, reinterpret_cast<uintptr_t>(address));
    return text;
}

// Empty when the commands worked, the reason otherwise.
template <typename Client>
std::string CheckRemoteMemory(Client& client) {
    std::string address = HexAddress(&g_remoteValue);

    g_remoteValue = 734251;
    std::string found = client.Request("find i32 734251");
    g_remoteValue = 734252;
    std::string next = client.Request("next 734252");
    std::string unchanged = client.Request("next unchanged");
    std::string results = RemotePayload(client, client.Request("results 50"));
    printf("find: %s, next: %s, unchanged: %s\nresults:\n%s", found.c_str(), next.c_str(), unchanged.c_str(), results.c_str());
    if (found.rfind("ok ", 0) != 0 || next.rfind("ok ", 0) != 0 || unchanged.rfind("ok ", 0) != 0)
        return "the value scan failed";
    if (results.find(address + " 734252 ") == std::string::npos)
        return "the value scan did not find the value of the host";

    g_remoteFloat = 12.75f;
    std::string floatFound = client.Request("find f32 12.75");
    std::string floatResults = RemotePayload(client, client.Request("results 1000"));
    printf("find f32: %s\n", floatFound.c_str());
    if (floatResults.find(HexAddress(&g_remoteFloat) + " 12.75 ") == std::string::npos)
        return "the float scan did not find the value of the host";

    std::string write = client.Request("write " + address + " 9f 86 01 00");
    printf("write: %s, value %d\n", write.c_str(), static_cast<int>(g_remoteValue));
    if (write != "ok" || g_remoteValue != 99999)
        return "the write did not change the value of the host";

    std::string modules = RemotePayload(client, client.Request("modules"));
    size_t line = modules.find("uih_host");
    if (line == std::string::npos)
        return "the module list does not show the host";
    size_t start = modules.rfind('\n', line);
    start = start == std::string::npos ? 0 : start + 1;
    uintptr_t base = strtoull(modules.c_str() + start, nullptr, 16);
    std::string name = modules.substr(line, modules.find('\n', line) - line);
    printf("modules: %zu bytes, executable %s at 0x%" PRIxPTR "\n", modules.size(), name.c_str(), base);
    if (base == 0)
        return "the module list does not show the base of the host";

    char relative[300];
    snprintf(relative, sizeof(relative), "read %s+%" PRIxPTR " 4", name.c_str(), reinterpret_cast<uintptr_t>(&g_remoteValue) - base);
    std::string read = client.Request(relative);
    int32_t value = 0;
    bool readBack = read == "ok 4" && client.Read(reinterpret_cast<uint8_t*>(&value), sizeof(value));
    printf("%s: %s, value %d\n", relative, read.c_str(), value);
    if (!readBack || value != 99999)
        return "the read at module+offset did not return the value of the host";

    std::string pattern = RemotePayload(client, client.Request("pattern 5a c3 17 e9 ?? 42 99 3c 7d ?? fe 66"));
    printf("pattern:\n%s", pattern.c_str());
    if (pattern.find(HexAddress(g_remotePattern) + " ") == std::string::npos)
        return "the pattern scan did not find the bytes of the host";
    return "";
}
