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

    virtual PluginFormat Format() const = 0;
};

// Builds one, or returns null with the reason logged. `path` is what the scanner recorded.
HostedPlugin* CreateHostedPlugin(PluginFormat format, const char* path, double sample_rate,
                                 uint32_t max_frames);

void PluginInstanceSetLogger(void (*logger)(const char*));

// What a file extension says the format is. Nothing is loaded to find out.
PluginFormat FormatFromPath(const char* path);

#endif
