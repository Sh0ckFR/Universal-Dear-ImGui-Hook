// The client of the remote control the Linux test hosts drive tests/remote_preload.cpp with.
#pragma once

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

// Socket of tests/remote_preload.cpp, named as src/linux/remote.cpp does.
inline std::string RemoteSocketPath() {
    std::string name = "uih-host-" + std::to_string(getpid());
    const char* runtime = getenv("XDG_RUNTIME_DIR");
    return runtime && *runtime ? std::string(runtime) + "/" + name : "/tmp/" + name + "-" + std::to_string(getuid());
}

// A client of the remote control, as remote/uih_mcp.py is.
class RemoteClient {
public:
    ~RemoteClient() {
        if (socket_ >= 0)
            close(socket_);
    }

    // Waits for the socket, opened at the first frame of the overlay.
    bool Open() {
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::string path = RemoteSocketPath();
        memcpy(address.sun_path, path.c_str(), path.size() + 1);
        for (int attempt = 0; attempt < 200; ++attempt) {
            socket_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
            if (connect(socket_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0)
                return true;
            close(socket_);
            socket_ = -1;
            usleep(50000);
        }
        return false;
    }

    // Sends one command and returns the line that answers it.
    std::string Request(const std::string& command) {
        std::string line = command + "\n";
        if (send(socket_, line.data(), line.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(line.size()))
            return "";
        std::string answer;
        char c = 0;
        while (recv(socket_, &c, 1, 0) == 1 && c != '\n')
            answer += c;
        return answer;
    }

    bool Read(uint8_t* data, size_t size) {
        while (size > 0) {
            ssize_t received = recv(socket_, data, size, 0);
            if (received <= 0)
                return false;
            data += received;
            size -= static_cast<size_t>(received);
        }
        return true;
    }

private:
    int socket_ = -1;
};

// Runs `work` on another thread while `frame` renders.
template <typename Work, typename Frame>
void WhileRendering(Work&& work, Frame&& frame) {
    std::atomic<bool> finished{false};
    std::thread thread([&] {
        work();
        finished = true;
    });
    while (!finished)
        frame();
    thread.join();
}
