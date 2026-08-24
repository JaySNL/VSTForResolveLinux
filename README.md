# Fairlight FX bridge (DaVinci Resolve Studio 21, Linux)

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

