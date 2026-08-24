// One CLAP plugin per effect. See clap_plugin.cpp for why this replaced clap_host.cpp.
#ifndef FXBRIDGE_CLAP_PLUGIN_H
#define FXBRIDGE_CLAP_PLUGIN_H

#include "plugin_instance.h"

// Loads one plugin, or returns null with the reason logged.
HostedPlugin* CreateClapPlugin(const char* path, double sample_rate, uint32_t max_frames);

void ClapPluginSetLogger(void (*logger)(const char*));

#endif
