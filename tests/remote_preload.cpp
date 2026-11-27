// The hook with the remote control on, preloaded by the remote scenario of
// host_linux.cpp, with the memory commands, the advanced features and the
// tools of remote_tools.h: served on the socket uih-host-<pid of the host>.
// With UIH_TEST_C_TOOLS=1, the tools are registered through the C interface
// (tests/c_api.c) instead.
#include <stdlib.h>
#include <unistd.h>

#include <string>

#include "uih.h"

#define UIH_TEST_TOOLS_HOOK
#define UIH_TEST_TOOLS_REGISTER_ONLY
#include "remote_tools.h"

extern "C" bool uih_test_register_c_tools(void (*ran)(void));

__attribute__((constructor)) static void Load() {
    std::string socket = "uih-host-" + std::to_string(getpid());
    uih::Config config;
    config.remoteControl = true;
    config.remoteMemory = true;
    config.aiAdvancedFeatures = true;
    config.remotePipe = socket.c_str();
    config.debugLog = true;
    const char* cTools = getenv("UIH_TEST_C_TOOLS");
    if (cTools && *cTools == '1')
        uih_test_register_c_tools([] {});
    else
        RegisterTestTools();
    uih::Start(config);
}
