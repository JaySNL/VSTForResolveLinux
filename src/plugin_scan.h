// Finding the plugins that are installed, so Fairlight's effect menu lists them.
//
// The goal this serves: on Mac and Windows, Fairlight scans the system plugin folders and every
// VST it finds appears in the effect menu by name. Linux has no such scan, because Resolve's own
// VST hosts on this platform are stubs. This file does the scan; proxy.cpp turns each result into
// one menu entry, and picking an entry loads that plugin through the wrapper.
//
// Nothing is loaded here. The menu name is the file's own stem, because loading a plugin to ask
// its name would start a Wine process per Windows plugin at Resolve's startup - twenty-two of them
// on this machine.
#ifndef FXBRIDGE_PLUGIN_SCAN_H
#define FXBRIDGE_PLUGIN_SCAN_H

#include <string>
#include <vector>

#include "plugin_instance.h"

struct ScannedPlugin {
    std::string path;  // what CreateHostedPlugin is given: a .clap or .so file, or a .vst3 bundle
    std::string name;  // what the menu reads, made unique across the whole scan
    std::string key;   // "<name>:1112360057" - the effect id Resolve stores in the project
    // Which plugin inside the file, for a VST3 shell. Empty for everything else.
    std::string class_name;
    // What the plugin calls itself: a VST3 subcategory such as "Fx|EQ". Empty when the format or
    // the plugin does not publish one, and empty is not a failure - it means fall back to the name.
    std::string category;
    PluginFormat format;
};

// Scans once and caches. Safe to call from anywhere after the library is loaded.
const std::vector<ScannedPlugin>& ScannedPlugins();

void PluginScanSetLogger(void (*logger)(const char*));

// Called once per module as the scan opens it, so a caller with a terminal can draw progress
// instead of leaving a person watching nothing. `done` counts modules finished, `total` is how
// many have to be opened this run, and `path` is the one that just answered. Both counts are
// zero-based on entry: the first call is (0, total, nullptr), before anything is opened.
void PluginScanSetProgress(void (*progress)(int done, int total, const char* path));

// The file naming the modules that are open right now. A caller that is stopped on purpose - by a
// person pressing Ctrl-C rather than by a plugin faulting - deletes it, so nothing gets the blame
// for a run that was ended deliberately. Never null; empty when HOME is not set.
const char* PluginScanInFlightPath();

#endif
