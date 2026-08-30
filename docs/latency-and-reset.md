# Latency compensation, and the buffers that survive a locate

Analysis only. **Nothing here is built.** Two tester reports, one measured mechanism, four
candidate paths, and the measurements that would choose between them.

Dates and offsets are from 2026-08-30 against Resolve Studio 21.0.4.0005.

---

## The two reports

> Looks like latency is somehow not compensated or it is but with some wrong value. Placing some
> FX that stack latency will desync the video with audio.

> I notice when i play a section then i stop and go back or go into another (in the timeline) the
> first ms of audio is a slice of the latest sentence before the stop. Apparently not a problem
> while rendering because there is no start stop.

The second report carries its own diagnosis. A render never locates, so a bug that only appears
after a locate is a bug in what happens *at* the locate.

Naming: this is **PDC**, plugin delay compensation. It was called "TDC" in the conversation and
that term finds nothing in Resolve's symbols or anywhere else.

---

## What the bridge does today

Verified by search, not by assumption:

| | result |
|---|---|
| any code reading a hosted plugin's latency | **none** — the single `initialDelay` in `src/vst2_abi.h` is an unused struct field |
| any latency entry point on `Vst3Plugin` / `ClapPlugin` / `Vst2Plugin` | **none** |
| any reset, flush, or transport entry point on those wrappers | **none** |
| `setProcessing`, `effStartProcess`, `effStopProcess` anywhere in `src/` | **none** |
| automation | **none** — the word appears four times in `src/`, all four in comments |

So Resolve is told nothing about latency and the hosted plugin is told nothing about a locate.
Both reports follow from that directly, and neither is a wrong value: both are an absent one.

---

## The mechanism, as measured

The first guess was wrong and the measurement is the reason to write this down. `GetPluginLatency`
looked like the slot to override. It is not the slot consumers read.

There are **three** methods, and they are three different jobs:

| slot | symbol | what it does |
|---|---|---|
| `+0x120` | `AudioPlugin::GetLatency() const` | reads the cached `this+0x160`, as **int32**. This is what consumers call. |
| `+0x478` | `BMDAudioPlugin<T>::GetPluginLatency() const` | asks the plugin what its latency *is*, returns **int64** in RAX |
| `+0x490` | `BMDAudioPluginImpl::LatencyChanged(unsigned long)` | writes `this+0x160` under a recursive mutex, then calls a further virtual |

`GetPluginLatency` on `BMDStereoDelay` is four instructions:

```
mov 0x550(%rdi),%rax     ; this->dsp
mov 0x28(%rax),%rax      ; dsp->latency
ret
```

`BMDLimiter` returns `this+0x6e0` the same way. `BMDAudioPluginImpl::GetLatency` is
`mov 0x160(%rdi),%eax`. So the cache at `+0x160` is the currency, and `LatencyChanged` is the only
thing that writes it.

### Who calls what

- **`BMDChainFX::OnIdle()` is the only caller of `+0x478`** in `libBMDAudioPlugins.so`. It polls.
- **`BMDChainFX::UpdateLatency()` calls slot `+0x120`** — `GetLatency`, the cached one — and slot
  `+0x490`, `LatencyChanged`. So the chain reads its children's cached values and republishes its
  own.
- The consumer is `DelayCompensation` in `libFairlightPage.so`:
  `CalculateEffectDelay(FL::ModuleIds, int) const`, `CalculateDownstreamDelay`,
  `DelayHandler::ActuateChange`, `ClearDelay`, `ClearAllDelays`.

So the chain is: **plugin knows → `OnIdle` polls `GetPluginLatency` → `LatencyChanged` writes the
cache → `GetLatency` serves it → `DelayCompensation` compensates.**

Resolve does full delay compensation. It is not missing the feature. It is being told zero.

### All three are already inside the vtable the bridge copies

`kVtableBytes = 0xab8` in `src/proxy.cpp`. `0x120`, `0x478` and `0x490` are all below it, so every
candidate below is the same technique the bridge already uses for `StorePreset` (`+0x380`) and
`SetDirty` (`+0xa68`). No new machinery.

---

## The four paths

### Path 1 — override `GetPluginLatency()` at `+0x478`

Return the hosted plugin's latency and let Resolve's own polling do the rest.

**For:** one slot, one integer, and it is the honest answer to the question being asked. Everything
downstream — the cache, the mutex, the notification, the chain's own recomputation — stays
Resolve's code doing Resolve's job.

**Against:** it only works if something actually polls our instance. The only caller found is
`BMDChainFX::OnIdle()`. **Whether a track effect of ours is a child of a `ChainFX` is not
established.** If it is not, this path changes nothing at all.

### Path 2 — override `GetLatency()` at `+0x120`

Answer the question consumers actually ask, bypassing the cache.

**For:** cannot be missed. `DelayCompensation` and `UpdateLatency` both read this slot.

**Against:** it lies about `this+0x160`, which stays zero while the method returns something else.
Anything else in Resolve that reads that field directly then disagrees with the getter. Returns
int32 where the plugin-side value is int64, so it needs a clamp and a decision about what to do
with a plugin reporting more than 2^31 samples. This is the bandaid of the four: it makes the
symptom go away by making one accessor disagree with the state behind it.

### Path 3 — call `LatencyChanged()` at `+0x490` on ourselves

When the hosted plugin reports its latency — at load, and again whenever it changes — call
Resolve's own setter, exactly as a stock plugin does.

**For:** this is the path the stock plugins use. It takes the mutex, writes the cache, and fires
the onward notification, so every consumer sees a consistent value and nothing has to be
overridden at all. It also handles the case Path 1 cannot: a plugin whose latency changes at
runtime, which is normal on an oversampling or linear-phase switch.

**Against:** we call *into* Resolve rather than answering it, so the `this` pointer and the call
convention have to be right. The bridge's thunks already adjust `this` by `-0x20`, and that
adjustment is the part to get wrong. `LatencyChanged` also calls a further virtual (slot `0x28`,
symbol offset `+0x38`) which has not been read yet — calling it from the wrong thread may not be
safe, and it takes a `KriticalSection`.

### Path 4 — write `this+0x160` directly

**Rejected.** It skips the mutex and the onward notification. It is the shape of fix that this
document exists to avoid.

---

## What is not yet known

Each of these is one measurement, and each changes the answer.

1. **Is a track effect of ours inside a `BMDChainFX`?** Decides whether Path 1 works at all.
   *Measure:* patch `+0x478` to log and return the stock value, load a plugin on a track, and see
   whether the line ever appears. Instrumentation, not a fix — it returns exactly what it
   returned before.

2. **What are the units?** Samples is the obvious reading and every stock implementation returns a
   raw field, but no stock plugin was caught writing a known number.
   *Measure:* the same instrumentation on a stock plugin with known latency — `BMDLimiter` has
   lookahead — and compare against its block size and sample rate.

3. **Is `GetLatency` at `+0x120` reached on our instance?** If Path 1's poll never happens but
   consumers still call `+0x120`, that is the real seam.
   *Measure:* the same patch on `+0x120`.

4. **What is the virtual at slot `0x28` that `LatencyChanged` calls, and on what thread must it
   run?** Decides whether Path 3 is safe from the audio thread, the GUI thread, or neither.
   *Measure:* `rmap slot` on that offset, then read it.

5. **Does a re-query happen after `restartComponent(kLatencyChanged)`?** VST3 plugins announce a
   latency change through the component handler. The bridge's handler has not been read for this.

---

## The reset problem

Same file, different slots, and less is known.

| slot | symbol |
|---|---|
| `+0x1c0` | `BMDAudioPluginImpl::ResetHistory(bool)` |
| `+0x1c8` | `BMDAudioPluginImpl::RequiresHistoryReset(long) const` |

Resolve has a way to tell a plugin that the playhead jumped and its buffers are stale. The bridge
does not forward it, so delay lines, lookahead buffers and reverb tails survive a locate — and the
first block after the jump is the tail of wherever the playhead used to be. That is Frankie's
report word for word, and it is why a render is clean.

**Not checked:** who calls `+0x1c0`, what the `bool` selects, what the `long` argument to
`RequiresHistoryReset` is, and whether Resolve expects the plugin to answer `true` before it
bothers. The raw offset appears 484 times in `libFairlightPage.so`, which is meaningless on its
own — `+0x1c0` collides with unrelated classes, and the same collision made the first pass at the
latency callers useless.

On the plugin side the reset already has a standard shape in all three formats: VST3
`IAudioProcessor::setProcessing(false)` then `(true)`, VST2 `effStopProcess` / `effStartProcess`,
CLAP `clap_plugin::reset()`. None of the three is wired up.

---

## Proposal

**One instrumentation build, then a decision, then one change.**

1. Patch `+0x478`, `+0x120` and `+0x1c0` to log and pass through to the stock implementation.
   Nothing returns a different value than it does today.
2. Load a plugin on a Fairlight track, play, stop, locate, and read the log. That answers unknowns
   1, 2 and 3, and names the caller of the reset slot.
3. Then pick: Path 3 if `LatencyChanged` proves safe to call, Path 1 if the poll turns out to reach
   us, and Path 2 only if neither does — with its dishonesty written down here rather than
   discovered later.
4. Add the latency getter and the reset entry point to all three format wrappers as one change,
   since neither exists yet in any of them.

Automation is a separate piece of work — four more slots (`+0x0f0`, `+0x108`, `+0x368`, `+0x370`)
and none of the groundwork above helps with it.
