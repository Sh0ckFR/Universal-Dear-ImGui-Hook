// Code commands of the remote control, with Config::remoteMemory and
// Config::aiAdvancedFeatures, for an assistant that reads and changes the
// code of the game rather than only its data:
//   disasm <address> [count] [before]  -> ok <bytes> <address>, then lines: <address> <module+offset> <symbol|-> <bytes> <instruction>
//   patch <address> <hex bytes>        -> ok <id> [cuts <instruction> <its end>]
//   unpatch <id>|all                   -> ok [changed]  (all: ok <count>)
//   patches                            -> ok <bytes>, then lines: <id> <address> <module+offset> <original bytes> <patched bytes>
//   signature <address> [length]       -> ok <bytes>, then a line: <module> <pattern>
// The instructions are decoded by Zydis (third_party/zydis), x86 and x64
// only: on other processors, disasm and signature answer an error. A
// patch keeps the original bytes, which unpatch puts back, as Stop() does
// for those left. Addresses are those of src/memory.cpp.
#include "internal.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef __linux__
#include <link.h>
#endif

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#define UIH_DISASSEMBLER 1
// As src/zydis_build.c compiles it.
#define ZYDIS_STATIC_BUILD
#define ZYCORE_STATIC_BUILD
#define ZYDIS_DISABLE_ENCODER
#include <Zydis.h>
#else
#define UIH_DISASSEMBLER 0
#endif

namespace uih::detail::remote {
namespace {

constexpr size_t kPage = 4096;
constexpr size_t kMaxInstruction = 15;
constexpr int kMaxCount = 200;
constexpr int kMaxBefore = 50;
constexpr size_t kMaxPatch = 256;
constexpr size_t kMaxSignature = 128;
// Candidates the signature keeps while it grows: a first pattern found more
// often than this grows before the module is searched again.
constexpr size_t kMaxCandidates = 4096;

std::mutex g_patchMutex;
std::vector<CodePatch> g_patches;
int g_nextPatch = 1;

std::string Hex(uintptr_t value) {
    char text[32];
    snprintf(text, sizeof(text), "0x%" PRIxPTR, value);
    return text;
}

std::string HexBytes(const uint8_t* bytes, size_t size, const char* separator) {
    std::string text;
    char byte[4];
    for (size_t i = 0; i < size; ++i) {
        snprintf(byte, sizeof(byte), "%02x", bytes[i]);
        if (i)
            text += separator;
        text += byte;
    }
    return text;
}

bool Overlaps(uintptr_t a, size_t aSize, uintptr_t b, size_t bSize) { return a < b + bSize && b < a + aSize; }

// With g_patchMutex held.
const CodePatch* PatchOver(uintptr_t address, size_t size) {
    for (const CodePatch& patch : g_patches)
        if (Overlaps(address, size, patch.address, patch.patched.size()))
            return &patch;
    return nullptr;
}

const LoadedModule* ModuleOf(uintptr_t address, const std::vector<LoadedModule>& modules) {
    for (const LoadedModule& module : modules)
        if (address >= module.base && address - module.base < module.size)
            return &module;
    return nullptr;
}

// Empty when the library may patch there, the reason otherwise.
std::string Refusal(uintptr_t address, size_t size, const std::vector<LoadedModule>& modules) {
    if (!IsCode(address, size))
        return "this is not code: the memory there is not executable, or not all of it. Write data with write";
    // The library linked into the executable cannot tell its code from the
    // game's: only its hooks are protected then.
    const LoadedModule* own = ModuleOf(reinterpret_cast<uintptr_t>(&HandleCode), modules);
    if (own && !own->main && Overlaps(address, size, own->base, own->size))
        return "this is the code of the overlay itself (" + own->name + "), which the patch would break";
    std::string reason = ProtectedCode(address, size);
    if (!reason.empty())
        return reason;
    std::lock_guard<std::mutex> lock(g_patchMutex);
    if (const CodePatch* patch = PatchOver(address, size))
        return "patch " + std::to_string(patch->id) + " is already there, at " + Hex(patch->address) + ": unpatch it first";
    return "";
}

#if UIH_DISASSEMBLER

// The readable bytes from `address` on, up to `size`.
size_t ReadUpTo(uintptr_t address, uint8_t* out, size_t size) {
    if (ReadMemory(address, out, size))
        return size;
    size_t done = 0;
    while (done < size) {
        uintptr_t at = address + done;
        size_t chunk = std::min(size - done, kPage - (at & (kPage - 1)));
        if (!ReadMemory(at, out + done, chunk))
            break;
        done += chunk;
    }
    return done;
}

// The readable bytes right before `address`, up to `size`, at the start of `out`.
size_t ReadBefore(uintptr_t address, uint8_t* out, size_t size) {
    size = std::min<size_t>(size, address);
    if (ReadMemory(address - size, out, size))
        return size;
    size_t done = 0;
    while (done < size) {
        uintptr_t end = address - done;
        size_t chunk = std::min(size - done, ((end - 1) & (kPage - 1)) + 1);
        if (!ReadMemory(end - chunk, out + size - done - chunk, chunk))
            break;
        done += chunk;
    }
    memmove(out, out + size - done, done);
    return done;
}

struct Instruction {
    ZydisDecodedInstruction decoded;
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
};

class Decoder {
public:
    Decoder() {
#if defined(__x86_64__) || defined(_M_X64)
        ZydisDecoderInit(&decoder_, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
#else
        ZydisDecoderInit(&decoder_, ZYDIS_MACHINE_MODE_LEGACY_32, ZYDIS_STACK_WIDTH_32);
#endif
        ZydisFormatterInit(&formatter_, ZYDIS_FORMATTER_STYLE_INTEL);
        ZydisFormatterSetProperty(&formatter_, ZYDIS_FORMATTER_PROP_HEX_UPPERCASE, ZYAN_FALSE);
        ZydisFormatterSetProperty(&formatter_, ZYDIS_FORMATTER_PROP_ADDR_PADDING_ABSOLUTE, ZYDIS_PADDING_DISABLED);
        ZydisFormatterSetProperty(&formatter_, ZYDIS_FORMATTER_PROP_DISP_PADDING, ZYDIS_PADDING_DISABLED);
        ZydisFormatterSetProperty(&formatter_, ZYDIS_FORMATTER_PROP_IMM_PADDING, ZYDIS_PADDING_DISABLED);
    }

    // Length of the instruction at `bytes`, 0 when they hold none.
    size_t Length(const uint8_t* bytes, size_t size) const {
        ZydisDecodedInstruction decoded;
        return ZYAN_SUCCESS(ZydisDecoderDecodeInstruction(&decoder_, nullptr, bytes, size, &decoded)) ? decoded.length : 0;
    }

    bool Decode(const uint8_t* bytes, size_t size, Instruction& out) const {
        return ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder_, bytes, size, &out.decoded, out.operands));
    }

    // Intel syntax, branch targets and RIP-relative operands as absolute addresses.
    std::string Text(const Instruction& in, uintptr_t address) const {
        char text[256];
        if (!ZYAN_SUCCESS(ZydisFormatterFormatInstruction(&formatter_, &in.decoded, in.operands, in.decoded.operand_count_visible, text,
                                                          sizeof(text), address, nullptr)))
            return "(bad)";
        return text;
    }

private:
    ZydisDecoder decoder_;
    ZydisFormatter formatter_;
};

const Decoder& GetDecoder() {
    static const Decoder decoder;
    return decoder;
}

// The addresses an instruction refers to: branch targets, RIP-relative
// operands, and on x86 absolute memory operands.
std::vector<uintptr_t> Targets(const Instruction& in, uintptr_t address) {
    std::vector<uintptr_t> targets;
    for (int i = 0; i < in.decoded.operand_count_visible; ++i) {
        const ZydisDecodedOperand& operand = in.operands[i];
        bool relative = operand.type == ZYDIS_OPERAND_TYPE_IMMEDIATE && operand.imm.is_relative;
        bool memory = operand.type == ZYDIS_OPERAND_TYPE_MEMORY &&
                      (operand.mem.base == ZYDIS_REGISTER_RIP || operand.mem.base == ZYDIS_REGISTER_EIP ||
                       (operand.mem.base == ZYDIS_REGISTER_NONE && operand.mem.index == ZYDIS_REGISTER_NONE && operand.mem.disp.has_displacement));
        ZyanU64 target = 0;
        if ((relative || memory) && ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&in.decoded, &operand, address, &target)))
            targets.push_back(static_cast<uintptr_t>(target));
    }
    return targets;
}

// The instructions before `address`: decoded from as far back as possible,
// where the decoding falls in step with the code, so that one lands on
// `address`. At most `count`, the last first; empty when none lands there.
std::vector<uintptr_t> InstructionsBefore(uintptr_t address, int count) {
    std::vector<uintptr_t> starts;
    if (count <= 0)
        return starts;
    uint8_t bytes[kMaxBefore * kMaxInstruction + 16];
    size_t window = std::min(sizeof(bytes), static_cast<size_t>(count) * kMaxInstruction + 16);
    size_t readable = ReadBefore(address, bytes, window);
    const Decoder& decoder = GetDecoder();
    for (size_t back = readable; back > 0; --back) {
        std::vector<uintptr_t> stream;
        size_t at = readable - back;
        while (at < readable) {
            size_t length = decoder.Length(bytes + at, readable - at);
            if (!length)
                break;
            stream.push_back(address - readable + at);
            at += length;
        }
        if (at != readable)
            continue;
        if (stream.size() > static_cast<size_t>(count))
            stream.erase(stream.begin(), stream.end() - count);
        return stream;
    }
    return starts;
}

std::string Annotation(const Instruction& in, uintptr_t address, const std::vector<LoadedModule>& modules) {
    std::string note;
    for (uintptr_t target : Targets(in, address)) {
        std::string where = LocateAddress(target, modules);
        std::string symbol = SymbolAt(target);
        if (where == "-" && symbol.empty())
            continue;
        note += note.empty() ? "  ; " : ", ";
        note += where != "-" ? where : Hex(target);
        if (!symbol.empty())
            note += " " + symbol;
    }
    std::lock_guard<std::mutex> lock(g_patchMutex);
    if (const CodePatch* patch = PatchOver(address, in.decoded.length))
        note += (note.empty() ? "  ; " : ", ") + std::string("patch ") + std::to_string(patch->id);
    return note;
}

std::string Disassemble(const std::string& rest, std::vector<uint8_t>& payload) {
    std::vector<std::string> words;
    for (size_t at = 0; at < rest.size();) {
        size_t end = rest.find(' ', at);
        if (end == std::string::npos)
            end = rest.size();
        if (end > at)
            words.push_back(rest.substr(at, end - at));
        at = end + 1;
    }
    std::vector<LoadedModule> modules = SortedModules();
    uintptr_t address = 0;
    std::string error;
    if (words.empty() || !ResolveAddress(words[0], modules, address, &error))
        return "error " + (words.empty() ? std::string("give an address, then optionally how many instructions, and how many before it") : error);
    int count = words.size() > 1 ? std::clamp(atoi(words[1].c_str()), 1, kMaxCount) : 20;
    int before = words.size() > 2 ? std::clamp(atoi(words[2].c_str()), 0, kMaxBefore) : 0;
    std::vector<uintptr_t> starts = InstructionsBefore(address, before);
    uintptr_t from = starts.empty() ? address : starts.front();
    std::vector<uint8_t> bytes((address - from) + static_cast<size_t>(count) * kMaxInstruction);
    size_t readable = ReadUpTo(from, bytes.data(), bytes.size());
    if (readable <= address - from)
        return "error the memory is not readable there";
    const Decoder& decoder = GetDecoder();
    std::string text;
    size_t at = 0;
    int after = 0;
    char line[64];
    while (at < readable && after < count) {
        uintptr_t current = from + at;
        Instruction in;
        if (!decoder.Decode(bytes.data() + at, readable - at, in)) {
            // A byte that starts no instruction, as data between functions.
            snprintf(line, sizeof(line), "0x%" PRIxPTR " ", current);
            text += line + LocateAddress(current, modules) + " - " + HexBytes(bytes.data() + at, 1, "") + " (bad)\n";
            at += 1;
            after += current >= address;
            continue;
        }
        std::string symbol = SymbolAt(current);
        snprintf(line, sizeof(line), "0x%" PRIxPTR " ", current);
        text += line + LocateAddress(current, modules) + " " + (symbol.empty() ? "-" : symbol) + " " +
                HexBytes(bytes.data() + at, in.decoded.length, "") + " " + decoder.Text(in, current) + Annotation(in, current, modules) + "\n";
        at += in.decoded.length;
        after += current >= address;
    }
    payload.assign(text.begin(), text.end());
    return "ok " + std::to_string(payload.size()) + " " + Hex(address);
}

// The bytes of the instruction a signature cannot keep: relative offsets,
// and addresses the loader relocates.
void Wildcards(const Instruction& in, const std::vector<LoadedModule>& modules, std::vector<int>& pattern, size_t start) {
    const ZydisDecodedInstructionRaw& raw = in.decoded.raw;
    auto clear = [&](size_t offset, size_t bits) {
        for (size_t i = 0; i < bits / 8; ++i)
            pattern[start + offset + i] = -1;
    };
    bool relocated = false;
    for (int i = 0; i < in.decoded.operand_count; ++i) {
        const ZydisDecodedOperand& operand = in.operands[i];
        if (operand.type == ZYDIS_OPERAND_TYPE_MEMORY && (operand.mem.base == ZYDIS_REGISTER_RIP || operand.mem.base == ZYDIS_REGISTER_EIP))
            relocated = true;
        // x86: [0x401234] holds an address of the module.
        if (operand.type == ZYDIS_OPERAND_TYPE_MEMORY && operand.mem.base == ZYDIS_REGISTER_NONE && operand.mem.index == ZYDIS_REGISTER_NONE &&
            sizeof(void*) == 4)
            relocated = true;
    }
    if (raw.disp.size && (relocated || (raw.disp.size >= 32 && ModuleOf(static_cast<uintptr_t>(raw.disp.value), modules))))
        clear(raw.disp.offset, raw.disp.size);
    for (const auto& imm : raw.imm) {
        if (!imm.size)
            continue;
        if (imm.is_relative || (imm.size >= 32 && ModuleOf(static_cast<uintptr_t>(imm.value.u), modules)))
            clear(imm.offset, imm.size);
    }
}

std::string PatternText(const std::vector<int>& pattern) {
    std::string text;
    char byte[4];
    for (size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] < 0) {
            text += i ? " ??" : "??";
        } else {
            snprintf(byte, sizeof(byte), i ? " %02x" : "%02x", pattern[i]);
            text += byte;
        }
    }
    return text;
}

bool MatchesAt(uintptr_t address, const std::vector<int>& pattern) {
    uint8_t bytes[kMaxSignature + kMaxInstruction];
    if (pattern.size() > sizeof(bytes) || !ReadMemory(address, bytes, pattern.size()))
        return false;
    for (size_t i = 0; i < pattern.size(); ++i)
        if (pattern[i] >= 0 && bytes[i] != pattern[i])
            return false;
    return true;
}

std::string Signature(const std::string& rest, std::vector<uint8_t>& payload) {
    size_t space = rest.find(' ');
    std::vector<LoadedModule> modules = SortedModules();
    uintptr_t address = 0;
    std::string error;
    if (!ResolveAddress(rest.substr(0, space), modules, address, &error))
        return "error " + error;
    size_t minimum = space == std::string::npos ? 1 : std::clamp<size_t>(strtoul(rest.c_str() + space + 1, nullptr, 10), 1, kMaxSignature);
    const LoadedModule* module = ModuleOf(address, modules);
    if (!module)
        return "error the address is in no module: a signature finds code of a module again";
    uint8_t bytes[kMaxSignature + kMaxInstruction];
    size_t readable = ReadUpTo(address, bytes, std::min<size_t>(sizeof(bytes), module->base + module->size - address));
    {
        std::lock_guard<std::mutex> lock(g_patchMutex);
        if (const CodePatch* patch = PatchOver(address, readable))
            return "error patch " + std::to_string(patch->id) + " changed the code there: unpatch it first, a signature is made of the game's own bytes";
    }
    const Decoder& decoder = GetDecoder();
    std::vector<int> pattern;
    std::vector<uintptr_t> candidates;
    bool searched = false;
    size_t at = 0;
    while (at < readable && at < kMaxSignature) {
        Instruction in;
        if (!decoder.Decode(bytes + at, readable - at, in))
            return "error no instruction at " + Hex(address + at) + ": give the address of an instruction";
        size_t start = pattern.size();
        pattern.insert(pattern.end(), bytes + at, bytes + at + in.decoded.length);
        Wildcards(in, modules, pattern, start);
        at += in.decoded.length;
        if (pattern.size() < minimum)
            continue;
        if (!searched) {
            if (!FindPattern(pattern, *module, kMaxCandidates, candidates))
                return "error out of memory";
            // Too common yet: grow it first.
            if (candidates.size() >= kMaxCandidates)
                continue;
            searched = true;
        } else {
            candidates.erase(std::remove_if(candidates.begin(), candidates.end(), [&](uintptr_t c) { return !MatchesAt(c, pattern); }),
                             candidates.end());
        }
        if (candidates.size() <= 1)
            break;
    }
    while (!pattern.empty() && pattern.back() < 0)
        pattern.pop_back();
    // Verified as memory_pattern searches it.
    std::vector<uintptr_t> found;
    if (!FindPattern(pattern, *module, 2, found))
        return "error out of memory";
    if (found.size() != 1 || found[0] != address)
        return "error no unique signature within " + std::to_string(kMaxSignature) + " bytes: the code is repeated in " + module->name +
               "; try an instruction nearby";
    std::string text = module->name + " " + PatternText(pattern) + "\n";
    payload.assign(text.begin(), text.end());
    return "ok " + std::to_string(payload.size());
}

#endif // UIH_DISASSEMBLER

bool ParseHex(const std::string& text, std::vector<uint8_t>& bytes) {
    std::string hex;
    for (char c : text) {
        if (isxdigit(static_cast<unsigned char>(c)))
            hex += c;
        else if (c != ' ')
            return false;
    }
    if (hex.empty() || hex.size() % 2)
        return false;
    for (size_t i = 0; i < hex.size(); i += 2)
        bytes.push_back(static_cast<uint8_t>(strtoul(hex.substr(i, 2).c_str(), nullptr, 16)));
    return true;
}

std::string Patch(const std::string& rest) {
    size_t space = rest.find(' ');
    std::vector<LoadedModule> modules = SortedModules();
    uintptr_t address = 0;
    std::string error;
    if (space == std::string::npos || !ResolveAddress(rest.substr(0, space), modules, address, &error))
        return "error " + (error.empty() ? std::string("give an address, then the new bytes in hexadecimal, such as 90 90") : error);
    std::vector<uint8_t> bytes;
    if (!ParseHex(rest.substr(space + 1), bytes) || bytes.size() > kMaxPatch)
        return "error give 1 to 256 whole bytes in hexadecimal, such as b8 01 00 00 00 c3";
    std::string refusal = Refusal(address, bytes.size(), modules);
    if (!refusal.empty())
        return "error " + refusal;
    CodePatch patch{0, address, std::vector<uint8_t>(bytes.size()), bytes};
    if (!ReadMemory(address, patch.original.data(), bytes.size()))
        return "error the memory is not readable there";
    std::string cut;
#if UIH_DISASSEMBLER
    // An instruction the patch ends inside of leaves its last bytes behind.
    {
        uint8_t code[kMaxPatch + kMaxInstruction];
        size_t readable = ReadUpTo(address, code, bytes.size() + kMaxInstruction);
        size_t at = 0;
        while (at < bytes.size()) {
            size_t length = GetDecoder().Length(code + at, readable - at);
            if (!length)
                break;
            if (at + length > bytes.size()) {
                cut = " cuts " + Hex(address + at) + " " + Hex(address + at + length);
                break;
            }
            at += length;
        }
    }
#endif
    std::lock_guard<std::mutex> lock(g_patchMutex);
    // Another command may have patched there meanwhile.
    if (PatchOver(address, bytes.size()))
        return "error another patch is already there: unpatch it first";
    error = WriteCode(address, bytes.data(), bytes.size(), address);
    if (!error.empty())
        return "error " + error;
    patch.id = g_nextPatch++;
    g_patches.push_back(patch);
    return "ok " + std::to_string(patch.id) + cut;
}

// With g_patchMutex held: puts the original bytes of `patch` back when its
// bytes are still there. False when they changed, and the code is left as it is.
bool Restore(const CodePatch& patch, std::string& error) {
    std::vector<uint8_t> now(patch.patched.size());
    if (!ReadMemory(patch.address, now.data(), now.size()) || now != patch.patched)
        return false;
    error = WriteCode(patch.address, patch.original.data(), patch.original.size(), patch.address);
    return true;
}

std::string Unpatch(const std::string& rest) {
    if (rest == "all") {
        std::lock_guard<std::mutex> lock(g_patchMutex);
        int restored = 0;
        for (auto it = g_patches.begin(); it != g_patches.end();) {
            std::string error;
            bool same = Restore(*it, error);
            if (!error.empty())
                return "error patch " + std::to_string(it->id) + ": " + error;
            restored += same;
            it = g_patches.erase(it);
        }
        return "ok " + std::to_string(restored);
    }
    char* end = nullptr;
    long id = strtol(rest.c_str(), &end, 10);
    if (rest.empty() || *end)
        return "error give the id of a patch, or all";
    std::string error = RestorePatch(static_cast<int>(id));
    return error.empty() ? "ok" : error == "changed" ? "ok changed" : "error " + error;
}

std::string List(std::vector<uint8_t>& payload) {
    std::vector<LoadedModule> modules = SortedModules();
    std::string text;
    for (const CodePatch& patch : CodePatches())
        text += std::to_string(patch.id) + " " + Hex(patch.address) + " " + LocateAddress(patch.address, modules) + " " +
                HexBytes(patch.original.data(), patch.original.size(), "") + " " + HexBytes(patch.patched.data(), patch.patched.size(), "") + "\n";
    payload.assign(text.begin(), text.end());
    return "ok " + std::to_string(payload.size());
}

} // namespace

void StoreCode(uintptr_t address, const void* data, size_t size) {
    uintptr_t word = address & ~uintptr_t{7};
    if (size == 0 || address + size > word + 8) {
        memcpy(reinterpret_cast<void*>(address), data, size);
        return;
    }
    uint64_t value = 0;
    memcpy(&value, reinterpret_cast<const void*>(word), sizeof(value));
    memcpy(reinterpret_cast<uint8_t*>(&value) + (address - word), data, size);
#ifdef _MSC_VER
    InterlockedExchange64(reinterpret_cast<volatile LONG64*>(word), static_cast<LONG64>(value));
#else
    __atomic_store_n(reinterpret_cast<uint64_t*>(word), value, __ATOMIC_SEQ_CST);
#endif
}

std::vector<CodePatch> CodePatches() {
    std::lock_guard<std::mutex> lock(g_patchMutex);
    return g_patches;
}

std::string RestorePatch(int id) {
    std::lock_guard<std::mutex> lock(g_patchMutex);
    auto it = std::find_if(g_patches.begin(), g_patches.end(), [&](const CodePatch& patch) { return patch.id == id; });
    if (it == g_patches.end())
        return "no patch " + std::to_string(id) + ": list them with patches";
    std::string error;
    bool same = Restore(*it, error);
    if (!error.empty())
        return error;
    g_patches.erase(it);
    return same ? "" : "changed";
}

void RestorePatches() {
    std::lock_guard<std::mutex> lock(g_patchMutex);
    // The last first: patches do not overlap, but the game may have changed the code since.
    for (auto it = g_patches.rbegin(); it != g_patches.rend(); ++it) {
        std::string error;
        if (Restore(*it, error) && !error.empty())
            Log("Patch %d at %p not restored: %s", it->id, reinterpret_cast<void*>(it->address), error.c_str());
    }
    if (!g_patches.empty())
        Log("Restored the code the assistant patched");
    g_patches.clear();
}

std::string InstructionBefore(uintptr_t next) {
#if UIH_DISASSEMBLER
    std::vector<uintptr_t> starts = InstructionsBefore(next, 1);
    if (starts.empty())
        return "";
    uint8_t bytes[kMaxInstruction];
    size_t length = next - starts[0];
    Instruction in;
    if (length > sizeof(bytes) || !ReadMemory(starts[0], bytes, length) || !GetDecoder().Decode(bytes, length, in))
        return "";
    return Hex(starts[0]) + " " + GetDecoder().Text(in, starts[0]);
#else
    (void)next;
    return "";
#endif
}

namespace {

#if UIH_DISASSEMBLER
#if defined(__x86_64__) || defined(_M_X64)
constexpr ZydisMachineMode kMode = ZYDIS_MACHINE_MODE_LONG_64;
#else
constexpr ZydisMachineMode kMode = ZYDIS_MACHINE_MODE_LEGACY_32;
#endif

// [register + displacement], without an index: a member of an object.
bool IsMember(const ZydisDecodedOperand& operand) {
    return operand.type == ZYDIS_OPERAND_TYPE_MEMORY && operand.mem.base != ZYDIS_REGISTER_NONE && operand.mem.base != ZYDIS_REGISTER_RIP &&
           operand.mem.base != ZYDIS_REGISTER_EIP && operand.mem.index == ZYDIS_REGISTER_NONE;
}

bool IsHighByte(ZydisRegister reg) { return reg == ZYDIS_REGISTER_AH || reg == ZYDIS_REGISTER_BH || reg == ZYDIS_REGISTER_CH || reg == ZYDIS_REGISTER_DH; }

// The function a jump at `address` goes to, or `address` when it is no jump.
uintptr_t FollowJumps(uintptr_t address) {
    for (int hop = 0; hop < 4; ++hop) {
        uint8_t bytes[kMaxInstruction];
        size_t size = ReadUpTo(address, bytes, sizeof(bytes));
        Instruction in;
        if (!size || !GetDecoder().Decode(bytes, size, in) || in.decoded.mnemonic != ZYDIS_MNEMONIC_JMP)
            break;
        ZyanU64 target = 0;
        const ZydisDecodedOperand& operand = in.operands[0];
        if (!ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&in.decoded, &operand, address, &target)))
            break;
        // jmp [rip + x], to an imported function.
        if (operand.type == ZYDIS_OPERAND_TYPE_MEMORY) {
            uintptr_t pointer = 0;
            if (!ReadMemory(static_cast<uintptr_t>(target), &pointer, sizeof(pointer)))
                break;
            target = pointer;
        }
        address = static_cast<uintptr_t>(target);
    }
    return address;
}
#endif

// The bytes from `address` to the end of its function, from the unwind
// tables compilers write for every function, 0 when unknown: the .pdata of
// 64-bit Windows modules, and the sorted table of .eh_frame_hdr on Linux,
// where the next function's start bounds the function. Decoding stops there:
// the end of a function is not always a ret, after a conditional tail call
// or a call that does not return.
size_t FunctionExtent(uintptr_t address) {
#if defined(_WIN64)
    DWORD64 base = 0;
    PRUNTIME_FUNCTION entry = RtlLookupFunctionEntry(static_cast<DWORD64>(address), &base, nullptr);
    if (!entry || base + entry->EndAddress <= address)
        return 0;
    return static_cast<size_t>(base + entry->EndAddress - address);
#elif defined(__linux__)
    struct Query {
        uintptr_t address;
        size_t extent;
    } query{address, 0};
    dl_iterate_phdr(
        [](dl_phdr_info* info, size_t, void* data) -> int {
            auto* query = static_cast<Query*>(data);
            const ElfW(Phdr)* frame = nullptr;
            bool inside = false;
            for (int i = 0; i < info->dlpi_phnum; ++i) {
                const ElfW(Phdr)& header = info->dlpi_phdr[i];
                uintptr_t start = info->dlpi_addr + header.p_vaddr;
                if (header.p_type == PT_LOAD && query->address >= start && query->address < start + header.p_memsz)
                    inside = true;
                if (header.p_type == PT_GNU_EH_FRAME)
                    frame = &header;
            }
            if (!inside)
                return 0;
            if (!frame)
                return 1;
            // Version 1, the encodings of eh_frame_ptr, fde_count and the table,
            // which is pairs of int32 (start, FDE) relative to the header.
            const auto* hdr = reinterpret_cast<const uint8_t*>(info->dlpi_addr + frame->p_vaddr);
            uint8_t pointer = hdr[1] & 0x0f;
            size_t pointerSize = pointer == 0x03 || pointer == 0x0b ? 4 : pointer == 0x04 || pointer == 0x0c ? 8 : 0;
            if (hdr[0] != 1 || !pointerSize || hdr[2] != 0x03 || hdr[3] != 0x3b)
                return 1;
            uint32_t count = 0;
            memcpy(&count, hdr + 4 + pointerSize, sizeof(count));
            const uint8_t* table = hdr + 8 + pointerSize;
            auto startAt = [&](uint32_t index) {
                int32_t offset = 0;
                memcpy(&offset, table + static_cast<size_t>(index) * 8, sizeof(offset));
                return reinterpret_cast<uintptr_t>(hdr) + static_cast<intptr_t>(offset);
            };
            // The first function that starts after the address.
            uint32_t low = 0;
            uint32_t high = count;
            while (low < high) {
                uint32_t middle = low + (high - low) / 2;
                if (startAt(middle) <= query->address)
                    low = middle + 1;
                else
                    high = middle;
            }
            if (low > 0 && low < count)
                query->extent = static_cast<size_t>(startAt(low) - query->address);
            return 1;
        },
        &query);
    return query.extent;
#else
    (void)address;
    return 0;
#endif
}

#if defined(__aarch64__) || defined(_M_ARM64)
// ldr wT, [xN, #imm], ldrb, ldur, then tbz/tbnz wT, #bit or tst wT, #mask.
bool TestsBitsArm64(uintptr_t function, uint32_t displacement, uint32_t mask, size_t length) {
    if (size_t extent = FunctionExtent(function))
        length = std::min(length, extent);
    std::vector<uint32_t> code(length / 4);
    if (code.empty() || !ReadMemory(function, code.data(), code.size() * 4))
        return false;
    // The byte of the value each register holds, -1 when none.
    int loaded[32];
    for (int& shift : loaded)
        shift = -1;
    int64_t reach = 0;
    for (size_t i = 0; i < code.size(); ++i) {
        uint32_t in = code[i];
        uint32_t rt = in & 0x1f;
        int64_t offset = -1;
        if ((in & 0xffc00000) == 0xb9400000)
            offset = ((in >> 10) & 0xfff) * 4;  // ldr w, [x, #imm]
        else if ((in & 0xffc00000) == 0x39400000)
            offset = (in >> 10) & 0xfff;  // ldrb w, [x, #imm]
        else if ((in & 0xffe00c00) == 0xb8400000)
            offset = static_cast<int64_t>(static_cast<int32_t>(in << 11) >> 23);  // ldur w, [x, #simm9]
        if (offset >= 0) {
            int64_t delta = offset - static_cast<int64_t>(displacement);
            loaded[rt] = delta >= 0 && delta < 4 ? static_cast<int>(delta) : -1;
            continue;
        }
        if ((in & 0x7e000000) == 0x36000000 && loaded[rt] >= 0) {
            uint32_t bit = ((in >> 31) << 5) | ((in >> 19) & 0x1f);
            if (bit < 32 && (uint64_t{1} << (bit + 8 * loaded[rt])) == mask)
                return true;
        }
        // tst w, #imm (ands wzr): a single bit is imms 0, rotated right by immr.
        if ((in & 0xffc0001f) == 0x7200001f && loaded[(in >> 5) & 0x1f] >= 0 && ((in >> 10) & 0x3f) == 0) {
            uint32_t rotate = (in >> 16) & 0x3f;
            uint32_t bit = (32 - rotate) % 32;
            if ((uint64_t{1} << (bit + 8 * loaded[(in >> 5) & 0x1f])) == mask)
                return true;
        }
        // The furthest instruction a branch seen so far goes to: a ret or b
        // after it ends the function, whatever the compiler placed next.
        int64_t target = -1;
        if ((in & 0xfc000000) == 0x14000000)
            target = static_cast<int64_t>(i) + (static_cast<int32_t>(in << 6) >> 6);  // b
        else if ((in & 0xff000010) == 0x54000000 || (in & 0x7e000000) == 0x34000000)
            target = static_cast<int64_t>(i) + (static_cast<int32_t>(in << 8) >> 13);  // b.cond, cbz, cbnz
        else if ((in & 0x7e000000) == 0x36000000)
            target = static_cast<int64_t>(i) + (static_cast<int32_t>(in << 13) >> 18);  // tbz, tbnz
        if (target > reach)
            reach = target;
        if ((in == 0xd65f03c0 || (in & 0xfc000000) == 0x14000000) && static_cast<int64_t>(i) >= reach)
            break;  // ret, or a tail call
        // Another write of the register forgets it.
        loaded[rt] = -1;
    }
    return false;
}
#endif

} // namespace

bool CodeTestsBits(uintptr_t function, uint32_t displacement, uint32_t mask, size_t length) {
#if UIH_DISASSEMBLER
    const Decoder& decoder = GetDecoder();
    uintptr_t start = FollowJumps(function);
    if (size_t extent = FunctionExtent(start))
        length = std::min(length, extent);
    std::vector<uint8_t> bytes(length + kMaxInstruction);
    size_t size = ReadUpTo(start, bytes.data(), bytes.size());
    // The registers loaded from the value, and the byte of the value they start at.
    std::vector<std::pair<ZydisRegister, int>> loaded;
    auto forget = [&](ZydisRegister reg) {
        loaded.erase(std::remove_if(loaded.begin(), loaded.end(), [&](const std::pair<ZydisRegister, int>& entry) { return entry.first == reg; }),
                     loaded.end());
    };
    size_t reach = 0;
    for (size_t at = 0; at < std::min(size, length);) {
        // endbr64 or endbr32 past every branch: the next function, after one
        // that ended with a call that does not return, such as a sanitizer's report.
        if (at > 0 && at >= reach && at + 4 <= size && bytes[at] == 0xf3 && bytes[at + 1] == 0x0f && bytes[at + 2] == 0x1e &&
            (bytes[at + 3] == 0xfa || bytes[at + 3] == 0xfb))
            break;
        Instruction in;
        if (!decoder.Decode(bytes.data() + at, size - at, in))
            break;
        const ZydisDecodedInstruction& d = in.decoded;
        const ZydisDecodedOperand* operands = in.operands;
        size_t next = at + d.length;
        // The furthest byte a branch seen so far goes to: a ret or jmp after it
        // ends the function, whatever the compiler placed next (MSVC pads with
        // int3, GCC and Clang with multi-byte nops or the next function).
        bool jump = d.mnemonic == ZYDIS_MNEMONIC_JMP;
        if ((jump || d.meta.category == ZYDIS_CATEGORY_COND_BR) && d.operand_count_visible >= 1 &&
            operands[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE && operands[0].imm.is_relative) {
            ZyanU64 target = 0;
            if (ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&d, &operands[0], start + at, &target)) && target > start + at)
                reach = std::max<size_t>(reach, static_cast<size_t>(target - start));
        }
        if (d.mnemonic == ZYDIS_MNEMONIC_INT3 || ((d.mnemonic == ZYDIS_MNEMONIC_RET || jump) && next > reach))
            break;
        bool testing = d.mnemonic == ZYDIS_MNEMONIC_TEST || d.mnemonic == ZYDIS_MNEMONIC_BT || d.mnemonic == ZYDIS_MNEMONIC_AND;
        if (testing && d.operand_count_visible >= 2 && operands[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
            uint64_t immediate = operands[1].imm.value.u;
            if (operands[0].size < 64)
                immediate &= (uint64_t{1} << operands[0].size) - 1;
            if (d.mnemonic == ZYDIS_MNEMONIC_BT)
                immediate = immediate < 32 ? uint64_t{1} << immediate : 0;
            int shift = -1;
            if (IsMember(operands[0])) {
                int64_t delta = operands[0].mem.disp.value - static_cast<int64_t>(displacement);
                shift = delta >= 0 && delta < 4 ? static_cast<int>(delta) : -1;
            } else if (operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER) {
                ZydisRegister reg = ZydisRegisterGetLargestEnclosing(kMode, operands[0].reg.value);
                for (const auto& entry : loaded)
                    if (entry.first == reg)
                        shift = entry.second + (IsHighByte(operands[0].reg.value) ? 1 : 0);
            }
            if (shift >= 0 && immediate && (immediate << (8 * shift)) == mask)
                return true;
        }
        // mov, movzx or movsx of the value into a register.
        bool load = (d.mnemonic == ZYDIS_MNEMONIC_MOV || d.mnemonic == ZYDIS_MNEMONIC_MOVZX || d.mnemonic == ZYDIS_MNEMONIC_MOVSX) &&
                    d.operand_count_visible >= 2 && operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER && IsMember(operands[1]);
        if (operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER && (operands[0].actions & ZYDIS_OPERAND_ACTION_MASK_WRITE)) {
            ZydisRegister reg = ZydisRegisterGetLargestEnclosing(kMode, operands[0].reg.value);
            forget(reg);
            int64_t delta = load ? operands[1].mem.disp.value - static_cast<int64_t>(displacement) : -1;
            if (delta >= 0 && delta < 4)
                loaded.push_back({reg, static_cast<int>(delta)});
        }
        at = next;
    }
    return false;
#elif defined(__aarch64__) || defined(_M_ARM64)
    return TestsBitsArm64(function, displacement, mask, length);
#else
    (void)function, (void)displacement, (void)mask, (void)length;
    return false;
#endif
}

bool HandleCode(const std::string& command, const std::string& rest, std::vector<uint8_t>& payload, std::string& answer) {
    if (command != "disasm" && command != "patch" && command != "unpatch" && command != "patches" && command != "signature")
        return false;
    const Config& config = GetConfig();
    if (!config.remoteMemory)
        answer = "error memory access is off: set config.remoteMemory in example/config.cpp and rebuild the library";
    else if (!config.aiAdvancedFeatures)
        answer = "error the advanced features are off: set config.aiAdvancedFeatures in example/config.cpp and rebuild the library";
    else if (!MemoryAllowed())
        answer = "error the player paused the memory tools in the AI Advanced Features window of the overlay";
    else if (command == "patch")
        answer = Patch(rest);
    else if (command == "unpatch")
        answer = Unpatch(rest);
    else if (command == "patches")
        answer = List(payload);
#if UIH_DISASSEMBLER
    else if (command == "disasm")
        answer = Disassemble(rest, payload);
    else
        answer = Signature(rest, payload);
#else
    else
        answer = "error the disassembler decodes x86 and x64 code only, not the code of this processor";
#endif
    return true;
}

} // namespace uih::detail::remote
