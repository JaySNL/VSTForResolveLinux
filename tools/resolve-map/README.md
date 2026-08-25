# resolve-map — read the offsets instead of hunting them

Every constant this bridge needs is already in Resolve's binaries. The vtable offsets, the
member offsets, which class implements what: all of it sits in the ELF symbol table and the
relocations. This reads it.

It exists because the alternative was measured and it is expensive. The 21 vtable offsets the
bridge uses on `BMDStereoDelay` were found by hand over two evenings, one crash at a time.
`build-index.py` reproduces all 21 in seconds. `BMDAudioPluginImpl::UpdateChannelCount` writing
the channel counts to `this+0x150` and `this+0x158` cost an evening and three wrong theories;
`rmap members` answers it in one command.

## Build

    python3 build-index.py        # ~10 minutes, writes resolve-map.sqlite (~100 MB, gitignored)

Indexes `libBMDAudioPlugins.so`, `libFairlightPage.so`, `libFairlightPanelAPI.so` and
`bin/resolve`: 102,613 symbols, 3,753 classes, 123,572 vtable slots.

## Ask it things

    ./rmap vtable BMDStereoDelay                    every slot, with its offset
    ./rmap vtable BMDStereoDelay --grep Edit        just the editor calls
    ./rmap slot BMDStereoDelay 0x4d0                what lives at that offset
    ./rmap where Process                            every class with that method, and its offset
    ./rmap members 'BMDAudioPluginImpl::UpdateChannelCount'
    ./rmap calls 'BMDStereoDelay::Process'          what it calls, virtual calls as slot numbers
    ./rmap find UpdateChannelCount                  search demangled names
    ./rmap class Fairlight                          list indexed classes

**Offsets are from the vtable symbol**, which is what this bridge's constants use. The primary
vptr points at symbol+0x10, past offset-to-top and the typeinfo pointer. `~` marks a thunk.

## Two bugs worth keeping in mind, because both looked like success

**Only reading `R_X86_64_RELATIVE`.** These binaries put vtable targets in `R_X86_64_64`
relocations, which name the target symbol; `RELATIVE` carries a bare address and is the
minority. Filtering to `RELATIVE` alone found 135 slots across 2,166 vtables — a 99% miss that
exited 0.

**No upper bound on a vtable.** `.data.rel.ro` is packed solid with relocations, so "walk until
N slots are unrelocated" never stops and each vtable runs into the next. That reported
3,839,112 slots from 2,166 vtables — about 1,770 each. A vtable ends where the next symbol
begins, and nothing else bounds it.

Both were caught by checking a number against something already known, not by reading an exit
code. The `BMDStereoDelay` layout is the regression test: if those 21 offsets stop matching, the
extractor is wrong, not Resolve.

## What it does not do

- **Member offsets are one level deep.** `rmap members` follows `this` and any register copied
  from it, and says which registers it treated as `this`. A pointer loaded *from* a member and
  then dereferenced is not followed.
- **No cross-library call graph.** `rmap calls` disassembles one function on demand. A whole-
  binary call graph would need a pass over 44 MB of `libFairlightPage.so`.
- **Stripped locals.** Only dynamic symbols exist in these binaries; a static helper that is not
  exported has no name here.
