#include "carla_host.h"

#include <dlfcn.h>

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include "CarlaNative.h"

// Defined in proxy.cpp. Editor visibility has one owner for every host.
extern "C" void BridgeEditorWasClosedByUser();
extern "C" void BridgeEditorReassert();
extern "C" void BridgeArmEditorTrace();

namespace {

void (*g_logger)(const char*) = nullptr;

void Log(const char* format, ...) __attribute__((format(printf, 1, 2)));

void Log(const char* format, ...)
{
    char line[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (g_logger != nullptr) {
        g_logger(line);
    }
}

// Carla is opened, not linked.
const char kCarlaLibrary[] = "/usr/lib/carla/libcarla_native-plugin.so";
const char kCarlaResourceDir[] = "/usr/share/carla/resources";

using GetDescriptor = const NativePluginDescriptor* (*)(void);

void* g_carla_library = nullptr;
const NativePluginDescriptor* g_descriptor = nullptr;
NativePluginHandle g_plugin = nullptr;

double g_sample_rate = 48000.0;
uint32_t g_max_frames = 8192;
uint32_t g_channel_count = 2;
bool g_ready = false;
bool g_ui_open = false;

std::thread g_idle_thread;
std::atomic<bool> g_idle_running{false};

// Carla writes into a buffer it must not also read from, so the input is copied aside first.
std::vector<float>& InputScratch()
{
    static std::vector<float> scratch;
    return scratch;
}

NativeTimeInfo g_time_info;

// ---------------------------------------------------------------------------
// What Carla asks of its host. Every one of these must answer; a null field is a crash waiting for
// the first plugin that uses it.
// ---------------------------------------------------------------------------

uint32_t HostGetBufferSize(NativeHostHandle) { return g_max_frames; }
double HostGetSampleRate(NativeHostHandle) { return g_sample_rate; }
bool HostIsOffline(NativeHostHandle) { return false; }

const NativeTimeInfo* HostGetTimeInfo(NativeHostHandle)
{
    return &g_time_info;
}

bool HostWriteMidiEvent(NativeHostHandle, const NativeMidiEvent*) { return false; }

void HostUiParameterChanged(NativeHostHandle, uint32_t, float) {}
void HostUiMidiProgramChanged(NativeHostHandle, uint8_t, uint32_t, uint32_t) {}
void HostUiCustomDataChanged(NativeHostHandle, const char*, const char*) {}

void HostUiClosed(NativeHostHandle)
{
    g_ui_open = false;
    Log("carla: the window was closed");
    BridgeEditorWasClosedByUser();
    BridgeArmEditorTrace();
}

const char* HostUiOpenFile(NativeHostHandle, bool, const char*, const char*) { return nullptr; }
const char* HostUiSaveFile(NativeHostHandle, bool, const char*, const char*) { return nullptr; }

intptr_t HostDispatcher(NativeHostHandle, NativeHostDispatcherOpcode opcode, int32_t, intptr_t,
                        void* ptr, float)
{
    switch (opcode) {
        case NATIVE_HOST_OPCODE_GET_FILE_PATH:
            // Carla asks where its own resources live.
            if (ptr != nullptr && std::strcmp(static_cast<const char*>(ptr), "carla") == 0) {
                return reinterpret_cast<intptr_t>(kCarlaResourceDir);
            }
            return 0;
        case NATIVE_HOST_OPCODE_UI_UNAVAILABLE:
            g_ui_open = false;
            Log("carla: the window is unavailable");
            return 0;
        default:
            return 0;
    }
}

// Carla's window needs regular idle calls to stay alive and to pump its own events.
void IdleLoop()
{
    while (g_idle_running.load()) {
        if (g_ui_open && g_descriptor != nullptr && g_descriptor->ui_idle != nullptr &&
            g_plugin != nullptr) {
            g_descriptor->ui_idle(g_plugin);
        }
        BridgeEditorReassert();
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
}

}  // namespace

bool CarlaHostLoad(double sample_rate, uint32_t max_frames)
{
    if (g_ready) {
        return true;
    }

    g_sample_rate = sample_rate;
    g_max_frames = max_frames;
    std::memset(&g_time_info, 0, sizeof(g_time_info));

    g_carla_library = dlopen(kCarlaLibrary, RTLD_NOW | RTLD_LOCAL);
    if (g_carla_library == nullptr) {
        Log("carla: dlopen(%s) failed: %s", kCarlaLibrary, dlerror());
        return false;
    }

    // The rack is a serial chain; the patchbay is arbitrary routing. Both are the same API.
    const char* const wanted = std::getenv("FXBRIDGE_CARLA_MODE");
    const bool patchbay = wanted != nullptr && std::strcmp(wanted, "patchbay") == 0;
    const char* const symbol =
        patchbay ? "carla_get_native_patchbay_plugin" : "carla_get_native_rack_plugin";

    auto* const get_descriptor = reinterpret_cast<GetDescriptor>(dlsym(g_carla_library, symbol));
    if (get_descriptor == nullptr) {
        Log("carla: %s is missing from %s", symbol, kCarlaLibrary);
        return false;
    }

    g_descriptor = get_descriptor();
    if (g_descriptor == nullptr || g_descriptor->instantiate == nullptr) {
        Log("carla: %s returned no descriptor", symbol);
        return false;
    }

    static NativeHostDescriptor host;
    std::memset(&host, 0, sizeof(host));
    host.handle = nullptr;
    host.resourceDir = kCarlaResourceDir;
    host.uiName = "Jay's Desk";
    host.uiParentId = 0;
    host.get_buffer_size = HostGetBufferSize;
    host.get_sample_rate = HostGetSampleRate;
    host.is_offline = HostIsOffline;
    host.get_time_info = HostGetTimeInfo;
    host.write_midi_event = HostWriteMidiEvent;
    host.ui_parameter_changed = HostUiParameterChanged;
    host.ui_midi_program_changed = HostUiMidiProgramChanged;
    host.ui_custom_data_changed = HostUiCustomDataChanged;
    host.ui_closed = HostUiClosed;
    host.ui_open_file = HostUiOpenFile;
    host.ui_save_file = HostUiSaveFile;
    host.dispatcher = HostDispatcher;

    g_plugin = g_descriptor->instantiate(&host);
    if (g_plugin == nullptr) {
        Log("carla: instantiate failed for %s", symbol);
        return false;
    }

    g_channel_count = g_descriptor->audioIns > 0 ? g_descriptor->audioIns : 2;
    if (g_channel_count > 2) {
        g_channel_count = 2;  // Resolve hands this effect a stereo pair
    }

    if (g_descriptor->activate != nullptr) {
        g_descriptor->activate(g_plugin);
    }

    InputScratch().assign(static_cast<size_t>(g_channel_count) * g_max_frames, 0.0f);

    if (!g_idle_running.exchange(true)) {
        g_idle_thread = std::thread(IdleLoop);
    }

    g_ready = true;
    Log("carla: loaded \"%s\" (%s), %u in, %u out, %.0f Hz",
        g_descriptor->name != nullptr ? g_descriptor->name : "Carla",
        patchbay ? "patchbay" : "rack",
        g_descriptor->audioIns, g_descriptor->audioOuts, sample_rate);
    return true;
}

bool CarlaHostProcess(float** buffers, uint32_t channel_count, uint32_t frames)
{
    if (!g_ready || g_descriptor == nullptr || g_descriptor->process == nullptr ||
        buffers == nullptr || frames == 0 || frames > g_max_frames) {
        return false;
    }

    const uint32_t channels = channel_count < g_channel_count ? channel_count : g_channel_count;
    std::vector<float>& scratch = InputScratch();
    if (scratch.size() < static_cast<size_t>(channels) * g_max_frames) {
        return false;
    }

    float* input[2] = {nullptr, nullptr};
    for (uint32_t channel = 0; channel < channels && channel < 2; ++channel) {
        float* const slot = &scratch[static_cast<size_t>(channel) * g_max_frames];
        std::memcpy(slot, buffers[channel], frames * sizeof(float));
        input[channel] = slot;
    }

    g_descriptor->process(g_plugin, input, buffers, frames, nullptr, 0);
    return true;
}

uint32_t CarlaHostChannelCount() { return g_ready ? g_channel_count : 0; }

const char* CarlaHostName()
{
    if (g_descriptor != nullptr && g_descriptor->name != nullptr) {
        return g_descriptor->name;
    }
    return "Carla";
}

bool CarlaHostOpenEditor()
{
    if (!g_ready || g_descriptor == nullptr || g_descriptor->ui_show == nullptr) {
        return false;
    }
    g_descriptor->ui_show(g_plugin, true);
    g_ui_open = true;
    Log("carla: window open");
    return true;
}

void CarlaHostCloseEditor()
{
    if (!g_ready || g_descriptor == nullptr || g_descriptor->ui_show == nullptr) {
        return;
    }
    g_descriptor->ui_show(g_plugin, false);
    g_ui_open = false;
    Log("carla: window hidden");
}

void CarlaHostSetLogger(void (*logger)(const char*)) { g_logger = logger; }
