// One VST3 plugin per effect, on travesty's clean-room ABI. See vst3_plugin.cpp.
#ifndef FXBRIDGE_VST3_PLUGIN_H
#define FXBRIDGE_VST3_PLUGIN_H

#include "plugin_instance.h"

// `path` is the .vst3 bundle directory the scanner recorded, or a flat .vst3 file.
HostedPlugin* CreateVst3Plugin(const char* path, double sample_rate, uint32_t max_frames);

void Vst3PluginSetLogger(void (*logger)(const char*));

#endif
