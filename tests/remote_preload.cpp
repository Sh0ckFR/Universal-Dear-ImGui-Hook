// The hook with the remote control on, preloaded by the remote scenario of
// host_linux.cpp: served on the socket uih-host-<pid of the host>.
#include <unistd.h>

#include <string>

#include "uih.h"

__attribute__((constructor)) static void Load() {
    std::string socket = "uih-host-" + std::to_string(getpid());
    uih::Config config;
    config.remoteControl = true;
    config.remotePipe = socket.c_str();
    config.debugLog = true;
    uih::Start(config);
}
