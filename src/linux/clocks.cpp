// The clocks the game reads, for the speed the assistant sets
// (src/timing.cpp): clock_gettime() with the monotonic clocks, which
// std::chrono::steady_clock, SDL_GetTicks() and the engines read. The library
// stands for it once preloaded. Only the calls of the game's code are scaled:
// the executable, its libraries and code generated at run time. The graphics
// and audio drivers, the windowing libraries and the overlay keep real time,
// so that presenting, audio and the overlay keep working while the game is
// paused. The wall clock, CLOCK_REALTIME, always keeps real time.
#include "internal.h"

#include <dlfcn.h>
#include <link.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <cstring>

#define UIH_NO_SANITIZE __attribute__((no_sanitize("address", "undefined")))

namespace uih::detail {
namespace {

using ClockGettimeFn = int (*)(clockid_t, timespec*);

ClockGettimeFn g_next = nullptr;

// Looked up once the library is loaded, never from clock_gettime(): the
// sanitizer runtimes call it from their allocator, inside dlopen(), where
// dlsym() must not run.
__attribute__((constructor(101))) void FindNext() {
    __atomic_store_n(&g_next, reinterpret_cast<ClockGettimeFn>(dlsym(RTLD_NEXT, "clock_gettime")), __ATOMIC_RELEASE);
}

// The clock of the C library, or of the kernel before the library's constructors ran.
UIH_NO_SANITIZE int RealClockGettime(clockid_t id, timespec* now) {
    if (ClockGettimeFn next = __atomic_load_n(&g_next, __ATOMIC_ACQUIRE))
        return next(id, now);
    return static_cast<int>(syscall(SYS_clock_gettime, id, now));
}

// Which code keeps real time, by address range: libraries, and the regions of
// code generated at run time.
struct Range {
    uintptr_t begin;
    uintptr_t end;
    bool real;
};
constexpr int kMaxRanges = 1024;
Range g_ranges[kMaxRanges];
std::atomic<int> g_rangeCount{0};
std::mutex g_rangeMutex;
thread_local bool t_classifying = false;

// Libraries that keep real time, by the start of their file name.
const char* const kRealTimeLibraries[] = {
    "libGL", "libEGL", "libGLX", "libOpenGL", "libvulkan", "libVkLayer", "libnvidia", "libcuda", "libdrm", "libgallium", "libLLVM", "libvk",
    "libpulse", "libasound", "libpipewire", "libjack", "libX", "libxcb", "libwayland", "libdbus", "libsystemd", "libudev", "gameoverlayrenderer",
    "libMangoHud", "libgamemode",
};

bool KeepsRealTime(const char* path, uintptr_t base) {
    Dl_info self{};
    if (dladdr(reinterpret_cast<void*>(&KeepsRealTime), &self) && reinterpret_cast<uintptr_t>(self.dli_fbase) == base)
        return true;
    if (!path)
        return false;
    const char* name = strrchr(path, '/');
    name = name ? name + 1 : path;
    // The Mesa drivers, such as radeonsi_dri.so.
    if (strstr(name, "_dri.so"))
        return true;
    for (const char* prefix : kRealTimeLibraries) {
        if (strncmp(name, prefix, strlen(prefix)) == 0)
            return true;
    }
    return false;
}

struct Search {
    uintptr_t address;
    Range range;
    bool found;
};

int FindObject(dl_phdr_info* info, size_t, void* data) {
    auto* search = static_cast<Search*>(data);
    uintptr_t begin = UINTPTR_MAX;
    uintptr_t end = 0;
    bool inside = false;
    for (int i = 0; i < info->dlpi_phnum; ++i) {
        const auto& header = info->dlpi_phdr[i];
        if (header.p_type != PT_LOAD)
            continue;
        uintptr_t start = info->dlpi_addr + header.p_vaddr;
        uintptr_t stop = start + header.p_memsz;
        begin = start < begin ? start : begin;
        end = stop > end ? stop : end;
        inside = inside || (search->address >= start && search->address < stop);
    }
    if (!inside)
        return 0;
    search->range = {begin, end, KeepsRealTime(info->dlpi_name, info->dlpi_addr)};
    search->found = true;
    return 1;
}

// True when the code at `address`, which called the clock, keeps real time.
bool RealTimeCaller(void* address) {
    auto at = reinterpret_cast<uintptr_t>(address);
    int count = g_rangeCount.load(std::memory_order_acquire);
    for (int i = 0; i < count; ++i) {
        if (at >= g_ranges[i].begin && at < g_ranges[i].end)
            return g_ranges[i].real;
    }
    if (t_classifying)
        return true;
    t_classifying = true;
    // Code generated at run time, by its page.
    Search search{at, {at & ~uintptr_t{4095}, (at & ~uintptr_t{4095}) + 4096, false}, false};
    dl_iterate_phdr(FindObject, &search);
    t_classifying = false;
    std::lock_guard<std::mutex> lock(g_rangeMutex);
    int index = g_rangeCount.load(std::memory_order_relaxed);
    if (index < kMaxRanges) {
        g_ranges[index] = search.range;
        g_rangeCount.store(index + 1, std::memory_order_release);
    }
    return search.range.real;
}

bool Scaled(clockid_t id, timing::Clock& clock) {
    switch (id) {
    case CLOCK_MONOTONIC:
        clock = timing::kMonotonic;
        return true;
    case CLOCK_MONOTONIC_RAW:
        clock = timing::kMonotonicRaw;
        return true;
    case CLOCK_MONOTONIC_COARSE:
        clock = timing::kMonotonicCoarse;
        return true;
    case CLOCK_BOOTTIME:
        clock = timing::kBoottime;
        return true;
    default:
        return false;
    }
}

constexpr int64_t kNanoseconds = 1000000000;

} // namespace

namespace timing {

// Preloaded, the library stands for clock_gettime(), maybe behind a sanitizer
// runtime that calls it; loaded later, the game calls the C library directly.
bool ClocksHooked() {
    static const bool hooked = dlsym(RTLD_DEFAULT, "clock_gettime") != reinterpret_cast<void*>(__atomic_load_n(&g_next, __ATOMIC_ACQUIRE));
    return hooked;
}

bool RealClock(Clock clock, int64_t& real) {
    clockid_t id;
    switch (clock) {
    case kMonotonic:
        id = CLOCK_MONOTONIC;
        break;
    case kMonotonicRaw:
        id = CLOCK_MONOTONIC_RAW;
        break;
    case kMonotonicCoarse:
        id = CLOCK_MONOTONIC_COARSE;
        break;
    case kBoottime:
        id = CLOCK_BOOTTIME;
        break;
    default:
        return false;
    }
    timespec now{};
    if (RealClockGettime(id, &now) != 0)
        return false;
    real = now.tv_sec * kNanoseconds + now.tv_nsec;
    return true;
}

} // namespace timing
} // namespace uih::detail

using namespace uih::detail;

// Called before the library starts, while the process starts: nothing is
// scaled until the speed changes.
extern "C" UIH_NO_SANITIZE int clock_gettime(clockid_t id, timespec* now) noexcept {
    int result = RealClockGettime(id, now);
    timing::Clock clock;
    if (result != 0 || !now || !timing::Scaling() || !Scaled(id, clock) || RealTimeCaller(__builtin_return_address(0)))
        return result;
    int64_t game = timing::GameTime(clock, now->tv_sec * kNanoseconds + now->tv_nsec);
    now->tv_sec = static_cast<time_t>(game / kNanoseconds);
    now->tv_nsec = static_cast<long>(game % kNanoseconds);
    return result;
}
