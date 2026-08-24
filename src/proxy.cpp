// Fairlight FX bridge for DaVinci Resolve on Linux.
//
// Stage 3a: our own vtable sits in the call path, and QueryPluginList is instrumented.
//
// Resolve's Fairlight page loads exactly one audio-plugin library. FLPluginHost::Initialize()
// reads the config key "BMDPlugins.Path" (from configs/config-fairlight.dat), and when that key
// holds a path it loads that file instead of the stock /opt/resolve/libs/libBMDAudioPlugins.so.
// It then resolves the unmangled entry point GetBMDPluginInterface, calls it with no arguments,
// and requires vtable slot 0 (GetPluginInterfaceVersion) to return exactly 100.
//
// The override is exclusive: when we are loaded, the stock library is not. So we load the stock
// library ourselves, take its bmd::PluginInterface, and repoint the object at a vtable we own.
// Two slots become trampolines; the other four keep the stock addresses. Nothing changes yet —
// the trampolines record the call and tail-jump to the original.
//
// The trampolines are written in assembly on purpose. QueryPluginList and CreatePluginInstance
// return C++ objects whose exact types we have not recovered, so a returned value may travel
// through a hidden sret pointer. A tail jump forwards every calling convention untouched; a C++
// wrapper with a guessed signature would not.
//
// To recover the plugin-list type, the QueryPluginList trampoline logs its argument registers,
// calls the stock function, and logs what comes back. Whether the first argument is `this` or a
// hidden sret pointer decides the convention, and we know the interface address to compare it
// against. Reads of returned memory go through process_vm_readv, which reports EFAULT instead of
// faulting, so a wrong guess about a pointer cannot take Resolve down.

#include "clap_host.h"
#include "carla_host.h"
#include "vst2_host.h"
#include "plugin_instance.h"

#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstring>

#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <atomic>
#include <mutex>
#include <vector>

namespace {

constexpr const char* kStockLibrary = "/opt/resolve/libs/libBMDAudioPlugins.so";
constexpr const char* kEntryPoint = "GetBMDPluginInterface";

// bmd::PluginInterface, read from the relocations of `vtable for BMDPluginInterfaceImpl`.
enum Slot {
    kGetPluginInterfaceVersion = 0,
    kQueryPluginList = 1,
    kCreatePluginInstance = 2,
    kSetAudioPluginHost = 3,
    kSetPluginUserInterfaceHost = 4,
    kQueryMacroFXResourceList = 5,
    kSlotCount = 6,
};

constexpr int kExpectedInterfaceVersion = 100;

using EntryPointFn = void* (*)();
using GetVersionFn = int (*)(const void*);

void Log(const char* format, ...) __attribute__((format(printf, 1, 2)));

void Log(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    std::fputs("[fxbridge] ", stderr);
    std::vfprintf(stderr, format, args);
    std::fputc('\n', stderr);
    std::fflush(stderr);
    va_end(args);
}

// ---------------------------------------------------------------------------
// Which host runs.
//
// FXBRIDGE_HOST=carla puts Carla in the chain instead of the CLAP host. Carla loads VST2, VST3,
// LV2, CLAP and AU itself, and a Windows plugin bridged by yabridge presents as a native VST2 or
// VST3 - so one host reaches every format, and its patchbay is the patcher. FXBRIDGE_CARLA_MODE
// picks "rack" (a serial chain, the default) or "patchbay" (arbitrary routing).
//
// The CLAP host stays the default: it is direct, it has no second process, and it is the one that
// has been exercised.
// ---------------------------------------------------------------------------

namespace {

// Which loader is behind the wrapper. Everything below this line is the same for all of them:
// the Fairlight carrier, the vtable claim, the editor visibility owner and the window.
enum class Loader { Clap, Carla, Vst2 };

Loader SelectedLoader()
{
    const char* const host = std::getenv("FXBRIDGE_HOST");
    if (host == nullptr) {
        return Loader::Clap;
    }
    if (std::strcmp(host, "carla") == 0) {
        return Loader::Carla;
    }
    if (std::strcmp(host, "vst2") == 0) {
        return Loader::Vst2;
    }
    return Loader::Clap;
}

bool UsingCarla() { return SelectedLoader() == Loader::Carla; }
bool UsingVst2() { return SelectedLoader() == Loader::Vst2; }

// Which plugin the configured path names, and what to call it in the menu before anything is
// loaded. The scanner will replace both: one entry per plugin found, named from the file.
const char* ConfiguredPluginPath()
{
    const char* const vst2 = std::getenv("FXBRIDGE_VST2");
    if (vst2 != nullptr && vst2[0] != '\0') {
        return vst2;
    }
    const char* const clap = std::getenv("FXBRIDGE_CLAP");
    if (clap != nullptr && clap[0] != '\0') {
        return clap;
    }
    return "/home/jooshua/.clap/DragonflyHallReverb.clap";
}

// The file's own name, with its directory and extension removed. Reading it costs nothing, while
// loading a plugin to ask its name can start a Wine process.
const char* MenuNameFromPath(const char* path)
{
    static char name[128];
    const char* const slash = std::strrchr(path, '/');
    std::snprintf(name, sizeof(name), "%s", slash != nullptr ? slash + 1 : path);
    char* const dot = std::strrchr(name, '.');
    if (dot != nullptr) {
        *dot = '\0';
    }
    return name;
}

}  // namespace

// ---------------------------------------------------------------------------
// One plugin per effect.
//
// The audio path holds no shared state. Each claimed effect owns its plugin and its dry buffer,
// and a lookup on the audio thread is a linear scan over a fixed array with an atomic count -
// entries are appended under a lock and never removed, so a reader never waits and never sees a
// half-written entry.
//
// This replaces one global dry buffer, two global counters and one shared plugin. Resolve renders
// on several threads, so with more than one effect those globals were written by two threads at
// once and the same non-re-entrant plugin was called from both. Every crash inside Resolve came
// back to BridgeAfterProcess, and it only ever appeared once a project carried a second effect.
// ---------------------------------------------------------------------------

namespace {

constexpr size_t kMaxClaimedEffects = 64;

struct ClaimedEffect {
    // Resolve may call us with either the bmd::PluginInstance subobject or the AudioPlugin one,
    // so both addresses map to the same effect.
    void* instance_key = nullptr;
    void* audio_plugin_key = nullptr;
    HostedPlugin* plugin = nullptr;
    std::vector<float> dry;
    unsigned long dry_frames = 0;
    unsigned int dry_channels = 0;
};

ClaimedEffect g_effects[kMaxClaimedEffects];
std::atomic<size_t> g_effect_count{0};
std::mutex g_effect_append_lock;

// The effect whose editor Resolve last opened. The window is shared, so one editor shows at a time.
std::atomic<ClaimedEffect*> g_focused_effect{nullptr};

ClaimedEffect* FindEffect(const void* key)
{
    const size_t count = g_effect_count.load(std::memory_order_acquire);
    for (size_t index = 0; index < count; ++index) {
        ClaimedEffect& effect = g_effects[index];
        if (effect.instance_key == key || effect.audio_plugin_key == key) {
            return &effect;
        }
    }
    return nullptr;
}

ClaimedEffect* AppendEffect(void* instance_key, void* audio_plugin_key)
{
    const std::lock_guard<std::mutex> guard(g_effect_append_lock);
    const size_t count = g_effect_count.load(std::memory_order_relaxed);
    if (count >= kMaxClaimedEffects) {
        return nullptr;
    }
    ClaimedEffect& effect = g_effects[count];
    effect.instance_key = instance_key;
    effect.audio_plugin_key = audio_plugin_key;
    // Published last: a reader that sees the new count sees a complete entry.
    g_effect_count.store(count + 1, std::memory_order_release);
    return &effect;
}

}  // namespace

// ---------------------------------------------------------------------------
// Editor visibility.
//
// One place decides whether the hosted window should be on screen, for every host. Before this,
// each host was wired separately - the CLAP window through Resolve's editor calls, the Carla window
// through a claim-time open - so every new host needed its own fix and a change to one broke the
// other.
//
// Two rules, and they are what makes it self-healing:
//
//   * Any signal that means "the user is looking at this effect" asks for the window. Asking twice
//     is free, because both hosts remap an existing window instead of building a second one.
//   * The state is re-asserted on the idle tick. If the window went away without telling us - a
//     window manager, a host that dropped it - the next tick puts it back, rather than leaving a
//     button that does nothing.
//
// A close that the user performed is remembered, so re-asserting never fights them.
// ---------------------------------------------------------------------------

namespace {

std::atomic<bool> g_editor_wanted{false};
std::atomic<bool> g_editor_shown{false};

}  // namespace

extern "C" void BridgeEditorShow(const char* because)
{
    const bool was_shown = g_editor_shown.exchange(true);
    g_editor_wanted.store(true);
    ClaimedEffect* const effect = g_focused_effect.load();
    if (effect == nullptr || effect->plugin == nullptr || !effect->plugin->OpenEditor()) {
        g_editor_shown.store(false);
        Log("editor: %s asked for the window and the host refused", because);
        return;
    }
    if (!was_shown) {
        Log("editor: shown (%s)", because);
    }
}

extern "C" void BridgeEditorHide(const char* because)
{
    g_editor_wanted.store(false);
    if (g_editor_shown.exchange(false)) {
        ClaimedEffect* const effect = g_focused_effect.load();
        if (effect != nullptr && effect->plugin != nullptr) {
            effect->plugin->CloseEditor();
        }
        Log("editor: hidden (%s)", because);
    }
}

// The host telling us its window is gone. Not a request to bring it back.
extern "C" void BridgeEditorWasClosedByUser()
{
    g_editor_wanted.store(false);
    g_editor_shown.store(false);
}

// Called from the idle tick. Puts the window back if it should be there and is not.
extern "C" void BridgeEditorReassert()
{
    if (g_editor_wanted.load() && !g_editor_shown.load()) {
        BridgeEditorShow("re-asserted");
    }
}


// The vtable we hand out. Two entries below the function pointers carry offset-to-top and the
// typeinfo pointer, so dynamic_cast on the interface keeps working.
void* g_vtable[2 + kSlotCount];
const void* g_interface = nullptr;
void* g_stock_handle = nullptr;

// Make the page holding `address` writable. The interface object may live in a mapping that RELRO
// has already turned read-only; without this the vptr write would fault.
bool MakeWritable(void* address)
{
    const long page_size = sysconf(_SC_PAGESIZE);
    auto page = reinterpret_cast<uintptr_t>(address) & ~static_cast<uintptr_t>(page_size - 1);
    if (mprotect(reinterpret_cast<void*>(page), page_size, PROT_READ | PROT_WRITE) != 0) {
        Log("mprotect on %p failed", address);
        return false;
    }
    return true;
}

void* LoadStockInterface()
{
    void* const handle = dlopen(kStockLibrary, RTLD_NOW | RTLD_LOCAL);
    g_stock_handle = handle;
    if (handle == nullptr) {
        Log("dlopen(%s) failed: %s", kStockLibrary, dlerror());
        return nullptr;
    }

    auto* const entry = reinterpret_cast<EntryPointFn>(dlsym(handle, kEntryPoint));
    if (entry == nullptr) {
        Log("dlsym(%s) failed: %s", kEntryPoint, dlerror());
        return nullptr;
    }

    void* const interface_ptr = entry();
    if (interface_ptr == nullptr) {
        Log("%s returned null", kEntryPoint);
    }
    return interface_ptr;
}

}  // namespace

// Trampoline state and call counters. Hidden visibility keeps the assembly references
// RIP-relative instead of going through the GOT.
#define BRIDGE_HIDDEN __attribute__((visibility("hidden")))

extern "C" {
BRIDGE_HIDDEN void* g_original_query_plugin_list = nullptr;
BRIDGE_HIDDEN void* g_original_create_plugin_instance = nullptr;

BRIDGE_HIDDEN void BridgeInspectQueryEntry(const void* arg0, unsigned long arg1, unsigned long arg2);
BRIDGE_HIDDEN void BridgeInspectQueryExit(void* result);
BRIDGE_HIDDEN void* BridgeInspectCreateEntry(const void* self, unsigned long category, const void* key);
BRIDGE_HIDDEN void BridgeInspectCreateExit(void* result);

BRIDGE_HIDDEN void BridgeThunkQueryPluginList();
BRIDGE_HIDDEN void BridgeThunkCreatePluginInstance();
}

namespace {
constexpr int kMaxLoggedCalls = 20;
int g_query_calls = 0;
}  // namespace

// ---------------------------------------------------------------------------
// Safe introspection.
//
// process_vm_readv against our own pid copies memory and reports EFAULT for an unmapped page,
// where a plain dereference would fault. Every read of a pointer we have not proven goes through
// this, so a wrong guess costs a log line instead of the application.
// ---------------------------------------------------------------------------

namespace {

bool SafeRead(const void* address, void* out, size_t length)
{
    if (address == nullptr) {
        return false;
    }
    iovec local{out, length};
    iovec remote{const_cast<void*>(address), length};
    return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == static_cast<ssize_t>(length);
}

bool LooksLikePointer(unsigned long value)
{
    return value > 0x10000 && value < 0x800000000000UL && (value & 7) == 0;
}

// Qt5 QString holds a QArrayData* whose header is {int ref; int size; uint alloc; qptrdiff offset}
// and whose UTF-16 payload starts at header + offset. Read it back as Latin-1 for the log.
bool ReadQStringData(const void* data_pointer, char* out, size_t out_size)
{
    struct Header {
        int ref;
        int size;
        unsigned int alloc;
        long offset;
    } header;

    if (!SafeRead(data_pointer, &header, sizeof(header))) {
        return false;
    }
    if (header.size <= 0 || header.size > 128 || header.offset < 0 || header.offset > 4096) {
        return false;
    }

    unsigned short utf16[128];
    const auto* payload = static_cast<const unsigned char*>(data_pointer) + header.offset;
    const size_t count = static_cast<size_t>(header.size);
    if (!SafeRead(payload, utf16, count * sizeof(unsigned short))) {
        return false;
    }

    size_t written = 0;
    for (size_t i = 0; i < count && written + 1 < out_size; ++i) {
        out[written++] = (utf16[i] >= 0x20 && utf16[i] < 0x7f) ? static_cast<char>(utf16[i]) : '.';
    }
    out[written] = '\0';
    return written > 0;
}

// Dump one machine word per line: raw value, and whatever it turns out to point at.
void DumpWords(const char* label, const void* address, int count)
{
    unsigned long words[16];
    if (count > 16) {
        count = 16;
    }
    if (!SafeRead(address, words, static_cast<size_t>(count) * sizeof(unsigned long))) {
        Log("  %s @%p unreadable", label, address);
        return;
    }

    for (int i = 0; i < count; ++i) {
        char text[160];
        if (ReadQStringData(reinterpret_cast<const void*>(words[i]), text, sizeof(text))) {
            Log("  %s[%d] = 0x%016lx  QString \"%s\"", label, i, words[i], text);
            continue;
        }

        unsigned long inner = 0;
        if (LooksLikePointer(words[i]) &&
            SafeRead(reinterpret_cast<const void*>(words[i]), &inner, sizeof(inner))) {
            char inner_text[160];
            if (ReadQStringData(reinterpret_cast<const void*>(inner), inner_text, sizeof(inner_text))) {
                Log("  %s[%d] = 0x%016lx -> 0x%016lx  QString \"%s\"",
                    label, i, words[i], inner, inner_text);
            } else {
                Log("  %s[%d] = 0x%016lx -> 0x%016lx", label, i, words[i], inner);
            }
            continue;
        }
        Log("  %s[%d] = 0x%016lx", label, i, words[i]);
    }
}

}  // namespace


// libc++ std::map<QString, AudioPluginDefinition> returned by value.
//
// The container is three words: {__begin_node_, root, size}. A node is
// {__left_, __right_, __parent_, __is_black_ (padded)} followed by the value pair at +32, so the
// QString key sits at +32 and the AudioPluginDefinition begins at +40. Nothing here is guessed
// past that: every read is checked, and the keys must come back as the effect names in the menu.
namespace {

extern const char kSourcePluginKey[];
extern const void* g_source_node;

constexpr int kNodeLeft = 0;
constexpr int kNodeRight = 1;
constexpr int kNodeKey = 4;    // +32 bytes
constexpr int kNodeValue = 5;  // +40 bytes

unsigned long NodeWord(const void* node, int index)
{
    unsigned long value = 0;
    if (!SafeRead(static_cast<const unsigned char*>(node) + index * 8, &value, sizeof(value))) {
        return 0;
    }
    return value;
}

void WalkPluginMap(const void* container, bool quiet)
{
    g_source_node = nullptr;

    unsigned long header[3];
    if (!SafeRead(container, header, sizeof(header))) {
        if (!quiet) { Log("  plugin map @%p unreadable", container); }
        return;
    }
    if (!quiet) { Log("  map: begin=0x%lx root=0x%lx size=%lu", header[0], header[1], header[2]); }

    // In-order walk without a parent chain: an explicit stack is simpler to verify than __tree_next.
    const void* stack[64];
    int depth = 0;
    const void* node = reinterpret_cast<const void*>(header[1]);
    int listed = 0;

    while ((node != nullptr || depth > 0) && listed < 64) {
        while (node != nullptr && depth < 64) {
            stack[depth++] = node;
            node = reinterpret_cast<const void*>(NodeWord(node, kNodeLeft));
        }
        if (depth == 0) {
            break;
        }
        node = stack[--depth];

        char name[160];
        const auto key = reinterpret_cast<const void*>(NodeWord(node, kNodeKey));
        if (!ReadQStringData(key, name, sizeof(name))) {
            std::snprintf(name, sizeof(name), "<key 0x%lx unreadable>", reinterpret_cast<unsigned long>(key));
        }

        if (std::strcmp(name, kSourcePluginKey) == 0) {
            g_source_node = node;
        }

        if (quiet) {
            // nothing to report
        } else if (listed < 6) {
            Log("  [%02d] \"%s\"", listed, name);
            for (int word = 0; word < 8; ++word) {
                char text[160];
                const unsigned long raw = NodeWord(node, kNodeValue + word);
                if (ReadQStringData(reinterpret_cast<const void*>(raw), text, sizeof(text))) {
                    Log("        def+%02d = 0x%016lx  QString \"%s\"", word * 8, raw, text);
                } else {
                    Log("        def+%02d = 0x%016lx", word * 8, raw);
                }
            }
        } else {
            Log("  [%02d] \"%s\"", listed, name);
        }
        ++listed;

        node = reinterpret_cast<const void*>(NodeWord(node, kNodeRight));
    }
    if (!quiet) { Log("  walked %d entries", listed); }
}

}  // namespace


// ---------------------------------------------------------------------------
// Adding an entry of our own.
//
// The stock library exports the map's own insert helper, so we do not have to reproduce libc++
// node allocation or red-black rebalancing:
//
//   std::__tree<__value_type<QString, BMDAudioPluginFactory::PluginDefinition>, …>
//       ::__emplace_unique_key_args<QString, std::pair<QString const, PluginDefinition>>(
//           QString const& key, pair&& value)
//
// The node is copy-constructed from the pair we pass, so every QString inside the definition is
// deep-copied by Qt with correct reference counts. We build our own key and display name as
// *static* QStringData (ref = -1, the marker Qt uses for QStringLiteral), which Qt never frees and
// never refcounts — that removes the whole ownership question from this experiment.
// ---------------------------------------------------------------------------

namespace {

const char kSourcePluginKey[] = "Delay:1112360057";

// Measured, not guessed: __emplace_unique_key_args allocates the node with `mov $0x68,%edi` before
// `operator new`, so a node is 104 bytes. Take off the 32-byte node header and the 8-byte QString
// key and BMDAudioPluginFactory::PluginDefinition is 64 bytes. The interface version gate (100)
// guards this number against a Resolve update.
constexpr size_t kNodeBytes = 0x68;
constexpr size_t kNodeHeaderBytes = 32;
constexpr size_t kPairBytes = kNodeBytes - kNodeHeaderBytes;  // 72: key + definition

// Qt5 QArrayData followed by its UTF-16 payload.
struct StaticQtString {
    int reference_count;
    int size;
    unsigned int allocated_and_flags;
    long offset;
    unsigned short text[64];
};

void InitStaticQtString(StaticQtString& target, const char* ascii)
{
    int length = 0;
    while (ascii[length] != '\0' && length < 63) {
        target.text[length] = static_cast<unsigned short>(ascii[length]);
        ++length;
    }
    target.text[length] = 0;
    target.reference_count = -1;  // static: Qt neither counts nor frees it
    target.size = length;
    target.allocated_and_flags = 0;
    target.offset = offsetof(StaticQtString, text);
}

// One clone of the source entry, named after the plugin we host. The menu entry, the effect slot on
// the track and the audio are then the same thing under one name, instead of a Delay that secretly
// sounds like something else. The category stays Uncategorized - see the note further down.
struct Clone {
    const char* key;
    const char* name;
    StaticQtString key_string;
    StaticQtString name_string;
    void* key_slot;
};

char g_clone_key_text[160];
char g_clone_name_text[128];

Clone g_clones[] = {
    {g_clone_key_text, g_clone_name_text, {}, {}, nullptr},
};

char kOurPluginKey[160] = "Bridge Test:1112360057";  // the clone the create hook redirects
const void* g_source_node = nullptr;

using EmplaceFn = void* (*)(void* tree, const void* key, void* value_pair);

const char kEmplaceSymbol[] =
    "_ZNSt3__16__treeINS_12__value_typeI7QStringN21BMDAudioPluginFactory16PluginDefinitionEEENS_19"
    "__map_value_compareIS2_S5_NS_4lessIS2_EELb1EEENS_9allocatorIS5_EEE25__emplace_unique_key_args"
    "IS2_JNS_4pairIKS2_S4_EEEEENSE_INS_15__tree_iteratorIS5_PNS_11__tree_nodeIS5_PvEElEEbEERKT_DpOT0_";

void InsertOurEntry(void* container)
{
    if (g_source_node == nullptr || g_stock_handle == nullptr) {
        return;
    }

    static auto* const emplace = reinterpret_cast<EmplaceFn>(dlsym(g_stock_handle, kEmplaceSymbol));
    if (emplace == nullptr) {
        Log("  insert skipped: __emplace_unique_key_args not found");
        return;
    }

    static bool strings_ready = false;
    if (!strings_ready) {
        // The hosted plugin supplies the name. Keep the source effect's numeric id, because that is
        // what the stock factory needs when the create hook redirects the key.
        const char* const hosted = MenuNameFromPath(ConfiguredPluginPath());
        std::snprintf(g_clone_name_text, sizeof(g_clone_name_text), "%s",
                      hosted != nullptr ? hosted : "Bridge Test");
        std::snprintf(g_clone_key_text, sizeof(g_clone_key_text), "%s:1112360057", g_clone_name_text);
        std::snprintf(kOurPluginKey, sizeof(kOurPluginKey), "%s", g_clone_key_text);

        for (auto& clone : g_clones) {
            InitStaticQtString(clone.key_string, clone.key);
            InitStaticQtString(clone.name_string, clone.name);
            clone.key_slot = &clone.key_string;
        }
        strings_ready = true;
        Log("  menu entry will read \"%s\"", g_clone_name_text);
    }

    unsigned char value_pair[kPairBytes];
    const auto* source_pair = static_cast<const unsigned char*>(g_source_node) + kNodeHeaderBytes;

    for (auto& clone : g_clones) {
        if (!SafeRead(source_pair, value_pair, sizeof(value_pair))) {
            Log("  insert skipped: source entry unreadable");
            return;
        }

        // Swap in this clone's key and display name. The other 56 bytes stay the source's.
        void* name_data = &clone.name_string;
        std::memcpy(value_pair + 0, &clone.key_slot, sizeof(void*));  // pair.first = key
        std::memcpy(value_pair + 8, &name_data, sizeof(void*));       // definition.name

        emplace(container, &clone.key_slot, value_pair);
        if (g_query_calls <= kMaxLoggedCalls) {
            Log("  inserted \"%s\" as \"%s\"", clone.key, clone.name);
        }
    }
}

}  // namespace

extern "C" void BridgeInspectQueryEntry(const void* arg0, unsigned long arg1, unsigned long arg2)
{
    if (++g_query_calls > kMaxLoggedCalls) {
        return;
    }
    // If arg0 is the interface, the result is a scalar in rax and arg1 is the category.
    // If it is not, arg0 is a hidden sret pointer and the interface has moved to arg1.
    Log("QueryPluginList #%d entry: arg0=%p arg1=0x%lx arg2=0x%lx  interface=%p  (%s)",
        g_query_calls, arg0, arg1, arg2, g_interface,
        arg0 == g_interface ? "arg0 is this: value returned in rax"
                            : "arg0 is an sret pointer: this is arg1");
}

extern "C" void BridgeInspectQueryExit(void* result)
{
    const bool quiet = g_query_calls > kMaxLoggedCalls;
    if (!quiet) {
        Log("QueryPluginList #%d exit: rax=%p", g_query_calls, result);
        DumpWords("rax", result, 3);
    }
    WalkPluginMap(result, quiet);
    InsertOurEntry(result);
}

// Save every argument register, report the call, restore, then tail-jump to the stock function.
// The extra 8 bytes keep the stack 16-byte aligned across the call.
#define BRIDGE_TRAMPOLINE(name, note, target)   \
    ".text\n"                                   \
    ".globl " name "\n"                         \
    ".hidden " name "\n"                        \
    ".type " name ", @function\n"               \
    name ":\n"                                  \
    "  pushq %rdi\n"                            \
    "  pushq %rsi\n"                            \
    "  pushq %rdx\n"                            \
    "  pushq %rcx\n"                            \
    "  pushq %r8\n"                             \
    "  pushq %r9\n"                             \
    "  subq $8, %rsp\n"                         \
    "  call " note "\n"                         \
    "  addq $8, %rsp\n"                         \
    "  popq %r9\n"                              \
    "  popq %r8\n"                              \
    "  popq %rcx\n"                             \
    "  popq %rdx\n"                             \
    "  popq %rsi\n"                             \
    "  popq %rdi\n"                             \
    "  jmp *" target "(%rip)\n"

asm(".text\n"
    ".globl BridgeThunkQueryPluginList\n"
    ".hidden BridgeThunkQueryPluginList\n"
    ".type BridgeThunkQueryPluginList, @function\n"
    "BridgeThunkQueryPluginList:\n"
    "  pushq %rbp\n"
    "  movq %rsp, %rbp\n"
    "  pushq %rdi\n"
    "  pushq %rsi\n"
    "  pushq %rdx\n"
    "  pushq %rcx\n"
    "  pushq %r8\n"
    "  pushq %r9\n"
    "  call BridgeInspectQueryEntry\n"   // arg registers are already in place
    "  popq %r9\n"
    "  popq %r8\n"
    "  popq %rcx\n"
    "  popq %rdx\n"
    "  popq %rsi\n"
    "  popq %rdi\n"
    "  call *g_original_query_plugin_list(%rip)\n"
    "  subq $16, %rsp\n"
    "  movq %rax, (%rsp)\n"
    "  movq %rax, %rdi\n"
    "  call BridgeInspectQueryExit\n"
    "  movq (%rsp), %rax\n"             // rax also carries the sret address on a memory return
    "  addq $16, %rsp\n"
    "  movq %rbp, %rsp\n"
    "  popq %rbp\n"
    "  ret\n");

asm(".text\n"
    ".globl BridgeThunkCreatePluginInstance\n"
    ".hidden BridgeThunkCreatePluginInstance\n"
    ".type BridgeThunkCreatePluginInstance, @function\n"
    "BridgeThunkCreatePluginInstance:\n"
    "  pushq %rbp\n"
    "  movq %rsp, %rbp\n"
    "  pushq %rdi\n"
    "  pushq %rsi\n"
    "  pushq %rdx\n"
    "  pushq %rcx\n"
    "  pushq %r8\n"
    "  pushq %r9\n"
    "  call BridgeInspectCreateEntry\n"    // arg registers are already in place
    "  movq %rax, %r10\n"                  // a replacement QString argument, or null
    "  popq %r9\n"
    "  popq %r8\n"
    "  popq %rcx\n"
    "  popq %rdx\n"
    "  popq %rsi\n"
    "  popq %rdi\n"
    "  testq %r10, %r10\n"
    "  je .Lbridge_keep_key\n"
    "  movq %r10, %rdx\n"
    ".Lbridge_keep_key:\n"
    "  call *g_original_create_plugin_instance(%rip)\n"
    "  subq $16, %rsp\n"
    "  movq %rax, (%rsp)\n"
    "  movq %rax, %rdi\n"
    "  call BridgeInspectCreateExit\n"
    "  movq (%rsp), %rax\n"
    "  addq $16, %rsp\n"
    "  movq %rbp, %rsp\n"
    "  popq %rbp\n"
    "  ret\n");



// ---------------------------------------------------------------------------
// Wrapping the instance.
//
// bmd::PluginInstance is abstract, and the concrete BMDAudioPluginImpl is a QObject: slots 0-11 are
// QObject's own, the plugin API starts at slot 12, and the audio callbacks sit at the end:
//
//   66: PreProcess(float**, long, unsigned long)
//   67: PostProcess(float**, float**, unsigned long)
//
// So we do not build an instance. We let the stock factory build a real one and repoint it at a
// copy of its own vtable, with those two slots replaced. Every QObject mechanism, the parameter
// panel and the automation stay genuine, and only the audio path becomes ours.
//
// This step only observes. It calls the stock callback, then reports the buffers and the frame
// count. Writing samples comes after we have seen what the arguments really are.
// ---------------------------------------------------------------------------

namespace {

constexpr int kInstanceSlotCount = 80;   // past slot 67; spare slots are copied, never called
constexpr int kSlotGetAudioPluginInterface = 12;
constexpr int kSlotPreProcess = 66;
constexpr int kSlotPostProcess = 67;

void* g_instance_vtable[2 + kInstanceSlotCount];
void* g_stock_instance_vtable = nullptr;
bool g_instance_vtable_built = false;
int g_instances_wrapped = 0;
bool g_creating_ours = false;
int g_pre_process_calls = 0;
int g_post_process_calls = 0;

}  // namespace

#define BRIDGE_HIDDEN2 __attribute__((visibility("hidden")))

extern "C" {
BRIDGE_HIDDEN2 void* g_original_get_audio_interface = nullptr;
BRIDGE_HIDDEN2 void* g_original_pre_process = nullptr;
BRIDGE_HIDDEN2 void* g_original_post_process = nullptr;
BRIDGE_HIDDEN2 void BridgeAfterPreProcess(void* self, float** buffers, long a, unsigned long frames);
BRIDGE_HIDDEN2 void BridgeAfterPostProcess(void* self, float** input, float** output, unsigned long frames);
BRIDGE_HIDDEN2 void BridgeAfterGetAudioInterface(void* self);
BRIDGE_HIDDEN2 void BridgeThunkGetAudioInterface();
BRIDGE_HIDDEN2 void BridgeThunkPreProcess();
BRIDGE_HIDDEN2 void BridgeThunkPostProcess();
}

extern "C" void BridgeAfterGetAudioInterface(void* returned)
{
    static int calls = 0;
    if (++calls > 3 || returned == nullptr) {
        return;
    }
    unsigned long vptr = 0;
    SafeRead(returned, &vptr, sizeof(vptr));
    Log("GetAudioPluginInterface #%d returned %p, its vtable is 0x%lx", calls, returned, vptr);
}

extern "C" void BridgeAfterPreProcess(void* self, float** buffers, long a, unsigned long frames)
{
    (void)self;
    if (++g_pre_process_calls <= 3) {
        Log("PreProcess #%d: buffers=%p arg=%ld frames=%lu", g_pre_process_calls, buffers, a, frames);
    }
}

extern "C" void BridgeAfterPostProcess(void* self, float** input, float** output, unsigned long frames)
{
    (void)self;
    if (++g_post_process_calls > 3) {
        return;
    }

    // Read one sample through the checked reader before trusting either buffer.
    float first_in = 0.0f;
    float first_out = 0.0f;
    unsigned long input_channel = 0;
    unsigned long output_channel = 0;
    SafeRead(input, &input_channel, sizeof(input_channel));
    SafeRead(output, &output_channel, sizeof(output_channel));
    SafeRead(reinterpret_cast<const void*>(input_channel), &first_in, sizeof(first_in));
    SafeRead(reinterpret_cast<const void*>(output_channel), &first_out, sizeof(first_out));

    Log("PostProcess #%d: frames=%lu in[0]=%p out[0]=%p first in=%.6f out=%.6f",
        g_post_process_calls, frames,
        reinterpret_cast<void*>(input_channel), reinterpret_cast<void*>(output_channel),
        static_cast<double>(first_in), static_cast<double>(first_out));
}

// Save the arguments, run the stock callback, restore them, then report. The original's return
// value is preserved across our reporter.
// A Process trampoline with a half on each side of the stock call.
//
// The five arguments (this, timebase, input, output, frames) go to the stack on entry, because the
// stock call clobbers every one of those registers and the second half needs them again. The stock
// return value is kept in the same frame and handed back untouched - we never have to know its type.
#define BRIDGE_PROCESS_THUNK(name, target) \
    ".text\n" \
    ".globl " name "\n" \
    ".hidden " name "\n" \
    ".type " name ", @function\n" \
    name ":\n" \
    "  .cfi_startproc\n" \
    "  pushq %rbp\n" \
    "  .cfi_def_cfa_offset 16\n" \
    "  .cfi_offset %rbp, -16\n" \
    "  movq %rsp, %rbp\n" \
    "  .cfi_def_cfa_register %rbp\n" \
    "  subq $48, %rsp\n" \
    "  movq %rdi, -8(%rbp)\n" \
    "  movq %rsi, -16(%rbp)\n" \
    "  movq %rdx, -24(%rbp)\n" \
    "  movq %rcx, -32(%rbp)\n" \
    "  movq %r8, -40(%rbp)\n" \
    "  call BridgeBeforeProcess\n" \
    "  movq -8(%rbp), %rdi\n" \
    "  movq -16(%rbp), %rsi\n" \
    "  movq -24(%rbp), %rdx\n" \
    "  movq -32(%rbp), %rcx\n" \
    "  movq -40(%rbp), %r8\n" \
    "  call *" target "(%rip)\n" \
    "  movq %rax, -48(%rbp)\n" \
    "  movq -8(%rbp), %rdi\n" \
    "  movq -16(%rbp), %rsi\n" \
    "  movq -24(%rbp), %rdx\n" \
    "  movq -32(%rbp), %rcx\n" \
    "  movq -40(%rbp), %r8\n" \
    "  call BridgeAfterProcess\n" \
    "  movq -48(%rbp), %rax\n" \
    "  movq %rbp, %rsp\n" \
    "  popq %rbp\n" \
    "  .cfi_def_cfa %rsp, 8\n" \
    "  ret\n" \
    "  .cfi_endproc\n" \
    ".size " name ", .-" name "\n"

#define BRIDGE_AUDIO_THUNK(name, target, reporter) \
    ".text\n"                                      \
    ".globl " name "\n"                            \
    ".hidden " name "\n"                           \
    ".type " name ", @function\n"                  \
    name ":\n"                                     \
    "  pushq %rbp\n"                               \
    "  movq %rsp, %rbp\n"                          \
    "  pushq %rdi\n"                               \
    "  pushq %rsi\n"                               \
    "  pushq %rdx\n"                               \
    "  pushq %rcx\n"                               \
    "  pushq %r8\n"                                \
    "  pushq %r9\n"                                \
    "  call *" target "(%rip)\n"                   \
    "  popq %r9\n"                                 \
    "  popq %r8\n"                                 \
    "  popq %rcx\n"                                \
    "  popq %rdx\n"                                \
    "  popq %rsi\n"                                \
    "  popq %rdi\n"                                \
    "  pushq %rax\n"                               \
    "  subq $8, %rsp\n"                            \
    "  call " reporter "\n"                        \
    "  addq $8, %rsp\n"                            \
    "  popq %rax\n"                                \
    "  movq %rbp, %rsp\n"                          \
    "  popq %rbp\n"                                \
    "  ret\n"

asm(BRIDGE_AUDIO_THUNK("BridgeThunkGetAudioInterface", "g_original_get_audio_interface", "BridgeAfterGetAudioInterface"));
asm(BRIDGE_AUDIO_THUNK("BridgeThunkPreProcess", "g_original_pre_process", "BridgeAfterPreProcess"));
asm(BRIDGE_AUDIO_THUNK("BridgeThunkPostProcess", "g_original_post_process", "BridgeAfterPostProcess"));

namespace {

// Resolve builds more than one instance for a single effect on a track, and only one of them ends
// up in the audio graph. Wrap every instance we are asked to create, not the first.
void WrapInstance(void* instance)
{
    if (instance == nullptr) {
        return;
    }

    void** const current_vtable = *reinterpret_cast<void***>(instance);
    if (current_vtable == &g_instance_vtable[2]) {
        return;  // already ours
    }

    if (!g_instance_vtable_built) {
        if (!SafeRead(current_vtable - 2, g_instance_vtable, sizeof(g_instance_vtable))) {
            Log("  instance wrap skipped: vtable unreadable");
            return;
        }
        g_original_get_audio_interface = g_instance_vtable[2 + kSlotGetAudioPluginInterface];
        g_instance_vtable[2 + kSlotGetAudioPluginInterface] =
            reinterpret_cast<void*>(&BridgeThunkGetAudioInterface);
        g_original_pre_process = g_instance_vtable[2 + kSlotPreProcess];
        g_original_post_process = g_instance_vtable[2 + kSlotPostProcess];
        g_instance_vtable[2 + kSlotPreProcess] = reinterpret_cast<void*>(&BridgeThunkPreProcess);
        g_instance_vtable[2 + kSlotPostProcess] = reinterpret_cast<void*>(&BridgeThunkPostProcess);
        g_stock_instance_vtable = current_vtable;
        g_instance_vtable_built = true;
        Log("  built our instance vtable from %p: PreProcess %p, PostProcess %p",
            static_cast<void*>(current_vtable), g_original_pre_process, g_original_post_process);
    }

    // Our shared copy is only valid for the class we copied it from.
    if (current_vtable != g_stock_instance_vtable) {
        Log("  instance %p has vtable %p, not the one we copied (%p) - left alone",
            instance, static_cast<void*>(current_vtable), g_stock_instance_vtable);
        return;
    }

    if (!MakeWritable(instance)) {
        Log("  instance wrap skipped: the object is not writable");
        return;
    }
    *reinterpret_cast<void***>(instance) = &g_instance_vtable[2];
    ++g_instances_wrapped;
    Log("  instance %p wrapped (%d so far)", instance, g_instances_wrapped);
}

}  // namespace

// ---------------------------------------------------------------------------
// Instantiating our entry.
//
// CreatePluginInstance(bmd::PluginCategory, QString const&) const returns a pointer, so there is no
// sret here: rdi = this, esi = category, rdx = &QString. The stock factory has no effect under our
// key, so selecting our entry produces nothing. Until we build our own bmd::PluginInstance, the
// trampoline swaps the QString argument for the source key, and Resolve builds the effect we cloned.
// ---------------------------------------------------------------------------

namespace {

StaticQtString g_source_key_string;
void* g_source_key_slot = nullptr;
int g_create_inspections = 0;

// Read the QString a `QString const&` argument points at.
bool ReadQStringArgument(const void* reference, char* out, size_t out_size)
{
    unsigned long data_pointer = 0;
    if (!SafeRead(reference, &data_pointer, sizeof(data_pointer))) {
        return false;
    }
    return ReadQStringData(reinterpret_cast<const void*>(data_pointer), out, out_size);
}

}  // namespace

// Switches, with the proven arrangement as the default.
//
// Each of these was bisected on 2026-08-24 rather than reasoned about, because reasoning about
// them produced three wrong answers in a row. Set a switch to "0" to turn a default-on behaviour
// off, or to "1" to turn a default-off one on.
//
//   FXBRIDGE_RENAME          on   the effect names itself after the hosted plugin
//   FXBRIDGE_EMPTY_PANEL     off  empties the panel by truncating the tree's item list. OFF after
//                                 a crash on this path: the item count is not stable across
//                                 effects (25 on one, 19 on another), so the list is not the
//                                 vector of pointers it was read as, and something else may still
//                                 index into it.
//   FXBRIDGE_PRIMARY_PROCESS off  hooks Process at +0x4d0 as well - this one crashed Resolve
static bool EnabledByEnvironment(const char* name, bool on_by_default = false)
{
    const char* const value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return on_by_default;
    }
    return value[0] != '0';
}

extern "C" void BridgeSuppressStockUserInterface(bool on);

// Returns a replacement for the QString argument, or null to leave it alone.
extern "C" void* BridgeInspectCreateEntry(const void* self, unsigned long category, const void* key)
{
    (void)self;

    char name[160];
    const bool readable = ReadQStringArgument(key, name, sizeof(name));
    const bool ours = readable && std::strcmp(name, kOurPluginKey) == 0;

    if (++g_create_inspections <= kMaxLoggedCalls) {
        Log("CreatePluginInstance #%d: category=%lu key=\"%s\"%s",
            g_create_inspections, category, readable ? name : "<unreadable>",
            ours ? "  <- ours, substituting the source key" : "");
    }

    g_creating_ours = ours;
    if (!ours) {
        return nullptr;
    }

    // The control tree is built inside CreatePluginInstance, so claiming the instance afterwards is
    // too late to stop it. The stock slot is redirected for the length of this one call and put
    // back in the exit - the window is a single call on Resolve's own thread.
    // No stock-vtable patch here. GenerateUserInterface is called on our own instance *after* the
    // claim - the trace caught it twice through our copy - so the no-op in our vtable is enough.
    // Patching the shared vtable for the length of the create call would hand our no-op to any
    // other Delay being built on another thread.

    if (g_source_key_slot == nullptr) {
        InitStaticQtString(g_source_key_string, kSourcePluginKey);
        g_source_key_slot = &g_source_key_string;
    }
    return &g_source_key_slot;
}

extern "C" void BridgeClaimInstance(void* instance);

extern "C" void BridgeInspectCreateExit(void* result)
{
    if (g_create_inspections <= kMaxLoggedCalls) {
        Log("CreatePluginInstance #%d returned %p%s", g_create_inspections, result,
            result == nullptr ? "  (nothing was created)" : "");
    }

    // Claim only what we asked for. A Delay the editor added stays a stock Delay, because its vptrs
    // still point at the stock vtable - that is the whole reason the class vtable is no longer
    // patched.
    if (g_creating_ours && result != nullptr) {
        BridgeClaimInstance(result);
    }

    g_creating_ours = false;  // the editor now follows Resolve's own open and close calls
}




namespace {

// Which plugin to run. Override with FXBRIDGE_CLAP; every plugin on this machine ships a .clap.
const char kDefaultClapPlugin[] = "/home/jooshua/.clap/DragonflyHallReverb.clap";

// Loggers only. Plugins are loaded per effect now, at claim time, not once at startup.
void LoadConfiguredPlugin()
{
    ClapHostSetLogger([](const char* line) { Log("%s", line); });
    CarlaHostSetLogger([](const char* line) { Log("%s", line); });
    Vst2HostSetLogger([](const char* line) { Log("%s", line); });
    PluginInstanceSetLogger([](const char* line) { Log("%s", line); });

    const char* const path = ConfiguredPluginPath();
    Log("plugin: the menu entry will load %s", path);
}

}  // namespace


// ---------------------------------------------------------------------------
// Knobs.
//
// BMDStereoDelay::SetParameterValue(unsigned int, float) at +0x310 is the call Resolve makes when a
// knob on the panel moves. The index arrives in esi and the value in xmm0, so this trampoline runs
// the reporter *before* the stock function and saves xmm0 across it - the stock code is free to
// clobber the float register, our reporter is not.
//
// The value's range is not documented anywhere, so this build reports what really arrives and only
// forwards positions that are already normalised. The mapping is written after the numbers are in,
// not before.
// ---------------------------------------------------------------------------

namespace {

constexpr size_t kSetParameterValueOffset = 0x310;
constexpr size_t kHasEditorOffset = 0x258;
constexpr size_t kInitializeEffectEditOffset = 0x250;
constexpr size_t kGetEffectEditOffset = 0x260;
constexpr size_t kCloseEffectEditOffset = 0x278;

// The same four calls again, in the AudioPlugin base vtable that starts at +0x648. Resolve's
// Fairlight engine holds an AudioPlugin*, so these thunks are the slots it really dials.
constexpr size_t kHasEditorThunkOffset = 0x718;
constexpr size_t kInitializeEffectEditThunkOffset = 0x7b0;
constexpr size_t kGetEffectEditThunkOffset = 0x7b8;
constexpr size_t kCloseEffectEditThunkOffset = 0x7d0;
int g_knob_reports = 0;

}  // namespace

extern "C" {
BRIDGE_HIDDEN2 void* g_original_set_parameter = nullptr;
BRIDGE_HIDDEN2 void* g_original_initialize_editor = nullptr;
BRIDGE_HIDDEN2 void* g_original_close_editor = nullptr;
BRIDGE_HIDDEN2 void* g_original_has_editor = nullptr;
BRIDGE_HIDDEN2 void* g_original_get_effect_edit = nullptr;
BRIDGE_HIDDEN2 void* g_original_has_editor_thunk = nullptr;
BRIDGE_HIDDEN2 void* g_original_initialize_editor_thunk = nullptr;
BRIDGE_HIDDEN2 void* g_original_get_effect_edit_thunk = nullptr;
BRIDGE_HIDDEN2 void* g_original_close_editor_thunk = nullptr;
BRIDGE_HIDDEN2 void BridgeOnSetParameter(void* self, unsigned int index, float value);
BRIDGE_HIDDEN2 void BridgeThunkSetParameter();
}

extern "C" void BridgeOnSetParameter(void* self, unsigned int index, float value)
{
    (void)self;

    if (++g_knob_reports <= 24) {
        Log("knob: index %u -> %.6f", index, static_cast<double>(value));
    }

    // Only forward what is unambiguously a 0..1 position. Anything else waits for the mapping.
    if (value >= 0.0f && value <= 1.0f) {
        ClapHostQueueParameterNormalised(index, value);
    }
}

// The editor gate. Resolve asks HasEditor() before it offers an external editor at all, and the
// stock Delay answers false: its panel is the declarative UI, not a window of its own. So the
// InitializeEffectEdit hook never fired. Answering true is what puts this plugin on the path that
// VST-style plugins take - the one that ends in a window we own.
//
// These four are plain C++ replacements, not assembly trampolines: every signature here is
// integers and pointers only, so the compiler's own calling sequence is already correct.

namespace {

int g_editor_reports = 0;

}  // namespace

extern "C" bool BridgeHasEditor(void* self)
{
    bool stock = false;
    if (g_original_has_editor != nullptr) {
        stock = reinterpret_cast<bool (*)(void*)>(g_original_has_editor)(self);
    }
    const ClaimedEffect* const effect = FindEffect(self);
    const bool ours = effect != nullptr && effect->plugin != nullptr &&
                      effect->plugin->ChannelCount() > 0;
    if (++g_editor_reports <= 8) {
        Log("HasEditor: stock says %s, we answer %s",
            stock ? "true" : "false", (stock || ours) ? "true" : "false");
    }
    return stock || ours;
}

extern "C" void* BridgeGetEffectEdit(void* self)
{
    void* widget = nullptr;
    if (g_original_get_effect_edit != nullptr) {
        widget = reinterpret_cast<void* (*)(void*)>(g_original_get_effect_edit)(self);
    }
    Log("GetEffectEdit: stock returns %p", widget);
    return widget;
}

extern "C" long BridgeInitializeEffectEdit(void* self, const char* title, void* parent)
{
    Log("InitializeEffectEdit(\"%s\", %p)", title != nullptr ? title : "(null)", parent);
    long result = 0;
    if (g_original_initialize_editor != nullptr) {
        result = reinterpret_cast<long (*)(void*, const char*, void*)>(
            g_original_initialize_editor)(self, title, parent);
    }
    Log("InitializeEffectEdit: stock returned %ld, opening the hosted editor", result);
    BridgeEditorShow("InitializeEffectEdit");
    return result;
}

extern "C" void BridgeCloseEffectEdit(void* self)
{
    Log("CloseEffectEdit");
    BridgeEditorHide("CloseEffectEdit");
    if (g_original_close_editor != nullptr) {
        reinterpret_cast<void (*)(void*)>(g_original_close_editor)(self);
    }
}

// The AudioPlugin-base copies. `self` arrives pointing at the AudioPlugin subobject, and the saved
// slot value is the thunk itself, so passing `self` straight through keeps the adjustment correct.

extern "C" bool BridgeHasEditorThunk(void* self)
{
    bool stock = false;
    if (g_original_has_editor_thunk != nullptr) {
        stock = reinterpret_cast<bool (*)(void*)>(g_original_has_editor_thunk)(self);
    }
    // The rule: if a host window exists AND Resolve has a control tree to hang an editor on, this
    // effect has an editor.
    //
    // The second half is not optional. The stock answer follows the control tree: true while the
    // Delay knobs are there, false once GenerateUserInterface is suppressed. Answering true anyway
    // makes Resolve build its editor from a tree that was never built, and the first virtual call
    // on that object jumps to a garbage address - measured 2026-08-24, SIGSEGV with the faulting
    // frame at 0x3306 and nothing below it.
    //
    // So an empty panel and Resolve's own show button are mutually exclusive, and the switch that
    // removes the panel also gives up the button.
    // Safe again: the empty panel is now a built tree with its items removed, not an unbuilt one,
    // so Resolve has something valid to hang an editor on.
    const ClaimedEffect* const effect = FindEffect(self);
    const bool ours = effect != nullptr && effect->plugin != nullptr &&
                      effect->plugin->ChannelCount() > 0;
    if (++g_editor_reports <= 8) {
        Log("AudioPlugin::HasEditor: stock says %s, we answer %s",
            stock ? "true" : "false", (stock || ours) ? "true" : "false");
    }
    return stock || ours;
}

extern "C" void* BridgeGetEffectEditThunk(void* self)
{
    void* widget = nullptr;
    if (g_original_get_effect_edit_thunk != nullptr) {
        widget = reinterpret_cast<void* (*)(void*)>(g_original_get_effect_edit_thunk)(self);
    }
    Log("AudioPlugin::GetEffectEdit: stock returns %p", widget);
    return widget;
}

// The return is not a bool: the stock call answered 136 on the first run, and a `bool` return type
// truncates that to the low byte at -O2. Take it as a machine word and hand back exactly what the
// stock code produced - we do not know what Resolve reads out of it.
extern "C" long BridgeInitializeEffectEditThunk(void* self, const char* title, void* parent)
{
    Log("AudioPlugin::InitializeEffectEdit(\"%s\", %p)",
        title != nullptr ? title : "(null)", parent);
    long result = 0;
    if (g_original_initialize_editor_thunk != nullptr) {
        result = reinterpret_cast<long (*)(void*, const char*, void*)>(
            g_original_initialize_editor_thunk)(self, title, parent);
    }
    Log("AudioPlugin::InitializeEffectEdit: stock returned %ld, opening the hosted editor", result);
    BridgeEditorShow("AudioPlugin::InitializeEffectEdit");
    return result;
}

extern "C" void BridgeCloseEffectEditThunk(void* self)
{
    Log("AudioPlugin::CloseEffectEdit");
    BridgeEditorHide("AudioPlugin::CloseEffectEdit");
    if (g_original_close_editor_thunk != nullptr) {
        reinterpret_cast<void (*)(void*)>(g_original_close_editor_thunk)(self);
    }
}

asm(".text\n"
    ".globl BridgeThunkSetParameter\n"
    ".hidden BridgeThunkSetParameter\n"
    ".type BridgeThunkSetParameter, @function\n"
    "BridgeThunkSetParameter:\n"
    "  pushq %rbp\n"
    "  movq %rsp, %rbp\n"
    "  pushq %rdi\n"
    "  pushq %rsi\n"
    "  pushq %rdx\n"
    "  pushq %rcx\n"
    "  pushq %r8\n"
    "  pushq %r9\n"
    "  subq $16, %rsp\n"
    "  movss %xmm0, (%rsp)\n"
    "  call BridgeOnSetParameter\n"     // rdi, esi and xmm0 are already the right arguments
    "  movss (%rsp), %xmm0\n"
    "  addq $16, %rsp\n"
    "  popq %r9\n"
    "  popq %r8\n"
    "  popq %rcx\n"
    "  popq %rdx\n"
    "  popq %rsi\n"
    "  popq %rdi\n"
    "  movq %rbp, %rsp\n"
    "  popq %rbp\n"
    "  jmp *g_original_set_parameter(%rip)\n");

// ---------------------------------------------------------------------------
// The real audio hook.
//
// Two earlier attempts missed, and both were instructive:
//
//   * Patching the instance's vptr fails because CreatePluginInstance hands back a pointer to the
//     secondary bmd::PluginInstance subobject (+0x4f8 into the class vtable group), not the object
//     itself, and because those instances are short-lived - the address we wrapped was later reused
//     as an audio buffer.
//   * PreProcess/PostProcess(float**) are inherited helpers that nothing calls at runtime.
//
// The DSP entry point of every Fairlight effect is its own Process method - 74 classes define one:
//
//   BMDStereoDelay::Process(AudioPluginTimebaseInformation const*, float**, float**, unsigned long)
//
// It sits in the class vtable at +0x4d0, with a non-virtual thunk at +0x690 for calls made through
// a secondary base pointer. Patching the class vtable once covers every instance, every lifetime
// and both call paths. The vtable lives in RELRO, so it needs mprotect first.
// ---------------------------------------------------------------------------

namespace {

const char kDelayVtableSymbol[] = "_ZTV14BMDStereoDelay";
constexpr size_t kProcessPrimaryOffset = 0x4d0;
constexpr size_t kProcessThunkOffset = 0x690;

int g_process_calls = 0;

}  // namespace


extern "C" {
BRIDGE_HIDDEN2 void* g_original_process_primary = nullptr;
BRIDGE_HIDDEN2 void* g_original_process_thunk = nullptr;
BRIDGE_HIDDEN2 void BridgeAfterProcess(void* self, const void* timebase, float** input,
                                       float** output, unsigned long frames);
BRIDGE_HIDDEN2 void BridgeBeforeProcess(void* self, const void* timebase, float** input,
                                        float** output, unsigned long frames);
BRIDGE_HIDDEN2 void BridgeThunkProcessPrimary();
BRIDGE_HIDDEN2 void BridgeThunkProcessSecondary();
}

// Run a CLAP plugin over Resolve's buffers, and keep the stock effect out of the sound.
//
// The host class is BMDStereoDelay, so letting the stock Process run and then processing its output
// puts an audible delay in front of the hosted plugin. Skipping the stock call outright would mean
// inventing its return value, and two return types were already named wrongly today. So instead:
// copy the dry input aside, let the stock call run and answer for itself, then put the dry signal
// back and give the hosted plugin the untouched audio.
//
// The cost is one buffer copy per block. The gain is that the stock effect's own state still
// advances and its return value reaches Resolve exactly as it would have.
//
// Every channel pointer is validated and both ends of the block are probed before the plugin is
// handed anything, so a surprising argument costs a skipped block instead of the application.
constexpr unsigned long kMaxFrames = 8192;
constexpr unsigned int kMaxChannels = 16;

namespace {

// Both ends of the block must be readable before the plugin sees the buffer.
bool ChannelsAreReadable(float** buffers, unsigned int channels, unsigned long frames)
{
    for (unsigned int channel = 0; channel < channels; ++channel) {
        unsigned long channel_pointer = 0;
        if (!SafeRead(buffers + channel, &channel_pointer, sizeof(channel_pointer)) ||
            channel_pointer == 0) {
            return false;
        }
        float probe = 0.0f;
        auto* const samples = reinterpret_cast<float*>(channel_pointer);
        if (!SafeRead(samples, &probe, sizeof(probe)) ||
            !SafeRead(samples + frames - 1, &probe, sizeof(probe))) {
            return false;
        }
    }
    return true;
}

}  // namespace

extern "C" void BridgeBeforeProcess(void* self, const void* timebase, float** input,
                                    float** output, unsigned long frames)
{
    (void)timebase;
    (void)output;

    ClaimedEffect* const effect = FindEffect(self);
    if (effect == nullptr || effect->plugin == nullptr) {
        return;
    }

    const unsigned int wanted = effect->plugin->ChannelCount();
    if (wanted == 0 || wanted > kMaxChannels || input == nullptr || frames == 0 ||
        frames > kMaxFrames || !ChannelsAreReadable(input, wanted, frames)) {
        effect->dry_frames = 0;
        return;
    }

    if (effect->dry.size() < static_cast<size_t>(wanted) * kMaxFrames) {
        effect->dry_frames = 0;
        return;  // allocated at claim time; never allocate on the audio thread
    }

    for (unsigned int channel = 0; channel < wanted; ++channel) {
        std::memcpy(&effect->dry[channel * kMaxFrames], input[channel], frames * sizeof(float));
    }
    effect->dry_frames = frames;
    effect->dry_channels = wanted;
}

extern "C" void BridgeAfterProcess(void* self, const void* timebase, float** input,
                                   float** output, unsigned long frames)
{
    (void)timebase;
    (void)input;

    ClaimedEffect* const effect = FindEffect(self);
    if (effect == nullptr || effect->plugin == nullptr) {
        return;  // not ours: a stock Delay elsewhere in the project
    }

    const unsigned int wanted = effect->plugin->ChannelCount();
    if (wanted == 0 || output == nullptr || frames == 0 || frames > kMaxFrames ||
        !ChannelsAreReadable(output, wanted, frames)) {
        return;
    }

    // Put the dry signal back over whatever the carrier effect wrote, so the hosted plugin hears
    // the track and not a delayed copy of it.
    bool restored = false;
    if (effect->dry_frames == frames && effect->dry_channels == wanted &&
        effect->dry.size() >= static_cast<size_t>(wanted) * kMaxFrames) {
        for (unsigned int channel = 0; channel < wanted; ++channel) {
            std::memcpy(output[channel], &effect->dry[channel * kMaxFrames],
                        frames * sizeof(float));
        }
        restored = true;
    }

    const bool processed = effect->plugin->Process(output, wanted,
                                                   static_cast<unsigned int>(frames));

    if (++g_process_calls <= 3) {
        Log("Process #%d: frames=%lu, %u channels, dry %s, plugin %s",
            g_process_calls, frames, wanted, restored ? "restored" : "MISSING",
            processed ? "ran" : "declined");
    }
}

asm(BRIDGE_PROCESS_THUNK("BridgeThunkProcessPrimary", "g_original_process_primary"));
asm(BRIDGE_PROCESS_THUNK("BridgeThunkProcessSecondary", "g_original_process_thunk"));

namespace {

// Replace one vtable entry in place. The two entries need separate trampolines: the primary slot
// holds the function, the other holds a thunk that adjusts `this` before jumping to it.
bool PatchVtableSlot(unsigned char* vtable, size_t offset, void** saved, void* replacement)
{
    auto* const slot = reinterpret_cast<void**>(vtable + offset);
    if (!MakeWritable(slot)) {
        return false;
    }
    *saved = *slot;
    *slot = replacement;
    return true;
}


}  // namespace

// ---------------------------------------------------------------------------
// The editor trace.
//
// Closing the plugin window with the window manager's X unmaps it, and Resolve never hears about
// it - the plugin has no way to tell the host that its editor went away, because AudioPluginHost
// carries no such notification. So the panel button afterwards does whatever Resolve does when it
// believes the editor is already open, and that is the thing to measure.
//
// The trace arms itself on the X close and logs the first few calls to every remaining editor slot
// in the AudioPlugin base. Press the panel button after closing the window and the log names the
// call that arrives - or shows an empty trace, which says the button reaches nothing at all.
//
// Each trampoline reports and then tail-jumps to the stock function, so the return value is
// whatever the stock code produced. That matters here: InitializeEffectEdit returns a pointer and
// GetPluginType returns something wider than an int, and naming either type wrongly corrupts it.
// ---------------------------------------------------------------------------

#define BRIDGE_TRACE_THUNK(name, original, slot) \
    ".text\n" \
    ".globl " name "\n" \
    ".hidden " name "\n" \
    ".type " name ", @function\n" \
    name ":\n" \
    "  pushq %rbp\n" \
    "  movq %rsp, %rbp\n" \
    "  pushq %rdi\n  pushq %rsi\n  pushq %rdx\n" \
    "  pushq %rcx\n  pushq %r8\n  pushq %r9\n" \
    "  movl $" slot ", %edi\n" \
    "  call BridgeReportSlot\n" \
    "  popq %r9\n  popq %r8\n  popq %rcx\n" \
    "  popq %rdx\n  popq %rsi\n  popq %rdi\n" \
    "  movq %rbp, %rsp\n" \
    "  popq %rbp\n" \
    "  jmp *" original "(%rip)\n" \
    ".size " name ", .-" name "\n"

extern "C" {
BRIDGE_HIDDEN2 void BridgeReportSlot(unsigned int slot);
BRIDGE_HIDDEN2 void* g_trace_original_16 = nullptr;
BRIDGE_HIDDEN2 void BridgeTraceThunk16();
BRIDGE_HIDDEN2 void* g_trace_original_17 = nullptr;
BRIDGE_HIDDEN2 void BridgeTraceThunk17();
BRIDGE_HIDDEN2 void* g_trace_original_18 = nullptr;
BRIDGE_HIDDEN2 void BridgeTraceThunk18();
BRIDGE_HIDDEN2 void* g_trace_original_19 = nullptr;
BRIDGE_HIDDEN2 void BridgeTraceThunk19();
BRIDGE_HIDDEN2 void* g_trace_original_20 = nullptr;
BRIDGE_HIDDEN2 void BridgeTraceThunk20();
BRIDGE_HIDDEN2 void* g_trace_original_21 = nullptr;
BRIDGE_HIDDEN2 void BridgeTraceThunk21();
BRIDGE_HIDDEN2 void* g_trace_original_22 = nullptr;
BRIDGE_HIDDEN2 void BridgeTraceThunk22();
BRIDGE_HIDDEN2 void* g_trace_original_23 = nullptr;
BRIDGE_HIDDEN2 void BridgeTraceThunk23();
BRIDGE_HIDDEN2 void* g_trace_original_0 = nullptr;
BRIDGE_HIDDEN2 void BridgeTraceThunk0();
BRIDGE_HIDDEN2 void* g_trace_original_1 = nullptr;
BRIDGE_HIDDEN2 void BridgeTraceThunk1();
BRIDGE_HIDDEN2 void* g_trace_original_2 = nullptr;
BRIDGE_HIDDEN2 void BridgeTraceThunk2();
BRIDGE_HIDDEN2 void* g_trace_original_3 = nullptr;
BRIDGE_HIDDEN2 void BridgeTraceThunk3();
BRIDGE_HIDDEN2 void* g_trace_original_4 = nullptr;
BRIDGE_HIDDEN2 void BridgeTraceThunk4();
BRIDGE_HIDDEN2 void* g_trace_original_5 = nullptr;
BRIDGE_HIDDEN2 void BridgeTraceThunk5();
BRIDGE_HIDDEN2 void* g_trace_original_6 = nullptr;
BRIDGE_HIDDEN2 void BridgeTraceThunk6();
BRIDGE_HIDDEN2 void* g_trace_original_7 = nullptr;
BRIDGE_HIDDEN2 void BridgeTraceThunk7();
BRIDGE_HIDDEN2 void* g_trace_original_8 = nullptr;
BRIDGE_HIDDEN2 void BridgeTraceThunk8();
BRIDGE_HIDDEN2 void* g_trace_original_9 = nullptr;
BRIDGE_HIDDEN2 void BridgeTraceThunk9();
BRIDGE_HIDDEN2 void* g_trace_original_10 = nullptr;
BRIDGE_HIDDEN2 void BridgeTraceThunk10();
BRIDGE_HIDDEN2 void* g_trace_original_11 = nullptr;
BRIDGE_HIDDEN2 void BridgeTraceThunk11();
BRIDGE_HIDDEN2 void* g_trace_original_12 = nullptr;
BRIDGE_HIDDEN2 void BridgeTraceThunk12();
BRIDGE_HIDDEN2 void* g_trace_original_13 = nullptr;
BRIDGE_HIDDEN2 void BridgeTraceThunk13();
BRIDGE_HIDDEN2 void* g_trace_original_14 = nullptr;
BRIDGE_HIDDEN2 void BridgeTraceThunk14();
BRIDGE_HIDDEN2 void* g_trace_original_15 = nullptr;
BRIDGE_HIDDEN2 void BridgeTraceThunk15();
}

asm(BRIDGE_TRACE_THUNK("BridgeTraceThunk0", "g_trace_original_0", "0"));
asm(BRIDGE_TRACE_THUNK("BridgeTraceThunk1", "g_trace_original_1", "1"));
asm(BRIDGE_TRACE_THUNK("BridgeTraceThunk2", "g_trace_original_2", "2"));
asm(BRIDGE_TRACE_THUNK("BridgeTraceThunk3", "g_trace_original_3", "3"));
asm(BRIDGE_TRACE_THUNK("BridgeTraceThunk4", "g_trace_original_4", "4"));
asm(BRIDGE_TRACE_THUNK("BridgeTraceThunk5", "g_trace_original_5", "5"));
asm(BRIDGE_TRACE_THUNK("BridgeTraceThunk6", "g_trace_original_6", "6"));
asm(BRIDGE_TRACE_THUNK("BridgeTraceThunk7", "g_trace_original_7", "7"));
asm(BRIDGE_TRACE_THUNK("BridgeTraceThunk8", "g_trace_original_8", "8"));
asm(BRIDGE_TRACE_THUNK("BridgeTraceThunk9", "g_trace_original_9", "9"));
asm(BRIDGE_TRACE_THUNK("BridgeTraceThunk10", "g_trace_original_10", "10"));
asm(BRIDGE_TRACE_THUNK("BridgeTraceThunk11", "g_trace_original_11", "11"));
asm(BRIDGE_TRACE_THUNK("BridgeTraceThunk12", "g_trace_original_12", "12"));
asm(BRIDGE_TRACE_THUNK("BridgeTraceThunk13", "g_trace_original_13", "13"));
asm(BRIDGE_TRACE_THUNK("BridgeTraceThunk14", "g_trace_original_14", "14"));
asm(BRIDGE_TRACE_THUNK("BridgeTraceThunk15", "g_trace_original_15", "15"));
asm(BRIDGE_TRACE_THUNK("BridgeTraceThunk16", "g_trace_original_16", "16"));
asm(BRIDGE_TRACE_THUNK("BridgeTraceThunk17", "g_trace_original_17", "17"));
asm(BRIDGE_TRACE_THUNK("BridgeTraceThunk18", "g_trace_original_18", "18"));
asm(BRIDGE_TRACE_THUNK("BridgeTraceThunk19", "g_trace_original_19", "19"));
asm(BRIDGE_TRACE_THUNK("BridgeTraceThunk20", "g_trace_original_20", "20"));
asm(BRIDGE_TRACE_THUNK("BridgeTraceThunk21", "g_trace_original_21", "21"));
asm(BRIDGE_TRACE_THUNK("BridgeTraceThunk22", "g_trace_original_22", "22"));
asm(BRIDGE_TRACE_THUNK("BridgeTraceThunk23", "g_trace_original_23", "23"));

namespace {

struct TraceSlot {
    size_t offset;
    const char* name;
    void** original;
    void* replacement;
    int reports;
};

TraceSlot g_trace_slots[] = {
    { 0x7a8, "CanResize", &g_trace_original_0, reinterpret_cast<void*>(&BridgeTraceThunk0) },
    { 0x7c0, "UpdateEffectEditTitle", &g_trace_original_1, reinterpret_cast<void*>(&BridgeTraceThunk1) },
    { 0x7c8, "GetEffectEditTitle", &g_trace_original_2, reinterpret_cast<void*>(&BridgeTraceThunk2) },
    { 0x7d8, "OnEditorIdle", &g_trace_original_3, reinterpret_cast<void*>(&BridgeTraceThunk3) },
    { 0x7e0, "EditorTop", &g_trace_original_4, reinterpret_cast<void*>(&BridgeTraceThunk4) },
    { 0x7e8, "EditorPreInitialize", &g_trace_original_5, reinterpret_cast<void*>(&BridgeTraceThunk5) },
    { 0x7f0, "EditorIdleRefresh", &g_trace_original_6, reinterpret_cast<void*>(&BridgeTraceThunk6) },
    { 0x7f8, "EditorMute", &g_trace_original_7, reinterpret_cast<void*>(&BridgeTraceThunk7) },
    { 0x830, "HideSubWindows", &g_trace_original_8, reinterpret_cast<void*>(&BridgeTraceThunk8) },
    { 0x858, "LockEditor", &g_trace_original_9, reinterpret_cast<void*>(&BridgeTraceThunk9) },
    { 0x8c8, "GetEffectRect", &g_trace_original_10, reinterpret_cast<void*>(&BridgeTraceThunk10) },
    { 0x8d0, "UpdateEffectRect", &g_trace_original_11, reinterpret_cast<void*>(&BridgeTraceThunk11) },
    { 0x8d8, "CheckEffectRect", &g_trace_original_12, reinterpret_cast<void*>(&BridgeTraceThunk12) },
    { 0x8e0, "UpdateDPI", &g_trace_original_13, reinterpret_cast<void*>(&BridgeTraceThunk13) },
    { 0x288, "Update", &g_trace_original_14, reinterpret_cast<void*>(&BridgeTraceThunk14) },
    { 0x838, "UpdateInspector", &g_trace_original_15, reinterpret_cast<void*>(&BridgeTraceThunk15) },
    { 0x460, "GenerateUserInterface", &g_trace_original_16, reinterpret_cast<void*>(&BridgeTraceThunk16) },
    { 0x470, "GetResourcePath", &g_trace_original_17, reinterpret_cast<void*>(&BridgeTraceThunk17) },
    { 0x468, "UpdateParameterList", &g_trace_original_18, reinterpret_cast<void*>(&BridgeTraceThunk18) },
    { 0x350, "GetParameterList", &g_trace_original_19, reinterpret_cast<void*>(&BridgeTraceThunk19) },
    { 0x358, "GetDisplayGroups", &g_trace_original_20, reinterpret_cast<void*>(&BridgeTraceThunk20) },
    { 0x2c8, "GetNumberOfParameters", &g_trace_original_21, reinterpret_cast<void*>(&BridgeTraceThunk21) },
    { 0x2d0, "GetParameterName", &g_trace_original_22, reinterpret_cast<void*>(&BridgeTraceThunk22) },
    { 0x2d8, "GetControlType", &g_trace_original_23, reinterpret_cast<void*>(&BridgeTraceThunk23) },
};

constexpr int kTraceCap = 6;

// The two slots the panel button really drives.
constexpr size_t kUpdateEffectEditTitleOffset = 0x7c0;
constexpr size_t kHideSubWindowsOffset = 0x830;

}  // namespace

// ---------------------------------------------------------------------------
// The name in the effect list.
//
// The effect rides on BMDStereoDelay, so Resolve labels it "Delay". Three calls can carry the
// label and they do not agree on a string type, so each replacement reads what the stock call
// returned and answers in the same encoding. wchar_t is four bytes here, so an ASCII wide string
// reads as c,0,0,0 and an ASCII narrow one as c,x - that is enough to tell them apart.
// ---------------------------------------------------------------------------

namespace {

char g_label_narrow[128] = {0};
wchar_t g_label_wide[128] = {0};

void PrepareLabels()
{
    if (g_label_narrow[0] != '\0') {
        return;
    }
    const char* const name = MenuNameFromPath(ConfiguredPluginPath());
    if (name == nullptr || name[0] == '\0') {
        return;
    }
    std::snprintf(g_label_narrow, sizeof(g_label_narrow), "%s", name);
    size_t index = 0;
    for (; index + 1 < sizeof(g_label_wide) / sizeof(g_label_wide[0]) && name[index] != '\0';
         ++index) {
        g_label_wide[index] = static_cast<wchar_t>(name[index]);
    }
    g_label_wide[index] = 0;
}

// Returns true when the bytes look like a wide string. Reads through SafeRead, so a bad pointer
// costs a false instead of a fault.
bool LooksWide(const void* text)
{
    unsigned char bytes[4] = {0, 0, 0, 0};
    if (text == nullptr || !SafeRead(text, bytes, sizeof(bytes))) {
        return false;
    }
    return bytes[0] != 0 && bytes[1] == 0 && bytes[2] == 0 && bytes[3] == 0;
}

void LogStockLabel(const char* call, const void* text)
{
    if (text == nullptr) {
        Log("label: %s returned nullptr", call);
    } else if (LooksWide(text)) {
        Log("label: %s returned a wide string at %p", call, text);
    } else {
        char narrow[64] = {0};
        SafeRead(text, narrow, sizeof(narrow) - 1);
        Log("label: %s returned a narrow string \"%s\"", call, narrow);
    }
}

int g_label_reports = 0;

}  // namespace

extern "C" {
BRIDGE_HIDDEN2 void* g_original_effect_name = nullptr;
BRIDGE_HIDDEN2 void* g_original_effect_name_thunk = nullptr;
BRIDGE_HIDDEN2 void* g_original_user_effect_name = nullptr;
}

extern "C" const void* BridgeEffectName(void* self)
{
    const void* stock = nullptr;
    if (g_original_effect_name != nullptr) {
        stock = reinterpret_cast<const void* (*)(void*)>(g_original_effect_name)(self);
    }
    PrepareLabels();
    if (++g_label_reports <= 6) {
        LogStockLabel("GetEffectName", stock);
    }
    if (g_label_narrow[0] == '\0') {
        return stock;
    }
    return LooksWide(stock) ? static_cast<const void*>(g_label_wide)
                            : static_cast<const void*>(g_label_narrow);
}

extern "C" const void* BridgeEffectNameThunk(void* self)
{
    const void* stock = nullptr;
    if (g_original_effect_name_thunk != nullptr) {
        stock = reinterpret_cast<const void* (*)(void*)>(g_original_effect_name_thunk)(self);
    }
    PrepareLabels();
    if (++g_label_reports <= 6) {
        LogStockLabel("AudioPlugin::GetEffectName", stock);
    }
    if (g_label_narrow[0] == '\0') {
        return stock;
    }
    return LooksWide(stock) ? static_cast<const void*>(g_label_wide)
                            : static_cast<const void*>(g_label_narrow);
}

// SetUserEffectName takes a wchar_t const*, so this one is wide by declaration. It answers nullptr
// until the user renames the effect, and nullptr is the case we want to fill.
extern "C" const wchar_t* BridgeUserEffectName(void* self)
{
    const wchar_t* stock = nullptr;
    if (g_original_user_effect_name != nullptr) {
        stock = reinterpret_cast<const wchar_t* (*)(void*)>(g_original_user_effect_name)(self);
    }
    PrepareLabels();
    if (++g_label_reports <= 6) {
        LogStockLabel("GetUserEffectName", stock);
    }
    if (stock != nullptr || g_label_wide[0] == 0) {
        return stock;
    }
    return g_label_wide;
}

extern "C" void BridgeReportSlot(unsigned int slot)
{
    if (slot >= sizeof(g_trace_slots) / sizeof(g_trace_slots[0])) {
        return;
    }
    TraceSlot& entry = g_trace_slots[slot];
    if (++entry.reports <= kTraceCap) {
        Log("trace: AudioPlugin::%s  (+0x%03zx)", entry.name, entry.offset);
    }

    // The panel button is a show/hide toggle, not a second InitializeEffectEdit. Resolve builds its
    // editor object once and from then on drives it with these two calls, so these are the ones the
    // hosted window has to follow. Measured on 2026-08-24: one press logs UpdateEffectEditTitle and
    // the OnEditorIdle heartbeat resumes; the next press logs HideSubWindows.
    if (entry.offset == kUpdateEffectEditTitleOffset) {
        BridgeEditorShow("UpdateEffectEditTitle");
    } else if (entry.offset == kHideSubWindowsOffset) {
        BridgeEditorHide("HideSubWindows");
    }
}

// Called from the X event pump the moment the window manager closes our window. Everything the
// panel button does next lands in the log.
extern "C" void BridgeArmEditorTrace()
{
    for (TraceSlot& entry : g_trace_slots) {
        entry.reports = 0;
    }
    Log("trace: armed - the next %d calls per editor slot are logged", kTraceCap);
}

// ---------------------------------------------------------------------------
// Our own vtable.
//
// Patching the stock BMDStereoDelay vtable makes every Delay in the project our effect: the hosted
// plugin runs on all of them, the panel button drives all of them, and a real Delay is no longer a
// real Delay. So the class vtable is left alone. Instead the whole vtable object is copied once,
// our overrides go into the copy, and each instance we create is pointed at it.
//
// The object holds five sub-vtables, each preceded by its offset-to-top and typeinfo words. An
// instance carries one vptr per sub-vtable, and each vptr points at the first *function* slot -
// sixteen bytes past the start of its group:
//
//     +0x010  primary (BMDAudioPluginImpl)      +0xa88  fourth
//     +0x4f8  bmd::PluginInstance               +0xaa0  fifth
//     +0x650  AudioPlugin  <- the one Resolve holds
//
// Where those vptrs sit inside the object is not documented, so the instance is scanned for the
// five stock values instead of trusting an offset. A pointer-sized field that equals one of them
// is a vptr; nothing else in the object can hold that address by accident.
// ---------------------------------------------------------------------------

namespace {

constexpr size_t kVtableBytes = 0xab8;
constexpr size_t kVptrGroups[] = {0x010, 0x4f8, 0x650, 0xa88, 0xaa0};
constexpr size_t kVptrGroupCount = sizeof(kVptrGroups) / sizeof(kVptrGroups[0]);

// How far to look for vptrs, in both directions.
//
// CreatePluginInstance hands back the bmd::PluginInstance subobject, not the start of the object,
// so the primary vptr is at a *lower* address. Scanning forward only finds four of the five groups
// and misses the one that carries the name and the control tree. A word only counts as a vptr when
// it equals one of the five stock group addresses exactly, so a wide scan cannot claim anything by
// accident, and SafeRead turns a wrong guess into a skipped word instead of a fault.
// Measured on 2026-08-24, and the reason the scan is this narrow:
//
//     instance-16   primary        (+0x010)
//     instance+0    PluginInstance (+0x4f8)   <- what CreatePluginInstance returns
//     instance+16   AudioPlugin    (+0x650)
//     instance+200  (+0xa88)
//     instance+216  (+0xaa0)
//
// Every group lives inside 232 bytes. A wide scan is not merely wasteful, it is unsafe: any other
// BMDStereoDelay on the heap holds these same five values, so a scan that runs past this object
// will claim a neighbour's vptrs and make an unrelated effect ours.
constexpr long kInstanceScanBack = -0x40;
constexpr long kInstanceScanForward = 0x140;

unsigned char* g_stock_vtable = nullptr;
unsigned char* g_our_vtable = nullptr;
int g_instances_claimed = 0;

// Write one slot in our own copy. No mprotect: this memory is ours and already writable.
bool PatchOurSlot(size_t offset, void** saved, void* replacement)
{
    if (g_our_vtable == nullptr || offset + sizeof(void*) > kVtableBytes) {
        return false;
    }
    auto* const slot = reinterpret_cast<void**>(g_our_vtable + offset);
    if (saved != nullptr) {
        *saved = *slot;
    }
    *slot = replacement;
    return true;
}

// The Delay control tree.
//
// GenerateUserInterface builds the knobs Resolve draws in the effect panel. Our plugin brings its
// own window, so the Delay's tree is 10 KB of controls for an effect that is not running. Not
// calling it leaves the panel empty.
//
// The disassembly shows `mov %rdi,%r13` at entry and no second argument, so `this` is in rdi and
// there is no sret. The return type is not visible, but a builder that returns a value is unlikely
// and this now only affects instances we claimed.
constexpr size_t kGenerateUserInterfaceOffset = 0x460;

// Where the effect name really comes from.
//
// SetUserEffectName runs and Resolve still shows "Delay", so the label is not the user name. The
// disassembly of BMDAudioPlugin<BMDStereoDelay>::GetEffectName says where it does come from:
//
//     mov  0x550(%rsi), %rax     # rsi is `this`; a pointer field in the object
//     mov  (%rax), %rsi          # the first word of that record is a char const*
//     call flx::CA2W(char const*)
//
// So the name is one indirection off the primary object. Rather than hook a call that returns a
// string by value, the record is cloned for our instance and the clone's name pointer is ours. The
// stock record is untouched, so every other Delay keeps its own name.
constexpr size_t kNameRecordOffset = 0x550;
constexpr size_t kNameRecordCloneBytes = 256;

void RenameInstance(void* primary_base)
{
    if (!EnabledByEnvironment("FXBRIDGE_RENAME", true)) {
        return;
    }
    PrepareLabels();
    if (primary_base == nullptr || g_label_narrow[0] == '\0') {
        return;
    }

    auto* const field = reinterpret_cast<unsigned char*>(primary_base) + kNameRecordOffset;
    void* record = nullptr;
    if (!SafeRead(field, &record, sizeof(record)) || record == nullptr) {
        Log("rename skipped: no name record at +0x%zx", kNameRecordOffset);
        return;
    }

    const char* stock_name = nullptr;
    if (SafeRead(record, &stock_name, sizeof(stock_name)) && stock_name != nullptr) {
        char readable[64] = {0};
        SafeRead(stock_name, readable, sizeof(readable) - 1);
        Log("rename: the stock record at %p names the effect \"%s\"", record, readable);
    }

    // Clone generously. Nothing is known about the rest of the record, so the copy keeps every
    // field the stock one had and only the name pointer changes.
    auto* const clone = static_cast<unsigned char*>(std::malloc(kNameRecordCloneBytes));
    if (clone == nullptr) {
        return;
    }
    std::memset(clone, 0, kNameRecordCloneBytes);
    if (!SafeRead(record, clone, kNameRecordCloneBytes)) {
        std::free(clone);
        Log("rename skipped: the name record is not %zu bytes readable", kNameRecordCloneBytes);
        return;
    }

    const char* const our_name = g_label_narrow;
    std::memcpy(clone, &our_name, sizeof(our_name));
    *reinterpret_cast<void**>(field) = clone;
    Log("rename: instance now names itself \"%s\"", g_label_narrow);
}

// The AudioPlugin group, and the slot that names the effect.
constexpr size_t kAudioPluginGroup = 0x650;
constexpr size_t kSetUserEffectNameOffset = 0x6b8;

// Give the effect our name.
//
// The label is not hooked. GetEffectName returns a string by value, and faking an sret means
// building whatever string type it returns. SetUserEffectName is the other side of the same pair
// and its declaration already names the type: wchar_t const*. So the name is set, not faked.
void NameInstance(void* audio_plugin_subobject)
{
    PrepareLabels();
    if (audio_plugin_subobject == nullptr || g_label_wide[0] == 0) {
        return;
    }

    void** vtable = nullptr;
    if (!SafeRead(audio_plugin_subobject, &vtable, sizeof(vtable)) || vtable == nullptr) {
        return;
    }

    const size_t slot = (kSetUserEffectNameOffset - kAudioPluginGroup) / sizeof(void*);
    using Setter = void (*)(void*, const wchar_t*);
    reinterpret_cast<Setter>(vtable[slot])(audio_plugin_subobject, g_label_wide);
    Log("instance named \"%ls\" through SetUserEffectName", g_label_wide);
}

// Point one instance at our vtable. Returns how many vptr fields were rewritten.
int ClaimInstance(void* instance)
{
    if (instance == nullptr || g_our_vtable == nullptr || g_stock_vtable == nullptr) {
        return 0;
    }

    auto* const words = reinterpret_cast<unsigned char*>(instance);
    void* audio_plugin_subobject = nullptr;
    void* primary_base = nullptr;
    int claimed = 0;
    for (long offset = kInstanceScanBack; offset <= kInstanceScanForward;
         offset += static_cast<long>(sizeof(void*))) {
        unsigned long value = 0;
        if (!SafeRead(words + offset, &value, sizeof(value))) {
            continue;  // a hole in the scan is not the end of the object
        }
        for (size_t group = 0; group < kVptrGroupCount; ++group) {
            if (value != reinterpret_cast<unsigned long>(g_stock_vtable + kVptrGroups[group])) {
                continue;
            }
            auto* const field = reinterpret_cast<void**>(words + offset);
            *field = g_our_vtable + kVptrGroups[group];
            Log("  vptr group +0x%03zx found at instance%+ld", kVptrGroups[group], offset);
            if (kVptrGroups[group] == 0x010) {
                primary_base = field;
            }
            if (kVptrGroups[group] == kAudioPluginGroup) {
                // A vptr field is the first word of its subobject, so this address is the
                // AudioPlugin `this` that Resolve itself passes around.
                audio_plugin_subobject = field;
            }
            ++claimed;
            break;
        }
    }

    // This effect's own plugin. Nothing is shared with any other effect.
    ClaimedEffect* const entry = AppendEffect(instance, audio_plugin_subobject);
    if (entry != nullptr) {
        const char* const path = ConfiguredPluginPath();
        entry->plugin = CreateHostedPlugin(FormatFromPath(path), path, 48000.0, kMaxFrames);
        if (entry->plugin != nullptr) {
            entry->dry.assign(static_cast<size_t>(kMaxChannels) * kMaxFrames, 0.0f);
            g_focused_effect.store(entry);
            Log("effect: hosting \"%s\" for this instance", entry->plugin->Name());
        } else {
            Log("effect: no plugin for this instance - audio passes through");
        }
    } else {
        Log("effect: more than %zu effects claimed, this one gets no plugin", kMaxClaimedEffects);
    }

    NameInstance(audio_plugin_subobject);
    RenameInstance(primary_base);

    // With no control tree there is no panel, and Resolve never calls InitializeEffectEdit - the
    // editor path is gated on the panel existing. Verified on 2026-08-24: with the panel suppressed
    // the log carries no InitializeEffectEdit line at all. So the window is opened here instead.
    if (EnabledByEnvironment("FXBRIDGE_EMPTY_PANEL")) {
        BridgeEditorShow("the effect was added");
    }

    return claimed;
}

}  // namespace

// The resource tree lives at this+0x360, and the stock builder writes the panel extents on the
// object itself. Read out of BMDStereoDelay::GenerateUserInterface:
//
//     lea  0x360(%r13), %r14     # r13 is `this` - the PluginUIResourceTree
//     call SimpleUiGenerator::GetDefaultBackgroundColor()
//     mov  %eax, 0x398(%r13)     # the background colour
//     call SimpleUiGenerator::DefaultWidth()   -> movss %xmm0, 0x3a8(%r13)
//     call SimpleUiGenerator::DefaultHeight()  -> movss %xmm0, 0x3ac(%r13)
//
// and the controls are added through these, all exported and all named:
//
//     BMDAudioPluginImpl::BindParameterByName(char const*)  -> a PluginUIBinding*
//     bmd::PluginUIResourceTree::AddKnob(binding, ..., label, ..., min, max, ...)
//     bmd::PluginUIResourceTree::AddText / AddFrame / AddMeter / AddComboBoxToToolbar
//
// So replacing the panel is a matter of building this tree ourselves. What is not yet known is the
// tree's own layout, which is why this step lets the stock builder run and then reads the result.
constexpr size_t kResourceTreeOffset = 0x360;
constexpr size_t kPanelWidthOffset = 0x3a8;
constexpr size_t kPanelHeightOffset = 0x3ac;

// The tree's item list. Dumped from a freshly built Delay panel on 2026-08-24:
//
//     tree+0x00  0x7ff76f0b4aa8   a vptr - the tree is a real object
//     tree+0x10  0x7ff72b6baf00   begin
//     tree+0x18  0x7ff72b6bafc8   end            (end - begin = 0xc8 = 25 items)
//     tree+0x20  0x7ff72b6bb000   capacity end
//
// Three ascending pointers with end below the capacity end is a std::vector, and 25 items matches
// the Delay's knobs, text and frames.
constexpr size_t kTreeItemsBegin = 0x10;
constexpr size_t kTreeItemsEnd = 0x18;

extern "C" void BridgeGenerateUserInterface(void* self)
{
    (void)self;
    // Reached only while the switch is on: the slot is not patched otherwise.
    static bool reported = false;

    // Let the stock builder run, then empty the tree.
    //
    // Skipping the builder was the first attempt and it crashed: Resolve builds its editor from
    // this tree, and a tree that was never built is all zeros, so the first virtual call on it
    // jumps into nothing. Running the builder and then truncating its item list leaves a tree that
    // is fully constructed and simply has no controls in it - which is what an empty panel should
    // have been from the start.
    //
    // The items are leaked. They belong to the tree's own allocator and 25 small objects once per
    // effect is not worth the risk of freeing something we do not own.
    using Builder = void (*)(void*);
    void* const stock =
        g_stock_vtable != nullptr
            ? *reinterpret_cast<void**>(g_stock_vtable + kGenerateUserInterfaceOffset)
            : nullptr;
    if (stock != nullptr) {
        reinterpret_cast<Builder>(stock)(self);
    }

    auto* const tree = reinterpret_cast<unsigned char*>(self) + kResourceTreeOffset;
    void* begin = nullptr;
    void* end = nullptr;
    if (SafeRead(tree + kTreeItemsBegin, &begin, sizeof(begin)) &&
        SafeRead(tree + kTreeItemsEnd, &end, sizeof(end)) && begin != nullptr && end >= begin) {
        const long items = (reinterpret_cast<const char*>(end) -
                            reinterpret_cast<const char*>(begin)) / 8;
        *reinterpret_cast<void**>(tree + kTreeItemsEnd) = begin;
        if (!reported) {
            Log("panel: the stock tree had %ld items, now none", items);
        }
    } else if (!reported) {
        Log("panel: the tree does not look like a vector - left as the stock built it");
    }

    if (EnabledByEnvironment("FXBRIDGE_STUDY_PANEL")) {
        if (!reported) {
            reported = true;
            auto* const bytes = reinterpret_cast<const unsigned char*>(self);
            float width = 0.0f;
            float height = 0.0f;
            std::memcpy(&width, bytes + kPanelWidthOffset, sizeof(width));
            std::memcpy(&height, bytes + kPanelHeightOffset, sizeof(height));
            Log("panel: the stock tree is built - %.0f x %.0f", static_cast<double>(width),
                static_cast<double>(height));
            const unsigned char* const tree = bytes + kResourceTreeOffset;
            char line[200];
            for (int row = 0; row < 4; ++row) {
                int written = std::snprintf(line, sizeof(line), "panel: tree+0x%02x ", row * 16);
                for (int column = 0; column < 16 && written < static_cast<int>(sizeof(line)) - 4;
                     ++column) {
                    written += std::snprintf(line + written, sizeof(line) - written, "%02x ",
                                             tree[row * 16 + column]);
                }
                Log("%s", line);
            }
        }
    }

    reported = true;
}

// Redirect the stock GenerateUserInterface slot, and put it back. Only the create call for our own
// effect runs with this on.
extern "C" void BridgeSuppressStockUserInterface(bool on)
{
    static void* saved = nullptr;
    if (g_stock_vtable == nullptr) {
        return;
    }
    if (on) {
        if (saved != nullptr) {
            return;  // already on; never save our own function over the stock one
        }
        PatchVtableSlot(g_stock_vtable, kGenerateUserInterfaceOffset, &saved,
                        reinterpret_cast<void*>(&BridgeGenerateUserInterface));
        return;
    }
    if (saved == nullptr) {
        return;
    }
    PatchVtableSlot(g_stock_vtable, kGenerateUserInterfaceOffset, nullptr, saved);
    saved = nullptr;
}

// Called from the create trampoline's exit, for our instances only.
extern "C" void BridgeClaimInstance(void* instance)
{
    const int claimed = ClaimInstance(instance);
    ++g_instances_claimed;
    Log("instance %d at %p: %d vptr%s rewritten to our vtable%s",
        g_instances_claimed, instance, claimed, claimed == 1 ? "" : "s",
        claimed == 0 ? "  <- the effect will do nothing" : "");
}

namespace {

void PatchDelayClassVtable()
{
    if (g_stock_handle == nullptr) {
        return;
    }

    auto* const vtable = static_cast<unsigned char*>(dlsym(g_stock_handle, kDelayVtableSymbol));
    if (vtable == nullptr) {
        Log("audio hook skipped: %s not found", kDelayVtableSymbol);
        return;
    }

    g_stock_vtable = vtable;
    g_our_vtable = static_cast<unsigned char*>(std::aligned_alloc(16, kVtableBytes));
    if (g_our_vtable == nullptr) {
        Log("vtable copy failed: no memory - the stock vtable is left untouched");
        return;
    }
    std::memcpy(g_our_vtable, vtable, kVtableBytes);
    Log("vtable copied: stock %p, ours %p, %zu bytes", vtable, g_our_vtable, kVtableBytes);


    // The primary Process slot is OFF by default.
    //
    // Resolve reaches Process through the AudioPlugin thunk at +0x690, and that path has been
    // stable all evening. The primary slot at +0x4d0 only became reachable once the primary vptr
    // was claimed, and the first run that claimed it died inside BridgeThunkProcessPrimary - the
    // core dump named the frame, with libc's signal trampoline directly above it. Until that is
    // understood, the slot Resolve actually uses is the only one hooked.
    const bool primary =
        EnabledByEnvironment("FXBRIDGE_PRIMARY_PROCESS")
            ? PatchOurSlot(kProcessPrimaryOffset, &g_original_process_primary,
                           reinterpret_cast<void*>(&BridgeThunkProcessPrimary))
            : false;
    const bool secondary = PatchOurSlot(kProcessThunkOffset,
                                           &g_original_process_thunk,
                                           reinterpret_cast<void*>(&BridgeThunkProcessSecondary));

    const bool has_editor = PatchOurSlot(kHasEditorOffset, &g_original_has_editor,
                                           reinterpret_cast<void*>(&BridgeHasEditor));
    PatchOurSlot(kGetEffectEditOffset, &g_original_get_effect_edit,
                    reinterpret_cast<void*>(&BridgeGetEffectEdit));
    PatchOurSlot(kInitializeEffectEditOffset, &g_original_initialize_editor,
                    reinterpret_cast<void*>(&BridgeInitializeEffectEdit));
    PatchOurSlot(kCloseEffectEditOffset, &g_original_close_editor,
                    reinterpret_cast<void*>(&BridgeCloseEffectEdit));
    const bool has_editor_thunk = PatchOurSlot(kHasEditorThunkOffset,
                                                 &g_original_has_editor_thunk,
                                                 reinterpret_cast<void*>(&BridgeHasEditorThunk));
    PatchOurSlot(kGetEffectEditThunkOffset, &g_original_get_effect_edit_thunk,
                    reinterpret_cast<void*>(&BridgeGetEffectEditThunk));
    PatchOurSlot(kInitializeEffectEditThunkOffset, &g_original_initialize_editor_thunk,
                    reinterpret_cast<void*>(&BridgeInitializeEffectEditThunk));
    PatchOurSlot(kCloseEffectEditThunkOffset, &g_original_close_editor_thunk,
                    reinterpret_cast<void*>(&BridgeCloseEffectEditThunk));

    for (TraceSlot& entry : g_trace_slots) {
        PatchOurSlot(entry.offset, entry.original, entry.replacement);
    }
    // The name hooks are OUT. GetEffectName returns a string by value, so the ABI puts the hidden
    // return buffer in rdi and `this` in rsi. Calling it as `const void* (*)(void*)` hands the
    // object pointer over as a write buffer - the probe answered stack addresses like
    // 0x7ffc433d9560, which is what gave it away. Do not reinstate these without reading the
    // disassembly for an sret first.

    // After the trace loop, so the no-op wins the slot rather than the tracer.
    if (EnabledByEnvironment("FXBRIDGE_EMPTY_PANEL")) {
        PatchOurSlot(kGenerateUserInterfaceOffset, nullptr,
                     reinterpret_cast<void*>(&BridgeGenerateUserInterface));
        Log("empty panel is ON - GenerateUserInterface is a no-op in our vtable");
    }

    Log("editor trace installed on %zu slots",
        sizeof(g_trace_slots) / sizeof(g_trace_slots[0]));

    Log("editor hooks installed - primary %s, AudioPlugin base %s",
        has_editor ? "patched" : "FAILED", has_editor_thunk ? "patched" : "FAILED");

    const bool knob = PatchOurSlot(kSetParameterValueOffset,
                                      &g_original_set_parameter,
                                      reinterpret_cast<void*>(&BridgeThunkSetParameter));
    Log("knob hook on BMDStereoDelay::SetParameterValue - %s (%p)",
        knob ? "patched" : "FAILED", g_original_set_parameter);

    Log("audio hook on BMDStereoDelay::Process - primary %s (%p), thunk %s (%p)",
        primary ? "patched" : "off", g_original_process_primary,
        secondary ? "patched" : "FAILED", g_original_process_thunk);
}

}  // namespace

extern "C" void* GetBMDPluginInterface()
{
    static void* const interface_ptr = [] () -> void* {
        void* const stock = LoadStockInterface();
        if (stock == nullptr) {
            return nullptr;
        }

        void** const stock_vtable = *reinterpret_cast<void***>(stock);
        const int version = reinterpret_cast<GetVersionFn>(stock_vtable[kGetPluginInterfaceVersion])(stock);
        Log("stock interface at %p, version %d", stock, version);
        if (version != kExpectedInterfaceVersion) {
            Log("unexpected interface version, forwarding untouched");
            return stock;
        }

        g_vtable[0] = stock_vtable[-2];  // offset-to-top
        g_vtable[1] = stock_vtable[-1];  // typeinfo
        for (int slot = 0; slot < kSlotCount; ++slot) {
            g_vtable[2 + slot] = stock_vtable[slot];
        }

        g_original_query_plugin_list = stock_vtable[kQueryPluginList];
        g_original_create_plugin_instance = stock_vtable[kCreatePluginInstance];
        g_vtable[2 + kQueryPluginList] = reinterpret_cast<void*>(&BridgeThunkQueryPluginList);
        g_vtable[2 + kCreatePluginInstance] = reinterpret_cast<void*>(&BridgeThunkCreatePluginInstance);

        if (!MakeWritable(stock)) {
            Log("cannot install our vtable, forwarding untouched");
            return stock;
        }
        *reinterpret_cast<void***>(stock) = &g_vtable[2];
        g_interface = stock;
        PatchDelayClassVtable();
        LoadConfiguredPlugin();

        Log("our vtable installed, version reads %d",
            reinterpret_cast<GetVersionFn>(g_vtable[2 + kGetPluginInterfaceVersion])(stock));
        return stock;
    }();

    return interface_ptr;
}
