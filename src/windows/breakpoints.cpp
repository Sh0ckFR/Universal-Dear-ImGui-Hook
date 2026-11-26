// The instructions that write an address, found as the data breakpoint of a
// debugger finds them, without a debugger: debug register 3 of every thread of
// the game watches the address, and a vectored exception handler counts the
// instructions that hit it before the thread goes on. For the "writes"
// command of src/advanced.cpp.
//
// A thread cannot change its own debug registers: a thread of its own
// suspends each thread of the game in turn, sets them, and resumes it, with
// no allocation and no lock while a thread is suspended.
#include "internal.h"

#include <tlhelp32.h>

#include <algorithm>
#include <type_traits>

namespace uih::detail::remote {
namespace {

constexpr int kSlot = 3;
constexpr size_t kMaxHits = 256;
// Threads started since the watch began are watched this often.
constexpr ULONGLONG kRefreshMs = 500;

struct HitSlot {
    std::atomic<uintptr_t> next{0};
    std::atomic<uint64_t> count{0};
};
HitSlot g_hits[kMaxHits];

std::mutex g_mutex;
PVOID g_handler = nullptr;
bool g_active = false;
uintptr_t g_address = 0;
// Bits 16 to 19 of the slot in DR7: the condition, then the length.
unsigned g_condition = 0;
std::vector<DWORD> g_watched;
ULONGLONG g_refreshedAt = 0;

// What the helper thread does.
struct Job {
    bool watch = false;
    uintptr_t address = 0;
    unsigned condition = 0;
    // Watch: threads to leave alone, already watched. Unwatch: the threads to unwatch.
    std::vector<DWORD> listed;
    std::vector<DWORD> done;
};
HANDLE g_refreshThread = nullptr;
Job* g_refreshJob = nullptr;

LONG CALLBACK OnSingleStep(EXCEPTION_POINTERS* info) {
    CONTEXT* context = info->ContextRecord;
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !(context->Dr6 & (1u << kSlot)))
        return EXCEPTION_CONTINUE_SEARCH;
    context->Dr6 &= ~static_cast<std::remove_reference_t<decltype(context->Dr6)>>(1u << kSlot);
#ifdef _WIN64
    uintptr_t next = context->Rip;
#else
    uintptr_t next = context->Eip;
#endif
    // Open addressing, without a lock: this runs in the threads of the game.
    size_t start = (next >> 1) % kMaxHits;
    for (size_t i = 0; i < kMaxHits; ++i) {
        HitSlot& slot = g_hits[(start + i) % kMaxHits];
        uintptr_t expected = 0;
        if (slot.next.compare_exchange_strong(expected, next) || expected == next) {
            slot.count.fetch_add(1, std::memory_order_relaxed);
            break;
        }
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

std::vector<DWORD> ProcessThreads() {
    std::vector<DWORD> threads;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return threads;
    DWORD process = GetCurrentProcessId();
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry))
        if (entry.th32OwnerProcessID == process)
            threads.push_back(entry.th32ThreadID);
    CloseHandle(snapshot);
    return threads;
}

bool SetSlot(DWORD id, const Job& job) {
    HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME | THREAD_QUERY_INFORMATION, FALSE, id);
    if (!thread)
        return false;
    bool done = false;
    if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
        CONTEXT context{};
        context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(thread, &context)) {
            using Register = std::remove_reference_t<decltype(context.Dr7)>;
            Register dr7 = context.Dr7;
            dr7 &= ~(Register{3} << (kSlot * 2));
            dr7 &= ~(Register{0xf} << (16 + kSlot * 4));
            if (job.watch) {
                context.Dr3 = job.address;
                dr7 |= Register{1} << (kSlot * 2);
                dr7 |= static_cast<Register>(job.condition) << (16 + kSlot * 4);
            }
            context.Dr7 = dr7;
            done = SetThreadContext(thread, &context) != FALSE;
        }
        ResumeThread(thread);
    }
    CloseHandle(thread);
    return done;
}

DWORD WINAPI RunJob(void* parameter) {
    Job& job = *static_cast<Job*>(parameter);
    DWORD self = GetCurrentThreadId();
    std::vector<DWORD> threads = job.watch ? ProcessThreads() : job.listed;
    job.done.reserve(threads.size());
    for (DWORD id : threads) {
        if (id == self || (job.watch && std::find(job.listed.begin(), job.listed.end(), id) != job.listed.end()))
            continue;
        if (SetSlot(id, job))
            job.done.push_back(id);
    }
    return 0;
}

// Runs `job` on a thread of its own and waits for it.
bool RunJobNow(Job& job) {
    HANDLE thread = CreateThread(nullptr, 0, RunJob, &job, 0, nullptr);
    if (!thread)
        return false;
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    return true;
}

// With g_mutex held: waits for the refresh in progress and keeps what it watched.
void FinishRefresh(bool wait) {
    if (!g_refreshThread || WaitForSingleObject(g_refreshThread, wait ? INFINITE : 0) != WAIT_OBJECT_0)
        return;
    CloseHandle(g_refreshThread);
    g_refreshThread = nullptr;
    if (g_active)
        g_watched.insert(g_watched.end(), g_refreshJob->done.begin(), g_refreshJob->done.end());
    delete g_refreshJob;
    g_refreshJob = nullptr;
}

// With g_mutex held.
void Unwatch() {
    FinishRefresh(true);
    if (!g_active)
        return;
    Job job;
    job.listed = g_watched;
    RunJobNow(job);
    g_watched.clear();
    g_active = false;
}

} // namespace

std::string WatchInstructions(uintptr_t address, unsigned size, bool access) {
#ifdef _WIN64
    const bool sizeOk = size == 1 || size == 2 || size == 4 || size == 8;
#else
    const bool sizeOk = size == 1 || size == 2 || size == 4;
#endif
    if (!sizeOk)
        return sizeof(void*) == 8 ? "the size is 1, 2, 4 or 8 bytes" : "the size is 1, 2 or 4 bytes";
    if (address % size)
        return "the address must be a multiple of the size: the processor watches aligned values";
    // DR7 lengths: 00 one byte, 01 two, 11 four, 10 eight. Conditions: 01 writes, 11 reads and writes.
    unsigned length = size == 1 ? 0 : size == 2 ? 1 : size == 4 ? 3 : 2;
    std::lock_guard<std::mutex> lock(g_mutex);
    Unwatch();
    for (HitSlot& slot : g_hits) {
        slot.next = 0;
        slot.count = 0;
    }
    if (!g_handler)
        g_handler = AddVectoredExceptionHandler(1, OnSingleStep);
    if (!g_handler)
        return "the exception handler cannot be installed";
    Job job;
    job.watch = true;
    job.address = address;
    job.condition = (access ? 3u : 1u) | length << 2;
    if (!RunJobNow(job) || job.done.empty())
        return "the debug registers of the threads of the game cannot be set";
    g_active = true;
    g_address = address;
    g_condition = job.condition;
    g_watched = job.done;
    g_refreshedAt = GetTickCount64();
    return "";
}

void UnwatchInstructions() {
    std::lock_guard<std::mutex> lock(g_mutex);
    Unwatch();
}

void ShutdownInstructionWatch() {
    std::lock_guard<std::mutex> lock(g_mutex);
    Unwatch();
    if (g_handler) {
        // A thread may have hit the address just before: its exception must
        // still find the handler.
        Sleep(100);
        RemoveVectoredExceptionHandler(g_handler);
        g_handler = nullptr;
    }
}

std::vector<InstructionHit> InstructionHits() {
    std::vector<InstructionHit> hits;
    for (const HitSlot& slot : g_hits) {
        uintptr_t next = slot.next.load();
        if (next)
            hits.push_back({next, slot.count.load()});
    }
    std::sort(hits.begin(), hits.end(), [](const InstructionHit& a, const InstructionHit& b) { return a.count > b.count; });
    return hits;
}

// The rendering thread never waits here: the threads are watched by a thread
// of their own, collected at a later frame.
void RefreshInstructionWatch() {
    std::unique_lock<std::mutex> lock(g_mutex, std::try_to_lock);
    if (!lock.owns_lock())
        return;
    FinishRefresh(false);
    if (!g_active || g_refreshThread || GetTickCount64() - g_refreshedAt < kRefreshMs)
        return;
    g_refreshedAt = GetTickCount64();
    auto* job = new Job;
    job->watch = true;
    job->address = g_address;
    job->condition = g_condition;
    job->listed = g_watched;
    g_refreshThread = CreateThread(nullptr, 0, RunJob, job, 0, nullptr);
    if (g_refreshThread)
        g_refreshJob = job;
    else
        delete job;
}

} // namespace uih::detail::remote
