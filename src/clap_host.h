// A minimal CLAP host, sized for one effect plugin running inside Resolve's audio callback.
#pragma once

#include <cstdint>

// Loads the plugin and prepares it for processing. Call from the main thread, once.
// `path` is a .clap file. Returns false and logs the reason on any failure.
bool ClapHostLoad(const char* path, double sample_rate, uint32_t max_frames);

// Runs the loaded plugin over `channels` in place. Call from the audio thread only.
// Returns false when the host is not ready or the request does not match the plugin's ports.
bool ClapHostProcess(float** channels, uint32_t channel_count, uint32_t frames);

// Name of the loaded plugin, or nullptr when nothing is loaded.
const char* ClapHostName();

// Number of channels the plugin expects, or 0 when nothing is loaded.
uint32_t ClapHostChannelCount();

// Parameters of the loaded plugin.
uint32_t ClapHostParameterCount();

// Writes the plugin's parameter list to the log, with ranges. Main thread.
void ClapHostLogParameters();

// Queues a parameter change for the next process() call. Safe to call from any thread; the value
// is delivered as a proper CLAP event, which is the only correct way to drive a parameter.
void ClapHostQueueParameter(uint32_t index, double plain_value);

// Maps a normalised 0..1 position onto parameter `index` and queues it.
void ClapHostQueueParameterNormalised(uint32_t index, double position);

// Opens the plugin's own editor window, or raises it when it already exists. Main thread only.
bool ClapHostOpenEditor();

// Hides the editor window. The GUI itself stays alive, so reopening is instant. Main thread only.
void ClapHostCloseEditor();

// Set by the bridge so this file does not depend on the bridge's logger.
void ClapHostSetLogger(void (*logger)(const char*));
