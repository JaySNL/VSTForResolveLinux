# 2026-08-25, 00:16 to 02:05 — every change, what it fixed, what it broke

Written because the session started circling: fixes were being built on theories, shipped, and then
replaced by the next theory. This file is the ledger. Each entry carries a **verdict**, and the
verdicts are the point — three of them are *refuted by measurement*, and those are the valuable ones.

Rule used throughout: a claim is **verified** only if a command or a log line proves it. Anything
else is **inferred** or **not checked**, and says so.

## The two crash families

Six Resolve crashes tonight. They are not one bug. Splitting them was the first thing that should
have happened and did not.

| Time | Top frame | Family |
|---|---|---|
| 01:22:28 | `MixPole::ApplyGainSmooth+0x188` | A — Fairlight mixer |
| 01:46:44 | `pp-track.clap(+0x109ad4)` over `libfxbridge.so`, on a `std::thread` | B — our timer thread |
| 01:51:19 | `MixPole::ApplyGainSmooth+0x188` | A |
| 01:56:15 | `AddChannelSourceToChannelBus+0x867` | A |
| 02:01:24 | `pp-track.clap(+0xacadd)` over `libfxbridge.so`, on a `std::thread` | B |

A seventh crash, at ~01:04, is excluded: frames #0 and #1 were inside `backtrace_symbols`, so
Resolve's own crash handler had died while unwinding and the dump records the handler, not the
fault. **Read frame #0 before believing frame #5.**

## Family A — the Fairlight mixer

**The fault, verified by disassembly.** `AddChannelSourceToChannelBus+0x867` is:

    mov  %r12,%rax
    shl  $0x9,%rax          # r12 * 512 - a stack table, 64 pointers per row
    add  %rsp,%rax
    add  $0x280,%rax
    mov  (%rax,%rbp,8),%r15 # table[source][channel]
    movss (%r15),%xmm0      # <- the fault

A channel index reached a table slot that was never filled. `MixPole::ApplyGainSmooth+0x188` is the
same shape one frame down: `mov (%rdi),%rax` then `call *0x20(%rax)` on an object whose null check
had already passed, so it was non-null garbage rather than null.

### Theory A1 — the hosted plugin wrote past its block

pp-track is a limiter with lookahead; such a plugin can round its work up to an internal block and
write more than `frames`. Resolve's block buffer holds exactly `frames` floats.

*Change:* the plugin no longer touches Resolve's buffers at all. It runs over our scratch, which has
`kMaxFrames` of headroom per channel, and exactly `frames` samples are copied back. A marker value
sits one sample past every block and is read back afterwards.

**Verdict: REFUTED.** `grep -c "wrote past the"` on the log returns **0**. The marker survived every
block and the crash recurred at 01:51 and 01:56. *The change is kept* — it is the right boundary,
and it is what converted the theory into a measurement instead of an argument.

### Theory A2 — Resolve's output buffer is shorter than `frames`

**Verdict: REFUTED on inspection, before any code changed.** `UsableChannels` already read- and
write-probes the **first and last** sample of every channel.

### Theory A3 — the emptied panel corrupts Fairlight's resource tree

`BridgeGenerateUserInterface` truncates the tree's item vector, and that is the one place we edit a
Fairlight structure. The README already warned this path had crashed once.

**Verdict: REFUTED by the dumps.** The 01:22 crash happened with `FXBRIDGE_EMPTY_PANEL` **off** and
the 01:56 crash with it **on**. Identical frames.

### Cause A4 — we indexed past the end of Resolve's pointer array

`UsableChannels` walked the buffer array one entry at a time and stopped when an entry failed a
write probe. To decide whether channel 1 existed it had to **read `buffers[1]` first** — eight bytes
past the end of a one-entry array. When those bytes happened to hold a writable address, the block
was copied over it.

*Verified afterwards, not assumed:* with the count now read from the object, the log says

    audio: Resolve provided 1 of the 2 channels "PodcastPlugins TRACK" wants (it reports 1 out)

Resolve reports **one** output channel on that track. So the old probe read past the end on **every
block**, not occasionally.

*Change:* the count is read, never guessed. `BMDAudioPluginImpl::UpdateChannelCount(int, int)` writes
it onto the object:

    4eb952:  mov %rax,0x150(%rbx)     # input channels
    4eb975:  mov %rsi,0x158(%rbx)     # output channels

and `BMDStereoDelay::Process` loops its channels against `0x158(%r12)`. Both are 64-bit. The probe
stays as a second gate but only ever sees indices that exist.

**Verdict: PLAUSIBLE, NOT YET PROVEN.** The mechanism is verified; the fix is not. Family A has not
recurred since the change went in at 02:00 — but that is **one** run, and family A took 12 to 24
seconds of playback to appear. It needs a deliberate soak with two effects on the track.

## Family B — our own timer thread

**The fault, verified.** `addr2line` on `libfxbridge.so+0xd4cc` (01:46) and `+0xde36` (02:01) both
resolve to:

    ClapPlugin::HostRegisterTimer(clap_host const*, unsigned int, unsigned int*)::{lambda()#1}

with four to six frames above it inside `pp-track.clap`'s GUI code.

### Cause B1 — no lock, and a thread check that lied

CLAP puts `on_timer` on the main thread, beside every GUI call, and a plugin may touch its editor
from it. Two things were wrong at 01:46: nothing stopped Resolve's UI thread calling `gui->show()`
while the timer thread was already inside the plugin, and `clap_host_thread_check` answered
"not main" for the timer thread — which reports it as the **audio** thread.

*Change (01:50):* one mutex **per plugin** around every main-thread call, and the timer thread
answers as main.

**Verdict: INSUFFICIENT.** The crash recurred at 02:01 with two pp-track instances loaded. The lock
was right; its **scope** was wrong.

### Cause B2 — one timer thread per plugin (open)

CLAP's "main thread" is singular and host-wide. A timer thread per plugin breaks that by
construction: two effects on one `.clap` gave two threads inside one library, and a plugin's GUI
toolkit is process-global — a font cache, a widget list, a drawing context.

*Proposed change, WRITTEN AND THEN REVERTED:* one timer thread and one host-wide lock, ticking every
registered plugin in turn, with lock order `g_main_lock` then `g_timer_lock` so a plugin cannot be
destroyed while the timer is inside it. The edit was applied by script, the script mangled the class
body, and the file was reverted with `git checkout` rather than repaired on top of a mess.

**Status: NOT IN THE TREE.** The installed library still has the per-plugin timer, so family B is
expected to recur.

## Everything else changed tonight

| Commit | What | Verdict |
|---|---|---|
| `47f25ac` | Scan the plugin folders; one menu entry per plugin, key maps back to the file | **Verified in Resolve** — 15 entries, picking pp-track loaded pp-track |
| `90596f6` | Vendor travesty (DPF), sparse and shallow, 1.8 MB | Builds |
| `225a088` | One CLAP plugin per effect; empty panel by default; knob binding removed | **Verified** — a second CLAP effect now gets its own plugin |
| `fe39cb1` | Editor state per effect; trace trampoline forwards `this` in `rsi` | Builds; the wrong-window symptom has not reappeared, **one run only** |
| `8665c6f` | VST3 host on travesty | **Never run.** Compiles and is wired into `CreateHostedPlugin`; VST3 stays out of the menu |
| `678cc79` | README record of the mixer crash and the dead theories | — |
| `c6b438c` | Channel count read from `this+0x150` / `this+0x158` | See A4 |

### A defect found by reading a log, not by guessing

The stock Delay panel's knobs call `SetParameterValue`, and the bridge forwarded that index straight
into the hosted plugin's parameters. Dragging *Delay Time* sent index 5 to pp-track and silenced it —
output at `-inf`, its own input meter flat. It read as a broken audio path until the `knob:` lines in
the log were read. The binding is gone; the index means one thing on a Delay and another on every
plugin, so there is no mapping to find.

### A crash that stopped without being fixed

The close-panel-during-playback crash (~01:04) disappeared after a rebuild of the **same commit** and
has not returned. **Nothing is credited with fixing it.** If it comes back, the first move is
`md5sum` on the installed library against `build/libfxbridge.so` — a stale install has already cost
one wrong diagnosis tonight.

## What this ledger says about the method

Three fixes went in on a theory before the theory was tested: A1, A3 and B1. All three cost a
restart, and A1 and B1 cost a shipped change that did not fix the thing it named. The two that held
up — A4 and the knob binding — came from **reading an instruction or a log line first**.

The rule for the rest of this work: disassemble the faulting address, or find the line in the log,
**before** editing a file. A theory that cannot name the instruction is not ready to be built.
