// The code commands of src/code.cpp on Windows: the protection of the pages
// of code, the threads of the game suspended while their code changes, the
// hooks of the overlay left alone, and the exports of the modules as symbols.
#include "internal.h"

#include <tlhelp32.h>

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>

namespace uih::detail::remote {
namespace {

constexpr DWORD kExecutable = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
// Attempts at a moment when no thread of the game runs the code being changed.
constexpr int kAttempts = 50;
// UNW_FLAG_CHAININFO of the flags of an UNWIND_INFO.
constexpr unsigned kChainInfo = 4;

uintptr_t PageSize() {
    static const uintptr_t size = [] {
        SYSTEM_INFO system{};
        GetSystemInfo(&system);
        return static_cast<uintptr_t>(system.dwPageSize);
    }();
    return size;
}

uintptr_t InstructionPointer(const CONTEXT& context) {
#ifdef _WIN64
    return static_cast<uintptr_t>(context.Rip);
#else
    return static_cast<uintptr_t>(context.Eip);
#endif
}

template <typename T>
bool ReadAt(uintptr_t address, T& out) {
    return ReadMemory(address, &out, sizeof(out));
}

// The start of the function `address` is in, from the unwind data of x64
// modules; 0 when it has none, as leaf functions.
uintptr_t FunctionStart(uintptr_t address) {
#ifdef _WIN64
    DWORD64 imageBase = 0;
    PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(address, &imageBase, nullptr);
    if (!function)
        return 0;
    RUNTIME_FUNCTION entry = *function;
    // A chained entry describes a part of the function: its parent comes after its unwind codes.
    for (int depth = 0; depth < 32; ++depth) {
        uint8_t header[4] = {};
        uintptr_t unwind = static_cast<uintptr_t>(imageBase) + (entry.UnwindData & ~DWORD{1});
        if ((entry.UnwindData & 1) || !ReadMemory(unwind, header, sizeof(header)) || !((header[0] >> 3) & kChainInfo))
            break;
        uintptr_t parent = unwind + 4 + ((header[2] + 1u) & ~1u) * 2;
        if (!ReadAt(parent, entry))
            return 0;
    }
    return static_cast<uintptr_t>(imageBase) + entry.BeginAddress;
#else
    (void)address;
    return 0;
#endif
}

} // namespace

bool IsCode(uintptr_t address, size_t size) {
    uintptr_t end = address + size;
    for (uintptr_t at = address; at < end;) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<void*>(at), &info, sizeof(info)) != sizeof(info) || info.State != MEM_COMMIT ||
            !(info.Protect & kExecutable) || (info.Protect & PAGE_GUARD))
            return false;
        at = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
    }
    return size > 0;
}

std::string ProtectedCode(uintptr_t address, size_t size) {
    if (OverlapsOwnHook(address, size))
        return "this is a hook of the overlay, the jump MinHook placed in a function of the game or its trampoline, which the patch would break";
    return "";
}

// Suspending the other threads: no allocation and no lock while they are
// suspended, since one of them may hold the lock of the heap.
std::string WriteCode(uintptr_t address, const void* data, size_t size, uintptr_t from) {
    struct Page {
        uintptr_t base;
        DWORD protection;
    };
    uintptr_t page = PageSize();
    std::vector<Page> pages;
    for (uintptr_t base = address & ~(page - 1); base < address + size; base += page) {
        DWORD old = 0;
        if (!VirtualProtect(reinterpret_cast<void*>(base), page, PAGE_EXECUTE_READWRITE, &old)) {
            for (const Page& done : pages)
                VirtualProtect(reinterpret_cast<void*>(done.base), page, done.protection, &old);
            return "the code cannot be made writable there";
        }
        pages.push_back({base, old});
    }

    std::vector<HANDLE> threads;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        DWORD process = GetCurrentProcessId();
        DWORD self = GetCurrentThreadId();
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
            if (entry.th32OwnerProcessID != process || entry.th32ThreadID == self)
                continue;
            if (HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, entry.th32ThreadID))
                threads.push_back(thread);
        }
        CloseHandle(snapshot);
    }
    std::vector<char> suspended(threads.size());
    bool written = false;
    for (int attempt = 0; attempt < kAttempts && !written; ++attempt) {
        if (attempt)
            Sleep(2);
        for (size_t i = 0; i < threads.size(); ++i)
            suspended[i] = SuspendThread(threads[i]) != static_cast<DWORD>(-1);
        bool busy = false;
        for (size_t i = 0; i < threads.size() && !busy; ++i) {
            if (!suspended[i])
                continue;
            // Also waits for the suspension to take effect.
            CONTEXT context{};
            context.ContextFlags = CONTEXT_CONTROL;
            if (!GetThreadContext(threads[i], &context))
                continue;
            uintptr_t ip = InstructionPointer(context);
            busy = ip > from && ip < address + size;
        }
        if (!busy) {
            StoreCode(address, data, size);
            FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(address), size);
            written = true;
        }
        for (size_t i = 0; i < threads.size(); ++i)
            if (suspended[i])
                ResumeThread(threads[i]);
    }
    for (HANDLE thread : threads)
        CloseHandle(thread);
    for (const Page& done : pages) {
        DWORD old = 0;
        VirtualProtect(reinterpret_cast<void*>(done.base), page, done.protection, &old);
    }
    return written ? "" : "a thread of the game kept running the code there for 100 ms: try again, or patch another instruction";
}

// The export the address is in: on x64, the one at the start of its function;
// elsewhere, and for leaf functions, the closest one before it, near enough.
std::string SymbolAt(uintptr_t address) {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(address),
                            &module))
        return "";
    uintptr_t base = reinterpret_cast<uintptr_t>(module);
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS nt{};
    if (!ReadAt(base, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || !ReadAt(base + dos.e_lfanew, nt) || nt.Signature != IMAGE_NT_SIGNATURE)
        return "";
    const IMAGE_DATA_DIRECTORY& directory = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    IMAGE_EXPORT_DIRECTORY exports{};
    if (!directory.VirtualAddress || !ReadAt(base + directory.VirtualAddress, exports) || exports.NumberOfNames > 65536 ||
        exports.NumberOfFunctions > 65536)
        return "";
    std::vector<DWORD> functions(exports.NumberOfFunctions);
    std::vector<DWORD> names(exports.NumberOfNames);
    std::vector<WORD> ordinals(exports.NumberOfNames);
    if ((!functions.empty() && !ReadMemory(base + exports.AddressOfFunctions, functions.data(), functions.size() * sizeof(DWORD))) ||
        (!names.empty() && (!ReadMemory(base + exports.AddressOfNames, names.data(), names.size() * sizeof(DWORD)) ||
                            !ReadMemory(base + exports.AddressOfNameOrdinals, ordinals.data(), ordinals.size() * sizeof(WORD)))))
        return "";
    uintptr_t start = FunctionStart(address);
    uintptr_t rva = address - base;
    uintptr_t wanted = start ? start - base : 0;
    size_t best = names.size();
    uintptr_t bestRva = 0;
    for (size_t i = 0; i < names.size(); ++i) {
        if (ordinals[i] >= functions.size())
            continue;
        uintptr_t function = functions[ordinals[i]];
        // Forwarded to another module.
        if (function >= directory.VirtualAddress && function < directory.VirtualAddress + directory.Size)
            continue;
        if (start ? function == wanted : function <= rva && (best == names.size() || function > bestRva)) {
            best = i;
            bestRva = function;
            if (start)
                break;
        }
    }
    if (best == names.size() || (!start && rva - bestRva >= 0x200))
        return "";
    // The name may end near the end of the image: read in small parts.
    char name[256] = {};
    for (size_t at = 0; at + 1 < sizeof(name) && !memchr(name, '\0', at); at += 16)
        if (!ReadMemory(base + names[best] + at, name + at, std::min<size_t>(16, sizeof(name) - 1 - at)))
            return "";
    if (rva == bestRva)
        return name;
    char offset[32];
    snprintf(offset, sizeof(offset), "+0x%" PRIxPTR, rva - bestRva);
    return name + std::string(offset);
}

} // namespace uih::detail::remote
