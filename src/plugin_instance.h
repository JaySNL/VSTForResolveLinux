// One hosted plugin, owned by one Fairlight effect.
//
// The goal this serves: VST2 and VST3 plugins are scanned and listed in Fairlight's effect menu the
// way they are on Mac and Windows, and picking one loads that plugin inside Resolve through this
// wrapper and the shared editor hook.
//
// That requires per-instance hosting, and it is also the fix for the fault that dogged the first
// evening. The audio path used to hold one global dry buffer, two global counters and one shared
// plugin, while Resolve renders audio on several threads: two effects meant two threads writing
// each other's state and re-entering a plugin that is not re-entrant. Every crash inside Resolve
// resolved to the same frame, BridgeAfterProcess, and it only appeared once a project carried more
// than one effect.
//
// So: no globals in the audio path. Each effect owns its plugin, its buffers and its window state,
// and a second effect is simply a second object.
#ifndef FXBRIDGE_PLUGIN_INSTANCE_H
#define FXBRIDGE_PLUGIN_INSTANCE_H

#include <cstdint>
#include <cstring>
#include <vector>

enum class PluginFormat { Unknown, Clap, Vst2, Vst3 };

class HostedPlugin {
public:
    virtual ~HostedPlugin() = default;

    // Processes in place over Resolve's buffers. Returns false when the block was left untouched.
    virtual bool Process(float** buffers, uint32_t channel_count, uint32_t frames) = 0;

    // Zero means this instance cannot process, and the effect passes audio through.
    virtual uint32_t ChannelCount() const = 0;

    virtual const char* Name() const = 0;

    // The plugin's own window. Opening twice remaps rather than building a second one.
    virtual bool OpenEditor() = 0;
    virtual void CloseEditor() = 0;

    // The X11 id of that window, or zero when there is none. The window pump knows which
    // window the user closed but not which effect owns it, and this is how it finds out.
    virtual unsigned long EditorWindow() const { return 0; }

    // The plugin's own settings, serialised by the plugin itself.
    //
    // Resolve saves the effect and the carrier's parameters in the project. It knows nothing
    // about what a hosted plugin keeps inside, so a reopened project restored the right
    // plugin at its defaults - reported by Delirio on 2026-08-27.
    //
    // A format that cannot produce a blob returns false rather than an empty one. Empty is a
    // legitimate state for a plugin with nothing to say, and restoring it must stay possible.
    virtual bool SaveState(std::vector<uint8_t>& out) { (void)out; return false; }
    virtual bool LoadState(const uint8_t* data, size_t size)
    {
        (void)data;
        (void)size;
        return false;
    }

    virtual PluginFormat Format() const = 0;
};

// Eight bytes of magic, then four naming the format that wrote the payload.
//
// The tag is not decoration. A VST3 component state handed to a VST2 plugin's setChunk is not
// a restore that fails, it is a plugin parsing a foreign buffer as its own - and that is a
// crash inside Resolve with our name on it.
constexpr size_t kStateHeaderSize = 12;
constexpr char kStateMagic[8] = {'F', 'X', 'B', 'S', 'T', 'A', 'T', '1'};
constexpr const char* kStateTagVst2Chunk = "V2CK";
constexpr const char* kStateTagVst2Params = "V2PM";
constexpr const char* kStateTagVst3 = "V3CS";      // component only, written before 0.2.1
constexpr const char* kStateTagVst3Both = "V3C2";  // component and controller
constexpr const char* kStateTagClap = "CLST";

inline void StateBegin(std::vector<uint8_t>& out, const char* tag)
{
    out.clear();
    out.insert(out.end(), kStateMagic, kStateMagic + sizeof(kStateMagic));
    out.insert(out.end(), tag, tag + 4);
}

// The payload, or null when the blob is not ours or was written by a different format.
inline const uint8_t* StateBody(const uint8_t* data, size_t size, const char* tag, size_t* body)
{
    if (data == nullptr || body == nullptr || size < kStateHeaderSize) {
        return nullptr;
    }
    if (std::memcmp(data, kStateMagic, sizeof(kStateMagic)) != 0) {
        return nullptr;
    }
    if (std::memcmp(data + sizeof(kStateMagic), tag, 4) != 0) {
        return nullptr;
    }
    *body = size - kStateHeaderSize;
    return data + kStateHeaderSize;
}

// Builds one, or returns null with the reason logged. `path` is what the scanner recorded.
//
// `class_name` is used by VST3 only, and only for a shell - one file that publishes many plugins,
// the way the Waves WaveShell publishes 718. Null or empty means "the first class in the file",
// which is what every ordinary plugin wants.
HostedPlugin* CreateHostedPlugin(PluginFormat format, const char* path, const char* class_name,
                                 double sample_rate, uint32_t max_frames);

void PluginInstanceSetLogger(void (*logger)(const char*));

// Tell Resolve that the plugin's own editor moved a parameter.
//
// An experiment, off unless FXBRIDGE_NOTIFY_PARAM=1. Resolve holds the parameter values it
// saves in a project and never asks an effect for them - GetParameterValue fires zero times
// on a Ctrl+S. The question this answers is whether a notification makes it come and read.
// If it does, settings belong in the project instead of in a file on a timer.
void BridgeParameterChangedByEditor(HostedPlugin* plugin, unsigned int index);

// What a file extension says the format is. Nothing is loaded to find out.
PluginFormat FormatFromPath(const char* path);

#endif
