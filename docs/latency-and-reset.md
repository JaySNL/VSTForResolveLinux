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
| `+0x200` | `BMDAudioPluginImpl::GetLatency() const` | reads the cached `this+0x160`, as **int32**. This is what consumers call. Thunk at `+0x760`. |
| `+0x478` | `BMDAudioPlugin<T>::GetPluginLatency() const` | asks the plugin what its latency *is*, returns **int64** in RAX |
| `+0x490` | `BMDAudioPluginImpl::LatencyChanged(unsigned long)` | writes `this+0x160` under a recursive mutex, then calls a further virtual |

**Corrected 2026-08-30.** This table first said `GetLatency` was at `+0x120`. It is not, on this
class. `+0x120` carries `AudioPlugin::GetLatency` on `ADMRenderer` and `AIARenderer`, and
`BMDAudioPluginImpl::DragDropEvent` on `BMDStereoDelay`, which is the class this bridge rides.
Probing `+0x120` would have replaced drag-and-drop in a build handed to a tester. The mistake came
from reading `rmap where GetLatency`, which lists the offset per class, and taking the first row.
**Every offset here is now read from `rmap vtable BMDStereoDelay`**, which is the only listing that
is about this class.

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
- **`BMDChainFX::UpdateLatency()` calls the cached getter** and `LatencyChanged`. So the chain
  reads its children's cached values and republishes its own.
- The consumer is `DelayCompensation` in `libFairlightPage.so`:
  `CalculateEffectDelay(FL::ModuleIds, int) const`, `CalculateDownstreamDelay`,
  `DelayHandler::ActuateChange`, `ClearDelay`, `ClearAllDelays`.

So the chain is: **plugin knows → `OnIdle` polls `GetPluginLatency` → `LatencyChanged` writes the
cache → `GetLatency` serves it → `DelayCompensation` compensates.**

Resolve does full delay compensation. It is not missing the feature. It is being told zero.

### All three are already inside the vtable the bridge copies

`kVtableBytes = 0xab8` in `src/proxy.cpp`. `0x200`, `0x478` and `0x490` are all below it, so every
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

### Path 2 — override `GetLatency()` at `+0x200`

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

## What the probe measured — 2026-08-30, a tester's session

437 lines, one Resolve session with ten plugin instances, play / stop / locate.

| slot | calls | result |
|---|---|---|
| `RequiresHistoryReset` `+0x1c8` | 400 (hit the cap) | varies — genuinely decides |
| `ResetHistory` **thunk** `+0x6a0` | 16 | argument 0 fourteen times, 1 twice |
| `GetPluginLatency` `+0x478` | 10 | **`0`**, once per instance |
| `GetLatency` **thunk** `+0x760` | 10 | **`0`**, once per instance |
| `LatencyChanged` `+0x490` | **0** | never called |
| `CanResetInternally` `+0x800` | **0** | never called |
| `GetLatency` `+0x200`, `ResetHistory` `+0x1c0`, `RequiresHistoryReset` thunk `+0xa00` | **0** | never called |

### Resolve calls the thunks, not the primary slots

`GetLatency` arrived at `+0x760` and never at `+0x200`. `ResetHistory` arrived at `+0x6a0` and
never at `+0x1c0`. The `this` pointers confirm they are the same objects: every `GetLatency` line
is exactly `0x20` above its paired `GetPluginLatency` line, which is the thunk's `this`
adjustment.

**A fix must patch the thunk offsets.** Patching `+0x200` and `+0x1c0`, which is what the analysis
above would have led to, would have changed nothing at all and looked like a failed theory.

### The latency chain reaches us, and carries zero

Both getters are called, once per instance, in the first twenty lines of the session — project
load. Both return `0`. That is the desync in one number: Resolve asks, and is told the effect has
no latency.

### But nothing republishes

`LatencyChanged` never fired. It is the only thing that writes the cache at `this+0x160`, and
`GetLatency` reads that cache. So for our instances the cache is written by nobody and stays zero
for the life of the session.

This is the hole in Path 1. Answering `GetPluginLatency` honestly only helps if something asks
again after the hosted plugin is loaded and its latency is known.

### What this probe could not see, and it is my instrument's fault

The reporter logs the first twelve calls per slot and after that **only when a value changes**.
The latency answer never changed — it was `0` every time — so every call after the first twelve
was suppressed. The absence of latency lines after line 21 therefore means *"never returned
anything different"*, **not** *"never called again"*.

So the poll frequency is unmeasured, and it is the one number that decides Path 1. That is a defect
in the measurement, not a finding.

### The reset seam is live

`ResetHistory` reaches our instances sixteen times, in a burst at load and then again around the
locates. Resolve is already telling the effect to throw its history away. The bridge receives that
and does nothing with it, because no wrapper has a reset entry point.

This one needs no further measurement. The call arrives; it is simply dropped.

---

## Probe 2 — the second session, and the decision

998 lines, nine plugin instances. The change filter is gone from the latency slots and each line
now carries the plugin's own answer beside Resolve's.

```
GetPluginLatency (+0x478) call 2 -> 0   plugin says 6144
GetPluginLatency (+0x478) call 4 -> 0   plugin says 2048
GetPluginLatency (+0x478) call 5 -> 0   plugin says 720
GetPluginLatency (+0x478) call 9 -> 0   plugin says 1080
GetLatency (thunk) (+0x760) call 6 -> 0 plugin says 768
```

**The bug is those two numbers on one line.** The plugin knows it has 6144 samples of latency.
Resolve asks. Resolve is told nothing.

Reported values across the nine: 0, 0, 0, 0, 720, 768, 1024, 1080, 2048, 6144. All plausible sample
counts at 48 kHz, which settles the units question by consistency — 6144 is 128 blocks, 720 is
15 ms.

### Asked once per instance, after the plugin is ready

The call counter says nine calls for nine instances and no more. **It is not a poll.** So a single
correct answer at that one moment is all that is needed — and the ordering says the moment is late
enough: `vst3: "Acon Digital DeNoise 2" ready, 2 channels, 48000 Hz` appears *before* the first
`GetPluginLatency`, and the plugin answers with a real number when asked.

That is unknown 1, unknown 2 and unknown 3 all closed.

### Why `LatencyChanged` never fires, and why that is good news

It writes the cache. The cache was already zero and `GetPluginLatency` answered zero, so there was
nothing to change. The sequence in the log is Resolve's own propagation running correctly on empty
input: it polls every instance's `GetPluginLatency` (lines 189–380), then reads every instance's
cached `GetLatency` (lines 396–401).

So answering `GetPluginLatency` honestly should make Resolve's own machinery do the rest. That is a
prediction, not a measurement, and the first run of the fix tests it: `LatencyChanged` should start
firing, and `GetLatency` should start returning the plugin's number.

### The one thing this does not cover

Nothing re-asks after load. A plugin that changes its latency at runtime — an oversampling or
linear-phase switch mid-session — will not be noticed, because there is no second poll. Path 3
stays on the table for exactly that case, and it is a smaller problem than the one being fixed.

---

## Decision — Path 1, at the measured offset

Override **`GetPluginLatency` at `+0x478`** and return the hosted plugin's `LatencySamples()`,
clamped at zero. Everything downstream stays Resolve's own code doing its own job: the poll, the
cache, the mutex, the notification, `DelayCompensation`.

Path 2 is not needed — the cache is written by Resolve as long as the poll gets a real answer.
Path 3 is deferred to the runtime-change case. Path 4 stays rejected.

The reset fix is separate and needs no further measurement: `ResetHistory` arrives at the **thunk**
`+0x6a0`, twelve to sixteen times a session, and is dropped because no wrapper has a reset entry
point. VST3 `setProcessing(false)/(true)`, VST2 `effStopProcess`/`effStartProcess`, CLAP
`clap_plugin::reset()`.

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

3. **Is `GetLatency` at `+0x200` reached on our instance?** If Path 1's poll never happens but
   consumers still call the cached getter, that is the real seam.
   *Measure:* the same patch on `+0x200` and on its thunk at `+0x760`.

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
| `+0x1c0` | `BMDAudioPluginImpl::ResetHistory(bool)` — thunk at `+0x6a0` |
| `+0x1c8` | `BMDAudioPluginImpl::RequiresHistoryReset(long) const` — thunk at `+0xa00` |
| `+0x800` | `AudioPlugin::CanResetInternally() const` |

`CanResetInternally` was found while checking the others and is not in the original analysis. If
Resolve asks it before deciding whether to bother resetting, an effect of ours answering the stock
value may be the reason nothing arrives.

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

1. Patch nine slots to log and pass through to the stock implementation — the three latency ones,
   the three reset ones, `CanResetInternally`, and the non-virtual thunks of the pair that has
   them. Nothing returns a different value than it does today. **Built 2026-08-30**: see
   `BRIDGE_PROBE_THUNK` and `g_probe_slots` in `src/proxy.cpp`. The wrapping thunk is unit-tested
   in isolation — arguments through untouched, the original's return value handed back unchanged,
   and the stack alignment proven by forcing an SSE spill in both callees.
2. Load a plugin on a Fairlight track, play, stop, locate, and read the log. That answers unknowns
   1, 2 and 3, and names the caller of the reset slot.
3. Then pick: Path 3 if `LatencyChanged` proves safe to call, Path 1 if the poll turns out to reach
   us, and Path 2 only if neither does — with its dishonesty written down here rather than
   discovered later.
4. Add the latency getter and the reset entry point to all three format wrappers as one change,
   since neither exists yet in any of them.

Automation is a separate piece of work — four more slots (`+0x0f0`, `+0x108`, `+0x368`, `+0x370`)
and none of the groundwork above helps with it.

---

## After the fix — the gap at play start

The tester confirms both reports fixed: sync is correct with the track back at zero, and the stale
audio after a locate is gone. What remains is **a few hundred milliseconds of silence when playback
starts**, and it is absent from an export.

### The stock ResetHistory does what we do

`BMDAudioPluginImpl::ResetHistory(bool)` at `0x4edcb0`, disassembled:

```
movb $0x1, 0x168(%rbx)      ; always - the "history needs resetting" flag
test %bpl,%bpl              ; the bool
je   ...
movq $-1, 0x1d0(%rbx)       ; only when true - a position field invalidated
```

Two things follow. The flag at `+0x168` is set on **every** call regardless of the argument, so
gating our reset on that bool would change almost nothing. And the stock implementation **defers**
the work behind a flag rather than resetting inline — which is the same shape as the
`reset_pending` flag consumed at the top of the next `Process`. The design matches Resolve's own,
by measurement rather than by luck.

`RequiresHistoryReset(long)` being asked ~400 times a session is consistent with it reading that
flag and comparing `+0x1d0` against the position it is handed.

### So the gap has two candidates, and I do not know which

1. **Our reset.** A plugin with 6144 samples of lookahead genuinely has nothing to output for its
   first 6144 samples after a reset. That is 128 ms at 48 kHz, and a stack of them is "a few
   hundred". An export never locates, so it never resets — which fits the report exactly.
2. **Resolve's playback priming.** With correct compensation the host must run the effect ahead of
   the playhead, or the first N samples are silence no matter who reset what. Its own plugins have
   lookahead too, so if this were it, they would show the same gap.

The earlier reading blamed candidate 1 outright. The disassembly above is why that was premature:
the stock code sets the same flag on the same calls, so whatever it does about the gap, it is not
"never reset".

### The measurement that settles it

Split the environment switch so the reset can be turned off while the latency answer stays on. If
the gap survives `FXBRIDGE_RESET=0`, our reset is not the cause and the priming is Resolve's.
If it disappears, the reset is the cause and the question becomes what the stock plugins do
differently — most likely being primed by a host that runs them ahead.

Not built.
