// A VST2 loader behind the same surface as the CLAP one.
//
// VST2 covers a lot on this machine: native Linux VST2 plugins, and every Windows VST2 that
// yabridge exposes at ~/.vst/yabridge. It is also the cheapest proof that the loader interface
// takes a second format without the wrapper or the window hook changing.
#ifndef FXBRIDGE_VST2_HOST_H
#define FXBRIDGE_VST2_HOST_H

#include <cstdint>

bool Vst2HostLoad(const char* path, double sample_rate, uint32_t max_frames);
bool Vst2HostProcess(float** buffers, uint32_t channel_count, uint32_t frames);
uint32_t Vst2HostChannelCount();
const char* Vst2HostName();
bool Vst2HostOpenEditor();
void Vst2HostCloseEditor();
void Vst2HostSetLogger(void (*logger)(const char*));

#endif
