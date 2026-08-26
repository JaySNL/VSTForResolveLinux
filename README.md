# VST for Resolve on Linux

**Run VST2, VST3 and CLAP audio plugins inside DaVinci Resolve on Linux.**

They appear in Fairlight's effect menu under their own names, sit in the right categories, load on a
track, open their own editor windows, and process audio live on the timeline. No bouncing a clip out
to an external editor and back.

Blackmagic does not support this. Their manual lists VST as macOS and Windows only, and the Linux
build ships no VST host at all — there is not one occurrence of `VSTPluginMain` or
`GetPluginFactory` anywhere under `/opt/resolve`. This adds one.

> **Not a supported product.** It patches structures inside Resolve's own process at run time. It
> can take Resolve down mid-edit. Save often, and do not put it on a machine you cannot afford to
> have crash.

## What works

| | |
|---|---|
| **CLAP** | audio + editor |
| **VST3**, native Linux | audio + editor |
| **VST3**, Windows via yabridge | audio + editor |
| **VST2**, Windows via yabridge | audio + editor |
| **Plugin names** | each effect carries its own name on the track, not "Delay" |
| **Categories** | Dynamics, EQ, Restoration, Reverb, Metering, Pitch, Guitar… alongside Resolve's own |
| **Shell plugins** | one file publishing hundreds — the Waves WaveShell exposes 718 — with a filter to pick which appear |
| Audio Units | not applicable, Apple-only format |

A scan on the development machine lists **130 plugins**, of which 75 come from one Waves shell.

**Verified working:** soothe2 · smartEQ4 · smartComp3 · Smooth Operator Pro · the ERA6 suite (14) ·
CrumplePop Complete (9) · Accentize SpectralBalance2 and dxRevivePro · Dragonfly reverbs ·
Airwindows Consolidated · Waves (Clarity Vx, RVox, DeBreath, F6, Vocal Rider, NS1, MaxxVolume,
Sibilance and more) · Carla · custom CLAP plugins.

**Known caveats:** two plugins draw their GUI but take no mouse input — Smooth Operator Pro (VST2)
and Accentize SpectralBalance2 (VST3). Carla reproduces both, so it is not this bridge. Resolve also
sometimes fails to exit cleanly; that one is undiagnosed.

## Install

Requires **DaVinci Resolve Studio 21** on Linux (the free version does not load this ABI), zlib,
Xlib and a C++17 compiler.

```sh
git clone --recurse-submodules https://github.com/JaySNL/VSTForResolveLinux.git
cd VSTForResolveLinux
./build.sh
```

Then point Resolve at it once, in
`~/.local/share/DaVinciResolve/configs/config-fairlight.dat`:

```
BMDPlugins.Path  ~/.local/share/BMDAudioPlugins
```

Restart Resolve. Your plugins are in the Audio FX panel.

**For Windows plugins you need a patched yabridge.** The version in your distribution's
repositories draws plugin GUIs correctly and then takes almost no mouse input — read
[`docs/yabridge.md`](docs/yabridge.md) before concluding this project is broken. Native Linux CLAP
and VST3 plugins need nothing extra.

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

Edits to the filter file take effect on the next Resolve start. No rebuild.

## How it works

Resolve on Linux loads its own Fairlight FX through a private Blackmagic plugin ABI, named by a
`BMDPlugins.Path` setting that nothing documents. This library registers itself through that ABI,
clones a stock effect's menu definition once per scanned plugin, and claims the instance Resolve
creates — then hosts a real VST2, VST3 or CLAP plugin behind it, with its own audio buffers, its own
editor window and no state shared between effects.

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
