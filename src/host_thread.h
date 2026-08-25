// The host's one main thread, shared by every loader.
//
// CLAP's "main thread" is singular and host-wide, and VST2's effEditIdle wants the same
// treatment: a plugin's GUI toolkit is process-global, so two threads inside one plugin
// library is a crash waiting for a second effect. That crash was paid for on 2026-08-25 -
// two pp-track instances, two per-plugin timer threads, the fault four frames inside the
// plugin's own GUI code.
//
// So there is exactly one thread here, and one lock. A loader registers for ticks and takes
// the same lock around every GUI call it makes from Resolve's thread.
#ifndef FXBRIDGE_HOST_THREAD_H
#define FXBRIDGE_HOST_THREAD_H

#include <mutex>

// Anything that needs a periodic call on the host's main thread.
class HostMainClient {
public:
    virtual ~HostMainClient() = default;

    // Called on the host main thread with HostMainLock() already held. Keep it short: every
    // registered client is ticked in the same pass.
    virtual void OnHostMainTick() = 0;
};

// Registering twice is a no-op. The shortest period any client asks for sets the rate.
void HostMainRegister(HostMainClient* client, unsigned int period_ms);

// Safe to call from inside OnHostMainTick: nothing is joined, only the list is edited.
void HostMainUnregister(HostMainClient* client);

// Stops the thread and joins it. Called for you when the library goes away; exposed because a
// loader may want the ticks to stop before it tears something down that a tick can reach.
//
// Nothing restarts after this except a fresh HostMainRegister.
void HostMainStop();

// True only on the host main thread. A loader's thread check must answer with this, not with
// "no" - reporting the tick thread as anything else tells a plugin it is on the wrong thread
// for the GUI work it is doing right then.
bool HostMainIsCurrentThread();

// Held by whoever is doing main-thread work. Never taken on the audio thread.
//
// Lock order for any code that touches both: this lock first, then any loader-private lock.
std::mutex& HostMainLock();

void HostThreadSetLogger(void (*logger)(const char*));

#endif
