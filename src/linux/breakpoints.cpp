// The instructions that write an address, found as the data breakpoint of a
// debugger finds them, without a debugger: a hardware breakpoint of the
// kernel's perf events on every thread of the game, each sampling the
// instruction that hit it into a small ring buffer, read at every frame. For
// the "writes" command of src/advanced.cpp; src/windows/breakpoints.cpp sets
// the debug registers itself.
//
// The game gets no signal: the kernel records the hit and the thread goes on.
// Unprivileged processes may do this on their own threads while
// kernel.perf_event_paranoid is 2 or lower.
#include "internal.h"

#include <dirent.h>
#include <linux/hw_breakpoint.h>
#include <linux/perf_event.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace uih::detail::remote {
namespace {

constexpr size_t kMaxHits = 256;
// A file descriptor each: the game keeps the rest.
constexpr size_t kMaxThreads = 512;
// Threads started since the watch began are watched this often.
constexpr auto kRefresh = std::chrono::milliseconds(500);

struct Watched {
    pid_t tid;
    int fd;
    void* ring;
};

std::mutex g_mutex;
bool g_active = false;
perf_event_attr g_attr{};
std::vector<Watched> g_watched;
std::vector<InstructionHit> g_hits;
uint64_t g_lost = 0;
std::chrono::steady_clock::time_point g_refreshedAt;
size_t g_page = 0;

// The metadata page, then one page of samples.
size_t RingSize() { return g_page * 2; }

std::vector<pid_t> ProcessThreads() {
    std::vector<pid_t> threads;
    DIR* tasks = opendir("/proc/self/task");
    if (!tasks)
        return threads;
    while (dirent* entry = readdir(tasks)) {
        char* end = nullptr;
        long tid = strtol(entry->d_name, &end, 10);
        if (tid > 0 && *end == 0)
            threads.push_back(static_cast<pid_t>(tid));
    }
    closedir(tasks);
    return threads;
}

void Count(uintptr_t ip) {
#ifdef __aarch64__
    // A watchpoint stops on the instruction itself, which is 4 bytes long:
    // the hits are kept as on x86, by the instruction after.
    ip += 4;
#endif
    for (InstructionHit& hit : g_hits)
        if (hit.next == ip) {
            ++hit.count;
            return;
        }
    if (g_hits.size() < kMaxHits)
        g_hits.push_back({ip, 1});
}

// With g_mutex held: counts the samples the kernel wrote since the last call.
void Drain(Watched& watched) {
    auto* meta = static_cast<perf_event_mmap_page*>(watched.ring);
    const uint8_t* data = static_cast<const uint8_t*>(watched.ring) + g_page;
    const uint64_t size = g_page;
    uint64_t head = __atomic_load_n(&meta->data_head, __ATOMIC_ACQUIRE);
    uint64_t tail = meta->data_tail;
    // A record may wrap around the end of the buffer.
    auto copy = [&](uint64_t at, void* out, size_t bytes) {
        for (size_t i = 0; i < bytes; ++i)
            static_cast<uint8_t*>(out)[i] = data[(at + i) % size];
    };
    while (tail + sizeof(perf_event_header) <= head) {
        perf_event_header header;
        copy(tail, &header, sizeof(header));
        if (header.size < sizeof(header) || tail + header.size > head)
            break;
        if (header.type == PERF_RECORD_SAMPLE && header.size >= sizeof(header) + sizeof(uint64_t)) {
            uint64_t ip = 0;
            copy(tail + sizeof(header), &ip, sizeof(ip));
            Count(static_cast<uintptr_t>(ip));
        } else if (header.type == PERF_RECORD_LOST && header.size >= sizeof(header) + 2 * sizeof(uint64_t)) {
            uint64_t lost = 0;
            copy(tail + sizeof(header) + sizeof(uint64_t), &lost, sizeof(lost));
            g_lost += lost;
        }
        tail += header.size;
    }
    __atomic_store_n(&meta->data_tail, tail, __ATOMIC_RELEASE);
}

void Close(Watched& watched) {
    munmap(watched.ring, RingSize());
    close(watched.fd);
}

// With g_mutex held: the breakpoint on `tid`, or the errno of the failure.
int Open(pid_t tid) {
    int fd = static_cast<int>(syscall(SYS_perf_event_open, &g_attr, tid, -1, -1, PERF_FLAG_FD_CLOEXEC));
    if (fd < 0)
        return errno;
    void* ring = mmap(nullptr, RingSize(), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ring == MAP_FAILED) {
        int error = errno;
        close(fd);
        return error;
    }
    g_watched.push_back({tid, fd, ring});
    return 0;
}

// With g_mutex held: watches the threads started since, and lets go of
// those that ended.
void Refresh() {
    std::vector<pid_t> threads = ProcessThreads();
    for (size_t i = 0; i < g_watched.size();) {
        if (std::find(threads.begin(), threads.end(), g_watched[i].tid) == threads.end()) {
            Drain(g_watched[i]);
            Close(g_watched[i]);
            g_watched.erase(g_watched.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
    for (pid_t tid : threads) {
        if (g_watched.size() >= kMaxThreads)
            break;
        bool known = std::any_of(g_watched.begin(), g_watched.end(), [tid](const Watched& watched) { return watched.tid == tid; });
        if (!known)
            Open(tid);
    }
    g_refreshedAt = std::chrono::steady_clock::now();
}

// With g_mutex held.
void Unwatch() {
    for (Watched& watched : g_watched) {
        Drain(watched);
        Close(watched);
    }
    g_watched.clear();
    g_active = false;
}

std::string OpenError(int error) {
    if (error == EACCES || error == EPERM) {
        int paranoid = 2;
        if (FILE* file = fopen("/proc/sys/kernel/perf_event_paranoid", "r")) {
            if (fscanf(file, "%d", &paranoid) != 1)
                paranoid = 2;
            fclose(file);
        }
        return "the kernel does not let the game watch its memory: kernel.perf_event_paranoid is " + std::to_string(paranoid) +
               (paranoid > 2 ? ", run sudo sysctl kernel.perf_event_paranoid=2"
                             : ", or a container or a seccomp filter blocks perf_event_open, or the limit of locked memory is reached");
    }
    if (error == ENOSPC)
        return "the debug registers of the processor are all in use, by a debugger or another watch";
    if (error == ENOENT || error == ENODEV || error == EOPNOTSUPP || error == ENOSYS)
        return std::string("the kernel or the processor has no hardware breakpoints: ") + strerror(error);
    return std::string("the hardware breakpoint cannot be set: ") + strerror(error);
}

} // namespace

std::string WatchInstructions(uintptr_t address, unsigned size, bool access) {
    const bool sizeOk = size == 1 || size == 2 || size == 4 || (size == 8 && sizeof(void*) == 8);
    if (!sizeOk)
        return sizeof(void*) == 8 ? "the size is 1, 2, 4 or 8 bytes" : "the size is 1, 2 or 4 bytes";
    if (address % size)
        return "the address must be a multiple of the size: the processor watches aligned values";
    std::lock_guard<std::mutex> lock(g_mutex);
    Unwatch();
    g_hits.clear();
    g_lost = 0;
    g_page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    g_attr = perf_event_attr{};
    g_attr.size = sizeof(g_attr);
    g_attr.type = PERF_TYPE_BREAKPOINT;
    g_attr.bp_type = access ? HW_BREAKPOINT_RW : HW_BREAKPOINT_W;
    g_attr.bp_addr = address;
    g_attr.bp_len = size;
    g_attr.sample_period = 1;
    g_attr.sample_type = PERF_SAMPLE_IP;
    g_attr.exclude_kernel = 1;
    g_attr.exclude_hv = 1;
    // Nobody waits on the buffer: it is read at every frame.
    g_attr.watermark = 1;
    g_attr.wakeup_watermark = static_cast<uint32_t>(g_page);
    std::vector<pid_t> threads = ProcessThreads();
    if (threads.empty())
        return "the threads of the game cannot be listed from /proc/self/task";
    int first = 0;
    for (pid_t tid : threads) {
        if (g_watched.size() >= kMaxThreads)
            break;
        int error = Open(tid);
        if (error && !first)
            first = error;
    }
    if (g_watched.empty())
        return OpenError(first);
    g_active = true;
    g_refreshedAt = std::chrono::steady_clock::now();
    return "";
}

void UnwatchInstructions() {
    std::lock_guard<std::mutex> lock(g_mutex);
    Unwatch();
}

void ShutdownInstructionWatch() { UnwatchInstructions(); }

std::vector<InstructionHit> InstructionHits() {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (Watched& watched : g_watched)
        Drain(watched);
    std::vector<InstructionHit> hits = g_hits;
    std::sort(hits.begin(), hits.end(), [](const InstructionHit& a, const InstructionHit& b) { return a.count > b.count; });
    return hits;
}

// At every frame: the buffers hold one page of samples each, a few hundred
// hits, so they are emptied often.
void RefreshInstructionWatch() {
    std::unique_lock<std::mutex> lock(g_mutex, std::try_to_lock);
    if (!lock.owns_lock() || !g_active)
        return;
    for (Watched& watched : g_watched)
        Drain(watched);
    if (std::chrono::steady_clock::now() - g_refreshedAt >= kRefresh)
        Refresh();
}

} // namespace uih::detail::remote
