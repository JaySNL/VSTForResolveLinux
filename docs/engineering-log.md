# Fairlight FX bridge (DaVinci Resolve Studio 21, Linux)

> **This is a chronological engineering log, not documentation.**
>
> Each entry was true on the day it was written, and several were later overtaken by work further
> down. Sections that no longer describe the code carry a **SUPERSEDED** note. For what the project
> actually does today, read [`../README.md`](../README.md).
>
> The dead ends are deliberately kept. They are most of the value here.


Goal: run our own audio effects **inside** Resolve's Fairlight page, tunable live on the timeline,
instead of bouncing a clip out to an external process.

Everything below was measured on this machine on 2026-08-24 against DaVinci Resolve Studio 21
(`/usr/bin/davinci-resolve-studio -> /opt/resolve/bin/resolve`). Nothing here is documented by
Blackmagic; it is read out of the shipped binaries.

## The seam

Resolve on Linux ships no VST, AU, LV2, LADSPA or CLAP support. It does ship a private plugin ABI,
and it loads its own Fairlight FX through it.

`FLPluginHost::Initialize()` in `/opt/resolve/libs/libFairlightPage.so` (at `0xc2d570`) does this:

1. Builds the key string `BMDPlugins.Path` on the stack with two overlapping `movabs` stores
   (`BMDPlug` + `ins.Path`), which is why the key has no string cross-reference.
2. Reads that key from the settings object. On success it converts the value with `flx::CA2W` and
   calls `flx::DLLLoader::Load(wchar_t const*)` on that path.
3. **Only when that fails** does it fall back to `flx::DLLLoader::LoadApplicationDLL("BMDAudioPlugins")`,
   which resolves to `/opt/resolve/bin/../libs/libBMDAudioPlugins.so`.
4. Resolves the unmangled entry point `GetBMDPluginInterface` and calls it with **no arguments**.
5. Calls vtable slot 0 on the returned pointer and compares the result with `100`. A different value
   aborts the whole audio-plugin setup.
6. Stores the interface on `BMDAudioPluginHost::Instance()` and passes it to
   `FLPluginUIController::Initialize(bmd::PluginInterface*)`.

**The override is exclusive.** When `BMDPlugins.Path` points at our library, the stock library is not
loaded at all (verified: `/proc/<pid>/maps` contains no `libBMDAudioPlugins.so`). So the bridge must
load the stock library itself and forward.

### The key goes in the Fairlight store, not the global one

```
~/.local/share/DaVinciResolve/configs/config-fairlight.dat
BMDPlugins.Path = /home/jooshua/.local/share/BMDAudioPlugins/libfxbridge.so
```

Putting it in `configs/config.dat` does nothing. Resolve keeps the line there across restarts and
ignores it. That was tested both ways; only the Fairlight store takes effect.

## The ABI

`GetBMDPluginInterface()` takes no arguments and returns a `bmd::PluginInterface*`.

The vtable is `vtable for BMDPluginInterfaceImpl` (exported, `0xcb4100` in `libBMDAudioPlugins.so`).
It lives in RELRO, so the file image is zeroed — read the **relocations**, not the bytes. Function
pointers start at `+0x10`. Six virtual functions, in this order:

| Slot | Signature |
|---|---|
| 0 | `GetPluginInterfaceVersion() const` — must return `100` |
| 1 | `QueryPluginList(bmd::PluginCategory) const` |
| 2 | `CreatePluginInstance(bmd::PluginCategory, QString const&) const` |
| 3 | `SetAudioPluginHost(AudioPluginHost*)` |
| 4 | `SetPluginUserInterfaceHost(bmd::PluginUserInterfaceHost*)` |
| 5 | `QueryMacroFXResourceList() const` |

The stock library needs **zero** symbols from `libFairlightPage.so` (779 undefined symbols, none of
them defined there), so `dlopen(RTLD_NOW | RTLD_LOCAL)` on it is safe from inside the bridge.

The plugin UI is declarative, not drawn by the plugin. The RTTI names the resource types:
`PluginUIResourceTree`, `PluginUIKnobResource`, `…Slider`, `…Switch`, `…Combo`, `…Meter`,
`…EditField`, `…ClientDraw`, bound through `PluginUIParameterBinding` and `PluginUIBinding`
(`Get`, `Set`, `SetFromText`, `Touch`, `GetDisplay`, `ParameterActive`). Resolve renders the panel,
so a hosted plugin gets a native Fairlight UI and automation, but not its own artwork.

## Stages

- **Stage 1 (done): transparent pass-through.** Load the stock library, return its interface
  unchanged. Proved the seam without changing behaviour.
- **Stage 2 (done, this code): vtable wrap.** Copy the stock vtable, patch slots 1 and 2, forward
  the rest with assembly tail jumps. Verified 2026-08-24 in a live session: the version reads 100
  back through our own copy, `QueryPluginList` runs through our trampoline at start-up, and adding
  a Delay to a track produced three `CreatePluginInstance` calls through ours. The FX menu and the
  effect behaved normally.
- **Stage 3a (done): recover the list type at runtime.** See below.
- **Stage 3b (done): add an entry of our own.** A clone of `Delay:1112360057` is inserted under the
  key `Bridge Test:1112360057` with our own display name, on every query.
- **Stage 3c (done): reach the audio path.** See "The audio hook" below.
- **Stage 3d (done): process the audio.** Our own DSP runs on live samples inside Fairlight.
- **Next: restrict, persist, host.** See "Remaining work".

## Build and install

```sh
./build.sh
install -m 0755 build/libfxbridge.so ~/.local/share/BMDAudioPlugins/libfxbridge.so
```

Then set the key in `config-fairlight.dat` **while Resolve is closed** and restart it.

## Revert

Delete the `BMDPlugins.Path` line from `config-fairlight.dat` (and from `config.dat` if it is there).
Resolve falls back to its own library. Config backups from the first session are in
`~/.local/share/DaVinciResolve-backups/20260824-205706/`.

## Known risks

- The ABI is private and undocumented. `SONAME` is `libBMDAudioPlugins.so.21`, so expect a break at
  every major Resolve version. The version gate (`== 100`) is the tripwire.
- The bridge runs on Resolve's audio thread. A fault takes Resolve down with the open project.
- A wrong path in `BMDPlugins.Path` is not fatal: the loader falls back to the stock library.

## Dead end: the built-in VST hosts are stubs

> **Corrected 2026-08-29.** This entry is right about `VST3Host` and wrong about the rest. Two
> `VST3Host` methods were measured as stubs and the conclusion was extended to `VSTHost`,
> `VSTPluginManager` and every member of all three. `VSTHost::LoadPlugin` at `0x15912f0` has a real
> body — it takes the recursive mutex at `this+0x70` and increments the counter at `this+0x98`. So
> does `VSTPlugin::StorePreset`, and reading it is what made v0.2.3 possible. What still holds is
> the practical finding: nothing in the Linux interface loads a VST. **Why** it does not is no
> longer explained by "there are no bodies", and is not established.

`libFairlightPage.so` on Linux defines `VSTHost` (44 symbols), `VST3Host` (26) and
`VSTPluginManager`, with a full member list — `LoadPlugin`, `RescanPlugins`, `GetPluginPaths`,
`PreProcess`, `PostProcess`. **They have no bodies.** `VST3Host::UpdateInstalledPluginList` at
`0x150e0b0` only runs the stack-guard sequence and returns 0; `VST3Host::SetSampleRate` returns 1
and does nothing. So there is no flag to flip and no host to re-enable — the Linux build compiles
the VST backends out and keeps the shells. Do not spend another session on this.

## Next: recover the plugin-list type

The host side is fully typed in the symbols:
`BMDAudioPluginHost::UpdateInstalledPluginList(std::map<QString, FL::AudioPluginInterface::AudioPluginDefinition>&) const`
(libc++, `std::__1`). So `AudioPluginDefinition` is the descriptor to reproduce.

The cheap way to recover its layout was **not** more disassembly. The trampoline runs inside the
process, so it logged the argument registers and read the returned memory. Results:

**Calling convention.** `arg0` was a stack address and `arg1` was the interface, so the value comes
back through a hidden sret pointer: `QueryPluginList(sret, this, category)`. `rax` carries the sret
address on return, which is why the trampoline can hand it straight back.

**The container** is a libc++ `std::map` returned by value — three words, `{__begin_node_, root,
size}`, size 38 for category 0. A node is `{__left_, __right_, __parent_, __is_black_}` with the
value pair at `+32`, so the key sits at `+32` and the definition at `+40`.

**The key** is `"<name>:<id>"` — exactly the `<ID>` strings in `FairlightFXConfiguration.xml`, e.g.
`Delay:1112360057`, `Chain FX:1112359784`. The definition's **first field is the display-name
QString**; the menu reads that, not the key.

**The value type is `BMDAudioPluginFactory::PluginDefinition`**, and the stock library exports the
map's own insert helper for it:

```
std::__tree<__value_type<QString, BMDAudioPluginFactory::PluginDefinition>, …>
    ::__emplace_unique_key_args<QString, std::pair<QString const, PluginDefinition>>
```

So there is no need to reproduce libc++ node allocation or rebalancing. Pass a pair copied from an
existing entry with the key and name fields swapped, and the node is copy-constructed properly —
Qt deep-copies every QString inside the definition with correct reference counts.

Our own key and name are built as **static** `QArrayData` (`ref = -1`, the marker Qt uses for
`QStringLiteral`), which Qt never refcounts and never frees. That removes the ownership question
from the experiment entirely.

Verified 2026-08-24: `inserted "Bridge Test", cloned from "Delay:1112360057"`, Resolve stayed up.

## The menu category is not the CategoryMask

> **SUPERSEDED.** The conclusion here — that the category is not the `CategoryMask` — is right, and
> the reason is now known: a category is a lookup in a compressed table compiled into
> `libFairlightPage.so`, keyed by `"<name>:<id>"`. The bridge patches that table in memory and
> plugins now appear under Dynamics, EQ, Restoration and the rest. See
> [`categories.md`](categories.md).

`FairlightFXConfiguration.xml` holds `<ID>Delay:1112360057</ID><CategoryMask>4</CategoryMask>`, so it
reads like the source of the submenu. **It is not.** Adding our own ID to that file with mask 4, with
Resolve closed, left the entry under *Uncategorized* on the next start — and the line survived the
restart, so it was not discarded (verified 2026-08-24).

Our clone differs from `Delay` in exactly two fields: the key string and the display-name QString.
Everything else in the definition is a copy. So the category cannot travel inside the definition
either. It is derived from the key or the name.

`Uncategorized` as a label lives only in `libFairlightPage.so`, so the host assigns it, not the
plugin library.

Three clones were then inserted in one run, each varying one field (verified 2026-08-24):

| Shown as | Key | Result |
|---|---|---|
| Bridge A | `Delay:1112360058` | Uncategorized |
| Delay | `ZZBridge:1112360057` | Uncategorized |
| Bridge Test | `Bridge Test:1112360057` | Uncategorized |

So the category comes from **none** of: the definition (copied whole, 64 bytes), the name part of
the key, the display name, or the numeric id. `BMDAudioPluginHost::UpdateInstalledPluginList` does
not assign it either — its two ASCII constants are `"AudioIntelligence"` and `"Studio"`, which are
licence checks, next to a `g_Studio()` call.

The remaining candidate is the Qt resource `:/FLSystem/Plugin Metadata` inside `libFairlightPage.so`
(**not checked**), which would be a table keyed by the plugin key. A compiled-in resource cannot be
edited, so placing our effect in a real category would mean hooking that lookup as well.

**Decision: leave our effects under Uncategorized for now.** The placement is cosmetic, it costs an
extra hook, and that hook is cheaper once we already wrap more of the interface.

Note: instantiation does **not** depend on this. Selecting our entry builds a working effect.

## The audio hook

Two attempts missed first, and both mistakes are worth keeping:

1. **Patching the instance vptr does nothing.** `CreatePluginInstance` returns a pointer to the
   secondary `bmd::PluginInstance` subobject, at +0x4f8 into the class vtable group — not to the
   object. Slot indices taken from `BMDAudioPluginImpl`'s vtable therefore addressed the wrong
   table: slots 66/67 there are non-virtual thunks to `SetSampleRate` and `GetSamplePeriod`. We
   patched two harmless accessors.
2. **Those instances are short-lived.** A later `gdb` read of a wrapped address returned float
   samples: the object had been freed and its memory reused as an audio buffer.
3. **`PreProcess`/`PostProcess(float**)` are inherited helpers that nothing calls at runtime.**

The real DSP entry point is each effect class's own `Process`, and **74 classes define one**:

```
BMDStereoDelay::Process(AudioPluginTimebaseInformation const*, float**, float**, unsigned long)
```

In `vtable for BMDStereoDelay` it sits at **+0x4d0**, with a non-virtual thunk at **+0x690** for
calls through a secondary base pointer. Patching the class vtable once — `mprotect`, write both
slots — covers every instance, every lifetime and both call paths. The vtable symbol
`_ZTV14BMDStereoDelay` is exported, so `dlsym` finds it; no address arithmetic is needed.

### The buffer contract (measured, 2026-08-24)

```
Process #1: frames=455 in=0x7f7e704cbfc0 out=0x7f7e704cbfc0 in[0]=0x7f7e8736d854 out[0]=0x7f7e8736d854
```

- `in == out` and `in[0] == out[0]`: processing is **in place**.
- **planar float32**, one contiguous block per channel; the channel pointer advances 455 floats
  (0x71c bytes) between calls.
- 455 frames per block — the 9.5 ms ALSA buffer at 48 kHz.
- Register layout: `rdi = this`, `rsi = timebase`, `rdx = in`, `rcx = out`, `r8 = frames`.

### Verified end to end

A gain of 0.5 applied to channel 0 after the stock effect: the left meter drops 6 dB, the right does
not, and the logged samples halve exactly (-0.011735 → -0.005867). Our code processes live audio
inside Fairlight.

## Remaining work

> **SUPERSEDED.** This list is from an early evening and most of it is done. See the README's
> status table for the current position.

1. **Restrict the hook to our own effect.** It is on the `BMDStereoDelay` class, so it currently
   touches every Delay in the project. Filter per instance.
2. **Our own identity.** The create hook substitutes Delay's key, so a Bridge Test effect is saved
   as a Delay and returns as one. An effect of ours must persist across a project save.
3. **Host a CLAP plugin** inside `Process`, and bind its parameters to the UI resource tree
   (`PluginUIKnobResource` and friends) for live tuning with automation.

## Testing

`test/bridge-test-tone.wav` — 30 s, 440 Hz, stereo, 48 kHz, 24-bit PCM. Resolve on Linux cannot
decode AAC, so an `.mp4` gives no waveform and no samples to process; use PCM for any audio test.
Resolve's stderr, and so every `[fxbridge]` line, lands in
`~/.local/share/DaVinciResolve/logs/ResolveDebug.txt`.

## Status: the CLAP host works, the Carla host does not

**Use the CLAP host.** It is the default and it is the one that has been exercised end to end:
`pp-track.clap` runs inside Fairlight with live audio, its own window, the show and hide button, a
correct effect name and an empty panel.

    FXBRIDGE_CLAP=/home/jooshua/.clap/pp-track.clap  /opt/resolve/bin/resolve

**`FXBRIDGE_HOST=carla` is not usable yet.** Carla loads, instantiates, activates, opens its window
and reports processing, but the audio is wrong and Resolve crashes. Measured on 2026-08-24:

- With the rack in the chain there is no audio and the bus clips into the red.
- Resolve crashes on project load, `SIGSEGV`, and the core dump names `BridgeAfterProcess` with
  `BridgeThunkProcessSecondary` above it - our audio path, faulting after Carla's `process` runs.
  The faulting frame is a jump to a small address such as `0x3146`, which is a call through a
  pointer that Carla's processing left in a bad state.

What has been ruled out, and what has not:

- **Not the block size, on the evidence so far.** Carla is told 512 at load and Resolve's block is
  455. Correcting it *from inside the audio callback* - deactivate, `BUFFER_SIZE_CHANGED`, activate -
  made things worse and crashed in the same place. Never re-arm an engine on the real-time thread.
- **Untested: the host handle.** `carla_create_native_plugin_host_handle(desc, handle)` is never
  called. Its documentation says it is "suitable for CarlaHost API calls", which suggests it is for
  adding plugins rather than required for processing - but it is the clearest missing piece, and it
  is the next thing to try.
- **Untested: whether an empty rack passes audio at all.** The build now logs the dry peak and the
  chain peak for the first three blocks. Silence and garbage look identical on a meter and that one
  line separates them. Run it and read the line before changing anything else.

There is an output guard in `CarlaHostProcess`: a block containing a sample beyond +/-4.0 or a NaN
is discarded and the dry signal passed instead. It is a hearing-safety measure, not a fix, and it
reports once.

## Per-instance: our own vtable

The stock `BMDStereoDelay` vtable is **not patched**. Patching it made every Delay in the project
our effect. Instead the whole 0xab8-byte vtable object is copied once at load, the overrides go into
the copy, and each instance we create is pointed at it. A Delay the editor adds stays a real Delay.

    vtable copied: stock 0x7fe6a16cebb8, ours 0x7fe6c53cc100, 2744 bytes

### The instance layout, measured

`CreatePluginInstance` hands back the `bmd::PluginInstance` subobject, **not** the start of the
object, so the primary vptr is at a *lower* address. Measured on 2026-08-24:

| Where | Group | What |
|---|---|---|
| instance **-16** | +0x010 | primary (`BMDAudioPluginImpl`) — carries the name and the control tree |
| instance **+0** | +0x4f8 | `bmd::PluginInstance` — what the factory returns |
| instance **+16** | +0x650 | `AudioPlugin` — what Resolve holds |
| instance **+200** | +0xa88 | |
| instance **+216** | +0xaa0 | |

Every group lives inside 232 bytes, and the scan is bounded to that. **A wide scan is not merely
wasteful, it is unsafe**: any other `BMDStereoDelay` on the heap holds these same five values, so a
scan that runs past this object claims a neighbour's vptrs. A ±0x800 scan shipped briefly and was a
real hazard, not a theoretical one.

Scanning forward only finds four groups and misses the primary — which is exactly the one that
carries the name and the panel, so both of those fixes silently did nothing until the scan ran
backwards too.

## The three cosmetic fixes, and how each was actually found

### The name: change the source, do not fake the getter

`SetUserEffectName` runs without error and Resolve still shows `Delay`, so the label is not the user
name. `GetEffectName` says where it does come from:

    mov  0x550(%rsi), %rax     # rsi is `this`; a pointer field in the object
    mov  (%rax), %rsi          # the first word of that record is a char const*
    call flx::CA2W(char const*)

So the effect name is one indirection off the **primary** object at **+0x550**. The record is cloned
per instance and the clone's first word points at our name. The stock record is untouched, so every
other Delay keeps its own name. The log prints the stock name it found first, which is what proves
the field was read correctly:

    rename: the stock record at 0x7f97262fb1b8 names the effect "Delay"
    rename: instance now names itself "PodcastPlugins TRACK"

### The panel: suppress `GenerateUserInterface`, then open the window yourself

`GenerateUserInterface` at **+0x460** builds the Delay knobs. It is called on our instance *after*
the claim, through our own copy — so patching the shared vtable for the length of the create call is
unnecessary **and** racy, since another thread building a Delay would get our no-op.

Two traps cost a run each:

- The trace table also owns 0x460, so installing the no-op **before** the trace loop let the tracer
  overwrite it. The diagnostic shadowed the fix, and the switch looked broken.
- With no control tree there is no panel, and Resolve then **never calls `InitializeEffectEdit`** —
  the editor path is gated on the panel existing. The log carried no `InitializeEffectEdit` line at
  all. So the hosted window is opened at claim time instead.

The consequence is worth knowing: with no panel there is no editor button, so the show/hide toggle
has nothing to drive it. Closing the window with its X leaves no way back except re-adding the
effect.

The better end state is not an empty panel but **our plugin's own parameters** in it: the trace shows
`GetNumberOfParameters`, `UpdateParameterList` and `GetParameterList` all being called, so Resolve
builds that panel from the parameter list. Filling it with the hosted plugin's parameters would give
native automation as well. That is a bigger job than a no-op.

### The stock DSP: restore the dry signal, do not skip the call

The host class is a Delay, so letting it run and then processing its output puts an audible delay in
front of the hosted plugin. Skipping the stock call means inventing its return value. So the dry
block is copied aside before, and put back after — the stock effect still runs and answers for
itself, and the plugin gets the untouched track. One buffer copy per block.

## Read the core dump before theorising

`BridgeThunkProcessPrimary` crashed Resolve, and three separate guesses blamed the rename, the panel
and the vptr claim. `coredumpctl info` named it in one line:

    #7  BridgeThunkProcessPrimary (libfxbridge.so + 0x3803)
    #6  libc.so.6 + 0x44cb0            <- __restore_rt, the signal trampoline
    #5  resolve                        <- Resolve's own crash handler

Frame #6 is the signal trampoline, so the fault was **inside** that frame, not below it. The signal
was `SIGBUS` with `si_code BUS_ADRERR`, and the crashes are kept: `coredumpctl list`.

Resolve **only** reaches `Process` through the `AudioPlugin` thunk at **+0x690**. The primary slot at
+0x4d0 became reachable for the first time when the primary vptr was claimed, and killed the process.
It is off by default (`FXBRIDGE_PRIMARY_PROCESS=1` to try again) and nothing needs it — the audio
path is unchanged with it off.

The assembly trampolines now carry `.cfi_startproc` / `.cfi_endproc`. Without unwind information any
crash handler that walks the stack dies in `_Unwind_Find_FDE` instead of reporting the fault.

## Why the effect is still called Delay, and what it would take not to be

> **SUPERSEDED.** Effects now carry their own names. `SetEffectLabel` writes the menu entry's name
> onto the claimed instance, so a track shows `soothe2_x64` or `RVox Mono`, not `Delay`. The
> analysis below of the second map and the creation function pointer at value offset `0x38` is still
> accurate and still the reason the create hook substitutes Delay's key.

The effect rides on `BMDStereoDelay`. The stock DSP is now kept out of the sound — see the Process
trampoline below — but the identity is still Delay: the mixer's Effects slot reads `Delay`, and the
panel under the hosted window is the Delay control tree.

### Creation goes through a second map, not the one we patch

`QueryPluginList` reads the map we insert our menu entry into. **Creation does not.** For category 0,
`BMDPluginInterfaceImpl::CreatePluginInstance` (0x4d8fb0) loads the `BMDAudioPluginFactory`
singleton and walks *its* `std::map`:

    mov  0x84f599(%rip), %r13        # BMDAudioPluginFactory::Instance()::_this
    mov  0x8(%r13), %rbp             # the tree root
    lea  0x20(%rbp), %rdi            # the key QString - node header is 32 bytes
    call operator<(QString const&, QString const&)
    ...
    lea  0x28(%rbx), %rdx            # the value, right after the 8-byte key
    call *0x60(%rbx)                 # <- the factory, at +0x38 inside the value

So each record carries a **creation function pointer at value offset 0x38**. Our menu entry is a
72-byte clone that ends well before that, which is exactly why `CreatePluginInstance` under our own
key returns null and why the trampoline has to substitute Delay's key.

Two routes out, and neither is small:

1. **Register in the factory map too**, with a creation function of our own. The insertion technique
   is the one already used for the menu map. But whatever that function returns still has to be a
   working `bmd::PluginInstance`, so this only moves the question.
2. **Own the instance's vtables.** Create the stock Delay, then point the new object at *our* copies
   of its three vtables. That is per-instance, so a real Delay elsewhere in the project stays a real
   Delay, and it is the only route that can change the name and replace the control tree.

Route 2 also fixes the two other known weaknesses in one move: the hook stops being class-wide, and
each effect instance can own its own hosted plugin and editor.

### The name is an sret — do not hook it as a pointer return

`BMDAudioPlugin<BMDStereoDelay>::GetEffectName() const` returns a string **by value**. On x86-64 that
means the hidden return buffer arrives in `rdi` and `this` moves to `rsi`. Hooking it as
`const void* (*)(void*)` therefore hands the object pointer over as a write buffer.

The tell was in the probe's own output: it reported the returned pointer as `0x7ffc433d9560`, a
**stack** address. A function returning a `wchar_t const*` to a name would not answer with the stack.

Read the disassembly for an sret before hooking any call whose return type you have not confirmed.
Three return types were named by guess in one session and all three were wrong: this one, the
pointer that `InitializeEffectEdit` returns, and the wider-than-`int` value from `GetPluginType`.

## The editor path: patch the `AudioPlugin` base, not the primary

`BMDStereoDelay` has **three** vtables inside the one `_ZTV14BMDStereoDelay` object, and Resolve
calls the third one. The editor slots appear twice at completely different offsets:

| Call | Primary (+0x000) | `AudioPlugin` base (+0x648) |
|---|---|---|
| `InitializeEffectEdit(char const*, void*)` | 0x250 | **0x7b0** |
| `HasEditor() const` | 0x258 | **0x718** |
| `GetEffectEdit() const` | 0x260 | **0x7b8** |
| `CloseEffectEdit()` | 0x278 | **0x7d0** |
| `LockEditor(bool)` | 0x248 | 0x858 |
| `OnEditorIdle()` | 0x280 | 0x7d8 |
| `GetEffectEditTitle() const` | 0x268 | 0x7c8 — sret, empty value; dead end |

Resolve's Fairlight engine holds an `AudioPlugin*`, so it dials the right-hand column. Those slots
are non-virtual thunks that adjust `this` and jump **straight into** the primary implementation, not
back through the primary vtable — so patching the primary slot changes nothing for them. This is the
same trap the audio hook hit, where `Process` needed both +0x4d0 and +0x690.

The `AudioPlugin` base is worth reading in full: alongside those it carries `EditorTop`,
`EditorPreInitialize`, `EditorIdleRefresh`, `EditorMute`, `CanResize`, `HideSubWindows` and
`IsWaveShell`. That last name is a Waves plugin concept, so this base is the host-side interface
every external plugin sits behind — a VST included.

Symptom when the primary is patched and the base is not: the effect loads, audio runs through the
hosted plugin, and pressing the editor button produces **nothing in the log at all** — not a failure
line, not a `gui create failed`. An empty log there means the call never arrived, not that it failed.

`HasEditor()` is **not** a gate to force. The stock `BMDStereoDelay` already answers true; forcing it
was a wrong guess that cost a restart.

`self` arrives pointing at the `AudioPlugin` subobject and the saved slot value is the thunk itself,
so passing `self` straight to the saved pointer keeps the adjustment correct.

### The panel button is a toggle, not a second open

`InitializeEffectEdit` runs **once** per effect instance. After that Resolve owns an editor object
and the panel button no longer touches it — it drives two other slots in the `AudioPlugin` base:

| Press | Call | What we do |
|---|---|---|
| show | `UpdateEffectEditTitle(char const*)` — **+0x7c0** | `ClapHostOpenEditor()` — remaps the window |
| hide | `HideSubWindows()` — **+0x830** | `ClapHostCloseEditor()` — unmaps it |

While the editor is shown, `OnEditorIdle()` at +0x7d8 runs as a heartbeat. Its resumption after a
gap is a second, independent signal that the editor is visible again.

Symptom when this is missed: the window opens the first time, closes on its X, and the panel button
is then dead — because Resolve has no reason to call `InitializeEffectEdit` again. Removing the
effect and re-adding it works, which makes it look like a lifecycle bug in the host when it is only
a missed slot. Measured on 2026-08-24 with a trace armed on the window-manager close.

There is no way for the plugin to tell Resolve its window went away: `AudioPluginHost` is a plugin
registry — `FindPlugin`, `LoadPlugin`, `GetPluginMenu`, `RefreshPluginLists` — and carries no editor
notification at all. So following the host's two calls is the whole mechanism, not a workaround.

### Tracing an unknown slot safely

The trace trampolines report and then **tail-jump** to the stock function, so the return value is
never touched. Use that shape whenever the return type is unknown — the two bugs below came from
naming a return type by guess. The counters arm on the window-manager close, so the calls that
follow a user action are the ones that reach the log instead of drowning in the heartbeat.

### Return types: read the value before you name it

Both of these were measured, and both would have corrupted Resolve if left alone:

- `InitializeEffectEdit` returned **136**, not a bool. Declaring a `bool` return truncates it to the
  low byte at `-O2`, and the replacement then handed Resolve `1`. Take it as a `long` and pass the
  stock value back untouched.
- `GetPluginType()` answered **1977175184** — the low half of a pointer. An `int` return writes only
  `eax` and leaves the top half of `rax` undefined, so the caller reads garbage. That probe was
  removed rather than fixed; we do not know what it really returns.

These hooks are plain C++ replacements, not assembly trampolines. Every signature is integers and
pointers only, so the compiler's own calling sequence is already right; the trampoline macro is only
needed where a float argument in `xmm0` has to survive the reporter.

### How to read the slot map

The vtable relocations are **`R_X86_64_64` with a symbol name**, not `R_X86_64_RELATIVE`. A parser
that only handles RELATIVE entries finds zero slots and looks like a missing vtable. Read them with:

    readelf -sW /opt/resolve/libs/libBMDAudioPlugins.so | grep _ZTV14BMDStereoDelay   # address + size
    readelf -rW /opt/resolve/libs/libBMDAudioPlugins.so                               # then match VT+offset

and pipe the names through `c++filt`.

## Hosting real plugins (2026-08-24)

`src/clap_host.cpp` is a minimal CLAP host. CLAP was chosen because it is a plain C ABI in one set
of MIT headers — no SDK, no COM, no C++ ABI to match — and **every plugin on this machine ships a
`.clap` beside its `.vst3`**, so one host covers the whole collection. Headers are vendored in
`third_party/clap` (headers only, ~360K).

Proven end to end: **Dragonfly Hall Reverb loaded, processing live audio, with its own native GUI
open next to Resolve.** The menu entry is named from the CLAP descriptor, so pointing
`FXBRIDGE_CLAP` at another plugin renames the entry with no rebuild.

### What the audio path needs

- `ClapHostLoad` runs on the main thread: entry → factory → create → init → activate →
  start_processing. Parameter ids and ranges are cached here so the audio thread never queries the
  params extension.
- Resolve processes **in place** (`in == out`), so the input is copied to scratch and the plugin
  writes into Resolve's buffer.
- Knob changes are queued as dirty slots and drained into real `clap_event_param_value` events at
  the top of `process()`. That is the only correct way to drive a CLAP parameter, and it allocates
  nothing in the callback.

### What the GUI needs

The plugin refuses to build its editor unless the **host** offers extensions. Ours provides
`clap.timer-support`, `clap.gui` and `clap.thread-check`. Without the timer, `create()` fails.

**A DPF-based plugin lies about floating windows.** `is_api_supported(X11, floating=true)` returns
true, and `create(..., true)` then crashes inside the plugin (verified: crash dump with
`DragonflyHallReverb.clap(+0x30a40)` above our frame). Only the **embedded** path is real: ask
`get_size`, create an X11 window, `set_parent`, `show`.

Editor lifecycle is driven by Resolve's own vtable slots, not by instance creation:
`InitializeEffectEdit` at **+0x250** opens or raises it, `CloseEffectEdit` at **+0x278** hides it.
The window registers `WM_DELETE_WINDOW` and unmaps instead of being destroyed, because the plugin's
editor lives inside it; a destroyed window left the plugin believing its GUI still existed.

### Known weaknesses

- The idle timer fires on **our** thread, not Resolve's Qt loop. Good enough to prove the path; a
  toolkit that insists on one thread can fault on it.
- The hook is on the `BMDStereoDelay` class, so **every Delay** routes through the hosted plugin.
- One hosted plugin per session, one editor window. Multiple instances are not separated yet.
- Resolve's own Delay panel still shows beside the plugin editor, and its knobs feed the parameter
  queue by index — that mapping is arbitrary and will fight the plugin's own GUI.

### Next

1. VST3 host behind the same interface — covers plugins with no CLAP build, and Windows plugins
   through yabridge, which presents them as native Linux VST3.
2. Move the timer onto Resolve's event loop.
3. One hosted plugin per effect instance, with its own editor.


## The crash in Resolve's mixer: three theories dead, cause still unknown

This one is written down because it has now survived two fixes, and each fix was built on a theory
that a measurement then killed. The record matters more than the theories.

**The fault, verified.** Two dumps (2026-08-25 01:22:27 and 01:51:18), identical frames:

    MixPole::ApplyGainSmooth+0x188
    BusSourcePreFader::AddChannelSourceToChannelBus
    BusSourcePreFader::AddToBus  →  AddToMix::Execute
    NativeAudioEngine2::AdvancedLoadSamples  →  ALSAPlayandCaptureLoop     (the audio thread)

Disassembled, `+0x188` is:

    mov  0x8(%r12),%rdi      # a member of the MixPole
    test %rdi,%rdi
    je   …                   # null is already handled
    mov  (%rdi),%rax         # its vptr
    call *0x20(%rax)         # <- the fault

So the object is **non-null garbage**, not null and not a short buffer. That reads as a freed or
overwritten `AudioNode`, and it says nothing about who freed it.

**Theory 1: the hosted plugin wrote past its block.** A limiter with lookahead rounding its work up
to an internal block would overrun Resolve's buffer, which holds exactly `frames` floats. Plausible,
and it fit pp-track (a limiter) crashing where Dragonfly (no lookahead) did not.

*Measured and refuted.* The plugin now runs over our own scratch with 8192 frames of headroom, and a
marker sits one sample past every block. The marker survived **every** block - zero reports - and the
crash came back unchanged. The isolation is kept anyway: it is the right boundary, and it is what
turned the theory into a measurement.

**Theory 2: Resolve's output buffer was shorter than `frames`.** Dead on inspection, not on a hunch:
`UsableChannels` already read- and write-probes the **first and last** sample of every channel before
anything is copied into it.

**Theory 3: the emptied panel.** The tree truncation in `BridgeGenerateUserInterface` is the one
place we edit a Fairlight data structure. *Refuted by the dumps themselves:* the 01:22 crash happened
with `FXBRIDGE_EMPTY_PANEL` off and the 01:51 crash with it on, and the frames are identical.

**What is left, and what would settle it.** Both crashes carried pp-track; Dragonfly ran all evening
without one. Two experiments decide whether the bridge is involved at all, and neither is a code
change:

1. the same timeline with Dragonfly Hall Reverb in place of pp-track;
2. the same timeline with our effect removed.

**A crash dump is not always the crash.** Earlier the same night, a dump named
`ClapHostLogParameters` and frames #0/#1 sat inside `backtrace_symbols`: Resolve's own crash handler
had faulted while walking the stack, so the dump recorded the handler's death and the attribution
below it was noise. Read frame #0 before believing frame #5. Of our hand-written trampolines only
`BRIDGE_PROCESS_THUNK` carries CFI unwind data, which is the likeliest reason a stack walker dies in
one - unproven, and it explains a *handler* crash, never a first fault.

**A crash that stops is not a crash that is fixed.** The close-panel-during-playback crash of the
same night disappeared after a rebuild of the *same commit* and has not returned. Nothing is credited
with fixing it. If it comes back, the first move is `md5sum` on the installed library against
`build/libfxbridge.so`, not a core dump.

## Scanning, and one plugin per effect (2026-08-25)

`plugin_scan.cpp` walks the standard folders and `proxy.cpp` turns each result into one menu entry.
Fifteen entries appear on this machine; the key each entry carries maps back to the plugin file, so
picking an entry loads that plugin and no other. Verified in Resolve.

Nothing is loaded to build the menu - the name is the file's stem. Asking a plugin its name would
start a Wine process per Windows plugin at Resolve's startup, twenty-two of them here.

Three traps the scanner already carries:

- `~/.vst` is a **root**, `carla.vst` is a **bundle**. A bundle is read flat, because Carla ships a
  `styles/` folder beside its plugins with a Qt style plugin in it, and `dlopen` on that runs a Qt
  plugin's constructors inside Resolve.
- Names that start with `lib`, or contain `bridge`, `interposer` or `chainloader`, are support
  libraries, not plugins. The filter is on the name, before anything is opened.
- The menu name is stored in the project, so the scan is sorted and de-duplicated by path. An entry
  that changes name between runs is an effect that stops loading.

**Everything that was shared is now per effect**: the plugin, its buffers, its editor window, its
name in both encodings, and its editor's shown/wanted flags. Each of those was a real defect while it
was shared, and each is recorded in the commit that removed it. The one thing still shared is the
`BridgeEditorWasClosedByUser` callback, which knows the window but not the effect.

**The carrier's knobs are not the plugin's parameters.** They were forwarded by index. Dragging the
Delay panel's Delay Time knob sent index 5 to pp-track and silenced it, and it read as a broken audio
path until someone read the `knob:` lines in the log. There is no mapping to find; the binding is
gone, behind `FXBRIDGE_KNOB_BINDING` for studying a plugin whose parameter order is known.

**CLAP's `on_timer` is main-thread work.** It runs beside every GUI call, and a plugin may touch its
editor from it. Our timer thread had neither a lock nor an honest answer from
`clap_host_thread_check` - it reported itself as the *audio* thread - and Resolve died six frames
inside pp-track's GUI code. One mutex per plugin around every main-thread call, and the timer thread
answers as main.


## A library that builds clean and will not load (2026-08-26)

`build.sh` ends by refusing to install a library with undefined symbols, because a shared object
links happily without them and only fails at `dlopen`. That guard has one blind spot: it runs
against the build machine, never the target.

glibc 2.34 merged `libpthread` into `libc`. This machine runs 2.44, so `pthread_create` resolved out
of `libc`, the guard passed, and the missing `-lpthread` on the link line was invisible. Below glibc
2.34 the guard still passes -- it is the same machine -- and the resulting file then fails to load.
Ubuntu 20.04, Debian 11 and Rocky 8 all sit under that line.

Found by compiling in an Ubuntu 20.04 rootfs under `bwrap`, unprivileged, no daemon and no root:

```
undefined symbol: pthread_create   (/out/libfxbridge.so)
```

Adding `-lpthread` takes that to zero. `libpthread.so.0` survives as a stub on glibc 2.34+, so the
flag costs nothing here.

The same build changed what the binary asks of a system, which is why releases are built that way
now:

| Symbol set | built on glibc 2.44 | built on glibc 2.31 |
|---|---|---|
| `GLIBC_` | 2.38 | **2.16** |
| `GLIBCXX_` | 3.4.22 | 3.4.22 |
| `CXXABI_` | 1.3.15 | **1.3.9** |

Three checks, because each proves something the others cannot. `ldd -r` inside the old rootfs proves
every symbol resolves against the old glibc. `objdump -T` gives the version floor, which is
mechanical and names the distributions that can load it. Loading it under Resolve here proves it
actually hosts plugins -- 130 listed, categories patched, audio processed -- and that check only
works in this direction, because glibc is backward compatible and this box has no old glibc to test
on.

What none of it proves is that **Resolve itself** runs on a distribution that old. The bridge was
tested there, the host application was not, and the release notes say so.


## 2026-08-27 — the first outside report

Delirio installed v0.1.3 on their own machine and reported four things. Three of them
have an answer in the source; the fourth does not, and saying which is which is the point of this
entry.

### An editor that will not reopen

Their words: *"sometimes, after i insert a plugin then i try to open it, the GUI doesn't open
anymore, i have to restart resolve or delete and add it again."*

`BridgeEditorWasClosedByUser` cleared `editor_wanted` and `editor_shown` on **every** effect, and
the comment above it said why that was safe:

> Nothing acts on the flag on its own - there is no re-assert loop

That was true when it was written and stopped being true in v0.1.1, which added
`BridgeEditorReassert` on the window pump — the fix for the project-switch deadlock. From that
release on, closing one editor made the re-assert loop forget every other one, and nothing set the
flag again. The comment aged into a bug.

The pump knows which X11 window the window manager closed, so the fix is to pass it: `HostedPlugin`
gained `EditorWindow()`, and only the effect that owns that window is marked closed. A window with
no owner still clears everything — a `wanted` flag left set on an effect whose window is gone would
have the pump reopen it against the user, and that is the one failure here that cannot be clicked
away.

### Idle CPU: not diagnosed, now measurable

Their words: *"Cpu usage with NO plugin added is 20-25% on my system, 0 to 3% with the normal
resolve."*

Every loop in this library was read. Three threads exist: the X11 pump at 30 ms, the host tick at
16 ms, and Carla's idle loop — and `CarlaHostLoad` has **zero callers**, so the third never starts.
The trace hooks are capped at six reports per slot, so it is not a log flood either. Nothing in the
source accounts for a quarter of a core, and no attempt was made to invent one.

What was missing is the ability to ask. No thread called `pthread_setname_np`, so `top -H` against
Resolve showed all 300-odd threads as `GUI`. They are now `fxb-xpump`, `fxb-tick` and `fxb-carla`,
which turns the next report into a measurement.

### Settings do not survive a project reload

Their words: *"they works but it doesn't retain settings when i close and open the project again,
this is another big issue."*

Verified, and the cause is that the feature did not exist: `grep -rn "etChunk|IBStream|etState"
src/` returned nothing. The bridge never asked a plugin for its state.

That half is now written and is format-side only: `SaveState`/`LoadState` on `HostedPlugin`, VST2
via `effGetChunk`/`effSetChunk` with a parameter-sweep fallback for plugins that publish no chunk,
VST3 via `IComponent::get_state`/`set_state` over a `v3_bstream` implemented on a byte vector, CLAP
via `clap.state`. Every blob carries eight bytes of magic and a four-byte format tag, because a
VST3 state handed to a VST2 plugin's `setChunk` is not a restore that fails — it is a plugin
parsing a foreign buffer as its own.

**Where the bytes should live is not yet measured.** Resolve saves an effect and its parameters in
the project; there is no blob channel a built-in effect can use, and this bridge impersonates a
built-in effect. The candidate is `AudioPluginPreset`: `rmap vtable BMDStereoDelay` puts
`StorePreset` at `+0x380` and `LoadPreset` at `+0x388`, and
`AudioPluginHost::AddPlaceholderPlugin(wchar_t const*, unsigned long, AudioPluginPreset const&, …)`
takes one when Resolve restores a plugin that is not loaded yet. Its copy constructor copies two
`std::string`, one `std::wstring` and one `vector<string>`, so it has room. Both slots are now
traced. One project save with a plugin on a track will say whether they fire.

Until that is read, `FXBRIDGE_STATE_STORE=1` is the stopgap: one file per plugin under
`~/.local/share/BMDAudioPlugins/state/`, snapshotted on the host main thread about every ten
seconds. It is opt-in because it is keyed by the plugin rather than by the effect, so two instances
of one plugin in a project share a file. Shipping that as the default would trade a visible bug for
a silent one.

There is no moment to save at, which is why it is a timer. Resolve never tells the bridge that an
effect is going away — `g_effect_count` only grows — so a project close, a project switch and a
quit all look identical from here: nothing at all.

### 2026-08-29 — the preset is not the channel

Measured, in one session, on this machine: five plugins loaded (CLAP, VST2 and VST3, the last two
through yabridge), settings changed, project saved, project reloaded.

```
editor trace installed on 26 slots
trace: AudioPlugin::GetNumberOfParameters   6
trace: AudioPlugin::UpdateParameterList     6
trace: AudioPlugin::Update                  6
trace: AudioPlugin::OnEditorIdle            6
trace: AudioPlugin::HideSubWindows          4
trace: AudioPlugin::GetResourcePath         3
trace: AudioPlugin::GetEffectRect           3
trace: AudioPlugin::UpdateEffectEditTitle   1
trace: AudioPlugin::StorePreset             0
trace: AudioPlugin::LoadPreset              0
```

The zero is a real zero. The slot count says 26, so both new entries were patched in, and
`GetNumberOfParameters` is a `BMDAudioPluginImpl` method reached through the same vtable copy by
the same mechanism — it fired. The trace cap only limits how many calls are *logged*, never how
many happen, and the first six always log. `AudioPluginPreset` is the preset menu's object.
Resolve does not put an effect through it to save a project.

The file store is therefore not a stopgap for a better channel that exists. It is what exists.

What the same log does hand over is the next lead: `GetNumberOfParameters` fired, so Resolve asks
our effect about its parameters. If the values it reads are what lands in the project, then
publishing the hosted plugin's parameters as the effect's own is the per-instance channel — and it
would bring automation, which the file store never can. Measuring that means overriding four slots
(`GetNumberOfParameters` +0x2c8, `GetParameterName` +0x2d0, `GetParameterValue` +0x318,
`SetParameterValue` +0x310) and reading a saved project back. Not attempted yet.

The other half of the same session: save and restore work end to end, first time, for all three
formats.

```
state: restored 216 bytes into "PodcastPlugins TRACK"
state: restored 2517 bytes into "soothe2"
state: restored 3311 bytes into "Clarity Vx - DeReverb Mono"
state: restored 3477 bytes into "NS1 Mono"
state: restored 2320768 bytes into "smartEQ4"
```

Two classes out of one WaveShell got two files, so the path-plus-class key holds. smartEQ4's state
is 2.3 MB, which is a 2.3 MB write every ten seconds while somebody is actively tweaking it.

Idle cost, same session, five plugins hosted and no editor open: one bridge thread exists,
`fxb-tick`, at **0.31 s of CPU over 321 s alive — 0.10% of one core**. `fxb-xpump` had not started
because no editor was open, and `fxb-carla` cannot start at all. With no plugin instance at all,
the bridge starts no threads: the tick is registered by a plugin wrapper's constructor. So the
20-25% idle CPU reported from another machine is not one of these loops.

### 2026-08-29 — settings, per instance

v0.2.0 keyed the settings store by the plugin. Delirio asked the obvious question the same
day: he runs a chain with the same compressor and the same EQ twice at different settings, and one
file per plugin gives both copies whichever was touched last.

An instance needs an identity that survives a save and a reload, and Resolve gives an effect none.
`AudioPluginPreset` was already ruled out above. `BMDAudioPluginImpl::GetClipID` looked promising -
it is a plain load from `this+0x1c8`, so it costs one `SafeRead` to try - and it is **zero on all
fourteen effects of a session**. It is a clip id, and a track effect has no clip.

What is left is the position in the chain, and the trap in it is that nothing here is ever freed.
Counting every effect ever claimed gives the second project's first EQ the number 1, and the same
project reopened a third number again. So the count runs over the effects that are still **live**,
and liveness is read off the object itself:

```c
bool EffectIsLive(const ClaimedEffect& effect)
{
    unsigned long word = 0;
    if (!SafeRead(effect.primary_base, &word, sizeof(word))) return false;
    return word == reinterpret_cast<unsigned long>(g_our_vtable + 0x010);
}
```

A claimed instance carries our vptr in its primary sub-object. Once Resolve frees it, that word is
either unmapped or somebody else's, and `SafeRead` says so without faulting. The same test now also
stops the save thread snapshotting plugins that no longer belong to anything, which it had been
doing on a ten-second timer since the store was written.

Tested with two Waves F6-RTA Mono on one track, set to visibly different curves:

```
743–942     pass 1: the project loads, five plugins, no F6
43455/43494 two F6 added by hand - no restore, no files yet
43590–43748 pass 2: the project reloads, seven effects
  43610 hosting "F6-RTA Mono"  ->  43611 state: restored 2391 bytes into "F6-RTA Mono" #0
  43666 hosting "F6-RTA Mono"  ->  43667 state: restored 2373 bytes into "F6-RTA Mono" #1
```

Two files, two different sizes, the right numbers. The sizes are the proof: one shared file would
have restored the same bytes twice.

The liveness test is what that second pass actually exercises. All five effects from pass 1 are
still in the array, and the two F6 sit at chain positions 2 and 4 rather than next to each other.
A broken liveness test would have handed `pp-track` the number 1 and the F6 pair 2 and 3.

What it cannot do is survive a rearranged chain. Insert an EQ ahead of two others and everything
after it shifts up a number, so they come back wearing each other's settings. Appending to the end
and removing from the end are both safe. Better needs an identity from Resolve, and two candidates
are now measured and dead.

### 2026-08-29 — the parameter loop is real

The settings store runs on a timer because a project save asks the effect nothing: `StorePreset`,
`LoadPreset` and `GetParameterValue` all fire zero times on a Ctrl+S while other traced slots on
the same object fire normally. Jay's objection was the right one - a VST saves its state on every
other host, so how?

Because Resolve treats a built-in effect as a slave. It owns the parameter values, pushes them in
with `SetParameterValue`, and never needs to ask. The class that asks a *plugin* for opaque state
is Resolve's VST host, and that class is not in the Linux build at all:

```
rmap find "VstPlugin|VSTPlugin|VST3|AudioUnit"      -> no symbol matches
rmap find "SetPluginState|GetPluginState|GetChunk"  -> no symbol matches
```

So there is nothing to hook and nothing to impersonate. What there is instead is the loop Resolve
already runs for its own effects, and every link of it is now measured:

| Link | Evidence |
|---|---|
| Editor moves a control, `performEdit` reaches us | `moved parameter 12 to 0.5031`, per instance |
| We call `NotifyParameterUpdate` (+0x0c0) | 8 sent, 8 returned, no hang |
| Resolve reads the value back | `GetParameterValue` 6 calls - **0 in every prior session** |
| Resolve writes it into the project on Ctrl+S | how the stock Delay's parameters persist |
| Resolve pushes it back at load | 24 `knob:` lines with non-default values |

The third row is the one that was unknown, and it is the one that makes the design buildable. It
also settles that calling into Resolve from the plugin's own thread is survivable here - the notify
is capped at eight and logged on both sides precisely because it is the shape of the v0.1.1
deadlock.

What remains is construction, not discovery: the effect publishes the carrier's six Delay
parameters, so only six indices mean anything. Publishing the hosted plugin's list instead -
`GetNumberOfParameters` at +0x2c8, `GetParameterName` at +0x2d0, and forwarding values both ways -
puts the settings in the project, saves them with Ctrl+S, and makes them automatable. The file
store becomes a fallback for what the parameter list cannot carry.

The experiment stays in the tree behind `FXBRIDGE_NOTIFY_PARAM=1`, off by default.

## 2026-08-29 — the parameter route is a dead end, and the one call that has no bounds check

The previous entry ended by saying the next piece of work was publishing the hosted plugin's
parameter list as the effect's own, so the settings would live in the project rather than in a file
on a timer. That was built, measured, and it does not work. This entry is the measurement, because
the idea is obvious enough that somebody will have it again.

### What was built

A probe behind `FXBRIDGE_PARAM_PROBE=1`. Every claimed effect answers `GetNumberOfParameters` with
four more than the carrier owns, answers `GetParameterName` for those four, answers
`GetParameterValue` from a small array, and records anything Resolve pushes back through
`SetParameterValue`. The four seed values come from `FXBRIDGE_PROBE_SEED`, so run one and run two
can be told apart — without that, a value pushed back could be Resolve echoing what it had just
read from us in the same session, and the test would prove nothing.

### What Resolve does

It plays along much further than expected:

* it accepts a parameter count no built-in effect has, and enumerates every index;
* it calls `GetParameterName` on each one and takes the `std::wstring`;
* it reads every value through `GetParameterValue`, both at load and later;
* it saves the project, and stores **none** of them.

Two complete cycles — seed, load, Ctrl+S, quit, restart with a different seed — once with the four
unnamed and once named `FXB Probe 0..3`. Neither pushed a single value back. In the same logs the
carrier's own seven parameters came back on every load with non-default values, twenty-four
`knob:` lines of them, which is what proves the `SetParameterValue` hook was live and listening.
The negative is measured, not assumed.

### Why

What Resolve persists is tied to the real `BMDControlParameter` objects in the vector at
`this+0x2c8`. `GetNumberOfParameters` is derived from that vector — `(this+0x2d0 - this+0x2c8) / 8`
— so overriding it changes the count and creates no parameter. A count is not a parameter.

The real way in is exported: `ExposeControl(BMDControlParameter*)` at `0x4f4400`, with
`HideControl`, `UpdateParameterList` and `BindParameterByName` beside it. What is **not** exported
is any constructor or vtable for `BMDControlParameter` itself, so using them means building that
object by hand from its layout. That is a much larger job than a vtable override, and nobody should
start it believing the override route was left untried.

### The call that has no bounds check

`GetControlType` at `+0x2d8`, thunk at `+0x900`:

```
4efdd9:  mov 0x2c8(%rdi),%rcx      # the control vector
4efde0:  mov (%rcx,%rax,8),%rax    # vec[index], with nothing checked
4efde4:  mov 0x18(%rax),%eax       # <- the fault, at +0x24, rax == 0
```

Every function in `BMDAudioPluginImpl` that indexes that vector was read mechanically, asking one
question: after the vector is loaded, does a conditional branch execute before the indexed load?
Twenty-eight branch first. `GetControlType` does not. Fairlight walks `0` to
`GetNumberOfParameters() - 1` calling it, from `EffectImpl::CreateLinkToStudio` under
`StudioModel::Deserialize`, so raising the count without answering this call kills the project
load. Overriding it, and answering probe indices as the carrier's own control 0, made the load
clean: seven effects, zero faults, across four subsequent runs.

### Two wrong claims, and what they cost

Both were the same mistake — asserting a guard without reading it.

* The first crash was blamed on `NotifyParameterUpdate`, on the reasoning that it calls out to a
  listener the class does not own. The next run crashed with it switched off.
* Then a scanner scored `GetControlType` as guarded, because it counted the `ja` that compares the
  control *type* to 4 — after the dereference. The corrected rule, "a branch before the indexed
  load", names exactly one function, and it is the one in the backtrace.

Each wrong claim cost a project load and a Resolve restart.

### The fault handler, which is the part worth keeping

Neither crash left a core: `core_pattern` pipes to `systemd-coredump`, which drops a process this
size. So both were diagnosed by guessing, twice, wrongly. The bridge now installs a handler for
`SIGSEGV`, `SIGBUS`, `SIGILL` and `SIGFPE` on its own alternate stack, formats by hand and writes
with `write()` rather than through the logger — a fault inside `malloc` would deadlock in
`vfprintf` — dumps `backtrace_symbols_fd` to stderr, which Resolve captures into
`ResolveDebug.txt`, then restores the previous handler and re-raises so Resolve's own reporting
still runs. It is on by default and costs nothing until something faults. The very next crash named
its function, its caller and its offset in one line.

`FXBRIDGE_CRASH_TRACE=0` switches it off.

## 2026-08-29, later — the preset channel, and two retractions

Two claims in this log were wrong, and both were wrong the same way: a tool was silent and the
silence was written down as a fact.

**"No VST host exists in the Linux build."** `rmap find "VstPlugin|VSTPlugin|VST3|AudioUnit"`
returned no matches, and that became "there is nothing to hook and nothing to impersonate".
`nm -DC /opt/resolve/libs/*.so | grep -c VSTPlugin` returns **198**, all in `libFairlightPage.so`,
and `rmap vtable VSTPlugin` prints the whole class — the same tool would have answered if it had
been asked a different way. `VSTPlugin` carries the members of a VST2 host: `Load`, `Dispatch`,
`IsWaveShell`, `StorePreset`, `LoadPreset`.

**And then that correction overshot in its turn.** It first read "`VSTPlugin` is a complete VST2
host", which a symbol list cannot show. What is measured is a symbol list plus the bodies of three
methods — `VSTPlugin::StorePreset`, `VSTPlugin::LoadPreset` and `VSTHost::LoadPlugin`. Nothing here
has run any of it. Correcting a claim is not a licence to make a bigger one in the opposite
direction.

**"StorePreset and LoadPreset fire zero times."** Traced on `+0x380` and `+0x388`. Those are the
primary vtable slots. Resolve holds an `AudioPlugin*`, so it calls the thunks at `+0x990` and
`+0x998`. The offsets were derived rather than guessed: in `VSTPlugin`'s own vtable the second
group's vptr is at `symbol+0x238` and its `StorePreset` thunk at `symbol+0x578`, so the call site
reads `vptr+0x340` — and `EffectsController::SaveEffectPreset` has exactly that call at `0x14ac5fa`.
`0x990 - 0x340 = 0x650`, the carrier's AudioPlugin group, cross-checked against `HasEditor`
(`0x718 - 0x0c8 = 0x650`).

### The path, read statically

```
EffectsController::StudioModelSerialize
  -> SaveEffectPresets(EffectPresetMap&, ModuleIds, int, bool)
       skip if the effect name is L"SYSTEM", L"REWIRE" or L"INTERNAL"
       -> IsDirty()       vptr+0x420, false skips the effect
       -> StorePreset()   vptr+0x340, false skips it too
       -> name, inputs, MIDI
       -> SetDirty(false) vptr+0x418, cleared once the preset is taken
  -> operator<<(BinaryStream&, EffectPresetMap const&)
```

`AudioPlugin::SetDirty` is one byte and a walk upwards:

```
mov  %sil, 0x99(%rdi)     the flag
mov  0x40(%rdi), %rdi     the parent
cmp  %rax, %rdi
je   done                 a plugin that is its own parent stops here
call *0x418(%rax)         otherwise the parent is marked too
```

and `IsDirty` is `cmpb $0x0, 0x99(%rdi)`. A null parent would be dereferenced on the line after the
compare, so any code that calls `SetDirty` must read that word first.

### Measured

* `IsDirty` asked 7 times per save, once per hosted effect.
* Marking one effect by hand: six answered false, that one answered true, and `StorePreset` fired
  on exactly it and returned true.
* Answering true for every effect of ours: `IsDirty` 7, `StorePreset` 7, no faults.

### Why the flag is set unconditionally

Driving it from a detected change does not work. The store notices a change by comparing the
plugin's state blob against the last one, and two Waves F6-RTA edited by hand both returned a
byte-identical blob while smartEQ4 returned a changed one — the "known miss" carried since v0.2.1.
Chaining the dirty flag to that inherits the blindness. Answering true always costs one preset per
save, and Resolve clears the flag itself, so nothing accumulates.

## 2026-08-29, later still — the chunk rides, and the gate is project-wide

Both questions from the entry above are now measured.

**The chunk rides.** `AudioPluginPreset` carries a type tag at `+0x10`: 0 for a VST parameter
table, 1 for a VST opaque chunk, 2 for a Blackmagic effect. `EffectPresetHeader2` copies the
length from `+0x08` into `hdr+0x04` and the tag from `+0x10` into `hdr+0x08`;
`LoadAudioPluginPreset` copies both back and reallocates the buffer. The length is a `uint32` on
the way through, so the ceiling is 4 GiB. An opaque blob is a first-class shape in the project
format, not something smuggled.

The bridge does not claim the tag, because `BMDAudioPluginImpl::LoadPreset` never reads it — it
validates the pointer, the length, and `*(uint32*)payload >= 2`, and nothing else. The chunk is
appended behind the carrier's payload with a footer, and on load the length is set back to the
carrier's own for the stock call. That removes the last unmeasured dependency: whether the stock
parser tolerates trailing bytes. It never sees them.

Verified with eight effects and the file store off: eight attached, eight restored, smartEQ4's
2,320,783 bytes included, zero faults.

**The buffer is reallocated through `dlsym(RTLD_DEFAULT, "_Znam")`, not our own `new[]`.** Checked
against `/proc/<pid>/maps`: `operator new[]` in this process resolves into
`/opt/resolve/libs/libtbbmalloc_proxy.so.2`. Resolve replaces the global allocator with Intel
TBB's, so the payload is on neither libstdc++'s heap nor libc++'s.

**The gate is project-wide.** A plugin-only change followed by Ctrl+S produced zero `StorePreset`
calls. A fader on a track carrying no effects at all produced eight, one per effect in the project,
and the plugin-only change from the previous test was captured on that save. So nothing was ever
lost; it waits for the next thing Resolve can see.

**What would close it, and why it stays open.** The path a real knob takes is
`EDLEffectImpl::CreateEffectUndo` → `new LoadPresetEDLPluginIncrementalChange(...)` →
`UndoManager::AddIncrementalChange`. `CreateEffectUndo` is a no-op everywhere else:
`BMDChainFX::CreateEffectUndo` and `Effect::CreateEffectUndo` are 36 bytes each, a stack canary and
`ret`. Reaching the `EDLEffectImpl` runs through `AudioEngineController::Instance()` → `vtable+0x460`
→ `+0x28` for the `EDLEffectsController`, then a lookup by clip id — and
`BMDAudioPluginImpl::GetClipID` reads zero on every track effect.

Two dead ends closed on the way. `UndoManager::ActuateChange` is not a general mark-modified: it
acts only on `ModuleIds == 9` with `ParameterIds == 0x3e1` and writes one word to
`g_Studio()+0x610`. And `BMDAudioPluginImpl::TouchPluginParameter` reads the index out of the
binding and then calls slot `+0x2f8`, which is `SidechainEnabled()`.

**A rule that was backwards, and what it cost.** The file store and the project are reconciled by
stamp, newer wins. The first version treated an unstamped project chunk as the oldest possible,
so every chunk written before the stamp existed lost to any file — and a run loaded settings from
an earlier session over the ones the project actually held. Unknown now means the project wins: it
is the store with real per-instance identity.

**Also useful:** `AudioPluginPreset` is 160 bytes, zero-initialises to a valid empty object, and
its destructor frees the three strings and the string vector but **not** the payload at `+0x00`.

## 2026-08-29, later again — the editor opens when it is asked for

A tester's list, and the measurement that collapsed most of it into one cause.

The bridge opened every plugin GUI at project load. It did that because a comment in `proxy.cpp`
said Resolve never calls `InitializeEffectEdit` with the stock panel suppressed, so there would
otherwise be no way to open a window at all. **That was wrong.** One click on the effect in the
Audio FX panel logs `editor: shown (AudioPlugin::InitializeEffectEdit)`. The 2026-08-24 reading
watched the primary slot; the one that fires is the AudioPlugin base at `+0x7b0`.

With a real open signal, three reported problems go away together: the mass-open at project load,
a window that reappeared after the modal was closed — a hide arriving with our window already gone
used to be re-read as a show — and a reopen that needed the modal closed twice. Loading the test
project went from eight windows to zero.

A fourth was unrelated and simpler. Editors were `_NET_WM_WINDOW_TYPE_UTILITY`, chosen so a tiling
layout would not claim them, and KWin excludes utility windows from the switcher **by type**. They
are dialogs now. `WM_CLASS`, the `InputHint`, and an `_NET_ACTIVE_WINDOW` request on map were all
missing outright, so the window also never took the keyboard. The class is Resolve's own,
`"resolve"`, because a desktop environment labels a window by looking its class up in the installed
desktop files and a class of our own matched nothing — KDE listed every editor as "unknown".

### Pinned, not started — draw the editor inside Resolve's own panel

`BMDAudioPluginImpl` carries what reads as a complete embedding protocol, not a "do you have a GUI"
interface: `InitializeEffectEdit(char const*, void*)` at `+0x250`, `GetEffectEdit` at `+0x260`,
`CloseEffectEdit` at `+0x278`, and `GetEffectRect(QRect&, bool&)`, `UpdateEffectRect`,
`CheckEffectRect`, `UpdateDPI`, `LockEditor` and `HideSubWindows` in the `+0x8xx` group. A `QRect`
in the signature says the panel lays the editor out in Qt coordinates.

The `void*` is almost certainly the parent widget. If it is a `QWidget*`, `QWidget::winId()` is
exported from the bundled `libQt5Widgets` and `XReparentWindow` does the rest.

**The cheapest first step is one log line:** print that pointer and probe whether its vtable lands
in `libQt5Widgets`. It costs one restart and either opens the path or closes it.

Three reasons this is pinned rather than started, two of them places this project has already lost
time: bridged Windows plugins are the fussiest windows here and were given their own top-level
window deliberately; `InitializeEffectEdit` arrives on Resolve's main thread, which is the thread
that froze on 2026-08-26 with 329 threads asleep; and the panel is one slot, so several editors
open at once would probably be lost.

### Still open

What marks the *project* modified from inside a plugin instance. See the clip-id problem above.

---

## 2026-08-29, evening — the scan can stop a start, and a stopped start left nothing to read

A tester on v0.2.5 reported Resolve hanging on startup, then dying. The log he sent stopped four
lines into Resolve's own startup and contained no `[fxbridge]` line at all. His plugin directory
held `libfxbridge.so` and nothing else — no cache, no deny list.

That absence is the finding. Both files are written *after* the scan loop, so their absence says
the loop never reached its end. The hang is inside it.

### Two things in the close path are outside the VST3 contract

v0.2.5 started closing each module after the scan read it, which took the same tester's yabridge
host count from 336 to zero. The close was right; how it closed was not.

`GetPluginFactory` hands the caller a reference. The host side of `vst3_plugin.cpp` has always
given it back — `Release(factory_)` in the destructor. `Vst3ListClasses` never did, and then
called `ModuleExit` underneath it. On yabridge `ModuleExit` tears down the Wine host, so the
reference still held pointed into a process that no longer existed.

`ModuleExit` also ran on every path out, including the one where `ModuleEntry` had refused. Every
native Linux VST3 sitting in the yabridge directory takes that path, and the comment in the file
says so.

Both are fixed. **Neither reproduces here**: a harness built against the v0.2.5 source, run over
this machine's twelve yabridge bundles, returned exit 0 with the host count back where it started,
and the Waves shell answered 718 classes in 1.606 s. They are fixed because they are wrong, not
because they are proven to be his fault. Saying which of those two it is matters more than the fix.

### The escape hatch was written after the thing it escapes

The deny list is the only way past a plugin that hangs the scan. It was written from inside the
loop, after the loop finished. A scan that finishes does not need it; a scan that hangs never wrote
one. Nothing in the template needs the loop — every path comes from the candidate scan, which is
already complete — so it is now written before the first module opens.

This one came from Jay asking whether `build.sh` should write the file to stop it crashing on a
no-op. It does not crash on a missing file: `ScanDenyList()` returns empty and `ScanDenied()`
returns false, verified in the source. The question was aimed one step off a real defect, and the
real defect was the ordering.

### A module that does not come back is now skipped

Reading a plugin's name runs that plugin's code inside Resolve. Before this, a plugin that hung or
faulted there was permanent: every start opened the same modules in the same order and stopped in
the same place, so everything after it was never seen. The scan could not get past its worst
plugin.

The scan now writes the module it is about to open to `fxbridge-scan-open.txt`, closed before the
plugin's code runs, and unlinks it when the module answers. A name still there at the next start
belongs to a module that did not come back: it goes to `fxbridge-scan-crashed.txt`, is skipped from
then on, and is named in the log.

Verified end to end, not by reading:

| step | result |
|---|---|
| `timeout -s KILL 0.6` mid-scan | exit 137, note left naming the Waves shell |
| next start | blamed it, wrote the file, `1 skipped`, scan completed, 55 plugins listed |
| start after that | still skipped, `0 had to be opened` |
| delete the file, start again | Waves shell answered 718 classes in 1.013 s |

The kill lands on the Waves shell because it is the slowest module — about a second, against 0–1 ms
for a native Linux plugin. That is the same reason a first start with a large Windows collection
looks like a hang: the count of modules that have to be opened is now logged before the loop, and
each module is named before it opens rather than after it answers.

**A start killed by hand blames whatever was open at that moment.** That is the price of not being
able to tell one dead process from another. It is why the skip is one line in a file the log names,
and one delete away from being undone, and why it is never silent.

### Still open

Whether any of this is the tester's actual hang. His log had no bridge line in it, so the module
that stopped him is still unnamed. v0.2.6 is the instrument that would name it.

---

## 2026-08-29, night — it was never a crash, and the cache was written where it could not help

v0.2.6's logging answered the v0.2.5 report in one paste. Every module in the tester's
MeldaProduction bundle takes **1.6 to 3.0 seconds**:

```
MDelayMB.vst3      answered 1 classes in 2753 ms
MSpectralDelay.vst3 answered 1 classes in 2652 ms
MTurboDelay.vst3   answered 1 classes in 1611 ms
```

That is a Wine host started and stopped, per plugin. Jay did the arithmetic from the same log:
about 330 plugins at ~1.5 s is roughly ten minutes of splash screen. Nothing was hung. Nothing
faulted. The scan was working exactly as designed and the design was wrong.

### The cache was written where it could not help

`ScanCacheStore()` ran after the loop. So the only person whose collection was slow enough to need
a cache was the one who could never build one: every start he interrupted threw away everything it
had just learned and began again at the first module. Ten minutes, from zero, every time.

It is written after every module now. Rewriting a 150-line TSV costs a few hundred microseconds
against the ~2 s the module itself cost, and it goes through the same rename, so a start stopped
halfway leaves a complete file rather than a torn one.

Measured with `timeout -s KILL 0.6`: **0 modules cached before, 19 after.**

This is the same shape of mistake as the deny list earlier the same day — a thing written at the
end of a loop that exists to survive the loop not finishing. Two instances in one file in one day
is a pattern worth naming: *anything that makes the next start cheaper must be durable against this
start not finishing.*

### The first module this code ever blamed was innocent

`fxbridge-scan-open.txt` named `MAGC.vst3`, and v0.2.6 would have skipped it forever. It was fine.
It was simply what happened to be open when he closed Resolve, ten minutes into a scan that was
never going to finish.

One appearance in flight is now a **suspect**, written to `fxbridge-scan-suspect.txt` and opened
again next time. Only a module in flight at the end of a second start is skipped.

The two changes hold each other up. A second strike means nothing unless the second start reaches
the same module — which it now does, because everything already answered is cached and skipped. A
genuinely fatal module is reached again within seconds; a module that was merely open when someone
reached for the window button is not.

| step | result |
|---|---|
| `SIGKILL` mid-scan | 19 cached, shell recorded as a suspect |
| next start | shell **opened again**, answered, no crashed file |
| shell in flight a second time | skipped, named in the log with the file to delete |
| scan runs to the end | suspect list cleared |

### What this cost

Three of the tester's evenings' worth of launches produced nothing readable, because I told him to
capture the log with `tee` on the terminal. This bridge writes to
`~/.local/share/DaVinciResolve/logs/ResolveDebug.txt`, which line 190 of the readme has said all
along. I inferred the destination from `fputs(..., stderr)` in `proxy.cpp` and never checked where
that stderr goes. The check took four seconds when I finally ran it, and it is the same rule as
never asserting a negative: **an instruction handed to someone else is verified on a real machine
first.**
