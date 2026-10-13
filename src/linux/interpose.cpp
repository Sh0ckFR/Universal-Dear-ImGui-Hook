#include "internal.h"

#include <dlfcn.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

#if defined(__clang__)
#define UIH_MUSTTAIL [[clang::musttail]]
#elif defined(__has_cpp_attribute) && __has_cpp_attribute(gnu::musttail)
#define UIH_MUSTTAIL [[gnu::musttail]]
#else
#define UIH_MUSTTAIL
#endif

namespace uih::detail::interpose {
namespace {

using DlsymFn = void* (*)(void*, const char*) noexcept;

std::atomic<DlsymFn> g_dlsym{nullptr};

// This library exports dlsym, so the one of the C library is found by version.
DlsymFn FindDlsym() {
    for (const char* version : {"GLIBC_2.34", "GLIBC_2.2.5", "GLIBC_2.17", "GLIBC_2.4", "GLIBC_2.0"})
        if (void* function = dlvsym(RTLD_NEXT, "dlsym", version))
            return reinterpret_cast<DlsymFn>(function);
    fputs("[uih] dlsym() of the C library was not found\n", stderr);
    abort();
}

DlsymFn RealDlsym() {
    DlsymFn function = g_dlsym.load(std::memory_order_relaxed);
    if (!function) {
        function = FindDlsym();
        g_dlsym.store(function, std::memory_order_relaxed);
    }
    return function;
}

} // namespace

void* Library::Detour(const char* name) const {
    for (size_t i = 0; i < count; ++i)
        if (strcmp(functions[i].name, name) == 0)
            return functions[i].detour;
    return nullptr;
}

bool Library::Contains(const void* address) const {
    Dl_info info{};
    if (!address || !dladdr(address, &info) || !info.dli_fname)
        return false;
    const char* separator = strrchr(info.dli_fname, '/');
    const char* file = separator ? separator + 1 : info.dli_fname;
    for (const char* const* soname = sonames; *soname; ++soname)
        if (strncmp(file, *soname, strlen(*soname)) == 0)
            return true;
    return false;
}

void* Library::Next(const char* name) const {
    DlsymFn realDlsym = RealDlsym();
    if (void* function = realDlsym(RTLD_NEXT, name))
        return function;
    // The application loaded the library outside of the global scope.
    for (const char* const* soname = sonames; *soname; ++soname) {
        void* handle = dlopen(*soname, RTLD_LAZY | RTLD_NOLOAD);
        if (!handle)
            continue;
        void* function = realDlsym(handle, name);
        dlclose(handle);
        if (function)
            return function;
    }
    return nullptr;
}

void* Library::Substitute(void* function, const char* name, const void* caller) const {
    void* detour = function ? Detour(name) : nullptr;
    return detour && !Contains(caller) ? detour : function;
}

} // namespace uih::detail::interpose

UIH_EXPORT void* dlsym(void* handle, const char* name) noexcept {
    using namespace uih::detail::interpose;
    // These lookups depend on the caller: a tail call keeps it.
    if (handle == RTLD_DEFAULT || handle == RTLD_NEXT)
        UIH_MUSTTAIL return RealDlsym()(handle, name);
    void* function = RealDlsym()(handle, name);
    for (const Library* library : {&kGlx, &kEgl, &kVulkan})
        if (void* detour = library->Detour(name))
            return library->Contains(function) ? detour : function;
    return function;
}
