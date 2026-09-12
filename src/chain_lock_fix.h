// Repairs a lock order inversion inside Resolve's own audio plugin library.
//
// The whole story, with the stacks that produced it, is docs/freeze-deadlock.md. The short form:
// BMDChainFX::ResetHistory takes the chain mutex and then the history mutex, while
// BMDAudioPluginImpl::PreProcess takes the history mutex and then the chain mutex, on the same
// object. Two threads in those two functions at the same time wait for each other forever, and
// the Fairlight page is gone for good - no timeout, nothing to recover.
//
// Both are std::recursive_mutex, so taking the history mutex once more than needed costs a
// counter increment. That is the fix: wrap BMDChainFX::ResetHistory so the history mutex is
// taken first, and every path on both threads then takes the two locks in the same order.
//
// This only ever ADDS a lock. It cannot introduce a data race, and it costs no latency.
#ifndef FXBRIDGE_CHAIN_LOCK_FIX_H
#define FXBRIDGE_CHAIN_LOCK_FIX_H

// Installs the wrapper, or logs why it did not and leaves Resolve untouched.
//
// `handle` is the dlopen handle for libBMDAudioPlugins.so. Every offset this needs is read out of
// that library's own instructions rather than written down here, because an offset typed from
// memory passes the eye and fails the machine - see tools/check-offsets.py for the same rule.
//
// Off when FXBRIDGE_LOCKFIX=0. Returns true only when the wrapper is installed.
bool InstallChainLockFix(void* handle, void (*log)(const char*, ...));

#endif
