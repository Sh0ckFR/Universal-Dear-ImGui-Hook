// The tools of the game, uih::RegisterTool(), that the Windows and the Linux
// test hosts check the same way. RegisterTestTools() runs where the hook runs,
// which defines UIH_TEST_TOOLS_HOOK before including this file: the Windows
// host, and tests/remote_preload.cpp on Linux. CheckRemoteTools() runs in the
// hosts; include it after remote_memory.h, for RemotePayload().
#pragma once

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>

// Frame of the overlay when the last call of test_add ran: it must run inside one.
inline std::atomic<int> g_toolFrame{-1};

#ifdef UIH_TEST_TOOLS_HOOK
#include <stdexcept>

#include "imgui.h"
#include "uih.h"

inline bool RegisterTestTools() {
    bool added = uih::RegisterTool(
        "test_add", "Adds a and b, and says the sum with a label. Quotes \" and a\nnew line must reach the assistant escaped.",
        R"({"type":"object","properties":{"a":{"type":"number"},"b":{"type":"number"},"label":{"type":"string"}},"required":["a","b"]})",
        [](const std::string& arguments) {
            g_toolFrame = ImGui::GetCurrentContext() ? ImGui::GetFrameCount() : -1;
            double sum = atof(uih::ToolArgument(arguments, "a").c_str()) + atof(uih::ToolArgument(arguments, "b").c_str());
            char text[64];
            snprintf(text, sizeof(text), "%g", sum);
            return uih::ToolArgument(arguments, "label") + " = " + text + "\nnested: " + uih::ToolArgument(arguments, "nested");
        });
    bool failing = uih::RegisterTool("test_fail", "Always fails.", nullptr, [](const std::string&) -> std::string {
        throw std::runtime_error("the test tool failed\non purpose");
    });
    // Replaced below, then removed: neither may be listed.
    bool replaced = uih::RegisterTool("test_gone", "First.", nullptr, [](const std::string&) { return std::string("first"); }) &&
                    uih::RegisterTool("test_gone", "Second.", nullptr, [](const std::string&) { return std::string("second"); });
    uih::UnregisterTool("test_gone");
    // Refused: a name with a space, a schema that is not an object, no callback.
    bool refused = !uih::RegisterTool("bad name", "", nullptr, [](const std::string&) { return std::string(); }) &&
                   !uih::RegisterTool("bad_schema", "", "[1, 2]", [](const std::string&) { return std::string(); }) &&
                   !uih::RegisterTool("no_call", "", nullptr, nullptr);
    return added && failing && replaced && refused;
}
#endif

#ifndef UIH_TEST_TOOLS_REGISTER_ONLY

// Empty when the tools answer as remote/uih_mcp.py expects, the failure
// otherwise. `frameCount` gives the frame of the overlay where the hook runs
// in the host; the Linux host runs it in another library and passes null.
template <typename Client>
std::string CheckRemoteTools(Client& client, int (*frameCount)() = nullptr) {
    std::string listed = RemotePayload(client, client.Request("tools"));
    int frameBefore = frameCount ? frameCount() : 0;
    std::string added = RemotePayload(client, client.Request(R"(tool test_add {"label": "sum of \"x\" é", "a": 2, "b": 3.5, "nested": {"k": [1, "]"]}})"));
    std::string noArguments = RemotePayload(client, client.Request("tool test_add"));
    std::string failed = client.Request("tool test_fail {}");
    std::string unknown = client.Request("tool test_nothing {}");
    std::string malformed = client.Request("tool test_add {\"a\": ");
    printf("tools: %s\ntool test_add: %s\ntool test_add without arguments: %s\ntool test_fail: %s\ntool test_nothing: %s\nmalformed: %s\n",
           listed.c_str(), added.c_str(), noArguments.c_str(), failed.c_str(), unknown.c_str(), malformed.c_str());
    if (listed.find(R"({"name":"test_add","description":"Adds a and b, and says the sum with a label. Quotes \" and a\nnew line must reach the assistant escaped.","inputSchema":{"type":"object")") == std::string::npos ||
        listed.find(R"({"name":"test_fail","description":"Always fails.","inputSchema":{"type":"object","properties":{}}})") == std::string::npos)
        return "the tools of the game are not listed as JSON";
    if (listed.find("test_gone") != std::string::npos || listed.find("bad") != std::string::npos || listed.find("no_call") != std::string::npos)
        return "a removed or refused tool is listed";
    if (added != "sum of \"x\" \xc3\xa9 = 5.5\nnested: {\"k\": [1, \"]\"]}" || noArguments != " = 0\nnested: ")
        return "the tool did not get its arguments, or its answer did not come back";
    if (frameCount && g_toolFrame < frameBefore)
        return "the tool did not run in a frame of the overlay";
    if (failed != "error the test tool failed on purpose")
        return "an exception of a tool does not answer its message";
    if (unknown != "error the game has no tool named test_nothing" || malformed.rfind("error give the arguments", 0) != 0)
        return "an unknown tool or malformed arguments are not refused";
    return "";
}
#endif
