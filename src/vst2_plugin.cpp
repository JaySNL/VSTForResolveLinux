// A VST2 plugin instance. One object per Fairlight effect, no shared state.

#include "plugin_instance.h"

#include "host_thread.h"
#include "plugin_window.h"
#include "vst2_abi.h"
#include "clap_plugin.h"
#include "vst3_plugin.h"

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

class Vst2Plugin final : public HostedPlugin, public HostMainClient {
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
        // Deregister under the host lock, in that order, so a tick cannot land on a destroyed
        // object. This runs after kEffEditClose, so the plugin is already done with its editor.
        {
            std::lock_guard<std::mutex> held(HostMainLock());
            HostMainUnregister(this);
        }
        // The window goes after the editor is closed, never before: a plugin still drawing into a
        // destroyed window faults inside its own toolkit.
        if (window_ != nullptr) {
            PluginWindowDestroy(window_);
            window_ = nullptr;
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

        // Take the plugin's own bus counts, not a guess. soothe2 declares 4 in and 2 out - two
        // main inputs and a sidechain pair - and a host that sizes the input array from the
        // OUTPUT count hands it two pointers where it will read four. That crashed Resolve on
        // 2026-08-25, inside a memcpy in yabridge, on the ALSA audio thread.
        in_count_ = effect_->numInputs > 0 ? static_cast<uint32_t>(effect_->numInputs) : 0;
        out_count_ = effect_->numOutputs > 0 ? static_cast<uint32_t>(effect_->numOutputs) : 0;

        channels_ = out_count_;
        if (channels_ > 2) {
            channels_ = 2;  // Resolve hands this effect a stereo pair
        }
        if (channels_ == 0) {
            Log("vst2: \"%s\" reports no outputs", name_);
            effect_ = nullptr;
            return false;
        }

        // One block per declared channel, inputs then outputs. The plugin works entirely inside
        // this, so neither side of it can reach Resolve's memory.
        scratch_.assign(static_cast<size_t>(in_count_ + out_count_) * max_frames_, 0.0f);
        in_ptrs_.assign(in_count_, nullptr);
        out_ptrs_.assign(out_count_, nullptr);
        Log("vst2: loaded \"%s\", %d in, %d out, %d parameters, %s editor",
            name_, effect_->numInputs, effect_->numOutputs, effect_->numParams,
            (effect_->flags & kEffFlagsHasEditor) != 0 ? "has an" : "no");
        return true;
    }

    bool Process(float** buffers, uint32_t channel_count, uint32_t frames) override
    {
        if (effect_ == nullptr || effect_->processReplacing == nullptr ||
            buffers == nullptr || frames == 0 || frames > max_frames_) {
            return false;
        }
        const uint32_t usable = channel_count < channels_ ? channel_count : channels_;
        if (usable == 0 ||
            scratch_.size() < static_cast<size_t>(in_count_ + out_count_) * max_frames_) {
            return false;
        }

        const size_t block = max_frames_;
        const size_t bytes = static_cast<size_t>(frames) * sizeof(float);

        // VST2 does not say which inputs are main and which are sidechain. The convention every
        // plugin follows is that the first pair is the programme, so Resolve's audio goes there
        // and every remaining input is fed silence. A sidechain fed a copy of the programme is
        // not neutral - it is a plugin listening to itself.
        const uint32_t main_in = in_count_ < 2 ? in_count_ : 2;
        for (uint32_t channel = 0; channel < in_count_; ++channel) {
            float* const slot = &scratch_[static_cast<size_t>(channel) * block];
            if (channel < main_in) {
                // Mono from Resolve feeds both main inputs, so a stereo plugin is not half fed.
                const uint32_t source = channel < usable ? channel : 0;
                std::memcpy(slot, buffers[source], bytes);
            } else {
                std::memset(slot, 0, bytes);
            }
            in_ptrs_[channel] = slot;
        }

        // processReplacing may not read and write the same buffer, so the outputs are separate.
        for (uint32_t channel = 0; channel < out_count_; ++channel) {
            float* const slot = &scratch_[static_cast<size_t>(in_count_ + channel) * block];
            std::memset(slot, 0, bytes);
            out_ptrs_[channel] = slot;
        }

        // A bridged plugin lives in another process, and yabridge reports its death by THROWING.
        // An exception unwinding out of an audio callback into Resolve's C code reaches
        // std::terminate, and the whole application aborts - which is how one dead Wine host took
        // Resolve down on 2026-08-25. One broken effect must go quiet, not end the session.
        try {
            effect_->processReplacing(effect_, in_ptrs_.data(), out_ptrs_.data(),
                                      static_cast<int32_t>(frames));
        } catch (...) {
            // Caught so it cannot reach std::terminate, but NOT latched off. A throw here is not
            // proof the plugin is finished, and silently disabling a working effect is worse than
            // the block it just lost. Logged once so the log does not fill at block rate.
            if (!threw_) {
                threw_ = true;
                Log("vst2: \"%s\" threw while processing - block dropped, still in the path",
                    name_);
            }
            return false;
        }

        // Only what Resolve actually gave us goes back. usable is capped by channels_, which is
        // capped by out_count_, so this cannot read past the outputs the plugin wrote.
        for (uint32_t channel = 0; channel < usable; ++channel) {
            std::memcpy(buffers[channel], out_ptrs_[channel], bytes);
        }
        return true;
    }

    uint32_t ChannelCount() const override { return effect_ != nullptr ? channels_ : 0; }
    const char* Name() const override { return name_; }
    long long LatencySamples() const override
    {
        // VST2 has no call for this. The plugin writes the number into its own struct and the
        // host reads it there, which is why the field has sat unused in vst2_abi.h until now.
        return effect_ != nullptr ? static_cast<long long>(effect_->initialDelay) : -1;
    }

    void Reset() override
    {
        if (effect_ == nullptr || effect_->dispatcher == nullptr) {
            return;
        }
        // VST2 has no reset call. Stopping and restarting the process run is what every host does
        // instead, and it is what the specification points a plugin at for clearing its buffers.
        effect_->dispatcher(effect_, kEffStopProcess, 0, 0, nullptr, 0.0f);
        effect_->dispatcher(effect_, kEffStartProcess, 0, 0, nullptr, 0.0f);
    }

    PluginFormat Format() const override { return PluginFormat::Vst2; }
    unsigned long EditorWindow() const override { return PluginWindowHandle(window_); }

    // Settings, the way VST2 defines them: one opaque blob when the plugin says it keeps one,
    // and otherwise the parameter values read out one at a time.
    //
    // effGetChunk hands back a pointer into the PLUGIN's own memory. It stays valid only until the
    // plugin is called again, so the bytes are copied here and now.
    bool SaveState(std::vector<uint8_t>& out) override
    {
        if (effect_ == nullptr) {
            return false;
        }
        if ((effect_->flags & kEffFlagsProgramChunks) != 0) {
            void* chunk = nullptr;
            const intptr_t size = Dispatch(kEffGetChunk, 0, 0, &chunk, 0.0f);
            if (chunk != nullptr && size > 0) {
                StateBegin(out, kStateTagVst2Chunk);
                const auto* const bytes = static_cast<const uint8_t*>(chunk);
                out.insert(out.end(), bytes, bytes + static_cast<size_t>(size));
                return true;
            }
        }
        if (effect_->numParams <= 0 || effect_->getParameter == nullptr) {
            return false;
        }
        StateBegin(out, kStateTagVst2Params);
        for (int32_t index = 0; index < effect_->numParams; ++index) {
            float value = 0.0f;
            try {
                value = effect_->getParameter(effect_, index);
            } catch (...) {
                return false;  // a dead Wine host throws; a half-written state is worse than none
            }
            const auto* const raw = reinterpret_cast<const uint8_t*>(&value);
            out.insert(out.end(), raw, raw + sizeof(value));
        }
        return true;
    }

    bool LoadState(const uint8_t* data, size_t size) override
    {
        if (effect_ == nullptr) {
            return false;
        }
        size_t body = 0;
        if (const uint8_t* const chunk = StateBody(data, size, kStateTagVst2Chunk, &body)) {
            Dispatch(kEffSetChunk, 0, static_cast<intptr_t>(body),
                     const_cast<uint8_t*>(chunk), 0.0f);
            return true;
        }
        if (const uint8_t* const values = StateBody(data, size, kStateTagVst2Params, &body)) {
            if (effect_->setParameter == nullptr) {
                return false;
            }
            const size_t count = body / sizeof(float);
            const size_t limit = effect_->numParams > 0
                                     ? static_cast<size_t>(effect_->numParams) : 0;
            for (size_t index = 0; index < count && index < limit; ++index) {
                float value = 0.0f;
                std::memcpy(&value, values + index * sizeof(float), sizeof(value));
                try {
                    effect_->setParameter(effect_, static_cast<int32_t>(index), value);
                } catch (...) {
                    return false;
                }
            }
            return true;
        }
        return false;
    }

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
            return PluginWindowShow(window_);
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

        window_ = PluginWindowCreate(width, height, name_);
        const unsigned long handle = PluginWindowHandle(window_);
        if (handle == 0) {
            return false;
        }
        if (Dispatch(kEffEditOpen, 0, 0, reinterpret_cast<void*>(handle), 0.0f) == 0) {
            // Some plugins answer 0 and open anyway, so this is reported, not treated as failure.
            Log("vst2: effEditOpen answered 0 for \"%s\"", name_);
        }
        PluginWindowFlush(window_);
        editor_open_ = true;

        // Without this the window draws once and then hears nothing.
        //
        // A VST2 editor pumps its own event handling from effEditIdle, and the host is the only
        // thing that calls it. A Windows plugin bridged by yabridge needs it doubly: that call
        // is what lets the Wine side process its message queue. The symptom is exact - the GUI
        // appears, and no knob responds.
        //
        // 20 ms is a normal editor idle rate. It goes on the shared host main thread, never a
        // thread of its own: one thread per plugin inside one library is the crash this bridge
        // already paid for once.
        HostMainRegister(this, 20);

        Log("vst2: editor open for \"%s\" at %ux%u", name_, width, height);
        return true;
    }

    void CloseEditor() override
    {
        std::lock_guard<std::mutex> held(HostMainLock());
        PluginWindowHide(window_);
    }

    // From the host main thread, with HostMainLock() already held.
    void OnHostMainTick() override
    {
        if (effect_ != nullptr && editor_open_) {
            Dispatch(kEffEditIdle, 0, 0, nullptr, 0.0f);
        }
    }

private:
    intptr_t Dispatch(int32_t opcode, int32_t index, intptr_t value, void* ptr, float opt)
    {
        if (effect_ == nullptr || effect_->dispatcher == nullptr) {
            return 0;
        }
        // Same reason as Process: a dead Wine host is reported by throwing, and an exception that
        // escapes into Resolve is an abort. Editor opens and closes travel this path too.
        try {
            return effect_->dispatcher(effect_, opcode, index, value, ptr, opt);
        } catch (...) {
            if (!threw_) {
                threw_ = true;
                Log("vst2: \"%s\" threw on opcode %d - call dropped, still in the path", name_,
                    opcode);
            }
            return 0;
        }
    }

    // Answering "I do not know" to an unknown opcode is correct. Claiming a capability we do not
    // implement is worse than saying no, because the plugin will then use it.
    static intptr_t HostCallback(AEffect* effect, int32_t opcode, int32_t index, intptr_t value,
                                 void* ptr, float opt)
    {
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
            // index is the width the plugin wants, value is the height. Answering 1 without
            // resizing is worse than answering 0: the plugin lays itself out for a size it never
            // got, so it draws over the old frame and hit-tests against coordinates that are not
            // on screen. Smooth Operator Pro showed both symptoms on 2026-08-25 - two stacked
            // copies of its UI, and not one control that could be clicked.
            case kAudioMasterSizeWindow:
                if (self != nullptr && self->window_ != nullptr && index > 0 && value > 0) {
                    PluginWindowResize(self->window_, static_cast<unsigned int>(index),
                                       static_cast<unsigned int>(value));
                    return 1;
                }
                return 0;
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
    // Only so the first throw is logged and the rest are not. It never disables the plugin.
    bool threw_ = false;
    uint32_t channels_ = 0;   // what Resolve is told, capped at a stereo pair
    uint32_t in_count_ = 0;   // what the plugin declares - never assume it matches out_count_
    uint32_t out_count_ = 0;
    std::vector<float*> in_ptrs_;
    std::vector<float*> out_ptrs_;
    bool editor_open_ = false;
    PluginWindow* window_ = nullptr;
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

HostedPlugin* CreateHostedPlugin(PluginFormat format, const char* path, const char* class_name,
                                 double sample_rate, uint32_t max_frames)
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
        return CreateClapPlugin(path, sample_rate, max_frames);
    }
    if (format == PluginFormat::Vst3) {
        return CreateVst3Plugin(path, class_name, sample_rate, max_frames);
    }
    Log("plugin: %s is a format this build cannot host yet", path != nullptr ? path : "(null)");
    return nullptr;
}

void PluginInstanceSetLogger(void (*logger)(const char*))
{
    g_logger = logger;
    PluginWindowSetLogger(logger);
    ClapPluginSetLogger(logger);
    Vst3PluginSetLogger(logger);
}
