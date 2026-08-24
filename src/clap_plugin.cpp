// One CLAP plugin per Fairlight effect.
//
// This replaces the single-instance host in clap_host.cpp. That one kept its plugin, its editor,
// its timer and its parameter queue in file scope, so the second effect in a project got nothing:
// the log line read "no plugin for ... - audio passes through" and the effect silently did nothing.
// With eight CLAP entries in the effect menu that is not a corner case, it is the normal case.
//
// Threading follows the CLAP contract. Loading, init, activate and every GUI call run on the
// thread that created the effect; only process() is called from Resolve's audio thread.
#include "clap_plugin.h"

#include <clap/clap.h>
#include <dlfcn.h>

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "plugin_window.h"

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

// One dlopen per file, however many effects use it.
//
// clap_entry->init() and deinit() are per library, not per plugin, and calling init twice on one
// library is outside the contract. Two effects on the same .clap therefore share this record and
// the library is closed when the last one goes.
struct LoadedLibrary {
    std::string path;
    void* handle = nullptr;
    const clap_plugin_entry_t* entry = nullptr;
    int users = 0;
};

std::mutex g_library_lock;
std::vector<LoadedLibrary*> g_libraries;

LoadedLibrary* AcquireLibrary(const char* path)
{
    std::lock_guard<std::mutex> held(g_library_lock);
    for (LoadedLibrary* library : g_libraries) {
        if (library->path == path) {
            ++library->users;
            return library;
        }
    }

    void* const handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        Log("clap: dlopen(%s) failed: %s", path, dlerror());
        return nullptr;
    }
    const auto* const entry = static_cast<const clap_plugin_entry_t*>(dlsym(handle, "clap_entry"));
    if (entry == nullptr) {
        Log("clap: %s exports no clap_entry", path);
        dlclose(handle);
        return nullptr;
    }
    if (!entry->init(path)) {
        Log("clap: entry init failed for %s", path);
        dlclose(handle);
        return nullptr;
    }

    auto* const library = new LoadedLibrary();
    library->path = path;
    library->handle = handle;
    library->entry = entry;
    library->users = 1;
    g_libraries.push_back(library);
    return library;
}

void ReleaseLibrary(LoadedLibrary* library)
{
    if (library == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> held(g_library_lock);
    if (--library->users > 0) {
        return;
    }
    for (size_t index = 0; index < g_libraries.size(); ++index) {
        if (g_libraries[index] == library) {
            g_libraries.erase(g_libraries.begin() + static_cast<long>(index));
            break;
        }
    }
    library->entry->deinit();
    dlclose(library->handle);
    delete library;
}

constexpr uint32_t kMaxParameters = 128;

// A knob turn arrives on Resolve's UI thread; CLAP requires the change to reach the plugin as an
// event on the audio thread. So a turn only marks a slot dirty, and process() drains the dirty
// slots into a real clap_event_param_value. No locks, and nothing allocates in the callback.
struct PendingParameter {
    std::atomic<bool> dirty{false};
    std::atomic<double> value{0.0};
    clap_id id{0};
    double minimum{0.0};
    double maximum{1.0};
};

class ClapPlugin final : public HostedPlugin {
public:
    ~ClapPlugin() override
    {
        StopTimer();
        if (plugin_ != nullptr) {
            if (gui_created_) {
                const auto* const gui = static_cast<const clap_plugin_gui_t*>(
                    plugin_->get_extension(plugin_, CLAP_EXT_GUI));
                if (gui != nullptr) {
                    gui->destroy(plugin_);
                }
                gui_created_ = false;
            }
            if (active_) {
                plugin_->stop_processing(plugin_);
                plugin_->deactivate(plugin_);
            }
            plugin_->destroy(plugin_);
        }
        // The window goes after the editor, never before: a plugin still drawing into a destroyed
        // window faults inside its own toolkit.
        if (window_ != nullptr) {
            PluginWindowDestroy(window_);
            window_ = nullptr;
        }
        ReleaseLibrary(library_);
    }

    bool Load(const char* path, double sample_rate, uint32_t max_frames)
    {
        main_thread_id_ = std::this_thread::get_id();

        library_ = AcquireLibrary(path);
        if (library_ == nullptr) {
            return false;
        }

        const auto* factory = static_cast<const clap_plugin_factory_t*>(
            library_->entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
        if (factory == nullptr || factory->get_plugin_count(factory) == 0) {
            Log("clap: no plugin factory in %s", path);
            return false;
        }
        const clap_plugin_descriptor_t* const descriptor =
            factory->get_plugin_descriptor(factory, 0);
        if (descriptor == nullptr) {
            Log("clap: no plugin descriptor in %s", path);
            return false;
        }

        host_ = clap_host_t{CLAP_VERSION_INIT,
                            this,  // host_data: how every callback below finds its plugin
                            "Jay's Desk FX bridge",
                            "Jay's Desk",
                            "https://jaysdesk.com",
                            "0.1.0",
                            HostGetExtension,
                            HostRequestRestart,
                            HostRequestProcess,
                            HostRequestCallback};

        plugin_ = factory->create_plugin(factory, &host_, descriptor->id);
        if (plugin_ == nullptr) {
            Log("clap: create_plugin(%s) failed", descriptor->id);
            return false;
        }
        if (!plugin_->init(plugin_)) {
            Log("clap: plugin init failed for %s", path);
            return false;
        }

        channel_count_ = ReadPortChannelCount();
        if (channel_count_ == 0 || channel_count_ > 8) {
            Log("clap: unusable input port channel count %u", channel_count_);
            return false;
        }

        if (!plugin_->activate(plugin_, sample_rate, 1, max_frames)) {
            Log("clap: activate(%.0f Hz, max %u frames) failed", sample_rate, max_frames);
            return false;
        }
        if (!plugin_->start_processing(plugin_)) {
            Log("clap: start_processing failed");
            plugin_->deactivate(plugin_);
            return false;
        }
        active_ = true;

        max_frames_ = max_frames;
        scratch_.assign(static_cast<size_t>(channel_count_) * max_frames, 0.0f);
        scratch_pointers_.resize(channel_count_);
        for (uint32_t channel = 0; channel < channel_count_; ++channel) {
            scratch_pointers_[channel] =
                scratch_.data() + static_cast<size_t>(channel) * max_frames;
        }

        in_events_ = clap_input_events_t{this, InputEventsSize, InputEventsGet};
        out_events_ = clap_output_events_t{this, OutputEventsPush};

        // Cache the parameter ids and ranges once, so the audio thread never queries the extension.
        if (const clap_plugin_params_t* const params = ParamsExtension()) {
            const uint32_t count = params->count(plugin_);
            parameter_count_ = count < kMaxParameters ? count : kMaxParameters;
            for (uint32_t index = 0; index < parameter_count_; ++index) {
                clap_param_info_t info{};
                if (!params->get_info(plugin_, index, &info)) {
                    continue;
                }
                parameters_[index].id = info.id;
                parameters_[index].minimum = info.min_value;
                parameters_[index].maximum = info.max_value;
            }
        }

        std::snprintf(name_, sizeof(name_), "%s",
                      descriptor->name != nullptr ? descriptor->name : "CLAP plugin");
        Log("clap: loaded \"%s\" by %s, %u channels, %.0f Hz, up to %u frames", name_,
            descriptor->vendor != nullptr ? descriptor->vendor : "?", channel_count_, sample_rate,
            max_frames);
        return true;
    }

    bool Process(float** buffers, uint32_t channel_count, uint32_t frames) override
    {
        if (plugin_ == nullptr || buffers == nullptr || frames == 0 || frames > max_frames_) {
            return false;
        }
        if (channel_count != channel_count_) {
            return false;
        }

        // Resolve processes in place, so the plugin needs its own copy of the input.
        for (uint32_t channel = 0; channel < channel_count; ++channel) {
            std::memcpy(scratch_pointers_[channel], buffers[channel], frames * sizeof(float));
        }

        clap_audio_buffer_t input{};
        input.data32 = scratch_pointers_.data();
        input.channel_count = channel_count;

        clap_audio_buffer_t output{};
        output.data32 = buffers;
        output.channel_count = channel_count;

        clap_process_t process{};
        process.steady_time = -1;
        process.frames_count = frames;
        process.transport = nullptr;
        process.audio_inputs = &input;
        process.audio_inputs_count = 1;
        process.audio_outputs = &output;
        process.audio_outputs_count = 1;
        process.in_events = &in_events_;
        process.out_events = &out_events_;

        CollectParameterEvents();

        return plugin_->process(plugin_, &process) != CLAP_PROCESS_ERROR;
    }

    uint32_t ChannelCount() const override { return channel_count_; }
    const char* Name() const override { return name_[0] != '\0' ? name_ : nullptr; }
    PluginFormat Format() const override { return PluginFormat::Clap; }

    bool OpenEditor() override
    {
        if (plugin_ == nullptr) {
            return false;
        }
        const auto* const gui = static_cast<const clap_plugin_gui_t*>(
            plugin_->get_extension(plugin_, CLAP_EXT_GUI));
        if (gui == nullptr) {
            Log("clap: \"%s\" has no GUI extension", name_);
            return false;
        }

        // Already built: the window only needs raising. Creating a second GUI would leak the first.
        if (gui_created_) {
            PluginWindowShow(window_);
            gui->show(plugin_);
            Log("clap: editor remapped for \"%s\"", name_);
            return true;
        }

        // A floating window is one the plugin creates and drives itself. DPF-based plugins answer
        // is_api_supported(X11, floating) with true and then crash in create(), because only the
        // embedded path is really implemented. So we always give the plugin a parent window.
        if (!gui->is_api_supported(plugin_, CLAP_WINDOW_API_X11, false)) {
            Log("clap: \"%s\" does not support an embedded X11 editor", name_);
            return false;
        }
        if (!gui->create(plugin_, CLAP_WINDOW_API_X11, false)) {
            Log("clap: gui create failed for \"%s\"", name_);
            return false;
        }

        // Ask the editor's own size before making the window, so it is right the first time.
        uint32_t width = 800;
        uint32_t height = 600;
        gui->get_size(plugin_, &width, &height);

        window_ = PluginWindowCreate(width, height, name_);
        const unsigned long handle = PluginWindowHandle(window_);
        if (handle == 0) {
            gui->destroy(plugin_);
            return false;
        }

        clap_window_t parent{};
        parent.api = CLAP_WINDOW_API_X11;
        parent.x11 = handle;
        if (!gui->set_parent(plugin_, &parent)) {
            Log("clap: set_parent failed for \"%s\"", name_);
            return false;
        }
        if (!gui->show(plugin_)) {
            Log("clap: gui show failed for \"%s\"", name_);
            return false;
        }
        PluginWindowFlush(window_);
        gui_created_ = true;
        Log("clap: editor open for \"%s\" at %ux%u", name_, width, height);
        return true;
    }

    void CloseEditor() override { PluginWindowHide(window_); }

    void LogParameters() const
    {
        Log("clap: %u parameters in \"%s\"", parameter_count_, name_);
    }

private:
    const clap_plugin_params_t* ParamsExtension() const
    {
        if (plugin_ == nullptr) {
            return nullptr;
        }
        return static_cast<const clap_plugin_params_t*>(
            plugin_->get_extension(plugin_, CLAP_EXT_PARAMS));
    }

    // Ask the plugin how many channels its first input port carries.
    uint32_t ReadPortChannelCount() const
    {
        const auto* ports = static_cast<const clap_plugin_audio_ports_t*>(
            plugin_->get_extension(plugin_, CLAP_EXT_AUDIO_PORTS));
        if (ports == nullptr || ports->count(plugin_, true) == 0) {
            return 0;
        }
        clap_audio_port_info_t info{};
        if (!ports->get(plugin_, 0, true, &info)) {
            return 0;
        }
        return info.channel_count;
    }

    void CollectParameterEvents()
    {
        event_count_ = 0;
        for (uint32_t index = 0; index < parameter_count_ && event_count_ < kMaxParameters;
             ++index) {
            if (!parameters_[index].dirty.exchange(false)) {
                continue;
            }
            clap_event_param_value_t& event = event_storage_[event_count_];
            event = {};
            event.header.size = sizeof(event);
            event.header.time = 0;
            event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            event.header.type = CLAP_EVENT_PARAM_VALUE;
            event.header.flags = 0;
            event.param_id = parameters_[index].id;
            event.cookie = nullptr;
            event.note_id = -1;
            event.port_index = -1;
            event.channel = -1;
            event.key = -1;
            event.value = parameters_[index].value.load();
            event_pointers_[event_count_] = &event.header;
            ++event_count_;
        }
    }

    void StopTimer()
    {
        if (timer_running_.exchange(false) && timer_thread_.joinable()) {
            timer_thread_.join();
        }
    }

    void TimerLoop()
    {
        while (timer_running_.load()) {
            const uint32_t period = timer_period_.load();
            std::this_thread::sleep_for(std::chrono::milliseconds(period != 0 ? period : 16));
            if (!timer_running_.load() || plugin_ == nullptr) {
                continue;
            }
            const auto* const timer = static_cast<const clap_plugin_timer_support_t*>(
                plugin_->get_extension(plugin_, CLAP_EXT_TIMER_SUPPORT));
            if (timer != nullptr) {
                timer->on_timer(plugin_, timer_id_.load());
            }
        }
    }

    // --- the callbacks the plugin sees. host_data carries the object. --------------------------

    static ClapPlugin* From(const clap_host_t* host)
    {
        return host != nullptr ? static_cast<ClapPlugin*>(host->host_data) : nullptr;
    }

    static bool HostRegisterTimer(const clap_host_t* host, uint32_t period_ms, clap_id* out_id)
    {
        ClapPlugin* const self = From(host);
        if (self == nullptr || out_id == nullptr) {
            return false;
        }
        *out_id = 1;
        self->timer_id_.store(1);
        self->timer_period_.store(period_ms);
        if (!self->timer_running_.exchange(true)) {
            self->timer_thread_ = std::thread([self]() { self->TimerLoop(); });
        }
        return true;
    }

    static bool HostUnregisterTimer(const clap_host_t* host, clap_id)
    {
        ClapPlugin* const self = From(host);
        if (self != nullptr) {
            self->StopTimer();
        }
        return true;
    }

    static void HostResizeHintsChanged(const clap_host_t*) {}
    static bool HostRequestResize(const clap_host_t*, uint32_t, uint32_t) { return true; }
    static bool HostRequestShow(const clap_host_t*) { return true; }
    static bool HostRequestHide(const clap_host_t*) { return true; }
    static void HostGuiClosed(const clap_host_t*, bool) {}

    // The audio thread is Resolve's; every other call we make comes from the thread that built the
    // effect.
    static bool HostIsMainThread(const clap_host_t* host)
    {
        ClapPlugin* const self = From(host);
        return self != nullptr && std::this_thread::get_id() == self->main_thread_id_;
    }
    static bool HostIsAudioThread(const clap_host_t* host) { return !HostIsMainThread(host); }

    static const void* HostGetExtension(const clap_host_t*, const char* id)
    {
        static const clap_host_timer_support_t timer = {HostRegisterTimer, HostUnregisterTimer};
        static const clap_host_gui_t gui = {HostResizeHintsChanged, HostRequestResize,
                                            HostRequestShow, HostRequestHide, HostGuiClosed};
        static const clap_host_thread_check_t thread_check = {HostIsMainThread, HostIsAudioThread};
        if (id == nullptr) {
            return nullptr;
        }
        if (std::strcmp(id, CLAP_EXT_TIMER_SUPPORT) == 0) { return &timer; }
        if (std::strcmp(id, CLAP_EXT_GUI) == 0) { return &gui; }
        if (std::strcmp(id, CLAP_EXT_THREAD_CHECK) == 0) { return &thread_check; }
        return nullptr;
    }

    static void HostRequestRestart(const clap_host_t*) {}
    static void HostRequestProcess(const clap_host_t*) {}
    static void HostRequestCallback(const clap_host_t*) {}

    static uint32_t InputEventsSize(const clap_input_events_t* list)
    {
        const auto* const self = static_cast<const ClapPlugin*>(list->ctx);
        return self != nullptr ? self->event_count_ : 0;
    }

    static const clap_event_header_t* InputEventsGet(const clap_input_events_t* list,
                                                     uint32_t index)
    {
        const auto* const self = static_cast<const ClapPlugin*>(list->ctx);
        if (self == nullptr || index >= self->event_count_) {
            return nullptr;
        }
        return self->event_pointers_[index];
    }

    static bool OutputEventsPush(const clap_output_events_t*, const clap_event_header_t*)
    {
        return true;
    }

    LoadedLibrary* library_ = nullptr;
    const clap_plugin_t* plugin_ = nullptr;
    clap_host_t host_{};
    bool active_ = false;
    bool gui_created_ = false;
    PluginWindow* window_ = nullptr;
    uint32_t channel_count_ = 0;
    uint32_t max_frames_ = 0;
    char name_[128] = {0};

    std::vector<float> scratch_;  // by channel: [channel][max_frames]
    std::vector<float*> scratch_pointers_;

    std::thread timer_thread_;
    std::atomic<bool> timer_running_{false};
    std::atomic<clap_id> timer_id_{0};
    std::atomic<uint32_t> timer_period_{16};
    std::thread::id main_thread_id_;

    PendingParameter parameters_[kMaxParameters];
    uint32_t parameter_count_ = 0;
    clap_event_param_value_t event_storage_[kMaxParameters] = {};
    const clap_event_header_t* event_pointers_[kMaxParameters] = {nullptr};
    uint32_t event_count_ = 0;
    clap_input_events_t in_events_{};
    clap_output_events_t out_events_{};
};

}  // namespace

HostedPlugin* CreateClapPlugin(const char* path, double sample_rate, uint32_t max_frames)
{
    auto* const plugin = new ClapPlugin();
    if (plugin->Load(path, sample_rate, max_frames)) {
        plugin->LogParameters();
        return plugin;
    }
    delete plugin;
    return nullptr;
}

void ClapPluginSetLogger(void (*logger)(const char*)) { g_logger = logger; }
