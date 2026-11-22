# After a Resolve update: checking the bridge against a new Resolve

Maintainer procedure. What a user does after updating Resolve is in the
[README](../README.md#after-a-resolve-update).

**Every constant this bridge patches is a byte offset into one object**: the vtable of
`BMDStereoDelay` inside `libBMDAudioPlugins.so`. Blackmagic rebuilds that class on every release,
so a new Resolve can move a slot without changing anything a user would notice — and a bridge that
patches a moved slot replaces the wrong method. That failure is quiet: drag and drop stops working,
or a knob renames the effect.

So check it instead of finding out:

```sh
tools/check-offsets.py
```

It needs nothing but `python3` — no binutils, no debugger, no build. It prints the Resolve version,
every slot the bridge uses, and one of three verdicts each: **ok**, **MOVED** with the new offset,
or **GONE** if the method was renamed or removed. Exit code 0 means nothing moved. A move prints
the replacement constants ready to paste.

`--dump` lists every slot in the vtable, `--no-version` skips the version scan, and
`--emit-expected` regenerates the reference table from a Resolve you trust.

**It also answers an empty effect list**, which is the failure a moved offset does not cause. Three
things hide every plugin: the stock interface reporting a version the bridge refuses to wrap — it
forwards Resolve's own interface untouched rather than guess at a changed shape — a
`BMDPlugins.Path` that an update rewrote out of `config-fairlight.dat`, and an empty scan cache.
It also reads the size of one node in Resolve's plugin map — the bridge builds such a node by
hand, so a changed size means every entry it inserts has the wrong shape — and checks that every
library `libfxbridge.so` needs is present, because Resolve dlopens the bridge and falls back to
its own library **without saying so** when that fails. All of it is read without starting Resolve,
and a finding fails the run rather than only printing.

**Member offsets are not constants**: the channel counts, the bypass and dirty flags, the
per-plugin lock, the effect's name record and the panel. They are encoded inside instructions
rather than named in the symbol table, and 21.1.2 moved every one of them by eight bytes. So the
bridge reads each one out of the function that owns it when it loads, and refuses — forwarding
Resolve's own interface untouched — if any one is not where its instruction says. The bridge log
names every offset it read, on a line starting `layout:`.

The script decodes them by the same rules and prints what the bridge will read: **ok**, **ok,
moved from** the 21.1.0 number, or **REFUSED**, which fails the run. To run the bridge's own
decoding code over a library file, without Resolve:

```sh
tools/member-layout-test.sh /opt/resolve/libs/libBMDAudioPlugins.so 0x150 0x158 0x169 0x99 0x40 0x550 0x360 0x3a8 0x3ac
```

A new Resolve can be checked before installing it: `7z x -oOUT DaVinci_Resolve_Studio_X_Linux.run
libs/libBMDAudioPlugins.so libs/libFairlightPage.so`, then give either tool
`OUT/libs/libBMDAudioPlugins.so`. check-offsets reads the gate from the `libFairlightPage.so`
beside it; pass it `--no-version` too, because the version is read from the installed Resolve.
