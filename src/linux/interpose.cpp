#include "internal.h"

#include <dlfcn.h>
#include <link.h>

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

// The sanitizer runtimes look functions up with dlsym() while they start,
// before their shadow memory exists. That path is not instrumented and calls
// nothing that is: GCC does not inline instrumented functions into it.
#define UIH_NO_SANITIZE __attribute__((no_sanitize("address", "undefined")))

namespace uih::detail::interpose {
namespace {

using DlsymFn = void* (*)(void*, const char*) noexcept;

DlsymFn g_dlsym = nullptr;

const char* const kDlsymVersions[] = {"GLIBC_2.34", "GLIBC_2.2.5", "GLIBC_2.17", "GLIBC_2.4", "GLIBC_2.0"};

// This library exports dlsym, so the one of the C library is found by version.
UIH_NO_SANITIZE DlsymFn FindDlsym() {
    for (size_t i = 0; i < sizeof(kDlsymVersions) / sizeof(kDlsymVersions[0]); ++i)
        if (void* function = dlvsym(RTLD_NEXT, "dlsym", kDlsymVersions[i]))
            return reinterpret_cast<DlsymFn>(function);
    fputs("[uih] dlsym() of the C library was not found\n", stderr);
    abort();
}

UIH_NO_SANITIZE DlsymFn RealDlsym() {
    DlsymFn function = __atomic_load_n(&g_dlsym, __ATOMIC_RELAXED);
    if (!function) {
        function = FindDlsym();
        __atomic_store_n(&g_dlsym, function, __ATOMIC_RELAXED);
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

// By soname: the application may have opened the library under another
// name, such as libvulkan.so.
bool Library::Contains(const void* address) const {
    Dl_info info{};
    link_map* object = nullptr;
    if (!address || !dladdr1(address, &info, reinterpret_cast<void**>(&object), RTLD_DL_LINKMAP) || !object)
        return false;
    for (const char* const* soname = sonames; *soname; ++soname) {
        void* handle = dlopen(*soname, RTLD_LAZY | RTLD_NOLOAD);
        if (!handle)
            continue;
        link_map* library = nullptr;
        bool same = dlinfo(handle, RTLD_DI_LINKMAP, &library) == 0 && library == object;
        dlclose(handle);
        if (same)
            return true;
    }
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

UIH_EXPORT UIH_NO_SANITIZE void* dlsym(void* handle, const char* name) noexcept {
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
