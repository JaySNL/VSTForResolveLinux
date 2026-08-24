// One VST3 plugin per Fairlight effect.
//
// The ABI comes from travesty (third_party/dpf/distrho/src/travesty), DPF's clean-room C header
// set for VST3. The Steinberg SDK is GPL3-or-commercial and would decide this project's licence
// for it; travesty is ISC and is plain C structs, which is the whole reason it is usable here.
//
// A VST3 "plugin" is three objects, and mixing them up is the usual way this goes wrong:
//   - the component      (v3_component)        - buses, state, activation
//   - the audio processor (v3_audio_processor) - the same object, queried for another interface
//   - the edit controller (v3_edit_controller) - parameters and the editor window
// The controller is often a separate class with its own id, which is why get_controller_class_id
// is asked before falling back to querying the component itself.
//
// yabridge presents a Windows VST3 as a native Linux bundle with the same layout, so this host
// reaches the Windows plugins on this machine as well as the native ones.
#include "vst3_plugin.h"

#include <dlfcn.h>
#include <sys/stat.h>
#include <dirent.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "plugin_window.h"

#include "base.h"
#include "factory.h"
#include "component.h"
#include "audio_processor.h"
#include "edit_controller.h"
#include "view.h"
#include "host.h"

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

// --- calling the objects ------------------------------------------------------------------------
//
// Every VST3 object is a pointer to a pointer to its vtable, and the vtable always starts with
// funknown. The _cpp structs in travesty describe the whole vtable, so casting to those and
// naming the sub-struct is unambiguous - unlike v3_cpp_obj, which has to guess the offset from
// the type and needs a specialisation per interface.

template <class T>
v3_funknown* Unknown(T** object)
{
    return static_cast<v3_funknown*>(static_cast<void*>(*object));
}

template <class T>
bool Query(T** object, const v3_tuid iid, void** out)
{
    return object != nullptr && Unknown(object)->query_interface(object, iid, out) == V3_OK &&
           *out != nullptr;
}

template <class T>
void Release(T**& object)
{
    if (object != nullptr) {
        Unknown(object)->unref(object);
        object = nullptr;
    }
}

// --- the host we present ------------------------------------------------------------------------
//
// A plugin that finds no host context refuses to initialise, and one that finds no component
// handler often refuses to build its editor. Both are answered, and neither does anything: this
// bridge sends no parameter changes, so there is nothing for an edit gesture to report.

v3_result V3_API HostQueryInterface(void* self, const v3_tuid iid, void** obj)
{
    if (std::memcmp(iid, v3_funknown_iid, sizeof(v3_tuid)) == 0 ||
        std::memcmp(iid, v3_host_application_iid, sizeof(v3_tuid)) == 0) {
        *obj = self;
        return V3_OK;
    }
    *obj = nullptr;
    return V3_NO_INTERFACE;
}

uint32_t V3_API HostRef(void*) { return 1; }
uint32_t V3_API HostUnref(void*) { return 1; }

v3_result V3_API HostGetName(void*, v3_str_128 name)
{
    static const char text[] = "Jay's Desk FX bridge";
    for (size_t index = 0; index < sizeof(text); ++index) {
        name[index] = static_cast<int16_t>(text[index]);
    }
    return V3_OK;
}

v3_result V3_API HostCreateInstance(void*, v3_tuid, v3_tuid, void** obj)
{
    *obj = nullptr;
    return V3_NOT_IMPLEMENTED;
}

const v3_host_application_cpp g_host_vtable = {
    {HostQueryInterface, HostRef, HostUnref},
    {HostGetName, HostCreateInstance},
};
const v3_host_application_cpp* g_host_vtable_pointer = &g_host_vtable;
v3_funknown** const g_host_context =
    reinterpret_cast<v3_funknown**>(const_cast<v3_host_application_cpp**>(&g_host_vtable_pointer));

v3_result V3_API HandlerQueryInterface(void* self, const v3_tuid iid, void** obj)
{
    if (std::memcmp(iid, v3_funknown_iid, sizeof(v3_tuid)) == 0 ||
        std::memcmp(iid, v3_component_handler_iid, sizeof(v3_tuid)) == 0) {
        *obj = self;
        return V3_OK;
    }
    *obj = nullptr;
    return V3_NO_INTERFACE;
}

v3_result V3_API HandlerBeginEdit(void*, v3_param_id) { return V3_OK; }
v3_result V3_API HandlerPerformEdit(void*, v3_param_id, double) { return V3_OK; }
v3_result V3_API HandlerEndEdit(void*, v3_param_id) { return V3_OK; }
v3_result V3_API HandlerRestart(void*, int32_t) { return V3_OK; }

const v3_component_handler_cpp g_handler_vtable = {
    {HandlerQueryInterface, HostRef, HostUnref},
    {HandlerBeginEdit, HandlerPerformEdit, HandlerEndEdit, HandlerRestart},
};
const v3_component_handler_cpp* g_handler_vtable_pointer = &g_handler_vtable;
v3_component_handler** const g_component_handler = reinterpret_cast<v3_component_handler**>(
    const_cast<v3_component_handler_cpp**>(&g_handler_vtable_pointer));

// --- the bundle ----------------------------------------------------------------------------------

bool IsDirectory(const std::string& path)
{
    struct stat info;
    return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

// Name.vst3/Contents/x86_64-linux/Name.so. The scanner records the bundle directory, because that
// is the thing a user recognises; the binary inside is found here.
std::string BinaryInsideBundle(const std::string& bundle)
{
    if (!IsDirectory(bundle)) {
        return bundle;  // the flat, single-file variant
    }
    const std::string folder = bundle + "/Contents/x86_64-linux";
    DIR* const directory = opendir(folder.c_str());
    if (directory == nullptr) {
        return std::string();
    }
    std::string found;
    while (const dirent* const item = readdir(directory)) {
        const std::string name = item->d_name;
        if (name.size() > 3 && name.compare(name.size() - 3, 3, ".so") == 0) {
            found = folder + "/" + name;
            break;
        }
    }
    closedir(directory);
    return found;
}

constexpr v3_speaker_arrangement kArrangementMono = 1;    // front left only
constexpr v3_speaker_arrangement kArrangementStereo = 3;  // front left and front right

class Vst3Plugin final : public HostedPlugin {
public:
    ~Vst3Plugin() override
    {
        if (view_ != nullptr) {
            auto* const vt = *reinterpret_cast<v3_plugin_view_cpp**>(view_);
            vt->view.removed(view_);
            Release(view_);
        }
        if (window_ != nullptr) {
            PluginWindowDestroy(window_);
            window_ = nullptr;
        }
        if (controller_ != nullptr) {
            auto* const vt = *reinterpret_cast<v3_edit_controller_cpp**>(controller_);
            vt->base.terminate(controller_);
            Release(controller_);
        }
        if (processor_ != nullptr && processing_) {
            auto* const vt = *reinterpret_cast<v3_audio_processor_cpp**>(processor_);
            vt->proc.set_processing(processor_, false);
        }
        Release(processor_);
        if (component_ != nullptr) {
            auto* const vt = *reinterpret_cast<v3_component_cpp**>(component_);
            if (active_) {
                vt->comp.set_active(component_, false);
            }
            vt->base.terminate(component_);
            Release(component_);
        }
        Release(factory_);
        // The module is left loaded. ModuleExit tears down every instance the library made, and
        // this host has no way to know whether another effect still holds one.
    }

    bool Load(const char* path, double sample_rate, uint32_t max_frames)
    {
        const std::string binary = BinaryInsideBundle(path);
        if (binary.empty()) {
            Log("vst3: %s has no Contents/x86_64-linux binary", path);
            return false;
        }

        void* const handle = dlopen(binary.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (handle == nullptr) {
            Log("vst3: dlopen(%s) failed: %s", binary.c_str(), dlerror());
            return false;
        }

        // Linux bundles must be entered before the factory is asked for anything.
        using ModuleEntryFn = bool (*)(void*);
        if (auto* const entry = reinterpret_cast<ModuleEntryFn>(dlsym(handle, "ModuleEntry"))) {
            if (!entry(handle)) {
                Log("vst3: ModuleEntry refused for %s", binary.c_str());
                return false;
            }
        }

        using GetFactoryFn = v3_plugin_factory** (*)(void);
        auto* const get_factory =
            reinterpret_cast<GetFactoryFn>(dlsym(handle, "GetPluginFactory"));
        if (get_factory == nullptr) {
            Log("vst3: %s exports no GetPluginFactory", binary.c_str());
            return false;
        }
        factory_ = get_factory();
        if (factory_ == nullptr) {
            Log("vst3: GetPluginFactory returned null for %s", binary.c_str());
            return false;
        }

        if (!CreateComponent(path)) {
            return false;
        }
        if (!SetUpAudio(sample_rate, max_frames)) {
            return false;
        }
        CreateController();  // an editor is optional; audio is not
        return true;
    }

    bool Process(float** buffers, uint32_t channel_count, uint32_t frames) override
    {
        if (processor_ == nullptr || buffers == nullptr || frames == 0 || frames > max_frames_) {
            return false;
        }
        if (channel_count != channel_count_) {
            return false;
        }

        // VST3 wants separate input and output pointers, and Resolve hands us one buffer that is
        // both. The input is copied into scratch and the plugin writes into Resolve's buffer.
        for (uint32_t channel = 0; channel < channel_count; ++channel) {
            std::memcpy(scratch_pointers_[channel], buffers[channel], frames * sizeof(float));
        }

        v3_audio_bus_buffers input{};
        input.num_channels = static_cast<int32_t>(channel_count);
        input.channel_silence_bitset = 0;
        input.channel_buffers_32 = scratch_pointers_.data();

        v3_audio_bus_buffers output{};
        output.num_channels = static_cast<int32_t>(channel_count);
        output.channel_silence_bitset = 0;
        output.channel_buffers_32 = buffers;

        v3_process_data data{};
        data.process_mode = V3_REALTIME;
        data.symbolic_sample_size = V3_SAMPLE_32;
        data.nframes = static_cast<int32_t>(frames);
        data.num_input_buses = 1;
        data.num_output_buses = 1;
        data.inputs = &input;
        data.outputs = &output;

        auto* const vt = *reinterpret_cast<v3_audio_processor_cpp**>(processor_);
        return vt->proc.process(processor_, &data) == V3_OK;
    }

    uint32_t ChannelCount() const override { return channel_count_; }
    const char* Name() const override { return name_[0] != '\0' ? name_ : nullptr; }
    PluginFormat Format() const override { return PluginFormat::Vst3; }

    bool OpenEditor() override
    {
        if (controller_ == nullptr) {
            Log("vst3: \"%s\" has no edit controller, so no editor", name_);
            return false;
        }
        if (view_ != nullptr) {
            return PluginWindowShow(window_);
        }

        auto* const controller_vt = *reinterpret_cast<v3_edit_controller_cpp**>(controller_);
        view_ = controller_vt->ctrl.create_view(controller_, "editor");
        if (view_ == nullptr) {
            Log("vst3: create_view returned nothing for \"%s\"", name_);
            return false;
        }

        auto* const vt = *reinterpret_cast<v3_plugin_view_cpp**>(view_);
        if (vt->view.is_platform_type_supported(view_, V3_VIEW_PLATFORM_TYPE_X11) != V3_OK) {
            Log("vst3: \"%s\" does not support an X11 embedded editor", name_);
            Release(view_);
            return false;
        }

        // Ask the view its size before making the window, so it is right the first time.
        unsigned int width = 800;
        unsigned int height = 600;
        v3_view_rect rect{};
        if (vt->view.get_size(view_, &rect) == V3_OK && rect.right > rect.left &&
            rect.bottom > rect.top) {
            width = static_cast<unsigned int>(rect.right - rect.left);
            height = static_cast<unsigned int>(rect.bottom - rect.top);
        }

        window_ = PluginWindowCreate(width, height, name_);
        const unsigned long handle = PluginWindowHandle(window_);
        if (handle == 0) {
            Release(view_);
            return false;
        }

        if (vt->view.attached(view_, reinterpret_cast<void*>(handle),
                              V3_VIEW_PLATFORM_TYPE_X11) != V3_OK) {
            Log("vst3: attached() refused for \"%s\"", name_);
            Release(view_);
            return false;
        }
        PluginWindowFlush(window_);
        Log("vst3: editor open for \"%s\" at %ux%u", name_, width, height);
        return true;
    }

    void CloseEditor() override { PluginWindowHide(window_); }

private:
    // The first class the factory calls an Audio Module is the plugin. A bundle with several is
    // rare outside instrument collections, and picking the first matches what the scanner named.
    bool CreateComponent(const char* path)
    {
        auto* const factory_vt = *reinterpret_cast<v3_plugin_factory_cpp**>(factory_);
        const int32_t count = factory_vt->v1.num_classes(factory_);

        bool have_factory_2 = false;
        {
            void* probe = nullptr;
            if (Query(factory_, v3_plugin_factory_2_iid, &probe)) {
                have_factory_2 = true;
                Unknown(factory_)->unref(factory_);  // Query took a reference we do not keep
            }
        }

        for (int32_t index = 0; index < count; ++index) {
            v3_class_info info{};
            if (factory_vt->v1.get_class_info(factory_, index, &info) != V3_OK) {
                continue;
            }
            if (std::strcmp(info.category, "Audio Module Class") != 0) {
                continue;
            }

            void* created = nullptr;
            if (factory_vt->v1.create_instance(factory_, info.class_id, v3_component_iid,
                                               &created) != V3_OK ||
                created == nullptr) {
                Log("vst3: create_instance failed for \"%s\"", info.name);
                continue;
            }
            component_ = static_cast<v3_component**>(created);

            auto* const vt = *reinterpret_cast<v3_component_cpp**>(component_);
            if (vt->base.initialize(component_, g_host_context) != V3_OK) {
                Log("vst3: initialize refused for \"%s\"", info.name);
                Release(component_);
                continue;
            }

            std::snprintf(name_, sizeof(name_), "%s", info.name);
            std::memcpy(class_id_, info.class_id, sizeof(v3_tuid));
            if (have_factory_2) {
                v3_class_info_2 info2{};
                auto* const v2 = &factory_vt->v2;
                if (v2->get_class_info_2(factory_, index, &info2) == V3_OK) {
                    Log("vst3: loaded \"%s\" by %s", info2.name, info2.vendor);
                }
            }
            return true;
        }

        Log("vst3: %s exposes no Audio Module class", path);
        return false;
    }

    bool SetUpAudio(double sample_rate, uint32_t max_frames)
    {
        void* queried = nullptr;
        if (!Query(component_, v3_audio_processor_iid, &queried)) {
            Log("vst3: \"%s\" is not an audio processor", name_);
            return false;
        }
        processor_ = static_cast<v3_audio_processor**>(queried);

        auto* const component_vt = *reinterpret_cast<v3_component_cpp**>(component_);
        auto* const processor_vt = *reinterpret_cast<v3_audio_processor_cpp**>(processor_);

        if (processor_vt->proc.can_process_sample_size(processor_, V3_SAMPLE_32) != V3_OK) {
            Log("vst3: \"%s\" cannot process 32-bit samples", name_);
            return false;
        }

        if (component_vt->comp.get_bus_count(component_, V3_AUDIO, V3_INPUT) < 1 ||
            component_vt->comp.get_bus_count(component_, V3_AUDIO, V3_OUTPUT) < 1) {
            Log("vst3: \"%s\" has no main audio bus in one direction - not an effect", name_);
            return false;
        }

        v3_bus_info input_bus{};
        v3_bus_info output_bus{};
        component_vt->comp.get_bus_info(component_, V3_AUDIO, V3_INPUT, 0, &input_bus);
        component_vt->comp.get_bus_info(component_, V3_AUDIO, V3_OUTPUT, 0, &output_bus);

        // The bridge feeds one bus in and reads one bus out, so the narrower side decides.
        channel_count_ = static_cast<uint32_t>(
            input_bus.channel_count < output_bus.channel_count ? input_bus.channel_count
                                                               : output_bus.channel_count);
        if (channel_count_ == 0 || channel_count_ > 8) {
            Log("vst3: unusable channel count %u for \"%s\"", channel_count_, name_);
            return false;
        }

        v3_speaker_arrangement arrangement =
            channel_count_ >= 2 ? kArrangementStereo : kArrangementMono;
        if (processor_vt->proc.set_bus_arrangements(processor_, &arrangement, 1, &arrangement,
                                                    1) != V3_OK) {
            // Reported, not fatal: a plugin may keep its own arrangement and still process.
            Log("vst3: set_bus_arrangements refused for \"%s\"", name_);
        }

        component_vt->comp.activate_bus(component_, V3_AUDIO, V3_INPUT, 0, true);
        component_vt->comp.activate_bus(component_, V3_AUDIO, V3_OUTPUT, 0, true);

        v3_process_setup setup{};
        setup.process_mode = V3_REALTIME;
        setup.symbolic_sample_size = V3_SAMPLE_32;
        setup.max_block_size = static_cast<int32_t>(max_frames);
        setup.sample_rate = sample_rate;
        if (processor_vt->proc.setup_processing(processor_, &setup) != V3_OK) {
            Log("vst3: setup_processing refused for \"%s\"", name_);
            return false;
        }

        if (component_vt->comp.set_active(component_, true) != V3_OK) {
            Log("vst3: set_active refused for \"%s\"", name_);
            return false;
        }
        active_ = true;

        if (processor_vt->proc.set_processing(processor_, true) != V3_OK) {
            // Some plugins answer false here and process anyway, so this is reported only.
            Log("vst3: set_processing answered false for \"%s\"", name_);
        }
        processing_ = true;

        max_frames_ = max_frames;
        scratch_.assign(static_cast<size_t>(channel_count_) * max_frames, 0.0f);
        scratch_pointers_.resize(channel_count_);
        for (uint32_t channel = 0; channel < channel_count_; ++channel) {
            scratch_pointers_[channel] =
                scratch_.data() + static_cast<size_t>(channel) * max_frames;
        }

        Log("vst3: \"%s\" ready, %u channels, %.0f Hz, up to %u frames", name_, channel_count_,
            sample_rate, max_frames);
        return true;
    }

    // The controller is usually a second class with its own id. Only when the plugin says it has
    // none is the component itself asked for the interface.
    void CreateController()
    {
        auto* const component_vt = *reinterpret_cast<v3_component_cpp**>(component_);
        v3_tuid controller_id{};
        void* created = nullptr;

        if (component_vt->comp.get_controller_class_id(component_, controller_id) == V3_OK) {
            auto* const factory_vt = *reinterpret_cast<v3_plugin_factory_cpp**>(factory_);
            if (factory_vt->v1.create_instance(factory_, controller_id, v3_edit_controller_iid,
                                               &created) != V3_OK) {
                created = nullptr;
            }
        }
        if (created == nullptr) {
            void* queried = nullptr;
            if (Query(component_, v3_edit_controller_iid, &queried)) {
                created = queried;
            }
        }
        if (created == nullptr) {
            Log("vst3: \"%s\" exposes no edit controller", name_);
            return;
        }

        controller_ = static_cast<v3_edit_controller**>(created);
        auto* const vt = *reinterpret_cast<v3_edit_controller_cpp**>(controller_);
        if (vt->base.initialize(controller_, g_host_context) != V3_OK) {
            // A controller that came from query_interface is already initialised as the component.
            Log("vst3: controller initialize answered false for \"%s\"", name_);
        }
        vt->ctrl.set_component_handler(controller_, g_component_handler);
    }

    v3_plugin_factory** factory_ = nullptr;
    v3_component** component_ = nullptr;
    v3_audio_processor** processor_ = nullptr;
    v3_edit_controller** controller_ = nullptr;
    v3_plugin_view** view_ = nullptr;
    PluginWindow* window_ = nullptr;

    bool active_ = false;
    bool processing_ = false;
    uint32_t channel_count_ = 0;
    uint32_t max_frames_ = 0;
    char name_[128] = {0};
    v3_tuid class_id_{};

    std::vector<float> scratch_;
    std::vector<float*> scratch_pointers_;
};

}  // namespace

HostedPlugin* CreateVst3Plugin(const char* path, double sample_rate, uint32_t max_frames)
{
    auto* const plugin = new Vst3Plugin();
    if (plugin->Load(path, sample_rate, max_frames)) {
        return plugin;
    }
    delete plugin;
    return nullptr;
}

void Vst3PluginSetLogger(void (*logger)(const char*)) { g_logger = logger; }
