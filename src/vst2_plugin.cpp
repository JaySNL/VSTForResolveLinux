// A VST2 plugin instance. One object per Fairlight effect, no shared state.

#include "plugin_instance.h"

#include "plugin_window.h"
#include "vst2_abi.h"
#include "clap_host.h"

#include <dlfcn.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

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

class Vst2Plugin final : public HostedPlugin {
public:
    ~Vst2Plugin() override
    {
        if (effect_ != nullptr) {
            Dispatch(kEffMainsChanged, 0, 0, nullptr, 0.0f);  // suspend
            if (editor_open_) {
                Dispatch(kEffEditClose, 0, 0, nullptr, 0.0f);
            }
            Dispatch(kEffClose, 0, 0, nullptr, 0.0f);
        }
        if (library_ != nullptr) {
            dlclose(library_);
        }
    }

    bool Load(const char* path, double sample_rate, uint32_t max_frames)
    {
        sample_rate_ = sample_rate;
        max_frames_ = max_frames;

        library_ = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if (library_ == nullptr) {
            Log("vst2: dlopen(%s) failed: %s", path, dlerror());
            return false;
        }

        // "VSTPluginMain" is the current name; "main" is what older Linux plugins export, and
        // plenty of them export nothing else.
        auto entry = reinterpret_cast<VstPluginMain>(dlsym(library_, "VSTPluginMain"));
        if (entry == nullptr) {
            entry = reinterpret_cast<VstPluginMain>(dlsym(library_, "main"));
        }
        if (entry == nullptr) {
            Log("vst2: %s exports neither VSTPluginMain nor main", path);
            return false;
        }

        // The plugin gets our callback and, through effect->user, a way back to this object. That
        // is what makes a second instance possible at all.
        effect_ = entry(&Vst2Plugin::HostCallback);
        if (effect_ == nullptr) {
            Log("vst2: the entry point of %s returned no effect", path);
            return false;
        }
        if (effect_->magic != kEffectMagic) {
            Log("vst2: %s is not a VST2 plugin (magic 0x%08x)", path,
                static_cast<unsigned>(effect_->magic));
            effect_ = nullptr;
            return false;
        }
        if ((effect_->flags & kEffFlagsCanReplacing) == 0 || effect_->processReplacing == nullptr) {
            Log("vst2: %s has no processReplacing - too old to host", path);
            effect_ = nullptr;
            return false;
        }
        effect_->user = this;

        // The order matters: open, then rate and block size, then resume. A plugin told to resume
        // before it knows its rate allocates for the wrong one.
        Dispatch(kEffOpen, 0, 0, nullptr, 0.0f);
        Dispatch(kEffSetSampleRate, 0, 0, nullptr, static_cast<float>(sample_rate));
        Dispatch(kEffSetBlockSize, 0, static_cast<intptr_t>(max_frames), nullptr, 0.0f);
        Dispatch(kEffMainsChanged, 0, 1, nullptr, 0.0f);

        char name[64] = {0};
        if (Dispatch(kEffGetEffectName, 0, 0, name, 0.0f) != 0 && name[0] != '\0') {
            std::snprintf(name_, sizeof(name_), "%s", name);
        } else {
            const char* const slash = std::strrchr(path, '/');
            std::snprintf(name_, sizeof(name_), "%s", slash != nullptr ? slash + 1 : path);
        }

        channels_ = effect_->numOutputs > 0 ? static_cast<uint32_t>(effect_->numOutputs) : 0;
        if (channels_ > 2) {
            channels_ = 2;  // Resolve hands this effect a stereo pair
        }
        if (channels_ == 0) {
            Log("vst2: \"%s\" reports no outputs", name_);
            effect_ = nullptr;
            return false;
        }

        scratch_.assign(static_cast<size_t>(channels_) * max_frames_, 0.0f);
        Log("vst2: loaded \"%s\", %d in, %d out, %d parameters, %s editor",
            name_, effect_->numInputs, effect_->numOutputs, effect_->numParams,
            (effect_->flags & kEffFlagsHasEditor) != 0 ? "has an" : "no");
        return true;
    }

    bool Process(float** buffers, uint32_t channel_count, uint32_t frames) override
    {
        if (effect_ == nullptr || effect_->processReplacing == nullptr || buffers == nullptr ||
            frames == 0 || frames > max_frames_) {
            return false;
        }
        const uint32_t channels = channel_count < channels_ ? channel_count : channels_;
        if (scratch_.size() < static_cast<size_t>(channels) * max_frames_) {
            return false;
        }

        // processReplacing may not read and write the same buffer, so the input is copied aside.
        // The buffer belongs to this instance, which is why two effects no longer collide.
        float* input[2] = {nullptr, nullptr};
        for (uint32_t channel = 0; channel < channels && channel < 2; ++channel) {
            float* const slot = &scratch_[static_cast<size_t>(channel) * max_frames_];
            std::memcpy(slot, buffers[channel], frames * sizeof(float));
            input[channel] = slot;
        }

        effect_->processReplacing(effect_, input, buffers, static_cast<int32_t>(frames));
        return true;
    }

    uint32_t ChannelCount() const override { return effect_ != nullptr ? channels_ : 0; }
    const char* Name() const override { return name_; }
    PluginFormat Format() const override { return PluginFormat::Vst2; }

    bool OpenEditor() override
    {
        if (effect_ == nullptr) {
            return false;
        }
        if ((effect_->flags & kEffFlagsHasEditor) == 0) {
            Log("vst2: \"%s\" has no editor", name_);
            return false;
        }
        if (editor_open_) {
            return PluginWindowShow();
        }

        // Ask the plugin its size before making the window, so it is right the first time.
        unsigned int width = 800;
        unsigned int height = 600;
        ERect* rect = nullptr;
        if (Dispatch(kEffEditGetRect, 0, 0, &rect, 0.0f) != 0 && rect != nullptr) {
            const int rect_width = rect->right - rect->left;
            const int rect_height = rect->bottom - rect->top;
            if (rect_width > 0 && rect_height > 0) {
                width = static_cast<unsigned int>(rect_width);
                height = static_cast<unsigned int>(rect_height);
            }
        }

        const unsigned long window = PluginWindowOpen(width, height, name_);
        if (window == 0) {
            return false;
        }
        if (Dispatch(kEffEditOpen, 0, 0, reinterpret_cast<void*>(window), 0.0f) == 0) {
            // Some plugins answer 0 and open anyway, so this is reported, not treated as failure.
            Log("vst2: effEditOpen answered 0 for \"%s\"", name_);
        }
        PluginWindowFlush();
        editor_open_ = true;
        Log("vst2: editor open for \"%s\" at %ux%u", name_, width, height);
        return true;
    }

    void CloseEditor() override { PluginWindowHide(); }

private:
    intptr_t Dispatch(int32_t opcode, int32_t index, intptr_t value, void* ptr, float opt)
    {
        if (effect_ == nullptr || effect_->dispatcher == nullptr) {
            return 0;
        }
        return effect_->dispatcher(effect_, opcode, index, value, ptr, opt);
    }

    // Answering "I do not know" to an unknown opcode is correct. Claiming a capability we do not
    // implement is worse than saying no, because the plugin will then use it.
    static intptr_t HostCallback(AEffect* effect, int32_t opcode, int32_t index, intptr_t value,
                                 void* ptr, float opt)
    {
        (void)index;
        (void)value;
        (void)opt;
        auto* const self =
            effect != nullptr ? static_cast<Vst2Plugin*>(effect->user) : nullptr;

        switch (opcode) {
            case kAudioMasterVersion:
                return 2400;  // VST 2.4
            case kAudioMasterCurrentId:
                return effect != nullptr ? effect->uniqueID : 0;
            case kAudioMasterGetSampleRate:
                return self != nullptr ? static_cast<intptr_t>(self->sample_rate_) : 48000;
            case kAudioMasterGetBlockSize:
                return self != nullptr ? static_cast<intptr_t>(self->max_frames_) : 512;
            case kAudioMasterGetCurrentProcessLevel:
                return 2;  // realtime
            case kAudioMasterGetVendorString:
                if (ptr != nullptr) std::snprintf(static_cast<char*>(ptr), 64, "Jay's Desk");
                return 1;
            case kAudioMasterGetProductString:
                if (ptr != nullptr) std::snprintf(static_cast<char*>(ptr), 64, "Resolve FX Bridge");
                return 1;
            case kAudioMasterGetVendorVersion:
                return 1;
            case kAudioMasterCanDo:
                if (ptr != nullptr) {
                    const char* const what = static_cast<const char*>(ptr);
                    if (std::strcmp(what, "sizeWindow") == 0 ||
                        std::strcmp(what, "supplyIdle") == 0) {
                        return 1;
                    }
                }
                return 0;
            case kAudioMasterSizeWindow:
            case kAudioMasterUpdateDisplay:
            case kAudioMasterIdle:
            case kAudioMasterBeginEdit:
            case kAudioMasterEndEdit:
                return 1;
            default:
                return 0;
        }
    }

    void* library_ = nullptr;
    AEffect* effect_ = nullptr;
    double sample_rate_ = 48000.0;
    uint32_t max_frames_ = 8192;
    uint32_t channels_ = 0;
    bool editor_open_ = false;
    char name_[128] = {0};
    std::vector<float> scratch_;
};

}  // namespace

PluginFormat FormatFromPath(const char* path)
{
    if (path == nullptr) {
        return PluginFormat::Unknown;
    }
    const char* const dot = std::strrchr(path, '.');
    if (dot == nullptr) {
        return PluginFormat::Unknown;
    }
    if (std::strcmp(dot, ".clap") == 0) return PluginFormat::Clap;
    if (std::strcmp(dot, ".vst3") == 0) return PluginFormat::Vst3;
    if (std::strcmp(dot, ".so") == 0) return PluginFormat::Vst2;
    return PluginFormat::Unknown;
}

// CLAP, still behind the single-instance host in clap_host.cpp.
//
// That host keeps its plugin, its editor and its parameter queue in file scope, so it can back
// exactly one effect. Rather than leave CLAP unusable while VST2 moves to per-instance hosting,
// the first effect takes it and any second effect is refused out loud. Converting clap_host.cpp
// the way vst2_plugin.cpp is written here is the fix, and it is not done yet.
class ClapSingletonPlugin final : public HostedPlugin {
public:
    static bool taken;

    bool Load(const char* path, double sample_rate, uint32_t max_frames)
    {
        if (taken) {
            Log("clap: the single-instance host is already in use - this effect gets no plugin");
            return false;
        }
        if (!ClapHostLoad(path, sample_rate, max_frames)) {
            return false;
        }
        ClapHostLogParameters();
        taken = true;
        return true;
    }

    bool Process(float** buffers, uint32_t channel_count, uint32_t frames) override
    {
        return ClapHostProcess(buffers, channel_count, frames);
    }
    uint32_t ChannelCount() const override { return ClapHostChannelCount(); }
    const char* Name() const override { return ClapHostName(); }
    bool OpenEditor() override { return ClapHostOpenEditor(); }
    void CloseEditor() override { ClapHostCloseEditor(); }
    PluginFormat Format() const override { return PluginFormat::Clap; }
};

bool ClapSingletonPlugin::taken = false;

HostedPlugin* CreateHostedPlugin(PluginFormat format, const char* path, double sample_rate,
                                 uint32_t max_frames)
{
    if (format == PluginFormat::Vst2) {
        auto* const plugin = new Vst2Plugin();
        if (plugin->Load(path, sample_rate, max_frames)) {
            return plugin;
        }
        delete plugin;
        return nullptr;
    }
    if (format == PluginFormat::Clap) {
        auto* const plugin = new ClapSingletonPlugin();
        if (plugin->Load(path, sample_rate, max_frames)) {
            return plugin;
        }
        delete plugin;
        return nullptr;
    }
    Log("plugin: %s is a format this build cannot host yet", path != nullptr ? path : "(null)");
    return nullptr;
}

void PluginInstanceSetLogger(void (*logger)(const char*))
{
    g_logger = logger;
    PluginWindowSetLogger(logger);
}
