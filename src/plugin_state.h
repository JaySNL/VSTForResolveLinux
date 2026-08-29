// Where a hosted plugin's settings live between two runs of Resolve.
//
// Resolve saves an effect and the carrier's parameters in the project, and nothing else. A hosted
// plugin's own settings are opaque to it, so a reopened project used to restore the right plugin
// at its defaults. This is the store that fixes that, and it is deliberately the SMALL fix: one
// file per plugin, holding whatever that plugin last reported.
//
// This is no longer the main path. Resolve's own AudioPluginPreset carries a hosted plugin's
// settings now - see the preset section of proxy.cpp - and that path has real per-instance
// identity, which this one does not: instances are told apart by their position in the claim
// order, so rearranging a chain gives them each other's settings.
//
// What this store still buys is the one case the project cannot cover. Resolve serialises the
// effects model only when the project is modified, and a change made inside a hosted plugin's own
// window is invisible to it - measured on 2026-08-29: a plugin-only edit followed by Ctrl+S
// produced zero StorePreset calls, while a fader on an unrelated track produced eight. This store
// writes on its own timer and does not care what Resolve thinks. The two are reconciled by time:
// each side carries a stamp, and the newer one wins.
#ifndef FXBRIDGE_PLUGIN_STATE_H
#define FXBRIDGE_PLUGIN_STATE_H

#include <cstdint>
#include <string>
#include <vector>

bool StateStoreEnabled();

// A stable file name for one plugin. The same path and class always produce the same name.
std::string StateStoreKey(const char* path, const char* class_name);

bool StateStoreRead(const std::string& key, std::vector<uint8_t>& out);

// When that file was last written, in seconds since the epoch. Answers false when there is no
// file. Used to decide whether the project or this store holds the newer settings.
bool StateStoreStamp(const std::string& key, uint64_t* seconds);
bool StateStoreWrite(const std::string& key, const std::vector<uint8_t>& bytes);

void StateStoreSetLogger(void (*logger)(const char*));

#endif
