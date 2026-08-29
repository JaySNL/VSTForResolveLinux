# VST for Resolve on Linux

[![Latest release](https://img.shields.io/github/v/release/JaySNL/VSTForResolveLinux?label=release&color=2b7489)](https://github.com/JaySNL/VSTForResolveLinux/releases)
[![Licence: MIT](https://img.shields.io/badge/licence-MIT-blue)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-Linux%20%C2%B7%20Resolve%20Studio%2021-informational)](#install)
[![Changelog](https://img.shields.io/badge/changelog-read-lightgrey)](CHANGELOG.md)

**VST2, VST3 and CLAP plugins running inside DaVinci Resolve on Linux.** Not bounced out to an
external editor — loaded on the track, with their own GUIs, live on the timeline.

Blackmagic does not support this. Their manual lists VST as macOS and Windows only, and nothing in
the Linux interface loads a VST.

The binary is a different story. `libFairlightPage.so` carries a complete VST2 host — 198 symbols
under `VSTPlugin`, including the chunk calls a plugin uses to save its state. Reading that host is
what taught this project how Resolve stores plugin settings, and it is why your settings now live
in the project file. Whether that code can be reached from the Linux interface at all is a separate
question, and one this project has not answered.

*(An earlier version of this readme said the Linux build ships no VST host. That was wrong, and the
correction is worth more than the original claim was.)*

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

**Your plugin settings live in the Resolve project.** On by default since v0.2.3, nothing to turn
on. They belong to the effect, they move with the project, and they survive a chain that gets
rearranged.

Verified with eight effects on one timeline: all eight saved and all eight restored, including a
2.3 MB smartEQ4 chunk.

**One thing to know.** Resolve writes the effects model only when it already thinks the project
changed, and an edit made inside a plugin's own window is invisible to it. Nothing is lost — your
change is written on the next thing you do that Resolve *can* see, a fader or a clip. If you want a
session that changes nothing else to be covered too, start Resolve with `FXBRIDGE_STATE_STORE=1`.

The full account — how the channel was found, what it cannot do, and the two claims this project got
wrong on the way — is in [`docs/settings.md`](docs/settings.md).

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
  bridged effect stays black on purpose. Under Wayland both windows go through XWayland. It is
  listed in the window switcher and takes the keyboard when it opens; `FXBRIDGE_WINDOW_TYPE`
  accepts `dialog` (the default), `utility` and `normal` if your window manager wants otherwise.
- **An editor opens when you click the effect**, not when the project loads. Every plugin GUI used
  to be constructed during the project load, which is the last thing a large project needs.
  `FXBRIDGE_OPEN_ON_CLAIM=1` brings that back if a machine turns out to have no other way to open
  a window.
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


Every release, and what changed in it, is in [CHANGELOG.md](CHANGELOG.md).

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
