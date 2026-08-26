# How Fairlight categories work, and how the bridge sets them

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

