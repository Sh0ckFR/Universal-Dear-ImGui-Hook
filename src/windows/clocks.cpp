// The clocks the game reads, hooked for the speed the assistant sets
// (src/timing.cpp): RtlQueryPerformanceCounter, which kernel32's and
// kernelbase's QueryPerformanceCounter jump to on Windows (Wine calls it from
// its own, hooked too), GetTickCount, GetTickCount64 and timeGetTime. Only the calls of the game's code are scaled: the
// executable, the DLLs of its folder, its runtime and code generated at run
// time. Windows and the drivers, under the Windows directory, and the overlay
// keep real time, so that presenting, audio and the overlay keep working while
// the game is paused.
#include "../internal.h"
#include "platform.h"

#include <cwctype>
#include <string>

#ifdef _MSC_VER
#include <intrin.h>
#define UIH_CALLER() _ReturnAddress()
#else
#define UIH_CALLER() __builtin_return_address(0)
#endif

namespace uih::detail {
namespace {

using QpcFn = BOOLEAN(NTAPI*)(LARGE_INTEGER*);
using QueryFn = BOOL(WINAPI*)(LARGE_INTEGER*);
using TickFn = DWORD(WINAPI*)();
using Tick64Fn = ULONGLONG(WINAPI*)();

QpcFn g_qpc = nullptr;
QueryFn g_qpcKernel32 = nullptr;
QueryFn g_qpcKernelBase = nullptr;
TickFn g_tickKernel32 = nullptr;
TickFn g_tickKernelBase = nullptr;
Tick64Fn g_tick64Kernel32 = nullptr;
Tick64Fn g_tick64KernelBase = nullptr;
TickFn g_timeGetTime = nullptr;
std::atomic<bool> g_hooked{false};

// Which code keeps real time, by address range: modules, and the regions of
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
std::wstring g_windowsDirectory;
thread_local bool t_classifying = false;

std::wstring Lower(std::wstring text) {
    for (wchar_t& c : text)
        c = static_cast<wchar_t>(towlower(c));
    return text;
}

// The C and C++ runtimes that games load from the Windows directory run the game's code.
bool IsRuntime(const std::wstring& name) {
    for (const wchar_t* prefix : {L"msvcp", L"vcruntime", L"ucrtbase", L"msvcrt", L"concrt", L"vcomp", L"api-ms-win-crt-"}) {
        if (name.rfind(prefix, 0) == 0)
            return true;
    }
    return false;
}

bool KeepsRealTime(HMODULE module) {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&KeepsRealTime),
                       &self);
    if (module == self)
        return true;
    wchar_t path[MAX_PATH] = {};
    DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return false;
    std::wstring lower = Lower(path);
    if (g_windowsDirectory.empty() || lower.rfind(g_windowsDirectory, 0) != 0)
        return false;
    size_t slash = lower.find_last_of(L'\\');
    return !IsRuntime(slash == std::wstring::npos ? lower : lower.substr(slash + 1));
}

size_t ImageSize(uintptr_t base) {
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return 0;
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    return nt->Signature == IMAGE_NT_SIGNATURE ? nt->OptionalHeader.SizeOfImage : 0;
}

// True when the code at `address`, which called a clock, keeps real time.
bool RealTimeCaller(void* address) {
    auto at = reinterpret_cast<uintptr_t>(address);
    int count = g_rangeCount.load(std::memory_order_acquire);
    for (int i = 0; i < count; ++i) {
        if (at >= g_ranges[i].begin && at < g_ranges[i].end)
            return g_ranges[i].real;
    }
    // The functions below may read a clock themselves.
    if (t_classifying)
        return true;
    t_classifying = true;
    Range range{at, at + 1, false};
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(address, &info, sizeof(info)) == sizeof(info)) {
        auto base = reinterpret_cast<uintptr_t>(info.AllocationBase);
        size_t size = info.Type == MEM_IMAGE ? ImageSize(base) : 0;
        if (size)
            range = {base, base + size, KeepsRealTime(reinterpret_cast<HMODULE>(base))};
        else
            range = {reinterpret_cast<uintptr_t>(info.BaseAddress), reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize, false};
    }
    t_classifying = false;
    std::lock_guard<std::mutex> lock(g_rangeMutex);
    int index = g_rangeCount.load(std::memory_order_relaxed);
    if (index < kMaxRanges) {
        g_ranges[index] = range;
        g_rangeCount.store(index + 1, std::memory_order_release);
    }
    return range.real;
}

bool ScaleFor(void* caller) { return timing::Scaling() && !RealTimeCaller(caller); }

BOOLEAN NTAPI HookQpc(LARGE_INTEGER* counter) {
    ScopedCall call;
    BOOLEAN result = g_qpc(counter);
    if (result && counter && ScaleFor(UIH_CALLER()))
        counter->QuadPart = timing::GameTime(timing::kQpc, counter->QuadPart);
    return result;
}

// Their calls to RtlQueryPerformanceCounter come from the Windows directory: not scaled twice.
BOOL WINAPI HookQpcKernel32(LARGE_INTEGER* counter) {
    ScopedCall call;
    BOOL result = g_qpcKernel32(counter);
    if (result && counter && ScaleFor(UIH_CALLER()))
        counter->QuadPart = timing::GameTime(timing::kQpc, counter->QuadPart);
    return result;
}

BOOL WINAPI HookQpcKernelBase(LARGE_INTEGER* counter) {
    ScopedCall call;
    BOOL result = g_qpcKernelBase(counter);
    if (result && counter && ScaleFor(UIH_CALLER()))
        counter->QuadPart = timing::GameTime(timing::kQpc, counter->QuadPart);
    return result;
}

ULONGLONG RealTicks() { return g_tick64KernelBase ? g_tick64KernelBase() : g_tick64Kernel32 ? g_tick64Kernel32() : GetTickCount64(); }

ULONGLONG WINAPI HookTick64Kernel32() {
    ScopedCall call;
    ULONGLONG ticks = g_tick64Kernel32();
    return ScaleFor(UIH_CALLER()) ? static_cast<ULONGLONG>(timing::GameTime(timing::kTickMs, static_cast<int64_t>(ticks))) : ticks;
}

ULONGLONG WINAPI HookTick64KernelBase() {
    ScopedCall call;
    ULONGLONG ticks = g_tick64KernelBase();
    return ScaleFor(UIH_CALLER()) ? static_cast<ULONGLONG>(timing::GameTime(timing::kTickMs, static_cast<int64_t>(ticks))) : ticks;
}

// GetTickCount() is the low 32 bits of GetTickCount64(): both follow one clock.
DWORD WINAPI HookTickKernel32() {
    ScopedCall call;
    if (!ScaleFor(UIH_CALLER()))
        return g_tickKernel32();
    return static_cast<DWORD>(timing::GameTime(timing::kTickMs, static_cast<int64_t>(RealTicks())));
}

DWORD WINAPI HookTickKernelBase() {
    ScopedCall call;
    if (!ScaleFor(UIH_CALLER()))
        return g_tickKernelBase();
    return static_cast<DWORD>(timing::GameTime(timing::kTickMs, static_cast<int64_t>(RealTicks())));
}

DWORD WINAPI HookTimeGetTime() {
    ScopedCall call;
    DWORD ms = g_timeGetTime();
    return ScaleFor(UIH_CALLER()) ? static_cast<DWORD>(timing::GameTime(timing::kTimeGetTime, ms)) : ms;
}

template <typename Fn>
void HookClock(HMODULE module, const char* name, Fn detour, Fn* original, void* already = nullptr) {
    void* target = module ? GetProc<void*>(module, name) : nullptr;
    // kernel32 may lead to the same code as kernelbase, as under Wine.
    if (target && target != already)
        CreateHook(name, target, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(original));
}

bool Wanted() {
    const Config& config = GetConfig();
    return config.remoteControl && config.aiAdvancedFeatures;
}

bool InstallWinmm() {
    if (!Wanted())
        return false;
    HookClock(GetModuleHandleW(L"winmm.dll"), "timeGetTime", HookTimeGetTime, &g_timeGetTime);
    return g_timeGetTime != nullptr;
}

void UninstallWinmm() { g_timeGetTime = nullptr; }

void NoTeardown() {}

} // namespace

const Renderer kClockHooks = {"Clocks", L"winmm.dll", InstallWinmm, UninstallWinmm, NoTeardown};

namespace clocks {

void InstallHooks() {
    if (!Wanted())
        return;
    wchar_t directory[MAX_PATH] = {};
    UINT length = GetWindowsDirectoryW(directory, MAX_PATH);
    if (length > 0 && length < MAX_PATH)
        g_windowsDirectory = Lower(directory) + L"\\";
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    HMODULE kernelBase = GetModuleHandleW(L"kernelbase.dll");
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    void* rtlQpc = ntdll ? GetProc<void*>(ntdll, "RtlQueryPerformanceCounter") : nullptr;
    void* baseQpc = kernelBase ? GetProc<void*>(kernelBase, "QueryPerformanceCounter") : nullptr;
    HookClock(ntdll, "RtlQueryPerformanceCounter", HookQpc, &g_qpc);
    HookClock(kernelBase, "QueryPerformanceCounter", HookQpcKernelBase, &g_qpcKernelBase, rtlQpc);
    // On Windows, a jump to kernelbase's, whose hook would then see kernel32 as the caller.
    void* kernel32Qpc = kernel32 ? GetProc<void*>(kernel32, "QueryPerformanceCounter") : nullptr;
    auto* jump = static_cast<const uint8_t*>(kernel32Qpc);
    if (kernel32Qpc && kernel32Qpc != rtlQpc && kernel32Qpc != baseQpc && !(jump[0] == 0xFF && jump[1] == 0x25) &&
        !(jump[0] == 0x48 && jump[1] == 0xFF && jump[2] == 0x25))
        HookClock(kernel32, "QueryPerformanceCounter", HookQpcKernel32, &g_qpcKernel32);
    HookClock(kernelBase, "GetTickCount64", HookTick64KernelBase, &g_tick64KernelBase);
    HookClock(kernel32, "GetTickCount64", HookTick64Kernel32, &g_tick64Kernel32, kernelBase ? GetProc<void*>(kernelBase, "GetTickCount64") : nullptr);
    HookClock(kernelBase, "GetTickCount", HookTickKernelBase, &g_tickKernelBase);
    HookClock(kernel32, "GetTickCount", HookTickKernel32, &g_tickKernel32, kernelBase ? GetProc<void*>(kernelBase, "GetTickCount") : nullptr);
    g_hooked = g_qpc != nullptr;
}

void UninstallHooks() {
    g_hooked = false;
    g_qpc = nullptr;
    g_qpcKernel32 = g_qpcKernelBase = nullptr;
    g_tickKernel32 = g_tickKernelBase = nullptr;
    g_tick64Kernel32 = g_tick64KernelBase = nullptr;
}

} // namespace clocks

namespace timing {

bool ClocksHooked() { return g_hooked; }

bool RealClock(Clock clock, int64_t& real) {
    if (clock == kQpc) {
        LARGE_INTEGER counter{};
        if (g_qpc ? !g_qpc(&counter) : !QueryPerformanceCounter(&counter))
            return false;
        real = counter.QuadPart;
        return true;
    }
    if (clock == kTickMs) {
        real = static_cast<int64_t>(RealTicks());
        return true;
    }
    if (clock == kTimeGetTime && g_timeGetTime) {
        real = g_timeGetTime();
        return true;
    }
    return false;
}

} // namespace timing
} // namespace uih::detail
