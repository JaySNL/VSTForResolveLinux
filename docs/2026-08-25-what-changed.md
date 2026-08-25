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

## The VST2 editor took no mouse input — and it was never our bug

**Symptom.** Voice Changer's GUI rendered inside Resolve, but its controls could not be operated.
Only controls that happened to sit near the **top-left of the screen** responded, and moving the
window changed which ones worked.

**Everything that was measured and refuted first**, each with a purpose-built C tool in the
scratchpad: input focus (`XSetInputFocus` and `WM_TAKE_FOCUS` both no-ops), pointer and keyboard
grabs (`GrabSuccess`, nobody else holding them), XShape input regions (full rectangle), event
delivery (`ptrchain` proved the `yabridge plugin` window is the deepest window under the cursor and
does receive the events), XEmbed (no `_XEMBED_INFO` on any window in the chain), a synthetic
`ConfigureNotify` carrying absolute coordinates (`tellpos`), a real `ConfigureNotify` produced by
nudging the window, and the window-manager frame offset (`frameoff` reported a gap of `0,0`).

Six mechanisms, six refutations. One code change was made against an untested theory — forwarding
focus from the pump thread — and it black-screened the editor and froze Resolve. It was reverted in
`13e1ef0`. That is the same failure the ledger above already names.

**The actual cause, found by searching instead of guessing.** It is a documented yabridge bug:

> "Wine will interpret any local coordinates as global coordinates."
> — [yabridge PR #462](https://github.com/robbert-vdh/yabridge/pull/462)

Wine 9.22 changed editor embedding. When the Wine window is reparented into a host window, Wine
hit-tests in screen coordinates while believing its own window starts at the origin. So a control
responds only while its window position happens to equal its screen position. The fix is written but
**unreleased**: it sits in the `[Unreleased]` changelog section and on the `new-wine10-embedding`
branch. `editor_coordinate_hack` — which was set in `yabridge.toml` and did nothing — targets an
older, different problem, and that branch removes the option outright.

This machine ran **wine 11.15** against **yabridge 5.1.1, released 2024-12-23**. That combination
cannot work.

**The fix**, applied 2026-08-25:

    paru -S yabridge-wine10-git yabridgectl-wine10-git   # builds branch new-wine10-embedding
    sudo pacman -Rdd yabridge yabridgectl                # the conflict prompt defaults to N
    sudo pacman -U  .../yabridge{,ctl}-wine10-git-*.pkg.tar.zst
    yabridgectl sync                                     # bridge .so files are COPIES, not symlinks

Confirmed from `~/.local/share/DaVinciResolve/logs/ResolveDebug.txt`, not from the absence of a
complaint:

    Initializing yabridge version 5.1.1-41-gba7022df
    config from:   '/home/jooshua/.vst/yabridge/yabridge.toml, section "*"'

Result: full, precise control of the Voice Changer GUI at any window position.

### The traps this left behind

- **The scope is every Windows plugin with a GUI**, VST2 and VST3 alike, in every host. It is not a
  Voice Changer problem, not a Resolve problem, and not a defect in this bridge. The split that
  matters is **Wine versus native**, not VST2 versus VST3: the 14 ERA6 VST3 plugins go through
  yabridge and were equally affected; the CLAP and native-Linux VST3 plugins never were.
- **`yabridge.toml` is read from the plugin's own directory**, searching upward — never from
  `~/.config/yabridge`, which belongs to yabridgectl. An earlier test put it there, yabridge never
  loaded it, and an option looked refuted when it had never applied. Confirm the `config from:` line
  in the log before believing any result from that file.
- **`yabridgectl --version` still prints `5.1.1`** on this build. The package is `r3086.ba7022df`.
  Do not read the version string as a failed install; read the log line instead.
- **`pgrep -x resolve` finds nothing while Resolve is running.** Its process comm is `GUI Thread`.
  Match on `/opt/resolve/bin/resolve` in the full command line.
- The old packages come back with `sudo pacman -S extra/yabridge extra/yabridgectl`.

## VST3 ran for the first time, and the editor needed a run loop

The VST3 host had been written, wired in and never executed. Its first run produced two results in
one evening.

**Native Linux VST3 works.** Dragonfly Hall Reverb loads, configures, processes audio and now draws
its editor:

    vst3: loaded "Dragonfly Hall Reverb" by Michael Willis and Rob vd Berg
    vst3: "Dragonfly Hall Reverb" ready, 2 channels, 48000 Hz, up to 8192 frames
    vst3: editor open for "Dragonfly Hall Reverb" at 920x345

**The editor needed a host run loop, and its absence looked like a black window.** `attached()`
refused, we kept the window we had already made, and the result was an empty panel with a working
audio path behind it. The reason is in DPF's own `attached()`:

    DISTRHO_SAFE_ASSERT_RETURN(view->frame != nullptr, V3_INVALID_ARG);
    v3_cpp_obj_query_interface(view->frame, v3_run_loop_iid, &runloop);
    DISTRHO_SAFE_ASSERT_RETURN(runloop != nullptr, V3_INVALID_ARG);

VST3 on Linux puts the event loop in the host. The editor registers the file descriptor of its X11
connection and its timers with the host, and the host calls them back. This is not DPF being fussy;
it is the platform contract, and no other format on this bridge needs it.

What went in, all read out of that source rather than guessed:

- an `IPlugFrame` that also answers `IRunLoop`, carrying two vtable pointers the way a C++ class
  with two bases does, each face holding a back pointer so recovery never needs `offsetof`;
- `set_frame()` **before** `attached()`. The view looks for the run loop from inside `attached()`
  itself, so a frame handed over afterwards is handed over too late;
- a real service pass — `poll()` on every registered fd, a due-check on every timer — on the shared
  host main thread at 16 ms, never a thread of its own;
- `resize_view`, which resizes the parent and then tells the view the size it got. That needed a new
  `PluginWindowResize`;
- the window is destroyed when `attached()` fails, so a refusal cannot leave a black panel again;
- `attached()` failures print their result code.

### ERA6 through yabridge is still refused, and the obvious answer is wrong

Every one of the 14 Accusonus ERA6 plugins answers `initialize` with `0x00000002`. That is
`V3_INVALID_ARG` in travesty's non-COM enum — the enum that applies here, since `V3_COM_COMPAT` is 0
on Linux. So the plugin is rejecting the host context, not failing inside itself.

Two things are already ruled out:

- **It is not a missing `IPluginFactory3::set_host_context`.** Adding that call made it worse, not
  better: the factory accepted the context (`0x00000000`) and the next call never returned. Resolve
  hung at "load project 100%" and the project could not be opened. See the disabled call and its
  comment in `src/vst3_plugin.cpp`.
- **It is not an IID or result-code mismatch.** `V3_COM_COMPAT` is 0, matching the SDK on Linux.

yabridge's own Linux-side proxy returns that value in exactly one place:

    tresult PLUGIN_API Vst3PluginProxyImpl::initialize(FUnknown* context) {
        if (context) { ...; return response.result; }
        else { ...; return Steinberg::kInvalidArgument; }
    }

Our context is the address of a static object and cannot be null, so either the `2` travelled back
from the Windows plugin through the bridge, or the call does not land where we think it does.

#### Closed: the plugin crashes inside Wine, and this host never had a say

`carla-discovery-native` settles it without touching Resolve. It reports the same failure, and the
Wine side says why:

    [Wine STDERR] Finished initializing '.../Accusonus/ERA6_VoiceAutoEQ.vst3'
    [Wine STDERR] err:virtual:virtual_setup_exception stack overflow 4160 bytes
                  addr 0x6fffffbf3659 stack 0x7ffffe10ffc0 (0x7ffffe10ffc0-...)

The Windows plugin blows its stack inside Wine as soon as its VST3 entry point is used. The
`V3_INVALID_ARG` this host saw is the wreckage arriving back over the socket, not a judgement on the
host context. Carla — an unrelated host with a mature VST3 implementation — fails the same way, and
the same tool discovers the native Dragonfly VST3 without complaint.

So the earlier suspicion about `IHostApplication::createInstance` was wrong and was never built. It
did not need `YABRIDGE_DEBUG_LEVEL`; it needed a second host, which cost one command.

**The route around it, and it is not a compromise.** All 14 ERA6 plugins also ship VST2 builds, in
`~/.wine/drive_c/Program Files/Steinberg/VstPlugins/Accusonus`, beside the `VoiceChanger.dll` that
already works. Bridging that directory instead puts the whole suite on the VST2 path:

    yabridgectl rm  ".../Steinberg/VstPlugins/AccusonusVC"   # redundant, same DLL
    yabridgectl add ".../Steinberg/VstPlugins/Accusonus"
    yabridgectl sync

Menu went from 36 entries to 50. `ERA 6 Voice AutoEQ` and `ERA 6 Reverb Remover Pro` both load and
open their editors. Same plugins, same 2022 binaries, an interface that does not crash.

## The second half of 2026-08-25: three formats, and where the line actually falls

### Windows VST3 was miscompiled, not broken

Every Windows VST3 died the same way — ERA6, Accentize, CrumplePop, three unrelated vendors:

    err:virtual:virtual_setup_exception stack overflow 4160 bytes
    addr 0x6fffffbf3659 stack 0x7ffffe10ffc0

The **byte-identical** address and size across three vendors is what gave it away: that is shared
code, not three bad plugins. [yabridge issue #449](https://github.com/robbert-vdh/yabridge/issues/449)
carries a report from another CachyOS user, on the same `yabridge-wine10-git` AUR package, with the
same VST3 failure. The cause is `-march=native`, which CachyOS puts in `/etc/makepkg.conf` by
default and which this machine does use. The fix is a full rebuild with generic flags:

    sed -e 's/-march=native -O3/-march=x86-64 -mtune=generic -O2/g' /etc/makepkg.conf > generic.conf
    makepkg -f --config generic.conf     # in ~/.cache/paru/clone/yabridge-wine10-git

Nobody upstream has explained *why* it breaks. It reproduces and the rebuild fixes it; that is all
that is established. `virtual_setup_exception` count after the rebuild: **0**.

Build against a copy of the config, not the system one. The rebuilt package is ~0.27 MiB smaller,
which is the cheapest confirmation that the flags actually changed.

### A VST3 editor needs the component and the controller joined - by the host

After the rebuild the plugins loaded and had no GUI:

    vst3: create_view returned nothing for "SpectralBalance2"

`grep -n 'connection_point' src/vst3_plugin.cpp` returned nothing. In most VST3 plugins the
component and the controller are two separate objects, and **connecting them is the host's job**.
A controller that was never connected has never heard from its component, and a plugin in that state
is entitled to refuse to build an editor. Both objects are now queried for `IConnectionPoint` and
connected to each other, with a guard so a single-object plugin is not connected to itself, and
disconnected in the destructor before either side is terminated. The result:

    vst3: connected the component and the controller (0x00000000, 0x00000000)
    vst3: editor open for "Voice Enhance Complete" at 424x424

### The memlock cap is a systemd setting, not a limits.d one

yabridge printed `memlock limit: '8388608 bytes'` on every load, and the third or fourth plugin
aborted. `/etc/security/limits.d/20-audio.conf` had `@audio - memlock unlimited` and did nothing,
because it only applies to PAM logins. The systemd **user manager** is what starts desktop apps:

    systemctl --user show -p DefaultLimitMEMLOCK   ->   8388608

Fixed with `/etc/systemd/user.conf.d/10-memlock.conf` containing `DefaultLimitMEMLOCK=infinity`,
which needs a re-login. Verify per process, never per config file:

    grep 'Max locked memory' /proc/<pid>/limits

### An exception from a bridged plugin must never reach Resolve

A bridged plugin lives in another process, and **yabridge reports its death by throwing**. We called
`processReplacing` and `dispatcher` with no `try`, so the exception unwound out of an audio callback
into Resolve's C code, reached `std::terminate` and aborted the application.

Both are now wrapped, on VST2 and VST3. The block is dropped and the plugin **stays in the path** -
an earlier version latched the plugin off permanently and that was wrong: a throw is not proof the
plugin is finished, and silently disabling a working effect is worse than the block it just lost.
The first throw is logged, the rest are not.

This does not catch everything. yabridge also throws on **its own** threads, where there is no stack
of ours to wrap. Such a crash has no `libfxbridge` frame at all - check for one before assuming.

### Windows are retired, not destroyed

    X Error of failed request:  BadWindow (invalid Window parameter)
      Major opcode of failed request:  10 (X_UnmapWindow)

A bridged plugin's Wine process keeps our window id and unmaps it on its own schedule. Destroying
the window makes that id invalid, Xlib's default handler calls `exit()`, the Wine host dies, and
yabridge throws on the dead socket. `PluginWindowDestroy` now unmaps and keeps the window; the real
`XDestroyWindow` happens at unload, when no Wine host is left alive.

**Note the red herring.** The failing id was always `0x4c00001`, and it is **not one of ours** - we
hand out `0x2400001`, `0x2800002` and so on. It appears for plugins that work perfectly. Logging our
own ids at creation is what settled it, and that log line is worth keeping.

### The window manager will resize an editor unless told not to

A Smooth Operator Pro editor created at 1132x602 was found at 991x554 - the size of the soothe2
editor beside it. COSMIC tiles, and our windows carried no hints at all. Every editor window now
gets `WM_NORMAL_HINTS` with min, max and base equal, plus `_NET_WM_WINDOW_TYPE_UTILITY`, **before**
it is mapped. `PluginWindowResize` moves the hints with the window.

Also fixed: `audioMasterSizeWindow` was answered with 1 (success) while doing nothing. Answering
"done" without resizing is worse than answering 0, because the plugin then lays itself out for a
size it never got - two stacked copies of its UI, and nothing clickable.

And the embedded child now follows the parent, on a plugin-driven resize and on a
`ConfigureNotify` from anyone else. Resize the child only when the size actually changed: a drag
emits those continuously and per-event resizing makes the drag crawl.

### Where the line falls: two plugins take no mouse input, and it is not this bridge

Smooth Operator Pro (VST2) and Accentize SpectralBalance2 (VST3) render correctly and accept no
clicks. soothe2, smartEQ4, smartComp3 and the ERA6 suite are fine in the same session.

**Thirteen hypotheses were measured and refuted**: input focus, pointer and keyboard grabs, XShape
input regions, event delivery, XEmbed, `editor_coordinate_hack`, synthetic and real
`ConfigureNotify`, the WM frame offset, `editor_disable_host_scaling`, the EGL vendor (Mesa versus
NVIDIA), missing WM size hints, an ignored `audioMasterSizeWindow`, and a parent/child size
mismatch. The last three were real defects and are fixed. None of them was this.

The window trees are identical for a plugin that works and one that does not - same depth, same
geometry, same event masks. **The verdict came from Carla**: Smooth Operator Pro takes no input
there either, in its own process, in a fresh Wine session, through the same yabridge. This host is
exonerated. Whatever remains is inside Wine or yabridge, for these two plugins.

Do not reopen this without a new *kind* of evidence. A fourteenth variation on "try another X11
setting" is not one.
