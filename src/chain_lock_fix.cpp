#include "chain_lock_fix.h"

#include <dlfcn.h>
#include <link.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace {

void (*g_log)(const char*, ...) = nullptr;

#define LOG(...)                    \
    do {                            \
        if (g_log != nullptr) {     \
            g_log(__VA_ARGS__);     \
        }                           \
    } while (false)

using ResetHistoryFn = void (*)(void*, bool);
using MutexFn = void (*)(void*);

ResetHistoryFn g_chain_reset = nullptr;
ResetHistoryFn g_chain_reset_thunk = nullptr;
MutexFn g_mutex_lock = nullptr;
MutexFn g_mutex_unlock = nullptr;

// Read out of the library, never written down. 0x218 and 0x250 in 21.0.4 and 21.1.
long g_history_lock = -1;
long g_lock_flag = -1;

// What the stock thunk subtracts from `this` before it reaches the primary entry. 32 in both
// builds, and the mangled name says so too, but the name is not what runs.
long g_thunk_adjust = 0;

// ---------------------------------------------------------------------------
// Reading an offset out of an instruction.
//
// The alternative is a table of constants, and that table is wrong the day Blackmagic recompiles.
// These two decoders find "something at a fixed offset from a base register" and hand back the
// offset, so a build that moved the member still works and a build that changed shape refuses.
// ---------------------------------------------------------------------------

// True for a REX prefix. Every one of them, not only 0x48: a destination in r8 to r15 sets REX.R
// and the byte reads 0x4c, which is how the cross-reference scan in tools/check-offsets.py missed
// FLPluginHost::Initialize until it was fixed.
bool IsRex(unsigned char byte) { return byte >= 0x40 && byte <= 0x4f; }

// ModRM for "disp32 from `base`, into any register": mod is 10, r/m is the base register.
bool IsDisp32From(unsigned char modrm, unsigned int base) {
    return (modrm & 0xc0) == 0x80 && (modrm & 0x07) == base;
}

// The first `lea disp32(%base), %reg` in `code`, or false.
bool FindLea(const unsigned char* code, size_t length, unsigned int base, long* out) {
    for (size_t at = 0; at + 7 <= length; ++at) {
        size_t op = at;
        if (IsRex(code[op])) {
            ++op;
        }
        if (op + 6 > length || code[op] != 0x8d || !IsDisp32From(code[op + 1], base)) {
            continue;
        }
        int32_t disp = 0;
        std::memcpy(&disp, code + op + 2, sizeof(disp));
        *out = disp;
        return true;
    }
    return false;
}

// The first `mov disp32(%base), %reg8` in `code`, or false. The REX is optional here: the flag
// lands in %al in one of the two functions and in %r15b in the other.
bool FindMovByte(const unsigned char* code, size_t length, unsigned int base, long* out) {
    for (size_t at = 0; at + 6 <= length; ++at) {
        size_t op = at;
        if (IsRex(code[op])) {
            ++op;
        }
        if (op + 6 > length || code[op] != 0x8a || !IsDisp32From(code[op + 1], base)) {
            continue;
        }
        int32_t disp = 0;
        std::memcpy(&disp, code + op + 2, sizeof(disp));
        *out = disp;
        return true;
    }
    return false;
}

constexpr unsigned int kRdi = 7;
constexpr unsigned int kRbx = 3;
constexpr size_t kPrologueBytes = 0x60;

// `add $imm, %rdi` at the top of the stock thunk, as a positive adjustment.
bool FindThunkAdjust(const unsigned char* code, size_t length, long* out) {
    for (size_t at = 0; at + 4 <= length; ++at) {
        if (code[at] != 0x48) {
            continue;
        }
        if (code[at + 1] == 0x83 && code[at + 2] == 0xc7) {  // add imm8, %rdi
            *out = -static_cast<long>(static_cast<int8_t>(code[at + 3]));
            return true;
        }
        if (at + 7 <= length && code[at + 1] == 0x81 && code[at + 2] == 0xc7) {  // add imm32, %rdi
            int32_t value = 0;
            std::memcpy(&value, code + at + 3, sizeof(value));
            *out = -static_cast<long>(value);
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// The wrapper.
// ---------------------------------------------------------------------------

// Mirrors Resolve's own condition. Both lock sites read this byte first and skip the mutex when it
// is zero, so an unconditional lock here would stall the interface thread on objects Resolve
// deliberately leaves unlocked. The patch changes the order of two locks and nothing else.
bool Guarded(const void* self) {
    return *(reinterpret_cast<const unsigned char*>(self) + g_lock_flag) != 0;
}

void* HistoryMutex(void* self) {
    return reinterpret_cast<unsigned char*>(self) + g_history_lock;
}

// Proof that the patched slot is on the live path, and not a slot nothing calls. Once each, then
// silent: this runs on the interface thread on every locate.
void ReportFirstCall(const char* route, bool guarded) {
    static bool said_primary = false;
    static bool said_thunk = false;
    bool* const said = route[0] == 'p' ? &said_primary : &said_thunk;
    if (*said) {
        return;
    }
    *said = true;
    LOG("lockfix: a %s ResetHistory came through the wrapper, mutex %s", route,
        guarded ? "taken first" : "skipped the way Resolve skips it");
}

void ChainResetHistory(void* self, bool flag) {
    const bool guarded = Guarded(self);
    ReportFirstCall("primary", guarded);
    if (guarded) {
        g_mutex_lock(HistoryMutex(self));
    }
    g_chain_reset(self, flag);
    if (guarded) {
        g_mutex_unlock(HistoryMutex(self));
    }
}

// The secondary vtable entry hands us the second base subobject. The stock thunk still does the
// adjusting for the call itself; the adjustment is needed here only to find the mutex.
void ChainResetHistoryThunk(void* self, bool flag) {
    void* const primary = reinterpret_cast<unsigned char*>(self) - g_thunk_adjust;
    const bool guarded = Guarded(primary);
    ReportFirstCall("thunked", guarded);
    if (guarded) {
        g_mutex_lock(HistoryMutex(primary));
    }
    g_chain_reset_thunk(self, flag);
    if (guarded) {
        g_mutex_unlock(HistoryMutex(primary));
    }
}

// ---------------------------------------------------------------------------
// Finding the vtable slots.
//
// By value, not by index. A slot index is version specific and it only covers BMDChainFX itself,
// while the address appears in every vtable of every class that inherits the method - and this
// library carries a second one. Scanning the library's own writable data for the pointer finds
// all of them and cannot drift.
// ---------------------------------------------------------------------------

struct Segment {
    unsigned char* start;
    size_t length;
};

struct ScanRequest {
    const char* wanted;
    Segment found[8];
    size_t count;
};

int CollectSegments(struct dl_phdr_info* info, size_t, void* user) {
    auto* const request = static_cast<ScanRequest*>(user);
    if (info->dlpi_name == nullptr || std::strstr(info->dlpi_name, request->wanted) == nullptr) {
        return 0;
    }
    for (int index = 0; index < info->dlpi_phnum; ++index) {
        const ElfW(Phdr)& header = info->dlpi_phdr[index];
        if (header.p_type != PT_LOAD || (header.p_flags & PF_W) == 0) {
            continue;
        }
        if (request->count >= sizeof(request->found) / sizeof(request->found[0])) {
            break;
        }
        request->found[request->count].start =
            reinterpret_cast<unsigned char*>(info->dlpi_addr + header.p_vaddr);
        request->found[request->count].length = header.p_memsz;
        ++request->count;
    }
    return 1;
}

bool MakeWritable(void* address) {
    const long page_size = sysconf(_SC_PAGESIZE);
    const auto page =
        reinterpret_cast<uintptr_t>(address) & ~static_cast<uintptr_t>(page_size - 1);
    return mprotect(reinterpret_cast<void*>(page), static_cast<size_t>(page_size),
                    PROT_READ | PROT_WRITE) == 0;
}

// Replaces every pointer equal to `from` with `to`, and returns how many.
unsigned int ReplacePointers(const ScanRequest& segments, void* from, void* to) {
    unsigned int replaced = 0;
    for (size_t index = 0; index < segments.count; ++index) {
        auto* const start = reinterpret_cast<void**>(segments.found[index].start);
        const size_t words = segments.found[index].length / sizeof(void*);
        for (size_t word = 0; word < words; ++word) {
            if (start[word] != from) {
                continue;
            }
            if (!MakeWritable(&start[word])) {
                LOG("lockfix: mprotect failed at %p, leaving the rest alone", &start[word]);
                return replaced;
            }
            start[word] = to;
            ++replaced;
        }
    }
    return replaced;
}

bool ResolveSymbols(void* handle) {
    g_chain_reset = reinterpret_cast<ResetHistoryFn>(
        dlsym(handle, "_ZN10BMDChainFX12ResetHistoryEb"));
    g_chain_reset_thunk = reinterpret_cast<ResetHistoryFn>(
        dlsym(handle, "_ZThn32_N10BMDChainFX12ResetHistoryEb"));
    auto* const base_reset = reinterpret_cast<const unsigned char*>(
        dlsym(handle, "_ZN18BMDAudioPluginImpl12ResetHistoryEb"));
    auto* const pre_process = reinterpret_cast<const unsigned char*>(
        dlsym(handle, "_ZN18BMDAudioPluginImpl10PreProcessEPPflm"));

    if (g_chain_reset == nullptr || g_chain_reset_thunk == nullptr || base_reset == nullptr) {
        LOG("lockfix: BMDChainFX::ResetHistory is not where it was - not patching");
        return false;
    }

    // The history mutex and the byte that gates it, read out of the function that takes them.
    long mutex_at = -1;
    long flag_at = -1;
    if (!FindLea(base_reset, kPrologueBytes, kRdi, &mutex_at) ||
        !FindMovByte(base_reset, kPrologueBytes, kRdi, &flag_at)) {
        LOG("lockfix: BMDAudioPluginImpl::ResetHistory does not read a mutex and a flag off "
            "`this` - not patching");
        return false;
    }

    // The cross-check that decides whether those two numbers mean what we think. PreProcess is
    // the other half of the inversion, it keeps `this` in %rbx, and it must name the same two.
    if (pre_process != nullptr) {
        long other_mutex = -1;
        long other_flag = -1;
        const bool read = FindLea(pre_process, 0x80, kRbx, &other_mutex) &&
                          FindMovByte(pre_process, 0x80, kRbx, &other_flag);
        if (!read || other_mutex != mutex_at || other_flag != flag_at) {
            LOG("lockfix: PreProcess names mutex +0x%lx flag +0x%lx, ResetHistory names +0x%lx "
                "+0x%lx - not patching",
                other_mutex, other_flag, mutex_at, flag_at);
            return false;
        }
    } else {
        LOG("lockfix: PreProcess not exported, so the offsets are unconfirmed - not patching");
        return false;
    }

    // The inversion has to actually be there. BMDChainFX::ResetHistory takes the chain mutex
    // first; if it already took the history one there is nothing to reorder.
    long chain_mutex = -1;
    if (!FindLea(reinterpret_cast<const unsigned char*>(g_chain_reset), kPrologueBytes, kRdi,
                 &chain_mutex)) {
        LOG("lockfix: BMDChainFX::ResetHistory locks nothing off `this` - not patching");
        return false;
    }
    if (chain_mutex == mutex_at) {
        LOG("lockfix: BMDChainFX::ResetHistory already takes the history mutex first - "
            "nothing to fix");
        return false;
    }

    if (!FindThunkAdjust(reinterpret_cast<const unsigned char*>(g_chain_reset_thunk),
                         kPrologueBytes, &g_thunk_adjust) ||
        g_thunk_adjust <= 0) {
        LOG("lockfix: the thunk does not adjust `this` the way it used to - not patching");
        return false;
    }

    // Inside Resolve these are already global, because the executable links libc++. In a test
    // process that only dlopened the stock library they are reachable through its handle instead,
    // and asking both costs nothing.
    g_mutex_lock = reinterpret_cast<MutexFn>(
        dlsym(RTLD_DEFAULT, "_ZNSt3__115recursive_mutex4lockEv"));
    if (g_mutex_lock == nullptr) {
        g_mutex_lock = reinterpret_cast<MutexFn>(
            dlsym(handle, "_ZNSt3__115recursive_mutex4lockEv"));
    }
    g_mutex_unlock = reinterpret_cast<MutexFn>(
        dlsym(RTLD_DEFAULT, "_ZNSt3__115recursive_mutex6unlockEv"));
    if (g_mutex_unlock == nullptr) {
        g_mutex_unlock = reinterpret_cast<MutexFn>(
            dlsym(handle, "_ZNSt3__115recursive_mutex6unlockEv"));
    }
    if (g_mutex_lock == nullptr || g_mutex_unlock == nullptr) {
        LOG("lockfix: Resolve's libc++ does not export recursive_mutex::lock - not patching");
        return false;
    }

    g_history_lock = mutex_at;
    g_lock_flag = flag_at;
    LOG("lockfix: history mutex +0x%lx, gate byte +0x%lx, chain mutex +0x%lx, thunk adjusts %ld",
        g_history_lock, g_lock_flag, chain_mutex, g_thunk_adjust);
    return true;
}

}  // namespace

bool InstallChainLockFix(void* handle, void (*log)(const char*, ...)) {
    g_log = log;

    const char* const disabled = std::getenv("FXBRIDGE_LOCKFIX");
    if (disabled != nullptr && std::strcmp(disabled, "0") == 0) {
        LOG("lockfix: off by request (FXBRIDGE_LOCKFIX=0)");
        return false;
    }
    if (handle == nullptr) {
        LOG("lockfix: no handle for the stock library - not patching");
        return false;
    }
    if (!ResolveSymbols(handle)) {
        return false;
    }

    ScanRequest segments = {"libBMDAudioPlugins.so", {}, 0};
    dl_iterate_phdr(CollectSegments, &segments);
    if (segments.count == 0) {
        LOG("lockfix: libBMDAudioPlugins.so has no writable segment - not patching");
        return false;
    }

    const unsigned int primary = ReplacePointers(segments, reinterpret_cast<void*>(g_chain_reset),
                                                 reinterpret_cast<void*>(&ChainResetHistory));
    const unsigned int thunk =
        ReplacePointers(segments, reinterpret_cast<void*>(g_chain_reset_thunk),
                        reinterpret_cast<void*>(&ChainResetHistoryThunk));

    if (primary == 0 && thunk == 0) {
        LOG("lockfix: ResetHistory is in no vtable we can reach - not patching");
        return false;
    }
    LOG("lockfix: ResetHistory now takes the history mutex first - %u vtable slot(s) and %u "
        "thunk slot(s) rewritten. See docs/freeze-deadlock.md.",
        primary, thunk);
    return true;
}
