// Where a hosted plugin's settings live between two runs of Resolve.
//
// Resolve saves an effect and the carrier's parameters in the project, and nothing else. A hosted
// plugin's own settings are opaque to it, so a reopened project used to restore the right plugin
// at its defaults. This is the store that fixes that, and it is deliberately the SMALL fix: one
// file per plugin, holding whatever that plugin last reported.
//
// What it cannot do, stated plainly: two instances of the same plugin in one project share one
// file, so both come back with the settings of whichever one was saved last. The correct fix is
// per-instance and belongs in Resolve's own AudioPluginPreset - the object it hands to
// AudioPluginHost::AddPlaceholderPlugin when it restores a plugin. Whether Resolve calls
// StorePreset and LoadPreset on a project save is traced but not yet measured, which is why this
// store is off unless FXBRIDGE_STATE_STORE=1 is set.
#ifndef FXBRIDGE_PLUGIN_STATE_H
#define FXBRIDGE_PLUGIN_STATE_H

#include <cstdint>
#include <string>
#include <vector>

bool StateStoreEnabled();

// A stable file name for one plugin. The same path and class always produce the same name.
std::string StateStoreKey(const char* path, const char* class_name);

bool StateStoreRead(const std::string& key, std::vector<uint8_t>& out);
bool StateStoreWrite(const std::string& key, const std::vector<uint8_t>& bytes);

void StateStoreSetLogger(void (*logger)(const char*));

#endif
