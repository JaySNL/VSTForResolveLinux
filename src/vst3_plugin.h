// One VST3 plugin per effect, on travesty's clean-room ABI. See vst3_plugin.cpp.
#ifndef FXBRIDGE_VST3_PLUGIN_H
#define FXBRIDGE_VST3_PLUGIN_H

#include <string>
#include <vector>

#include "plugin_instance.h"

// `path` is the .vst3 bundle directory the scanner recorded, or a flat .vst3 file.
//
// `class_name` picks which Audio Module class inside the file to load, and matters only for a shell
// - one file that publishes many plugins. The Waves WaveShell publishes 718. An empty or null name
// keeps the old behaviour and takes the first class, which is right for an ordinary plugin.
HostedPlugin* CreateVst3Plugin(const char* path, const char* class_name, double sample_rate,
                               uint32_t max_frames);

// Lists the Audio Module classes a VST3 file publishes, in factory order.
//
// Nothing is instantiated: this is num_classes plus get_class_info, and it costs what the dlopen
// costs. Measured on the WaveShell, all 718 classes came back in 1.674 s, of which the enumeration
// itself was 0 ms - the whole bill is Wine starting behind yabridge. Carla needs fifteen minutes on
// the same file because it *creates* every plugin to report its bus counts. We do not.
// Lists the audio classes in a VST3 module. When sub_categories is given it is filled in step with
// out: each entry is the plugin's own subcategory string, "Fx|EQ" or "Fx|Restoration", or empty
// when the factory does not publish one. That string is what lets a plugin be filed correctly on a
// machine whose plugins nobody hardcoded a rule for.
bool Vst3ListClasses(const char* path, std::vector<std::string>& out,
                     std::vector<std::string>* sub_categories = nullptr);

void Vst3PluginSetLogger(void (*logger)(const char*));

#endif
