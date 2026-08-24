// A second host behind the same surface as the CLAP one.
//
// Carla loads VST2, VST3, LV2, CLAP and AU itself, and a Windows plugin bridged by yabridge
// presents as a native VST2 or VST3 - so hosting Carla is how this bridge reaches every format
// without writing a host per format. Its Rack is a serial chain and its Patchbay is arbitrary
// routing, which is the patcher.
//
// Nothing here is linked against Carla. The library is opened at run time, so a machine without
// Carla loses this host and keeps the rest.
#ifndef FXBRIDGE_CARLA_HOST_H
#define FXBRIDGE_CARLA_HOST_H

#include <cstdint>

// Opens Carla and instantiates the rack or the patchbay. Returns false if Carla is not installed.
bool CarlaHostLoad(double sample_rate, uint32_t max_frames);

// Runs the chain over Resolve's buffers, in place.
bool CarlaHostProcess(float** buffers, uint32_t channel_count, uint32_t frames);

uint32_t CarlaHostChannelCount();
const char* CarlaHostName();

// Carla's own window - the rack or the patchbay, where plugins are added and wired.
bool CarlaHostOpenEditor();
void CarlaHostCloseEditor();

void CarlaHostSetLogger(void (*logger)(const char*));

#endif
