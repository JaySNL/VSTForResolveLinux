# Fairlight FX bridge

**Audio plugins inside DaVinci Resolve on Linux.** CLAP, VST2 and VST3 — native Linux builds, and
Windows plugins through [yabridge](https://github.com/robbert-vdh/yabridge) — appear in Fairlight's
effect menu, load on a track, and open their own editor windows. Adjust them live on the timeline
instead of bouncing a clip out to an external process and back.

Resolve supports none of this on Linux. Blackmagic's manual states VST is macOS and Windows only,
and the Linux build ships no VST host at all: there is not a single occurrence of `VSTPluginMain`
or `GetPluginFactory` anywhere under `/opt/resolve`.

> **This is not a supported product, and it is not safe by default.** It patches vtables and
> structures inside Resolve's own process at run time. A bad interaction can take Resolve down
> mid-edit. Save often, and do not put it on a machine you cannot afford to have crash.

## Status

Measured on **DaVinci Resolve Studio 21**, Linux, on one machine. Everything below is a run, not an
expectation.

| | |
|---|---|
| CLAP plugins | works — audio and editor |
| Native Linux VST3 | works — audio and editor |
| Windows VST2 via yabridge | works — audio and editor |
| Windows VST3 via yabridge | works — audio and editor |
| VST3 shells (one file, many plugins) | works — the Waves WaveShell publishes 718; a filter file picks which are listed |
| Effect categories | works — plugins appear under Dynamics, EQ, Restoration, Reverb and the rest |
| Audio Units | not applicable — Apple-only format |

A representative scan on the development machine lists **130 plugins**, of which 75 come from a
single Waves shell.

## How categories work

A Fairlight category is a **lookup, not a property**. `libFairlightPage.so` carries seven
zlib-compressed XML tables keyed by `"<name>:<id>"`:

```xml
<Effect id="De-Esser:1112360051" category="nr"/>
<Effect id="ERA 6 De-Breath:1682076214" category="nr"/>
```

An effect whose key is in no table has no category — **on every platform**. soothe2, smartEQ4,
CrumplePop, Dragonfly, Airwindows and all of Waves are absent from Blackmagic's list, so they are
Uncategorized on macOS too. The bridge adds a row per plugin it lists, at run time, in memory.

The table cannot simply be edited in place: it has 590 stream bytes and needs about 1,565, and the
next record starts 598 bytes on. Instead the enlarged table is written into a neighbouring block
that nothing reads on this platform, and one four-byte field in the resource tree is repointed at
it. **Nothing on disk is modified**, so a Resolve update has nothing to revert.

The tree is located by signature rather than by address, so it survives a version change or it
fails safely: the tables are found by decompressing candidates and reading their marker, and the
tree is the run of big-endian words carrying exactly the distances between them. Every offset it
matches must occur exactly once in the library, or the patch is refused. Every failure path leaves
the original bytes untouched.

Categories are assigned by name in `CategoryFor()` in `src/fx_categories.cpp` — edit that to taste.

The one thing that cannot be reproduced is the top-level **"VST" group** macOS and Windows show:
that grouping comes from the plugin *type*, and Linux Resolve has no VST type.

**Routes that do not work**, so nobody repeats them: `FairlightFXConfiguration.xml`
`<CategoryMask>` is written and ignored; `FXConfiguration.xml` `<Category>` is parsed at startup and
then erased; and these blocks are not Qt resources, so `LD_PRELOAD` on `qRegisterResourceData` never
sees them. Full reasoning at the top of `src/fx_categories.cpp`.

## Requirements

- DaVinci Resolve **Studio** 21 on Linux (the free version does not load this ABI)
- zlib, Xlib, a C++17 compiler
- For Windows plugins: **yabridge, patched — see the next section. This is not optional.**

## Windows plugins need an unreleased yabridge

If you want Windows VST2 or VST3 plugins, the yabridge in your distribution's repositories will
**not** work, and the way it fails wastes a whole evening if you do not know about it.

**The symptom:** the plugin loads, the editor window opens, and the interface draws perfectly — but
it takes almost no mouse input. Controls respond only where the pointer happens to sit near the
top-left of the *screen*, and even there with a slight offset. It reads like a focus or an XEmbed
problem, and it is neither.

**The cause**, quoted from [yabridge PR #462](https://github.com/robbert-vdh/yabridge/pull/462):
when the Wine window is reparented into a host window, *"Wine will interpret any local coordinates
as global coordinates."* Wine 9.22 changed editor embedding, and released yabridge 5.1.1 predates
the change. Anything from Wine 9.22 onwards is affected, in **every** host, not just this one.

**The fix is written but unreleased**, on the `new-wine10-embedding` branch. On Arch derivatives the
AUR carries it as `yabridge-wine10-git` and `yabridgectl-wine10-git`; otherwise build that branch
yourself. Confirmed here against Wine 11.15 with full, precise control of every plugin GUI.

Four things that will otherwise cost you time:

- **Build it with generic compiler flags.** `-march=native` from `/etc/makepkg.conf` miscompiles
  yabridge, and every Windows VST3 then dies with an identical
  `err:virtual:virtual_setup_exception stack overflow`, across unrelated vendors —
  [yabridge #449](https://github.com/robbert-vdh/yabridge/issues/449). Build against a copy of
  `makepkg.conf` with `-march=x86-64 -mtune=generic -O2`.
- **Run `yabridgectl sync` after any upgrade.** The per-plugin bridge files are *copies*, so an
  upgrade does not reach them by itself.
- **`yabridgectl --version` still prints `5.1.1` on this branch.** Do not trust it. Read the
  `Initializing yabridge version` line in Resolve's log instead.
- **`editor_coordinate_hack` and `editor_xembed` no longer exist** on this branch. Do not set them.

`yabridge.toml` is read from the **plugin's own directory**, searching upward — never from
`~/.config/yabridge`, which belongs to yabridgectl. The `config from:` line in the log says which
file was actually used.

Two plugins still take no mouse input after all this — Smooth Operator Pro (VST2) and Accentize
SpectralBalance2 (VST3) — while soothe2, smartEQ4, smartComp3 and ERA6 are fine. Thirteen
hypotheses were measured and refuted, and **Carla reproduces it**, so it is not this bridge and not
the branch above.

## Build

```sh
git clone --recurse-submodules <this repo>
cd resolve-fx-bridge
./build.sh
```

`build.sh` compiles, refuses to install a library with undefined symbols, and installs to
`~/.local/share/BMDAudioPlugins/`. Resolve loads it at start, so restart Resolve afterwards.

Point Resolve at the directory once, in
`~/.local/share/DaVinciResolve/configs/config-fairlight.dat`:

```
BMDPlugins.Path  ~/.local/share/BMDAudioPlugins
```

## Configuration

**Which formats are listed** — `FXBRIDGE_SCAN_FORMATS`, default `clap,vst2,vst3`.

**Which plugins inside a shell are listed** — `~/.local/share/BMDAudioPlugins/fxbridge-shell-allow.txt`,
one substring per line, matched against the plugin's class name. Without the file a shell
contributes only its first plugin, so a 718-plugin shell never floods the menu. For example:

```
# Waves: these 25 patterns select 75 of the WaveShell's 718 plugins
Clarity Vx
DeBreath
RVox
Vocal Rider
```

Edit and restart. No rebuild.

## Known problems

- **Two plugins take no mouse input** — see the yabridge section above.
- **Resolve sometimes does not exit cleanly.** Undiagnosed. Reading a live stack needs
  `sudo sysctl kernel.yama.ptrace_scope=0`.
- **`IPluginFactory3::set_host_context` is deliberately disabled.** The factory accepts it and the
  next call never returns, hanging project load at 100%. Do not re-enable without understanding the
  callback path; the reason is written where the call would go.

## Prior art

**No published implementation was found.** `BMDAudioPluginFactory` and `QueryPluginList` return zero
hits across GitHub's indexed code, and four web searches turned up only the adjacent tools —
[yabridge](https://github.com/robbert-vdh/yabridge), [LinVst](https://github.com/osxmidi/LinVst) and
[airwave](https://github.com/psycha0s/airwave) let a *DAW* load Windows plugins, which is a
different problem; this project uses yabridge underneath.

That is a search result, not a fact about the world. GitHub code search covers indexed public
repositories on default branches, so it cannot see private repos, other forges, or a patch in a
forum post. If you have done this before, say so and it will be credited here.

## Licensing and legality

MIT — see [`LICENSE`](LICENSE). Third-party notices in [`THIRD-PARTY.md`](THIRD-PARTY.md).

**Nothing from Blackmagic Design is copied, redistributed or modified on disk.** This loads into
Resolve through Resolve's own documented-by-observation `BMDPlugins.Path` mechanism and patches
structures in its own process memory at run time. Uninstalling is deleting one file.

No Steinberg SDK is used. VST3 goes through [travesty](https://github.com/DISTRHO/DPF), DPF's
clean-room C headers (ISC); VST2 through a clean-room ABI written for this project. The VST2 SDK was
withdrawn in October 2018 and no licence is available for it, which is why no VST2 header is
included here.

This is interoperability work: making software talk to plugin formats its vendor chose not to
support on one platform. In the EU, [Directive 2009/24/EC Article 6](https://eur-lex.europa.eu/legal-content/EN/TXT/?uri=CELEX%3A32009L0024)
expressly permits decompilation for interoperability. Resolve's EULA is a separate, contractual
matter — read it and make your own decision before using this.

Worth noting: there is **no licensing obstacle to Blackmagic doing this themselves**. The VST3 SDK
is MIT-licensed, with no agreement to sign and no per-platform terms. The Linux gap is a product
decision, not a legal one.

## Documentation

- [`docs/engineering-log.md`](docs/engineering-log.md) — how the private ABI was found, and every
  dead end in order. Long, and the dead ends are the point.
- [`docs/2026-08-25-what-changed.md`](docs/2026-08-25-what-changed.md) — a night's ledger, including
  the thirteen refuted input hypotheses.
