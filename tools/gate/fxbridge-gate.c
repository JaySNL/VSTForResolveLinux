// fxbridge-gate - let DaVinci Resolve 21.1 load an external Fairlight plugin library again.
//
// WHAT 21.1 CHANGED
//
// FLPluginHost::Initialize() is what reads BMDPlugins.Path and dlopens the library named there.
// In 21.0 it does that immediately. In 21.1 it first asks Resolve's core interface one yes/no
// question, keyed on the string "Debug", and returns without doing anything when the answer is no:
//
//     call  QString::fromAscii_helper("Debug", 5)
//     call  *0x1d0(%rbx)          ; a virtual on the core interface -> bool
//     test  %bpl,%bpl
//     je    +0xd4                 ; false: never read the setting, never load anything
//
// The same virtual is called from 78 places in libFairlightPage.so; the four that pass a literal
// pass "Studio", "ProxyGenerator", "RemoteControl" and "Debug". It is a module gate that sits
// beside the edition check, not a preference - measured, not assumed. Writing a "Debug" row into
// configs/Fairlight/FLDebugSettings.csv was tried first and changes nothing, because that store is
// read by DebugSettingsManager::GetDebugSetting, which returns a map rather than the bool this
// gate reads.
//
// WHAT THIS DOES
//
// Replaces the six bytes of that `je` with NOPs, in memory, after Resolve has loaded the library
// and before it runs the function. The `test` is left alone; its result is simply ignored.
//
// Nothing is written to disk. Resolve's own files are untouched, no root is needed, and removing
// LD_PRELOAD removes the change. On 21.0 and earlier the gate is not there, this finds nothing and
// says so.
//
// WHY IT HOOKS dlopen
//
// libFairlightPage.so is not linked into bin/resolve - `readelf -d` lists no such NEEDED entry, it
// is dlopened at runtime. So a constructor here would run long before the library exists. The hook
// patches the first time a dlopen hands back a handle that knows the symbol.
//
//     cc -shared -fPIC -O2 -o libfxbridge-gate.so fxbridge-gate.c -ldl
//     LD_PRELOAD=/path/to/libfxbridge-gate.so /opt/resolve/bin/resolve

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

// The function that reads BMDPlugins.Path. Anchoring on the symbol matters: the gate's raw bytes
// occur TWICE in libFairlightPage.so, so a plain signature search over the library would be a
// coin toss. Inside this one function the sequence is unique.
static const char kTarget[] = "_ZN12FLPluginHost10InitializeEv";

// `test %bpl,%bpl` followed by `je rel32`. The six bytes of the je are what gets replaced.
static const unsigned char kGate[5] = { 0x40, 0x84, 0xed, 0x0f, 0x84 };

// `call *0x1d0(%rax)` then `mov %eax,%ebp` - the query whose answer the gate acts on. Required to
// appear before the gate, so this cannot fire on some unrelated test/je that happens to match.
static const unsigned char kQuery[8] = { 0xff, 0x90, 0xd0, 0x01, 0x00, 0x00, 0x89, 0xc5 };

// How far into the function to look. The gate sits at +0x90 in 21.1.0.0014; 0x200 leaves room for
// it to move without letting the search wander into the next function.
#define SCAN_BYTES 0x200

static int g_done = 0;

static void say(const char* format, ...)
{
    char line[512];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);
    // stderr, because that is where Resolve's own log ends up - see README.
    fprintf(stderr, "[fxbridge-gate] %s\n", line);
    fflush(stderr);
}

// Find one occurrence of `needle` in `hay`, and say how many there are.
static const unsigned char* find_once(const unsigned char* hay, size_t hay_size,
                                      const unsigned char* needle, size_t needle_size,
                                      int* count)
{
    const unsigned char* found = NULL;
    *count = 0;
    if (needle_size > hay_size) {
        return NULL;
    }
    for (size_t index = 0; index + needle_size <= hay_size; ++index) {
        if (memcmp(hay + index, needle, needle_size) == 0) {
            if (*count == 0) {
                found = hay + index;
            }
            ++(*count);
        }
    }
    return found;
}

static void patch_gate(void* handle)
{
    if (g_done) {
        return;
    }

    void* const function = dlsym(handle, kTarget);
    if (function == NULL) {
        return;  // not the Fairlight library, or a build without that symbol
    }
    g_done = 1;  // one attempt, whatever the outcome: a retry loop on a live process is not a fix

    unsigned char* const body = (unsigned char*)function;
    int queries = 0;
    const unsigned char* const query =
        find_once(body, SCAN_BYTES, kQuery, sizeof(kQuery), &queries);
    int gates = 0;
    const unsigned char* const gate = find_once(body, SCAN_BYTES, kGate, sizeof(kGate), &gates);

    if (gates == 0) {
        say("no gate in %s - this Resolve loads plugin libraries on its own (21.0 or earlier)",
            kTarget);
        return;
    }
    if (gates != 1 || queries != 1 || query == NULL || query > gate) {
        // Refuse rather than guess. A second match means the shape changed and the offset this
        // would write is no longer known to be the right one.
        say("REFUSED: %d gate matches and %d query matches in %s - the shape changed, "
            "and patching the wrong six bytes is worse than not loading plugins",
            gates, queries, kTarget);
        return;
    }

    unsigned char* const je = (unsigned char*)gate + 3;  // past `test %bpl,%bpl`

    const long page_size = sysconf(_SC_PAGESIZE);
    unsigned char* const first = (unsigned char*)((size_t)je & ~(size_t)(page_size - 1));
    unsigned char* const last =
        (unsigned char*)((size_t)(je + 6 - 1) & ~(size_t)(page_size - 1));
    const size_t span = (size_t)(last - first) + (size_t)page_size;

    if (mprotect(first, span, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        say("mprotect failed, the gate stays closed");
        return;
    }
    memset(je, 0x90, 6);
    if (mprotect(first, span, PROT_READ | PROT_EXEC) != 0) {
        say("warning: could not restore the page to read+execute");
    }

    say("gate open: 6 bytes at %s+0x%lx are NOPs, so Resolve reads BMDPlugins.Path again",
        kTarget, (unsigned long)(je - body));
}

// The interposed dlopen. Resolve loads libFairlightPage.so this way, so this is the first moment
// the function exists to be patched.
void* dlopen(const char* file, int mode)
{
    static void* (*real)(const char*, int) = NULL;
    if (real == NULL) {
        real = (void* (*)(const char*, int))dlsym(RTLD_NEXT, "dlopen");
        if (real == NULL) {
            return NULL;
        }
    }
    void* const handle = real(file, mode);
    if (handle != NULL) {
        patch_gate(handle);
    }
    return handle;
}

// And a try at startup, for the case where the library is linked rather than dlopened.
__attribute__((constructor)) static void at_start(void)
{
    void* const self = dlopen(NULL, RTLD_NOW | RTLD_GLOBAL);
    if (self != NULL) {
        patch_gate(self);
    }
}
