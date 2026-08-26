# Windows plugins: yabridge, and the branch you need

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

