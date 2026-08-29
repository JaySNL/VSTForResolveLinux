# Where your plugin settings live

The full account, moved out of the readme so the front page stays a front page. Linked from
[README.md](../README.md).

**Your settings live in the Resolve project.** On by default since v0.2.3. A hosted plugin's own
state travels inside the effect's `AudioPluginPreset`, so it belongs to that effect, it moves with
the project, and it does not depend on a file sitting beside it. Nothing to turn on.

Measured on 2026-08-29, eight effects on one timeline with the file store switched **off** so that
nothing else could have supplied them: all eight attached on save and all eight came back on load,
smartEQ4's 2,320,783-byte chunk included. Zero faults on either half.

**How it rides.** Resolve's own VST host does exactly this, and reading it is what made the fix
possible. `VSTPlugin::StorePreset` picks between two payload shapes on one `AEffect` flag — a
parameter table when the plugin has no chunk, the plugin's opaque chunk when it has one — and both
travel in the same two fields, the pointer at `+0x00` and the length at `+0x08`, with a type tag at
`+0x10` saying which. `EffectPresetHeader2` copies that length and that tag into the serialised
header and `LoadAudioPluginPreset` copies both back, so an opaque blob round-trips through a
project file. The length is a `uint32` on the way through: the ceiling is 4 GiB.

The bridge does not claim the tag. `BMDAudioPluginImpl::LoadPreset` never reads it — it checks only
that the pointer is not null, the length is not zero and the first word of the buffer is at least
2. So the carrier's payload is left exactly as it is and the chunk is appended behind a footer:

    [ the carrier's own payload ][ the chunk ][ uint64 stamp ][ uint64 length ][ "FXBRIDG2" ]

On the way back in, the length is set to the carrier's own for the duration of the stock call and
restored afterwards, so the stock parser never sees a byte it did not write.

**What it still needs from you.** Resolve serialises the effects model only when it already thinks
the project changed, and an edit inside a hosted plugin's own window is invisible to it. Measured
on 2026-08-29: a plugin-only change followed by Ctrl+S produced **zero** `StorePreset` calls, while
moving a fader on a track carrying no effects at all produced **eight**, one for every effect in
the project. The gate is project-wide, not per effect — so nothing is lost, it waits for the next
thing you do that Resolve can see.

Closing that gate properly is a door we cannot open yet. The path a real knob takes is
`EDLEffectImpl::CreateEffectUndo` → `LoadPresetEDLPluginIncrementalChange` →
`UndoManager::AddIncrementalChange`, and pushing an undo entry is what marks the project modified.
That method is a no-op on every other class in the chain — `BMDChainFX::CreateEffectUndo` and
`Effect::CreateEffectUndo` are 36 bytes each and contain nothing but a stack canary and `ret` — and
reaching the `EDLEffectImpl` needs a clip id that `BMDAudioPluginImpl::GetClipID` reports as zero on
every track effect.

**The file store, for the one session the project cannot cover.** `FXBRIDGE_STATE_STORE=1` keeps
the old behaviour running alongside: every plugin is asked for its state about every ten seconds
and it goes to a file under `~/.local/share/BMDAudioPlugins/state/`. It does not care what Resolve
thinks, so it covers the case above — a session whose only change is inside a plugin window, ended
without touching anything else.

    FXBRIDGE_STATE_STORE=1 /opt/resolve/bin/resolve

The two are reconciled by time. Both carry a stamp — the project in its footer, the file in its
mtime — and the newer one wins. A project chunk with no stamp, written before v0.2.3, counts as
newer: the project is the store with real per-instance identity and the file store is the fallback.
Getting that backwards loaded settings from an earlier session over the ones the project held, and
cost a restart to notice.

**Read this before turning the file store on.** It identifies an instance by its position among the
effects hosting the same plugin, in the order Resolve loads them, because a file has nothing better
to go on. So **rearranging a chain shuffles its settings**. The project path does not have this
problem: a preset belongs to one effect. That difference is why the file store stayed opt-in.

**How the channel was found.** The earlier answer here was that a project save asks the effect
nothing at all, on the evidence that `StorePreset` and `LoadPreset` fired zero times across a
session. **That was measured on the wrong vtable slots** — `+0x380` and `+0x388`, the primary ones.
Resolve holds an `AudioPlugin*` and calls the thunks at `+0x990` and `+0x998`. Watching the right
pair:

* `LoadPreset` fires once per effect on every project load, and returns true;
* `StorePreset` fires on a save for every effect that answers `IsDirty` true;
* the payload is a parameter table — a version, a count, then a 64-byte name and a float each.

`VSTPlugin` in `libFairlightPage.so` overrides exactly those two calls. An earlier note in the
engineering log said no VST host class exists in the Linux build; it does — 198 symbols — and that
note is retracted where it stands.

The gate is `AudioPlugin::IsDirty()`, one byte at `AudioPlugin+0x99`, and Resolve clears it once it
has taken the preset. Our effects answer it true unconditionally, because the alternative is to
detect a change, and detection is exactly what does not work: two Waves F6-RTA edited by hand both
returned a byte-identical state blob while smartEQ4 returned a changed one. Answering true costs
one preset per save and removes that blind spot entirely. `FXBRIDGE_ALWAYS_DIRTY=0` turns it off.

**Publishing the plugin's parameters was tried, and it does not work.** The idea was to report the
hosted plugin's parameters as the effect's own, so the settings would live in the project and bring
automation with them. It was measured on 2026-08-29 with a probe that reported four parameters more
than the carrier owns, gave them values and names, and watched what came back after a save and a
restart. Resolve plays along further than expected and stops short of the only step that matters:

* it accepts a count no built-in effect has, and enumerates every index;
* it calls `GetParameterName` on each one and takes the string;
* it reads every value through `GetParameterValue`;
* it saves the project, and stores **none** of them.

Two full save-and-restart cycles, once with the four unnamed and once named `FXB Probe 0..3`, and
neither pushed a single value back through `SetParameterValue` — while the carrier's own seven came
back on every load, which is what proves the hook was live and listening. What Resolve persists is
tied to the real `BMDControlParameter` objects in the vector at `this+0x2c8`; a count is not a
parameter. `ExposeControl`, `HideControl`, `UpdateParameterList` and `BindParameterByName` are all
exported and are the real way in, but no constructor and no vtable for `BMDControlParameter` is, so
that is a much larger job than a vtable override. **The file store is what there is.**

**One thing that raising the count will do is crash the project load.** `GetControlType` at `+0x2d8`
is the one accessor in `BMDAudioPluginImpl` that indexes the control vector with no bounds check —
one of twenty-nine, every other one branches first — and Fairlight calls it for every index while
deserialising. Anyone repeating this experiment must override that slot and its thunk at `+0x900`
first, or lose the project load to a null dereference at `+0x24`.

**One known miss.** Some plugin editors do not tell the host when a single control is dragged.
Waves F6-RTA pushes all 84 of its parameters on a preset recall — which is saved correctly — and
reported nothing for one band moved by hand. Where the editor stays silent, nothing on this side
can see the change.
