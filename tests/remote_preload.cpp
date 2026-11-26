// The hook with the remote control on, preloaded by the remote scenario of
// host_linux.cpp, with the memory commands, the advanced features and the
// tools of remote_tools.h: served on the socket uih-host-<pid of the host>.
#include <unistd.h>

#include <string>

#include "uih.h"

#define UIH_TEST_TOOLS_HOOK
#define UIH_TEST_TOOLS_REGISTER_ONLY
#include "remote_tools.h"

__attribute__((constructor)) static void Load() {
    std::string socket = "uih-host-" + std::to_string(getpid());
    uih::Config config;
    config.remoteControl = true;
    config.remoteMemory = true;
    config.aiAdvancedFeatures = true;
    config.remotePipe = socket.c_str();
    config.debugLog = true;
    RegisterTestTools();
    uih::Start(config);
}
