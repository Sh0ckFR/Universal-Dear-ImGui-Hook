// The code commands of src/code.cpp on Linux: the protection of the pages of
// code, changed with mprotect() for the write, and the dynamic symbols of the
// modules. The other threads keep running meanwhile: see StoreCode().
#include "internal.h"

#include <dlfcn.h>
#include <link.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>

namespace uih::detail::remote {
namespace {

struct Mapping {
    uintptr_t start;
    uintptr_t end;
    int protection;
};

// The parts of the mappings over address..address+size, from
// /proc/self/maps; false when a part of it is not mapped.
bool Mappings(uintptr_t address, size_t size, std::vector<Mapping>& out) {
    out.clear();
    FILE* maps = fopen("/proc/self/maps", "re");
    if (!maps)
        return false;
    char line[4096];
    uintptr_t covered = address;
    uintptr_t end = address + size;
    while (fgets(line, sizeof(line), maps)) {
        unsigned long start = 0;
        unsigned long stop = 0;
        char permissions[8] = {};
        if (sscanf(line, "%lx-%lx %7s", &start, &stop, permissions) < 3 || stop <= covered || start >= end)
            continue;
        if (start > covered)
            break;
        int protection = (permissions[0] == 'r' ? PROT_READ : 0) | (permissions[1] == 'w' ? PROT_WRITE : 0) | (permissions[2] == 'x' ? PROT_EXEC : 0);
        out.push_back({std::max<uintptr_t>(start, address), std::min<uintptr_t>(stop, end), protection});
        covered = std::min<uintptr_t>(stop, end);
        if (covered >= end)
            break;
    }
    fclose(maps);
    return size > 0 && covered >= end;
}

} // namespace

bool IsCode(uintptr_t address, size_t size) {
    std::vector<Mapping> mappings;
    if (!Mappings(address, size, mappings))
        return false;
    return std::all_of(mappings.begin(), mappings.end(), [](const Mapping& m) { return (m.protection & PROT_EXEC) != 0; });
}

// The hooks are symbols of the library, which src/code.cpp protects as a whole.
std::string ProtectedCode(uintptr_t, size_t) { return ""; }

std::string WriteCode(uintptr_t address, const void* data, size_t size, uintptr_t) {
    std::vector<Mapping> mappings;
    if (!Mappings(address, size, mappings))
        return "the memory is not mapped there";
    uintptr_t page = static_cast<uintptr_t>(sysconf(_SC_PAGESIZE));
    uintptr_t first = address & ~(page - 1);
    uintptr_t last = (address + size + page - 1) & ~(page - 1);
    // Executable all along, for the threads that run the rest of the page.
    if (mprotect(reinterpret_cast<void*>(first), last - first, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
        StoreCode(address, data, size);
        for (const Mapping& mapping : mappings) {
            uintptr_t start = std::max(first, mapping.start & ~(page - 1));
            uintptr_t end = std::min(last, (mapping.end + page - 1) & ~(page - 1));
            mprotect(reinterpret_cast<void*>(start), end - start, mapping.protection);
        }
    } else if (!WriteMemory(address, data, size)) {
        // SELinux, or a hardened kernel, refuses writable code: /proc/self/mem writes it anyway, unless it is off too.
        return "the code cannot be written there: the system refuses to make it writable";
    }
    __builtin___clear_cache(reinterpret_cast<char*>(address), reinterpret_cast<char*>(address + size));
    return "";
}

std::string SymbolAt(uintptr_t address) {
    Dl_info info{};
    void* entry = nullptr;
    if (!dladdr1(reinterpret_cast<void*>(address), &info, &entry, RTLD_DL_SYMENT) || !info.dli_sname || !info.dli_saddr || !entry)
        return "";
    const auto* symbol = static_cast<const ElfW(Sym)*>(entry);
    uintptr_t start = reinterpret_cast<uintptr_t>(info.dli_saddr);
    uintptr_t length = symbol->st_size ? symbol->st_size : 1;
    if (address < start || address - start >= length)
        return "";
    if (address == start)
        return info.dli_sname;
    char offset[32];
    snprintf(offset, sizeof(offset), "+0x%" PRIxPTR, address - start);
    return info.dli_sname + std::string(offset);
}

} // namespace uih::detail::remote
