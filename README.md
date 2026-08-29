# VST for Resolve on Linux

**VST2, VST3 and CLAP plugins running inside DaVinci Resolve on Linux.** Not bounced out to an
external editor — loaded on the track, with their own GUIs, live on the timeline.

Blackmagic does not support this. Their manual lists VST as macOS and Windows only, and the Linux
build ships no VST host at all — there is not one occurrence of `VSTPluginMain` or
`GetPluginFactory` anywhere under `/opt/resolve`.

![Four plugins open at once inside Resolve: PodcastPlugins TRACK, Waves NS1, Waves Clarity Vx DeReverb and soothe2, with the mixer showing the effect chain](docs/media/plugins-running.png)

Four plugins open at once, on one track. Look at the **Effects** column in the mixer: `pp-track`,
`soothe2…`, `Clarity V…`, `NS1` — each effect carries its own name, and each is a real plugin
processing real audio.

### Sixty seconds of it, warts included

![Loading several plugins in a row and driving smartEQ4's controls inside Resolve](docs/media/demo.gif)

[Full recording (66s, with audio)](docs/media/demo.mp4) — several plugins loaded one after another,
ending with smartEQ4 running a live spectrum analyser while its controls are driven.

It is not a clean take, deliberately. **One plugin opens with an empty editor and only draws after
the effect is removed and undone**, and the recording keeps that in. The others open first time.

## It works

**Plugins land in Resolve's own categories**, mixed in with the built-in Fairlight FX. `De-Esser`,
`De-Hummer`, `Noise Reduction`, `Limiter`, `Multiband Compressor` and `Soft Clipper` below are
Blackmagic's; everything else is ours.

| Restoration | Dynamics | Metering |
|---|---|---|
| ![](docs/media/category-restoration.png) | ![](docs/media/category-dynamics.png) | ![](docs/media/category-metering.png) |

| What | Status |
|---|---|
| **CLAP** | audio + editor |
| **VST3**, native Linux | audio + editor |
| **VST3**, Windows via yabridge | audio + editor |
| **VST2**, Windows via yabridge | audio + editor |
| **Plugin names** | each effect named on the track, not "Delay" |
| **Categories** | Dynamics, EQ, Restoration, Reverb, Metering, Pitch, Guitar… |
| **Shell plugins** | one file publishing hundreds — the Waves WaveShell exposes 718 — with a filter to choose which appear |
| **Several at once** | four plugins on one track in the shot above; no shared state between effects |

**Verified working:** soothe2 · smartEQ4 · smartComp3 · the ERA6 suite (14) · CrumplePop Complete
(9) · Accentize SpectralBalance2 and dxRevivePro · Dragonfly reverbs · Airwindows Consolidated ·
Waves (Clarity Vx, NS1, RVox, DeBreath, F6, Vocal Rider, MaxxVolume, PAZ, Sibilance and more) ·
Carla · custom CLAP plugins. **130 plugins listed** on the development machine, 75 of them from a
single Waves shell.

## It does not work, or not yet

- **Two plugins draw a GUI but take no mouse input** — Smooth Operator Pro (VST2) and Accentize
  SpectralBalance2 (VST3). Carla reproduces both, so this is not the bridge. YMMV, i haven't been able to test EVERY vst yet.
- **A plugin editor sometimes opens empty**, and fills only after the effect is removed and the
  removal undone. Visible in the recording above. Not diagnosed; it is not tied to one format.
- **A plugin-only change does not make the project dirty.** Settings themselves are kept now, in
  the project — see *Settings between sessions*. What is missing is the nudge: Resolve serialises
  the effects model only when it already thinks something changed, and an edit made inside a
  hosted plugin's own window is invisible to it. The settings are not lost, they are written on
  the next thing you do that Resolve can see. `FXBRIDGE_STATE_STORE=1` closes the gap for a
  session that changes nothing else at all.
- **Higher idle CPU than stock Resolve on one machine** — reported as 20–25% against 0–3%, where
  Reaper hosting the same plugins is quiet. **Not reproduced.** Measured here with five plugins
  loaded and the timeline idle, the one bridge thread that runs costs **0.31 s of CPU over 321 s
  alive — 0.10% of a core**. With no plugin loaded the bridge starts no threads at all. The three
  it can start are named, so `top -H -p $(pgrep -f /opt/resolve/bin/resolve)` names the culprit
  instead of guessing: `fxb-xpump` (X11 events, 30 ms), `fxb-tick` (plugin idle, 16 ms), and
  `fxb-carla`, which cannot start at all in this build.
- **A plugin's GUI is a separate window**, not a panel inside Resolve. The inspector panel for a
  bridged effect stays black on purpose. Under Wayland both windows go through XWayland.
- **Resolve sometimes does not exit cleanly.** Undiagnosed.
- **No top-level "VST" group** like macOS and Windows show. That grouping comes from the plugin
  *type*, and Linux Resolve has no VST type. Categories work; a separate VST section cannot.
- **Audio Units** — not applicable, Apple-only format.
- **Tested on three machines**, all DaVinci Resolve Studio 21, and only one of them is mine. The
  other two each found a bug this release fixes — an editor that would not reopen after the window
  manager closed it, and an editor that never opened at all. Neither fix has been confirmed by the
  person who reported it yet.

> **Not a supported product.** It patches structures inside Resolve's own process at run time and
> can take Resolve down mid-edit. Save often.


## Install

Requires **DaVinci Resolve Studio 21** on Linux. The free version does not load this ABI.

There are two ways in. **Pick one and follow it to the end** — they do not interleave.

| | Method 1 — build it | Method 2 — download the binary |
|---|---|---|
| You need | the Carla dev headers, zlib, Xlib, a C++17 compiler | nothing but `curl` |
| Config file | written for you | you add one line |
| Best when | you want the newest code | you want it working in a minute |

---

### Method 1 — build it

`build.sh` compiles, installs **and** writes Resolve's config line for you.

**Step 1. Install the Carla development headers.** `build.sh` compiles `src/carla_host.cpp` against
`/usr/include/carla/includes`; without them it stops at `fatal error: CarlaNative.h`.

| Distribution | Package |
|---|---|
| Arch, CachyOS, Manjaro | `carla` (tested against 2.5.10-4.1) |
| Debian, Ubuntu, Linux Mint | `carla-dev`, from the [KXStudio repositories](https://kx.studio/Repositories) |

Reported on Linux Mint 22.3 by u/slangbein, who also supplied the fix.

**Step 2. Close DaVinci Resolve.** Resolve owns its config file and rewrites it on quit, so an edit
made while it runs is thrown away. `build.sh` refuses to touch the file while Resolve is open.

**Step 3. Build.**

```sh
git clone --recurse-submodules https://github.com/JaySNL/VSTForResolveLinux.git
cd VSTForResolveLinux
./build.sh
```

That compiles, installs to `~/.local/share/BMDAudioPlugins/libfxbridge.so`, and adds the
`BMDPlugins.Path` line to `~/.local/share/DaVinciResolve/configs/config-fairlight.dat` — keeping a
timestamped copy of your file first. Run it twice and nothing is duplicated.

`FXBRIDGE_NO_CONFIGURE=1 ./build.sh` skips the config step and prints the line instead.

**Step 4. Start Resolve.** The plugins are in the Audio FX panel.

That is the whole method. Nothing below Step 4 applies to you.

---

### Method 2 — download the binary

Every release carries a prebuilt `libfxbridge.so`, free of cost, next to the source it was built
from.

**Step 1. Download it.**

```sh
mkdir -p ~/.local/share/BMDAudioPlugins
curl -L -o ~/.local/share/BMDAudioPlugins/libfxbridge.so \
  https://github.com/JaySNL/VSTForResolveLinux/releases/latest/download/libfxbridge.so
```

**Step 2. Close DaVinci Resolve.** It owns the config file and rewrites it on quit, so an edit made
while it runs is thrown away. This step is not optional.

**Step 3. Add one line to the config file.** Open

```
~/.local/share/DaVinciResolve/configs/config-fairlight.dat
```

in a text editor, and add this line anywhere in it:

```
BMDPlugins.Path = /home/YOU/.local/share/BMDAudioPlugins/libfxbridge.so
```

**Replace `YOU` with your own username.** `whoami` prints it. The path has to be written out in
full: `~` and `$HOME` are not expanded here.

**Step 4. Start Resolve.** The plugins are in the Audio FX panel.

#### If Step 3 goes wrong, nothing tells you

Resolve quietly loads its own plugin library and the Audio FX panel looks exactly as it did before.
No error, no warning. Three things have to be right:

- The value is the path to the **`.so` file**, not the folder it sits in.
- The path is **absolute**, with your real username.
- The key only works in **`config-fairlight.dat`**. Resolve keeps it in `config.dat` across restarts
  and ignores it there.

This readme published the wrong form until 2026-08-26 — the folder, no `=`, and a `~` — which is
three failures out of three. Reported by Delirio, who also asked for these two methods to be
pulled apart.

#### What the prebuilt binary asks of your system

Compiled on Ubuntu 20.04, so it asks for `GLIBC_2.16`, `GLIBCXX_3.4.22` and `CXXABI_1.3.9` at most —
libstdc++ from GCC 6.1 onward, which covers Ubuntu 18.04, Debian 9, Rocky 8 and Fedora 25 and newer.
Read the floor back rather than trusting it:

```sh
objdump -T ~/.local/share/BMDAudioPlugins/libfxbridge.so \
  | grep -o 'GLIBC[X]*_[0-9.]*' | sort -uV | tail -3
```

Whether **Resolve itself** runs on distributions that old is a separate question, and one this
project has not tested.

---

### Nothing appeared?

Either method. Ask whether the bridge was loaded at all:

```sh
grep -c fxbridge ~/.local/share/DaVinciResolve/logs/ResolveDebug.txt
```

**Zero** means Resolve never loaded it: the config line is wrong, or this is the free version rather
than Studio. **Above zero** means it loaded, and these lines say what it found:

```sh
grep 'fxbridge.*scan:' ~/.local/share/DaVinciResolve/logs/ResolveDebug.txt | head
```

**Windows plugins need a patched yabridge.** The version in your distribution's repositories draws
plugin GUIs correctly and then takes almost no mouse input — read [`docs/yabridge.md`](docs/yabridge.md)
before concluding this project is broken. Native Linux CLAP and VST3 plugins need nothing extra.

## Choosing which plugins appear

**Formats** — `FXBRIDGE_SCAN_FORMATS`, default `clap,vst2,vst3`.

**Plugins inside a shell** — `~/.local/share/BMDAudioPlugins/fxbridge-shell-allow.txt`, one
substring per line, matched against the plugin's name. Without this file a shell contributes only
its first plugin, so a 718-plugin shell never floods the menu:

```
# Selects 75 of the WaveShell's 718 plugins
Clarity Vx
RVox
Vocal Rider
DeBreath
```

**Categories** — edit `CategoryFor()` in `src/fx_categories.cpp`.

Both take effect on the next Resolve start. No rebuild.

## Settings between sessions

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


## How it works

Resolve on Linux loads its own Fairlight FX through a private Blackmagic plugin ABI, named by a
`BMDPlugins.Path` setting that nothing documents. This library registers through that ABI, clones a
stock effect's menu definition once per scanned plugin, and claims the instance Resolve creates —
then hosts a real VST2, VST3 or CLAP plugin behind it, with its own audio buffers, its own editor
window and no state shared between effects.

- [`docs/categories.md`](docs/categories.md) — how a category is decided, and how it is set
- [`docs/yabridge.md`](docs/yabridge.md) — Windows plugins, and the branch you need
- [`docs/engineering-log.md`](docs/engineering-log.md) — how the ABI was found, and every dead end
  in order. The dead ends are most of the value.

## Prior art

No published implementation was found. `BMDAudioPluginFactory` and `QueryPluginList` return zero
hits across GitHub's indexed code, and web searches turn up only the adjacent tools —
[yabridge](https://github.com/robbert-vdh/yabridge), [LinVst](https://github.com/osxmidi/LinVst) and
[airwave](https://github.com/psycha0s/airwave) let a *DAW* load Windows plugins, which is a different
problem; this project uses yabridge underneath.

That is a search result, not a fact about the world: code search cannot see private repositories,
other forges, or a patch in a forum post. If you got here first, say so and you will be credited.

## Licence

MIT — see [`LICENSE`](LICENSE). Third-party notices in [`THIRD-PARTY.md`](THIRD-PARTY.md).

Parts of this were written with an LLM in the loop. The source and a working binary are both here,
free of cost, so anyone can check what it does rather than take the description on trust.

**Nothing from Blackmagic Design is copied, redistributed or modified on disk.** This loads through
Resolve's own plugin path and patches structures in its own process memory. Uninstalling is deleting
one file.

No Steinberg SDK is used: VST3 goes through [travesty](https://github.com/DISTRHO/DPF), DPF's
clean-room C headers (ISC), and VST2 through a clean-room ABI written for this project.

This is interoperability work — making software talk to plugin formats its vendor chose not to
support on one platform. In the EU, [Directive 2009/24/EC Article 6](https://eur-lex.europa.eu/legal-content/EN/TXT/?uri=CELEX%3A32009L0024)
expressly permits decompilation for interoperability. Resolve's EULA is a separate, contractual
matter; read it and decide for yourself.

For what it is worth, there is **no licensing obstacle to Blackmagic doing this themselves**. The
VST3 SDK is MIT-licensed, with no agreement to sign and no per-platform terms. The Linux gap is a
product decision, not a legal one.
