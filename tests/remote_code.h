// The code commands of the remote control that the Windows and the Linux test
// hosts check the same way: the disassembler, a patch of a function of the
// host that changes its result, its restore, and a signature found again in
// the executable. Include after remote_advanced.h.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#define UIH_TEST_DISASSEMBLER 1
#else
#define UIH_TEST_DISASSEMBLER 0
#endif

// The function the assistant patches: called through a pointer, so that the
// calls are not folded into constants. Left out of the sanitizers, whose
// checks would push its ret past the instructions the test disassembles.
#if defined(__GNUC__) || defined(__clang__)
#define UIH_TEST_NO_SANITIZE __attribute__((no_sanitize("address", "undefined")))
#else
#define UIH_TEST_NO_SANITIZE
#endif
UIH_NOINLINE UIH_TEST_NO_SANITIZE inline int PatchTarget(int value) {
    volatile int kept = value;
    return kept * 3 + 7;
}
inline int (*volatile g_patchTarget)(int) = PatchTarget;
// Set while a patch is left for uih::Stop() to restore.
inline bool g_patchLeft = false;

// Where the code of PatchTarget starts: after the jump of an incremental
// link, and after endbr64, which the patch keeps.
inline uintptr_t PatchTargetCode() {
    auto* code = reinterpret_cast<const uint8_t*>(g_patchTarget);
#if UIH_TEST_DISASSEMBLER
    if (code[0] == 0xe9) {
        int32_t offset = 0;
        memcpy(&offset, code + 1, sizeof(offset));
        code += 5 + offset;
    }
    if (code[0] == 0xf3 && code[1] == 0x0f && code[2] == 0x1e && (code[3] == 0xfa || code[3] == 0xfb))
        code += 4;
#else
    // The landing pad of branch protection, bti c, in a function that saves no return address.
    uint32_t first = 0;
    memcpy(&first, code, sizeof(first));
    if (first == 0xd503245f)
        code += 4;
#endif
    return reinterpret_cast<uintptr_t>(code);
}

// True when no patch was left, or when uih::Stop() restored it.
inline bool PatchRestoredAtStop() { return !g_patchLeft || g_patchTarget(5) == 22; }

inline size_t CountLines(const std::string& text) {
    size_t lines = 0;
    for (char c : text)
        lines += c == '\n';
    return lines;
}

// Empty when the commands worked, the reason otherwise. `protectedCode` is
// an exported function the overlay must refuse to patch, `symbol` its name,
// and `refusal` the start of the reason.
template <typename Client>
std::string CheckRemoteCode(Client& client, const void* protectedCode, const char* symbol, const char* refusal) {
    if (!protectedCode)
        return std::string("the host did not find ") + symbol;
    uintptr_t code = PatchTargetCode();
    std::string at = HexAddress(reinterpret_cast<const void*>(code));
    std::string signature;
    std::string pattern;
    std::string module;
#if UIH_TEST_DISASSEMBLER
    std::string exported = RemotePayload(client, client.Request("disasm " + HexAddress(protectedCode) + " 2"));
    printf("disasm %s 2:\n%s", HexAddress(protectedCode).c_str(), exported.c_str());
    if (exported.find(std::string(" ") + symbol + " ") == std::string::npos || exported.find(std::string(" ") + symbol + " ") > exported.find('\n'))
        return std::string("the disassembly does not name the symbol ") + symbol;
    std::string listing = RemotePayload(client, client.Request("disasm " + at + " 12"));
    printf("disasm %s 12:\n%s", at.c_str(), listing.c_str());
    if (listing.rfind(at + " ", 0) != 0 || CountLines(listing) != 12 || listing.find(" ret") == std::string::npos)
        return "the disassembly of the function of the host does not start at it, or misses its ret";
    // The instructions before an address, in step with the code.
    std::string second = listing.substr(listing.find('\n') + 1, listing.find(' ', listing.find('\n') + 1) - listing.find('\n') - 1);
    std::string before = RemotePayload(client, client.Request("disasm " + second + " 1 1"));
    printf("disasm %s 1 1:\n%s", second.c_str(), before.c_str());
    if (before.rfind(at + " ", 0) != 0 || CountLines(before) != 2)
        return "the disassembly before an instruction does not start at the instruction before it";

    signature = RemotePayload(client, client.Request("signature " + at));
    size_t space = signature.find(' ');
    module = signature.substr(0, space);
    pattern = space == std::string::npos ? "" : signature.substr(space + 1, signature.find('\n') - space - 1);
    std::string found = RemotePayload(client, client.Request("pattern " + pattern + " " + module));
    printf("signature %s: %spattern: %s", at.c_str(), signature.c_str(), found.c_str());
    if (module.find("uih_host") != 0 || pattern.empty() || found.rfind(at + " ", 0) != 0 || CountLines(found) != 1)
        return "the signature of the function of the host is not found once, at the function, in the executable";
#endif

    // mov eax, 42; ret, or on ARM64 mov w0, #42; ret.
#if UIH_TEST_DISASSEMBLER
    const char* bytes = "b8 2a 00 00 00 c3";
#else
    const char* bytes = "40 05 80 52 c0 03 5f d6";
#endif
    int original = g_patchTarget(5);
    std::string patched = client.Request("patch " + at + " " + bytes);
    int changed = g_patchTarget(5);
    std::string id = patched.size() > 3 ? patched.substr(3, patched.find(' ', 3) - 3) : "";
    std::string list = RemotePayload(client, client.Request("patches"));
    std::string overlap = client.Request("patch " + HexAddress(reinterpret_cast<const void*>(code + 1)) + " 90");
    std::string refused = client.Request("patch " + HexAddress(protectedCode) + " 90");
    // Taken back at once, should the overlay have taken it.
    if (refused.rfind("ok ", 0) == 0)
        client.Request("unpatch " + refused.substr(3, refused.find(' ', 3) - 3));
    std::string data = client.Request("patch " + HexAddress(&g_remoteValue) + " 90");
    printf("patch: %s, %d before, %d after\npatches: %spatch over it: %s\npatch the overlay: %s\npatch data: %s\n", patched.c_str(), original,
           changed, list.c_str(), overlap.c_str(), refused.c_str(), data.c_str());
    if (original != 22 || patched.rfind("ok ", 0) != 0 || changed != 42)
        return "the patch did not change the result of the function of the host";
    if (list.rfind(id + " " + at + " ", 0) != 0)
        return "the list of patches does not show the patch";
    if (overlap.rfind("error patch " + id + " is already there", 0) != 0)
        return "a patch was placed over another one";
    if (refused.rfind(std::string("error ") + refusal, 0) != 0)
        return "the overlay let its own code be patched";
    if (data.rfind("error this is not code", 0) != 0)
        return "a patch was placed on data";
#if UIH_TEST_DISASSEMBLER
    std::string now = RemotePayload(client, client.Request("disasm " + at + " 2"));
    std::string whilePatched = client.Request("signature " + at);
    printf("disasm %s 2 once patched:\n%ssignature: %s\n", at.c_str(), now.c_str(), whilePatched.c_str());
    if (now.find(" mov eax, 0x2a  ; patch " + id + "\n") == std::string::npos)
        return "the disassembly does not show the patched instruction";
    if (whilePatched.rfind("error patch " + id + " changed the code there", 0) != 0)
        return "a signature was made of patched code";
#endif

    std::string unpatched = client.Request("unpatch " + id);
    int restored = g_patchTarget(5);
    std::string empty = RemotePayload(client, client.Request("patches"));
    std::string unknown = client.Request("unpatch 999");
    printf("unpatch %s: %s, %d now\npatches: %zu bytes\nunpatch 999: %s\n", id.c_str(), unpatched.c_str(), restored, empty.size(), unknown.c_str());
    if (unpatched != "ok" || restored != 22 || !empty.empty())
        return "unpatch did not restore the function of the host";
    if (unknown.rfind("error no patch 999", 0) != 0)
        return "unpatch took an unknown patch";
#if UIH_TEST_DISASSEMBLER
    std::string again = RemotePayload(client, client.Request("pattern " + pattern + " " + module));
    printf("pattern once restored: %s", again.c_str());
    if (again.rfind(at + " ", 0) != 0 || CountLines(again) != 1)
        return "the signature is not found again once the function is restored";
#endif

    // Left for uih::Stop(), which restores it.
    std::string left = client.Request(std::string("patch ") + at + " " + bytes);
    if (left.rfind("ok ", 0) != 0 || g_patchTarget(5) != 42)
        return "the patch left for uih::Stop() was not placed";
    g_patchLeft = true;
    return "";
}
