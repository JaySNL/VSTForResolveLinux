# The Resolve 21.1 plugin gate

Why Resolve 21.1 and later load no external Fairlight plugin library unless started through
`resolve-with-fxbridge`, and how the launcher's preload opens the gate. The user-facing summary is in
the [README](../README.md#start-resolve).

**21.1 will not load any external Fairlight plugin library, and says nothing about it.** The effect
list holds Resolve's own effects and none of yours. Nothing is wrong with your install: the
configuration is read, the file is found, and the library is never opened.

`FLPluginHost::Initialize()` is the function that reads `BMDPlugins.Path`. In 21.0 it does so
immediately. In 21.1 it first asks Resolve's core interface a yes/no question keyed on the string
`"Debug"`, and returns without doing anything when the answer is no:

```
call  QString::fromAscii_helper("Debug", 5)
call  *0x1d0(%rbx)          ; a virtual on the core interface -> bool
test  %bpl,%bpl
je    +0xd4                 ; false: never read the setting, never load anything
```

That is a module gate, not a preference. The same virtual is called from 78 places in
`libFairlightPage.so`; the four that pass a literal pass `"Studio"`, `"ProxyGenerator"`,
`"RemoteControl"` and `"Debug"`. Writing a `Debug` row into
`configs/Fairlight/FLDebugSettings.csv` was tried first and changes nothing — that store is read by
`DebugSettingsManager::GetDebugSetting`, which returns a map rather than the boolean this gate
reads. **There is no setting to turn on.**

So start Resolve through the launcher instead:

```sh
~/.local/share/BMDAudioPlugins/resolve-with-fxbridge
```

It preloads `libfxbridge-gate.so`, which replaces that one conditional jump with NOPs **in memory**,
after Resolve has loaded the library and before it runs the function. Nothing on disk is modified,
no root is needed, Blackmagic's own files stay as they shipped, and not using the launcher undoes
it. On 21.0 and earlier the gate is not there, the preload finds nothing and writes one line saying
so.

The patcher anchors on the symbol `_ZN12FLPluginHost10InitializeEv` and requires the query and the
gate to appear exactly once inside it — **the gate's raw bytes occur twice in the library**, so a
plain signature search would be a coin toss. If the shape ever changes it refuses and says why,
rather than writing six bytes into the wrong place.

`tools/check-offsets.py` reports the gate as present or absent, so you can tell which Resolve you
have without starting it.

Measured on 21.1.0.0014 and 21.0.0.0048: unpatched 21.1 loads nothing at all; through the launcher
it lists 48 plugins, and 21.0 behaves identically with and without the preload.
