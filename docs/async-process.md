# Draft: take the hosted call off Resolve's audio thread

Status: **proposal, nothing built.** The measurement it answers is in
`docs/freeze-deadlock.md`.

## Why

`BMDAudioPluginImpl::PreProcess` locks `this+0x218` at `0x4f1f04` and unlocks it
at `0x4f22f7`. Our `Process` runs between those two instructions, through the
vtable dispatch at `0x4f1f97`. One level up, `BMDChainFX::Process` locks
`this+0x568` at `0x5de4d5` and holds it across the child dispatch at `0x5de56d`.

So for as long as a hosted plugin processes a buffer, Resolve's audio thread
holds both of the mutexes that `BMDChainFX::ResetHistory` takes in the opposite
order. With every plugin bridged through yabridge, that is a Wine socket round
trip per block, per effect, and the deadlock stops being a race and becomes a
handoff.

The bridge cannot fix Blackmagic's lock order. It can stop standing inside it.
A memcpy holds those mutexes for microseconds. An IPC round trip holds them for
milliseconds. That ratio is the whole change.

## The shape

One worker thread per effect, and a two-slot handoff. Not a deep ring: a deeper
queue buys nothing here and costs latency per slot. Two slots give exactly one
block of delay, and that number never drifts.

```
Resolve's audio thread                worker thread
----------------------                -------------
copy input  -> slot[w]
publish slot[w]                 ->    wake, run plugin->Process over slot[w]
take slot[r] (previous block)   <-    publish slot[w] as done
copy slot[r] -> Resolve's buffers
swap r/w, return
```

The audio thread never waits on the worker. When the previous block is not done,
it emits the dry signal for that block and counts an underrun. A late block is
one block of dry audio; a waited-for block is the freeze we are removing.

## What the audio thread does

The copy-in already exists and does not change: the plugin has never seen
Resolve's buffers, because a plugin may write past the frames it was given
(`src/proxy.cpp:2028-2059`, the `MixPole::ApplyGainSmooth` crash). The scratch it
writes into becomes slot-owned instead of effect-owned, and the overrun marker
comes with it.

```cpp
// One block in flight, handed between the audio thread and the worker.
struct Slot {
    std::vector<float> audio;          // kMaxChannels * kMaxFrames
    unsigned long frames = 0;
    unsigned int channels = 0;         // what the plugin was asked to see
    unsigned int wanted = 0;           // what Resolve actually provided
    bool reset_first = false;          // the locate, carried in queue order
    bool processed = false;            // what Process returned
};

struct AsyncHost {
    Slot slot[2];
    std::atomic<int> submitted{-1};    // index the worker may take, or -1
    std::atomic<int> finished{-1};     // index the audio thread may take, or -1
    std::atomic<bool> running{true};
    std::binary_semaphore work{0};     // C++20; a futex or a condvar does as well
    std::thread thread;
};
```

`submitted` and `finished` are single-producer, single-consumer, one index each.
No lock is taken on the audio thread, and no allocation happens there: both
slots are sized at claim time, exactly as `spare` is today.

## The three things that must stay right

**The locate must not overtake the audio.** `reset_pending` is consumed today at
the top of `Process` (`src/proxy.cpp:2062`). It moves into `Slot::reset_first`,
so the worker runs `plugin->Reset()` immediately before the block that follows
the locate, and never while a `Process` is in flight. That is what
`HostedPlugin::Reset` already promises, and the async path keeps the promise
more strictly than the current one does.

**The block geometry may change.** Resolve can hand us a different `frames` or a
different channel count on the next call. The queued block then belongs to a
geometry that no longer applies. Drop it, emit dry for one block, restart the
handoff, and log it once. Rare, and the alternative is audio of the wrong
length in the wrong buffer.

**One producer only.** The current code assumes Resolve does not call `Process`
for one effect on two threads at once; the async handoff assumes it harder. Take
an `std::atomic_flag` at the top of the audio path and log loudly if it is
already set. An assumption that is checked costs one atomic per block.

## What it costs

**One block of latency, uncompensated.** At 48 kHz and 512 frames that is
10.7 ms. The bridge reads a plugin's latency today but does not report it
(`src/plugin_instance.h`, `LatencySamples`, and `docs/latency-and-reset.md`), so
this block is added to a number that is already wrong. It is a real cost and it
is Jay's call whether it is the smaller one.

**One thread per effect.** Frankie's session carries sixteen. Sixteen threads
asleep on a semaphore are not a problem; sixteen threads spinning would be, so
the worker blocks and never polls.

**A new teardown order.** The worker must be stopped and joined before the
plugin is destroyed. The claim path already owns the plugin's lifetime, so this
is one more step in the same place, but it is the step that turns a mistake into
a use-after-free on the audio thread.

## What it does not fix

The inversion stays in `libBMDAudioPlugins.so`. A stock Fairlight effect still
holds both mutexes across its own processing, and a slow enough native effect on
a slow enough machine can still collide. This makes our contribution
microseconds instead of milliseconds. It does not make Resolve correct.

## Rollout

1. Build it behind `FXBRIDGE_ASYNC=1`, default off. The synchronous path stays
   the shipped one until there is evidence.
2. Give Frankie the build. The test is his own freeze: the same project, the
   same drag on the timeline, with the flag on.
3. Keep the v0.2.12 watchdog. On the async path it measures the worker, where a
   slow block costs an underrun instead of a deadlock, which is what we want it
   to be measuring.
4. Flip the default only after a session that used to freeze does not.
