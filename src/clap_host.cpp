// A minimal CLAP host.
//
// CLAP is a plain C ABI in a single set of MIT headers, which is why it is the format to host here:
// no SDK, no COM, no C++ ABI to match. Every plugin on this machine ships a .clap alongside its
// .vst3, so this one host covers the whole collection.
//
// Threading follows the CLAP contract. Loading, init and activate run on the main thread, from the
// bridge's library initialisation. Only process() is called from Resolve's audio thread.
//
// Resolve hands us one buffer that is both input and output, so the input is copied into scratch
// storage first and the plugin writes its result straight into Resolve's buffer.

#include "clap_host.h"

#include <clap/clap.h>
#include <dlfcn.h>
#include <X11/Xlib.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <thread>
#include <chrono>
#include <vector>

namespace {

void (*g_logger)(const char*) = nullptr;

void Log(const char* format, ...) __attribute__((format(printf, 1, 2)));

void Log(const char* format, ...)
{
    if (g_logger == nullptr) {
        return;
    }
    char line[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    g_logger(line);
}

const clap_plugin_entry_t* g_entry = nullptr;
const clap_plugin_t* g_plugin = nullptr;
uint32_t g_channel_count = 0;
uint32_t g_max_frames = 0;
bool g_ready = false;
char g_name[128] = {0};

std::vector<float> g_scratch;              // interleaved by channel: [channel][max_frames]
std::vector<float*> g_scratch_pointers;

// --- the host we present to the plugin ---------------------------------------------------------

// --- host extensions the plugin's GUI needs ----------------------------------------------------
//
// is_api_supported() said yes and create() refused, which means the plugin was asking what we
// provide, not what it can do. A DPF-style editor needs a timer to run its own idle loop, the GUI
// callbacks for resize and close, and a thread check it can trust.

std::thread g_timer_thread;
std::atomic<bool> g_timer_running{false};
std::atomic<clap_id> g_timer_id{0};
std::atomic<uint32_t> g_timer_period{16};
std::thread::id g_main_thread_id;

void TimerLoop()
{
    while (g_timer_running.load()) {
        const uint32_t period = g_timer_period.load();
        std::this_thread::sleep_for(std::chrono::milliseconds(period ? period : 16));
        if (!g_timer_running.load() || g_plugin == nullptr) {
            continue;
        }
        const auto* const timer = static_cast<const clap_plugin_timer_support_t*>(
            g_plugin->get_extension(g_plugin, CLAP_EXT_TIMER_SUPPORT));
        if (timer != nullptr) {
            timer->on_timer(g_plugin, g_timer_id.load());
        }
    }
}

bool HostRegisterTimer(const clap_host_t*, uint32_t period_ms, clap_id* out_id)
{
    if (out_id == nullptr) {
        return false;
    }
    *out_id = 1;
    g_timer_id.store(1);
    g_timer_period.store(period_ms);
    if (!g_timer_running.exchange(true)) {
        g_timer_thread = std::thread(TimerLoop);
    }
    return true;
}

bool HostUnregisterTimer(const clap_host_t*, clap_id)
{
    if (g_timer_running.exchange(false) && g_timer_thread.joinable()) {
        g_timer_thread.join();
    }
    return true;
}

const clap_host_timer_support_t g_host_timer = {HostRegisterTimer, HostUnregisterTimer};

void HostResizeHintsChanged(const clap_host_t*) {}
bool HostRequestResize(const clap_host_t*, uint32_t, uint32_t) { return true; }
bool HostRequestShow(const clap_host_t*) { return true; }
bool HostRequestHide(const clap_host_t*) { return true; }
void HostGuiClosed(const clap_host_t*, bool) {}

const clap_host_gui_t g_host_gui = {HostResizeHintsChanged, HostRequestResize, HostRequestShow,
                                    HostRequestHide, HostGuiClosed};

// The audio thread is Resolve's; every other call we make comes from the thread that loaded us.
bool HostIsMainThread(const clap_host_t*)
{
    return std::this_thread::get_id() == g_main_thread_id;
}

bool HostIsAudioThread(const clap_host_t* host)
{
    return !HostIsMainThread(host);
}

const clap_host_thread_check_t g_host_thread_check = {HostIsMainThread, HostIsAudioThread};

const void* HostGetExtension(const clap_host_t*, const char* id)
{
    if (id == nullptr) {
        return nullptr;
    }
    if (std::strcmp(id, CLAP_EXT_TIMER_SUPPORT) == 0) {
        return &g_host_timer;
    }
    if (std::strcmp(id, CLAP_EXT_GUI) == 0) {
        return &g_host_gui;
    }
    if (std::strcmp(id, CLAP_EXT_THREAD_CHECK) == 0) {
        return &g_host_thread_check;
    }
    return nullptr;
}
void HostRequestRestart(const clap_host_t*) {}
void HostRequestProcess(const clap_host_t*) {}
void HostRequestCallback(const clap_host_t*) {}

const clap_host_t g_host = {
    CLAP_VERSION_INIT,
    nullptr,                 // host_data
    "Jay's Desk FX bridge",
    "Jay's Desk",
    "https://jaysdesk.com",
    "0.1.0",
    HostGetExtension,
    HostRequestRestart,
    HostRequestProcess,
    HostRequestCallback,
};

// --- empty event queues; this host sends no parameter changes yet -------------------------------

// --- parameters -------------------------------------------------------------------------------
//
// A knob turn arrives on Resolve's UI thread; CLAP requires the change to reach the plugin as an
// event on the audio thread. So a turn only marks a slot dirty, and process() drains the dirty
// slots into a real clap_event_param_value. No locks, and nothing allocates in the callback.

constexpr uint32_t kMaxParameters = 128;

struct PendingParameter {
    std::atomic<bool> dirty{false};
    std::atomic<double> value{0.0};
    clap_id id{0};
    double minimum{0.0};
    double maximum{1.0};
};

PendingParameter g_parameters[kMaxParameters];
uint32_t g_parameter_count = 0;

// Filled by process(), read by process() only.
clap_event_param_value_t g_event_storage[kMaxParameters];
const clap_event_header_t* g_event_pointers[kMaxParameters];
uint32_t g_event_count = 0;

uint32_t InputEventsSize(const clap_input_events_t*) { return g_event_count; }

const clap_event_header_t* InputEventsGet(const clap_input_events_t*, uint32_t index)
{
    return index < g_event_count ? g_event_pointers[index] : nullptr;
}

bool OutputEventsPush(const clap_output_events_t*, const clap_event_header_t*) { return true; }

const clap_input_events_t g_input_events = {nullptr, InputEventsSize, InputEventsGet};
const clap_output_events_t g_output_events = {nullptr, OutputEventsPush};

const clap_plugin_params_t* ParamsExtension()
{
    if (g_plugin == nullptr) {
        return nullptr;
    }
    return static_cast<const clap_plugin_params_t*>(
        g_plugin->get_extension(g_plugin, CLAP_EXT_PARAMS));
}

// Turn every dirty slot into an event for this block.
void CollectParameterEvents()
{
    g_event_count = 0;
    for (uint32_t i = 0; i < g_parameter_count && g_event_count < kMaxParameters; ++i) {
        if (!g_parameters[i].dirty.exchange(false)) {
            continue;
        }
        clap_event_param_value_t& event = g_event_storage[g_event_count];
        event = {};
        event.header.size = sizeof(event);
        event.header.time = 0;
        event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        event.header.type = CLAP_EVENT_PARAM_VALUE;
        event.header.flags = 0;
        event.param_id = g_parameters[i].id;
        event.cookie = nullptr;
        event.note_id = -1;
        event.port_index = -1;
        event.channel = -1;
        event.key = -1;
        event.value = g_parameters[i].value.load();
        g_event_pointers[g_event_count] = &event.header;
        ++g_event_count;
    }
}

// Ask the plugin how many channels its first input port carries.
uint32_t ReadPortChannelCount(const clap_plugin_t* plugin)
{
    const auto* ports = static_cast<const clap_plugin_audio_ports_t*>(
        plugin->get_extension(plugin, CLAP_EXT_AUDIO_PORTS));
    if (ports == nullptr || ports->count(plugin, true) == 0) {
        return 0;
    }
    clap_audio_port_info_t info{};
    if (!ports->get(plugin, 0, true, &info)) {
        return 0;
    }
    return info.channel_count;
}

}  // namespace

// Defined in proxy.cpp. Arming the trace here is the whole point: everything Resolve does after the
// window manager takes our window away lands in the log.
extern "C" void BridgeArmEditorTrace();

namespace {

Display* g_display = nullptr;
Window g_window = 0;
Atom g_delete_window = 0;
bool g_gui_created = false;
std::thread g_x_thread;
std::atomic<bool> g_x_running{false};

// The window manager's close button must not destroy the window: the plugin's editor lives inside
// it. Catch WM_DELETE_WINDOW and unmap instead, so reopening from Resolve is just a remap.
void XEventLoop()
{
    while (g_x_running.load()) {
        while (g_display != nullptr && XPending(g_display) > 0) {
            XEvent event;
            XNextEvent(g_display, &event);
            if (event.type == ClientMessage &&
                static_cast<Atom>(event.xclient.data.l[0]) == g_delete_window) {
                XUnmapWindow(g_display, g_window);
                XFlush(g_display);
                Log("clap: the window manager closed the editor window");
                BridgeArmEditorTrace();
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
}

// DPF-based plugins answer is_api_supported(X11, floating) with true and then crash in create(),
// because only the embedded path is really implemented. So we give the plugin a real parent window
// and never ask for a floating one.
bool OpenParentWindow(uint32_t width, uint32_t height, const char* title)
{
    if (g_display != nullptr) {
        return true;
    }
    XInitThreads();  // the editor is used from Resolve's thread and pumped from ours
    g_display = XOpenDisplay(nullptr);
    if (g_display == nullptr) {
        Log("clap: XOpenDisplay failed, no editor");
        return false;
    }

    const int screen = DefaultScreen(g_display);
    g_window = XCreateSimpleWindow(g_display, RootWindow(g_display, screen), 0, 0,
                                   width ? width : 800, height ? height : 600, 0,
                                   BlackPixel(g_display, screen), BlackPixel(g_display, screen));
    if (g_window == 0) {
        Log("clap: XCreateSimpleWindow failed");
        return false;
    }
    XStoreName(g_display, g_window, title);
    XSelectInput(g_display, g_window, StructureNotifyMask);
    g_delete_window = XInternAtom(g_display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(g_display, g_window, &g_delete_window, 1);
    XMapRaised(g_display, g_window);
    XFlush(g_display);

    if (!g_x_running.exchange(true)) {
        g_x_thread = std::thread(XEventLoop);
    }
    return true;
}

}  // namespace

bool ClapHostOpenEditor()
{
    if (!g_ready) {
        return false;
    }

    const auto* const gui = static_cast<const clap_plugin_gui_t*>(
        g_plugin->get_extension(g_plugin, CLAP_EXT_GUI));
    if (gui == nullptr) {
        Log("clap: the plugin has no GUI extension");
        return false;
    }

    // Already built: the window only needs raising. Creating a second GUI would leak the first.
    if (g_gui_created) {
        XMapRaised(g_display, g_window);
        XFlush(g_display);
        gui->show(g_plugin);
        Log("clap: editor remapped for \"%s\"", g_name);
        return true;
    }

    // A floating window is one the plugin creates and drives itself, including its own event loop.
    // That avoids every hard part of embedding: no X11 window from us, no timer or fd plumbing, and
    // no argument about which thread owns the toolkit. Embedding stays as the fallback for plugins
    // that only support a parent window.
    if (!gui->is_api_supported(g_plugin, CLAP_WINDOW_API_X11, false)) {
        Log("clap: the plugin does not support an embedded X11 editor");
        return false;
    }

    if (!gui->create(g_plugin, CLAP_WINDOW_API_X11, false)) {
        Log("clap: gui create failed");
        return false;
    }

    // Ask for the editor's own size before making the window, so it is right the first time.
    uint32_t width = 800;
    uint32_t height = 600;
    gui->get_size(g_plugin, &width, &height);
    Log("clap: editor is %ux%u", width, height);

    if (!OpenParentWindow(width, height, g_name)) {
        return false;
    }

    clap_window_t parent{};
    parent.api = CLAP_WINDOW_API_X11;
    parent.x11 = g_window;
    if (!gui->set_parent(g_plugin, &parent)) {
        Log("clap: set_parent failed");
        return false;
    }

    if (!gui->show(g_plugin)) {
        Log("clap: gui show failed");
        return false;
    }
    XFlush(g_display);
    g_gui_created = true;

    Log("clap: editor open for \"%s\"", g_name);
    return true;
}

void ClapHostCloseEditor()
{
    if (!g_gui_created || g_display == nullptr) {
        return;
    }
    XUnmapWindow(g_display, g_window);
    XFlush(g_display);
    Log("clap: editor hidden");
}

void ClapHostSetLogger(void (*logger)(const char*))
{
    g_logger = logger;
}

uint32_t ClapHostChannelCount()
{
    return g_ready ? g_channel_count : 0;
}

uint32_t ClapHostParameterCount()
{
    return g_ready ? g_parameter_count : 0;
}

void ClapHostLogParameters()
{
    const clap_plugin_params_t* const params = ParamsExtension();
    if (params == nullptr) {
        Log("clap: the plugin exposes no parameters extension");
        return;
    }
    Log("clap: %u parameters", g_parameter_count);
    for (uint32_t i = 0; i < g_parameter_count; ++i) {
        clap_param_info_t info{};
        if (!params->get_info(g_plugin, i, &info)) {
            continue;
        }
        double value = 0.0;
        params->get_value(g_plugin, info.id, &value);
        Log("clap:   [%2u] \"%s\"  %.3f .. %.3f  now %.3f",
            i, info.name, info.min_value, info.max_value, value);
    }
}

void ClapHostQueueParameter(uint32_t index, double plain_value)
{
    if (!g_ready || index >= g_parameter_count) {
        return;
    }
    g_parameters[index].value.store(plain_value);
    g_parameters[index].dirty.store(true);
}

void ClapHostQueueParameterNormalised(uint32_t index, double position)
{
    if (!g_ready || index >= g_parameter_count) {
        return;
    }
    if (position < 0.0) { position = 0.0; }
    if (position > 1.0) { position = 1.0; }
    const PendingParameter& p = g_parameters[index];
    ClapHostQueueParameter(index, p.minimum + position * (p.maximum - p.minimum));
}

const char* ClapHostName()
{
    return (g_ready && g_name[0] != '\0') ? g_name : nullptr;
}

bool ClapHostLoad(const char* path, double sample_rate, uint32_t max_frames)
{
    g_main_thread_id = std::this_thread::get_id();

    void* const handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        Log("clap: dlopen(%s) failed: %s", path, dlerror());
        return false;
    }

    g_entry = static_cast<const clap_plugin_entry_t*>(dlsym(handle, "clap_entry"));
    if (g_entry == nullptr) {
        Log("clap: %s exports no clap_entry", path);
        return false;
    }
    if (!g_entry->init(path)) {
        Log("clap: entry init failed");
        return false;
    }

    const auto* factory = static_cast<const clap_plugin_factory_t*>(
        g_entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    if (factory == nullptr || factory->get_plugin_count(factory) == 0) {
        Log("clap: no plugin factory");
        return false;
    }

    const clap_plugin_descriptor_t* const descriptor = factory->get_plugin_descriptor(factory, 0);
    if (descriptor == nullptr) {
        Log("clap: no plugin descriptor");
        return false;
    }

    g_plugin = factory->create_plugin(factory, &g_host, descriptor->id);
    if (g_plugin == nullptr) {
        Log("clap: create_plugin(%s) failed", descriptor->id);
        return false;
    }
    if (!g_plugin->init(g_plugin)) {
        Log("clap: plugin init failed");
        return false;
    }

    g_channel_count = ReadPortChannelCount(g_plugin);
    if (g_channel_count == 0 || g_channel_count > 8) {
        Log("clap: unusable input port channel count %u", g_channel_count);
        return false;
    }

    if (!g_plugin->activate(g_plugin, sample_rate, 1, max_frames)) {
        Log("clap: activate(%.0f Hz, max %u frames) failed", sample_rate, max_frames);
        return false;
    }
    if (!g_plugin->start_processing(g_plugin)) {
        Log("clap: start_processing failed");
        return false;
    }

    g_max_frames = max_frames;
    g_scratch.assign(static_cast<size_t>(g_channel_count) * max_frames, 0.0f);
    g_scratch_pointers.resize(g_channel_count);
    for (uint32_t channel = 0; channel < g_channel_count; ++channel) {
        g_scratch_pointers[channel] = g_scratch.data() + static_cast<size_t>(channel) * max_frames;
    }

    // Cache the parameter ids and ranges once, so the audio thread never queries the extension.
    if (const clap_plugin_params_t* const params = ParamsExtension()) {
        const uint32_t count = params->count(g_plugin);
        g_parameter_count = count < kMaxParameters ? count : kMaxParameters;
        for (uint32_t i = 0; i < g_parameter_count; ++i) {
            clap_param_info_t info{};
            if (!params->get_info(g_plugin, i, &info)) {
                continue;
            }
            g_parameters[i].id = info.id;
            g_parameters[i].minimum = info.min_value;
            g_parameters[i].maximum = info.max_value;
        }
    }

    std::snprintf(g_name, sizeof(g_name), "%s", descriptor->name ? descriptor->name : "CLAP plugin");
    g_ready = true;
    Log("clap: loaded \"%s\" by %s, %u channels, %.0f Hz, up to %u frames",
        descriptor->name, descriptor->vendor ? descriptor->vendor : "?",
        g_channel_count, sample_rate, max_frames);
    return true;
}

bool ClapHostProcess(float** channels, uint32_t channel_count, uint32_t frames)
{
    if (!g_ready || channels == nullptr || frames == 0 || frames > g_max_frames) {
        return false;
    }
    if (channel_count != g_channel_count) {
        return false;
    }

    // Resolve processes in place, so the plugin needs its own copy of the input.
    for (uint32_t channel = 0; channel < channel_count; ++channel) {
        std::memcpy(g_scratch_pointers[channel], channels[channel], frames * sizeof(float));
    }

    clap_audio_buffer_t input{};
    input.data32 = g_scratch_pointers.data();
    input.channel_count = channel_count;

    clap_audio_buffer_t output{};
    output.data32 = channels;
    output.channel_count = channel_count;

    clap_process_t process{};
    process.steady_time = -1;
    process.frames_count = frames;
    process.transport = nullptr;
    process.audio_inputs = &input;
    process.audio_inputs_count = 1;
    process.audio_outputs = &output;
    process.audio_outputs_count = 1;
    process.in_events = &g_input_events;
    process.out_events = &g_output_events;

    CollectParameterEvents();

    const clap_process_status status = g_plugin->process(g_plugin, &process);
    return status != CLAP_PROCESS_ERROR;
}
