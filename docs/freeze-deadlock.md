# The freeze is a lock order inversion in Resolve's own code

Measured on 2026-09-12 from a live `thread apply all bt` of a frozen Resolve
(468 threads, `02-backtrace.txt`, pid 16062, 43 minutes of uptime). This
replaces the two earlier guesses, both of which were wrong. They are kept at the
bottom, because a wrong reading that is deleted gets made again.

## The two mutexes

`libBMDAudioPlugins.so` puts two different `std::recursive_mutex` objects in one
audio plugin. Disassembly of the installed 21.1 library:

| Function | Locks | Then calls |
|---|---|---|
| `BMDChainFX::ResetHistory(bool)` @ `0x5de060` | `this+0x568` (`0x5de082`) | `BMDAudioPluginImpl::ResetHistory` (`0x5de126`) → `this+0x218` |
| `BMDAudioPluginImpl::PreProcess` @ `0x4f1e90` | `this+0x218` (`0x4f1eec`) | the child's virtual `Process` via `call *0x10(%rax)` (`0x4f1f97`) → `BMDChainFX::Process` → `this+0x568` (`0x5de4ba`) |

One path takes `0x568` and then `0x218`. The other takes `0x218` and then
`0x568`. Two threads that enter the two paths at the same time, on the same
object, block each other forever.

## The two threads in the capture

Both sit in `__lll_lock_wait`, which is a futex sleep.

```
Thread 1 "GUI Thread":
  #3  BMDAudioPluginImpl::ResetHistory(bool)        <- wants 0x218
  #4  BMDChainFX::ResetHistory(bool)                <- holds 0x568
  #6  EffectsController::ResetHistory(bool)
  #8  Previewer::UnPatch()
  #15 FLInterfaceImp::ClearAudioPreview()
  #30 QGraphicsScene::mouseMoveEvent(...)

Thread 450 "resolve-real":
  #3  BMDChainFX::Process(...)                      <- wants 0x568
  #5  BMDAudioPluginImpl::PreProcess(...)           <- holds 0x218
  #6  BMDChainFX::Process(...)
  #8  FxInst::Execute(...)
  #15 FLInterfaceImp::LoadFairlightAudioSamplesInternal(...)
```

Four threads wait on a mutex in total. The other two are
`AudioEngineController::GetRawMeterData` (the meter thread) and one unnamed GUI
thread. No thread runs the audio chain. Nothing recovers this.

## The trigger

A mouse drag on a timeline graphics scene. Qt delivers the move event, Resolve
turns it into `ClearAudioPreview`, and the previewer un-patches its chain while
the audio thread is inside a `Process` call. The Sep 5 crash dump shows the same
five return addresses (`0x431cfb7`, `0x2400e03`, `0x2425e25`, `0x239d4ec`,
`0x239e65e`) under the same `QGraphicsScene::mouseMoveEvent`, so both freezes
took one path.

## What the bridge has to do with it

The inversion is in Blackmagic's library. The bridge takes neither mutex, and
`libfxbridge.so` appears once in the whole dump: `fxb-xpump` asleep in
`nanosleep` inside `EventPump()`.

The bridge is still half of why it fires, and the reason is the span of the
`0x218` lock. `BMDAudioPluginImpl::PreProcess` locks `this+0x218` at `0x4f1f04`
and unlocks it at `0x4f22f7`. Between those two instructions it dispatches the
derived `Process` through the vtable (`call *0x10(%rax)` at `0x4f1f97`,
`call *0x40(%rax)` at `0x4f2291`). **The hosted plugin call runs inside the held
lock.** Verified by disassembly, not inferred.

`BMDChainFX::Process` does the same one level up: it locks `this+0x568` at
`0x5de4d5` and holds it across the one virtual call in that region,
`call *0x40(%rax)` at `0x5de56d`, which is the child `PreProcess` dispatch.

So while a hosted effect processes a buffer, the audio thread holds
`parent+0x568` **and** `effect+0x218`. Those are exactly the two mutexes that
`BMDChainFX::ResetHistory` wants, in the other order.

Every plugin in this capture is a Windows VST3 under yabridge, from a Bottles
prefix (`08-plugins(1).txt`). A `Process` call is therefore a socket round trip
to `yabridge-host.exe` under Wine, and the dump carries the yabridge threads to
match. A stock effect holds those two locks for microseconds. A yabridged one
holds them for a whole IPC round trip, every block, on every track that has one.

That turns a rare race into a handoff. The GUI thread reaches
`ClearAudioPreview`, parks on `C+0x568`, and is woken the moment the audio
thread releases it at the end of a block — which is the moment the audio thread
turns around and takes `C+0x218` for the next block. Both then wait forever.

What is still not measured: whether a project with no hosted effect, dragged on
the timeline for an hour, ever freezes. Resolve's defect does not need us. Our
lock span is what makes it reachable.

## What we can do

The bridge cannot remove the inversion, but it can stop standing inside it.

0. **Order the locks with a wrapper, and let recursion absorb the second take.**
   Both mutexes are `std::recursive_mutex`, so taking `this+0x218` once more
   than needed costs a counter increment and nothing else. Wrap
   `BMDChainFX::ResetHistory` so it reads:

   ```
   lock(this+0x218);        // added
   original_ResetHistory(this, flag);
   unlock(this+0x218);      // added
   ```

   Every path then takes `0x218` before `0x568`, on both threads, for the same
   object. The inversion is gone. This patch only ever *adds* a lock, so it
   cannot introduce a data race; the failure mode to rule out is a third path
   that takes `0x218` while holding `0x568`, which is the path being fixed.

   **Built.** `src/chain_lock_fix.cpp`, installed from `GetBMDPluginInterface`
   before any plugin loads, off with `FXBRIDGE_LOCKFIX=0`.
   `tools/lockfix-check.cpp` runs the same detection against an installed
   Resolve without starting it.

   Measured on 21.1.0.0014: the three offsets are read out of the instructions
   as `+0x218`, `+0x250` and `+0x568`, the thunk adjusts 32, and the wrapper
   lands in two primary vtable slots and one thunk slot. Resolve starts and
   runs with it. Not yet measured: a `ResetHistory` that actually travels
   through the wrapper, and a freeze that does not happen.

   This is the same kind of work as the 21.1 gate patcher, and it costs no
   latency, which makes it better than option 1 if it holds. Three things it
   had to settle, and does:

   - `0x218` and `0x568` are version specific. `tools/check-offsets.py` is the
     place that already proves an offset before use.
   - Both lock sites call `KriticalSection::SetOwner` right after the lock
     (`0x5de0ab`, `0x4f1f13`) and `ClearOwner` before the unlock. The wrapper
     touches neither, so the owner bookkeeping stays paired - unless something
     asserts "owner is me while the count is non-zero". Not observed; not
     checked.
   - `BMDChainFX::ResetHistory` descends into children at `0x5de107`
     (`call *0x50(%rax)`) while holding `this+0x568`, so a chain inside a chain
     gives nested frames. Walked through for the observed structure and it
     stays deadlock free, but that is analysis of one stack, not a measurement.

1. **Take the plugin call off the audio thread.** `Process` writes the input
   into a ring buffer, returns at once, and a worker thread does the yabridge
   round trip. `PreProcess` then holds `effect+0x218` for a memcpy instead of
   for an IPC round trip, and `BMDChainFX::Process` holds `parent+0x568` for
   the same short time. The cost is one block of latency, which the bridge
   already reports through the latency path. This is the only change on our
   side that touches the mechanism. Drafted in `docs/async-process.md`; it is a
   proposal, not a decision.
2. **Report it to Blackmagic** with the two symbols, the two offsets and the two
   stacks above. That is a complete bug report for whoever owns that file.
3. **Tell the user the trigger.** A drag on the timeline during playback with a
   hosted plugin live is the shape that freezes.

The v0.2.12 slow block watchdog stays useful as the instrument for step 1, but
it did not see this: it fired zero times in 134 KB of log, so no hosted block
passed 50 ms. A round trip well under 50 ms is still long enough, because the
collision needs a handoff, not a long stall.

## Two earlier readings that were wrong

**"A slow plugin holds the lock."** The watchdog logged no slow block at all.
The audio thread never reached our code; it blocked before the plugin call.

**"It is a spin, not a deadlock, because the process sits at 145%."** The `%CPU`
column of `ps` is total CPU time divided by elapsed time, over the whole life of
the process. In this capture it reads 201% over 43 minutes of uptime, which is
the average of a busy editing session, not the state at the freeze. Every
relevant thread is in a futex sleep at 0%. Read `/proc/<pid>/stat` twice and
subtract, or read nothing.
