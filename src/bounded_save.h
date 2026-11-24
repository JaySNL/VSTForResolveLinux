// A hosted plugin's settings, asked for on the host main thread and waited for a bounded time.
//
// Resolve asks every effect for its settings on its own GUI thread when it saves the project
// (StorePreset), and the bridge used to call straight into the plugin from there. A plugin that
// never answered then froze Resolve for good. Seen on 2026-11-24 on Resolve 21.2: a second
// smartEQ4 (VST2 through yabridge) was asked for its chunk within a second of its editor opening,
// never answered, and Resolve's GUI thread sat in yabridge's recv() until it was killed. Its Wine
// host had taken the request and stopped pumping its own window events. Not reproduced since.
//
// Two things change here.
//
// The call runs on the host main thread, under HostMainLock. That is the one thread VST2, VST3 and
// CLAP all accept for a state call, and it is the thread that also drives the plugin's editor, so
// a save can no longer land in the middle of editor work. The old call broke host_thread.h's own
// rule - every GUI-side call from Resolve's thread takes HostMainLock - and overlapped exactly
// that work in the hang above.
//
// And Resolve's thread waits only so long. Past the limit it is told so and falls back to the
// settings it saw last. The call it gave up on stays where it is: nothing can cancel a call that is
// inside a plugin. Until that call comes back, the host main thread is still in it, so every later
// save is answered at once instead of each one costing the full wait again.
#ifndef FXBRIDGE_BOUNDED_SAVE_H
#define FXBRIDGE_BOUNDED_SAVE_H

#include <chrono>
#include <cstdint>
#include <vector>

class HostedPlugin;

enum class BoundedSave {
    Saved,     // out holds what the plugin reported
    Refused,   // the plugin answered, and had nothing to report
    TimedOut,  // no answer within the limit; the call is still pending on the host main thread
    Stalled,   // an earlier call has not come back yet, so this one was never queued
};

// Blocks for at most `limit`. On the host main thread itself it calls the plugin directly, since
// that thread cannot wait for itself.
BoundedSave SaveStateBounded(HostedPlugin* plugin, std::vector<uint8_t>& out,
                             std::chrono::milliseconds limit);

const char* BoundedSaveName(BoundedSave result);

#endif  // FXBRIDGE_BOUNDED_SAVE_H
