# Changelog

Every release of VST for Resolve on Linux, newest first. Dates are the day the tag was cut.

A claim in here is either measured or marked as not measured. Where a release said something that
later turned out to be wrong, the correction stays next to the original rather than replacing it.

---

## v0.2.7 — 2026-08-29

**A slow first scan no longer starts over every time.**

The v0.2.5 report was never a crash. With v0.2.6's logging the tester's own log answered it: every
module in his MeldaProduction bundle takes **1.6 to 3.0 seconds**, because opening a Windows VST3
through yabridge starts a Wine host and closing it stops one. With well over a hundred of them,
his first start was minutes of an unmoving splash screen, and he closed it several minutes in.

### Fixed

- **The cache was written once, at the end of the scan.** So a start that did not finish threw away
  everything it had just learned, and the next one began again at the first module. The person with
  the collection slow enough to need the cache was the only one who could never build it.

  It is now written after every module, by rename, so a start stopped halfway leaves a complete
  file and the next start begins where it stopped. Measured: `SIGKILL` mid-scan used to leave 0
  cached modules and now leaves **19**.

- **One appearance was enough to skip a plugin, and the first module this blamed was innocent.**
  The tester's scan was not stuck on `MAGC.vst3`; his whole collection is slow and that module
  happened to be open when he closed Resolve. A module in flight once is now a **suspect** and is
  opened again next time. Only a module in flight at the end of a second start is skipped.

  That second start is a real test now that the cache survives: everything already answered is
  skipped, so a module that genuinely stops the scan is reached again immediately, while one that
  was merely open when someone reached for the window button is not. A scan that runs to the end
  clears the suspect list.

  Verified: killed mid-scan, the shell was recorded as a suspect and **opened again** on the next
  start; forced into flight a second time, it was skipped, named in the log with the file to delete
  to undo it, and the scan completed.

### Added

- **The scan says how far along it is** — `scan: opening <path> (37 of 153)`, so a long first start
  is legible as progress rather than as a hang.

### Note for anyone on v0.2.6

Its one-appearance rule may have written an innocent module into
`~/.local/share/BMDAudioPlugins/fxbridge-scan-crashed.txt`. Delete that file once after upgrading.

---

## v0.2.6 — 2026-08-29

**A plugin can no longer take the start down with it, and cannot block it twice.**

v0.2.5 is withdrawn. A tester's Resolve hung on startup with it and left nothing behind to read:
no bridge line in the log, no cache, no deny list — only the library. Every fix below follows from
that one report.

### Fixed

- **The VST3 factory reference was never given back.** `GetPluginFactory` hands the caller a
  reference. The host side of the bridge has always released it; the scan never did, and then
  called `ModuleExit` underneath it. On yabridge `ModuleExit` shuts the Wine host down, so the
  reference that was still held pointed into a process that no longer existed. Introduced in
  v0.2.5, which is when the scan started closing modules at all.

- **`ModuleExit` ran on modules that were never entered.** Every native Linux VST3 sitting in the
  yabridge directory refuses `ModuleEntry` — and the close path told it to exit anyway. Also
  introduced in v0.2.5.

  Neither of these reproduces on the development machine: both Waves shells survive the old code
  and the new. They are fixed because they are wrong, not because they are proven to be the cause
  of the tester's hang.

- **The deny list was written after the loop it exists to escape.** A scan that finishes does not
  need a deny list; a scan that hangs never wrote one. The one person who needed the escape hatch
  was the only one who could not have it. It is now written before the first module opens, from
  the candidate list, which is complete by then. An edited file is still never overwritten.

### Added

- **A plugin that stops a start is skipped on the next one.** Reading a plugin's name runs that
  plugin's code inside Resolve. Before v0.2.6, a plugin that hung or faulted there was permanent:
  every start opened the same modules in the same order and stopped in the same place, so nothing
  after it was ever reached.

  The scan now writes down which module it is about to open and rubs the note out when that module
  answers. A note still there at the next start names a module that did not come back. It goes
  into `fxbridge-scan-crashed.txt`, is skipped from then on, and is named in the log — never
  silently. A start killed by hand blames whatever was open at that moment, so delete a line to
  try that plugin again, or the file to try all of them.

  Verified by killing the scan with `SIGKILL` mid-module: the note survived and named the Waves
  shell, the next start blamed it and got past it, the start after that still skipped it, and
  deleting the file brought it back (718 classes, 1.013 s).

- **The scan says what it is doing.** Each module is named *before* it is opened, so a module that
  hangs or faults is the last line in the log instead of an absence. That absence is exactly what
  the v0.2.5 report produced. The number of modules that have to be opened is logged before the
  loop starts, because opening one Windows VST3 through yabridge costs about a second — 987 ms and
  792 ms for the two Waves shells here — so a first start with a large collection is a minute of a
  splash screen that does not move, and a tester reasonably read that as the program being stuck.

---

## v0.2.5 — 2026-08-29 (withdrawn)

**Startup stops starting a Wine host for every plugin you own.**

### Fixed

- **The high-CPU reports were us, and this is the cause.** Reading a VST3's class names means
  opening the module, and for a Windows plugin that means yabridge starting a Wine host — which
  the scan then kept alive for the whole session, deliberately, because a note in the source said
  it "costs a file handle". Measured on a tester's machine by turning the bridge off and on:

  | | disabled | enabled |
  |---|---|---|
  | Resolve threads | 229 | **706** |
  | Memory in use | 9.5 GB | **22.8 GB** |
  | Busiest thread | 4.9% | **3 × ~100%** |

  Resolve's own resident set moved 0.4 GB while the machine lost 13 GB, so the memory was in other
  processes. `pgrep -fc yabridge-host` returned **336**, with no project open.

  Modules are closed after the scan reads them, on every path out including the refusals — each of
  those used to strand a host for a module the scan had already given up on.
  `FXBRIDGE_SCAN_KEEP_OPEN=1` restores the old behaviour.

- **Startup no longer scales with the size of your plugin folder.** What the scan found is cached
  in `~/.local/share/BMDAudioPlugins/fxbridge-scan-cache.tsv`, keyed on each library's size and
  modification time, so a normal start opens nothing at all. Measured here: first start 0 from
  cache and 21 opened; every start after, 21 from cache and 0 opened, with the same 130 plugins
  listed. Delete the file to force a rescan.

### Added

- **A plugin can file itself into a category.** A VST3 publishes its own subcategory — `Fx|EQ`,
  `Fx|Restoration` — and that is used when the built-in name table has no opinion. Reported by a
  tester who saw 98% of his plugins land in the fallback, because that table is one person's
  plugin collection rather than a taxonomy. It rescues about 45% of an unknown collection, not all
  of it: VST2 cannot answer without being instantiated, and CLAP publishes its features somewhere
  the scan does not yet read.
- **A deny list, written for you with everything you own already in it.**
  `~/.local/share/BMDAudioPlugins/fxbridge-scan-deny.txt` is created on the first run listing every
  plugin found on the machine, all commented out — delete the `# ` in front of a line to stop that
  plugin being opened at all. A pattern is a plain **substring of the full path**, matched without
  regard to case: not a glob and not a regular expression, so `*.vst3` matches nothing. The check
  runs before anything is opened, which is the only ordering that helps against a plugin that hangs
  the scan. Your edits are never overwritten; delete the file to have it rebuilt.
- `FXBRIDGE_SCAN_KEEP_OPEN=1` restores the old keep-everything-open behaviour.

### Known

- **The deny list names modules, not menu entries.** One shell holds many plugins — 56 lines cover
  130 plugins on the development machine — so denying a Waves shell removes all 75 of its plugins
  at once. Usually the intent, but worth knowing before it surprises you.

- **The first start after installing or updating a plugin is still slow.** That is when the cache
  fills.
- **The cache is new code on the startup path**, which is where this project has historically
  broken things. It repairs itself if truncated and is safe to delete.

---

## v0.2.4 — 2026-08-29

**The editor opens when you ask for it.**

### Changed

- **A plugin GUI opens when you click the effect, not when the project loads.** The test project
  went from eight windows at load to zero. The bridge opened them all because a note in the source
  said Resolve never asks for an editor while the stock panel is suppressed — it does ask, and one
  click on the effect proves it. Set `FXBRIDGE_OPEN_ON_CLAIM=1` to bring the old behaviour back.
- **Editor windows are dialogs, not utility windows.** `FXBRIDGE_WINDOW_TYPE` accepts `dialog`
  (the default), `utility` and `normal`.

### Fixed

- **Editor windows appear in the window switcher and take the keyboard when they open.** They were
  `_NET_WM_WINDOW_TYPE_UTILITY`, which KWin excludes from the switcher by type, so no amount of
  window-state juggling would have brought them back. `WM_CLASS`, the input hint and the activation
  request were missing outright.
- **KDE listed every editor as "unknown".** A desktop environment labels a window by looking its
  class up in the installed desktop files, and a class of our own matched nothing. Editors now
  carry Resolve's class, which is also the honest description of what they are.
- **Closing a plugin window and then the modal brought the window straight back.** A hide arriving
  with our window already gone used to be re-read as a request to show it.
- **Deleting an effect could leave its GUI on screen.**

---

## v0.2.3 — 2026-08-29

**Settings that live in the Resolve project.**

### Added

- **A hosted plugin's settings travel inside the effect's own preset, in the project.** On by
  default, nothing to turn on. They belong to that effect, they move with the project, and they
  survive a chain that gets rearranged. Verified with eight effects and the file store switched
  off: all eight saved and all eight restored, including a 2,320,783-byte smartEQ4 chunk.
- **A fault handler.** A crash now writes a named backtrace to the log, because systemd-coredump
  drops a process this size.

### Fixed

- **A crash on project load.** `GetControlType` is the one accessor of twenty-nine in
  `BMDAudioPluginImpl` that indexes the control vector with no bounds check, and Fairlight calls it
  for every index while deserialising.

### Changed

- **The file store is the fallback now, not the mechanism.** Still opt-in with
  `FXBRIDGE_STATE_STORE=1`. When both hold settings for one effect, the newer wins.

### Known

- **A plugin-only edit does not mark the project modified.** Resolve serialises the effects model
  only when it already thinks the project changed, and a change made inside a plugin's own window
  is invisible to it. Nothing is lost — it is written on the next thing you do that Resolve can
  see. Measured: a plugin-only edit then Ctrl+S produced zero saves; a fader on a track with no
  effects on it produced eight, one per effect in the project.

### Corrections to earlier releases

- The v0.2.1 notes said a project save asks the effect nothing, on the evidence that `StorePreset`
  and `LoadPreset` fired zero times. **That was measured on the wrong vtable slots.** Both fire
  reliably on the pair Resolve actually calls. The whole of v0.2.3 rests on that correction.
- An engineering-log note said the Linux build contains no VST host class. It contains 198 symbols
  of one, and reading it is what made v0.2.3 possible.

---

## v0.2.2 — never released

Tagged work that shipped as part of v0.2.3 instead. Its notes would have repeated the two claims
retracted above, so the version was skipped rather than published wrong.

---

## v0.2.1 — 2026-08-29

**Settings per instance, and editor edits that reach the plugin.**

### Fixed

- **The same plugin twice in one chain shared one settings file**, so both copies came back with
  whichever was touched last. An instance is now identified by its position among the *live*
  effects hosting that plugin, in Resolve's own load order.
- **VST3 editor edits never reached the processor.** `performEdit` did nothing, behind a single
  component handler shared by every effect, so it could not have said which plugin was speaking.
  Each effect owns its handler now.
- **Only half of a VST3's state was saved.** A VST3 keeps settings in the component and in the
  controller, and the SDK does not require them to agree. Both halves are stored and restored.
  Blobs written by v0.2.0 are still read.
- **Edits made with the transport stopped went nowhere.** A parameter change only reaches the
  processor inside a `process()` call, and Resolve only makes those while audio runs.

### Superseded

- These notes stated that there is no save hook. See the correction under v0.2.3.

---

## v0.2.0 — 2026-08-28

**Editors that open and stay openable, and settings that survive a reload.**

### Fixed

- **The first editor could never open by itself.** The only thread that could open one was a thread
  that a window had to already exist to start. Every editor that did open was opened by Resolve's
  own panel button, which hid the hole on any machine where that button gets used.
- **An editor that would not reopen** until the plugin was deleted and added again.

### Added

- **Settings survive a project reload**, opt-in with `FXBRIDGE_STATE_STORE=1`.

---

## v0.1.3 — 2026-08-27

**Install split into two methods that do not interleave.**

Someone who ran `build.sh` was being told to edit a config file the script had already written, and
someone who downloaded the binary had to scroll past the build steps to reach their own. Each method
is now numbered 1 to 4 and ends at "start Resolve".

---

## v0.1.2 — 2026-08-26

**The install instructions were wrong, and `build.sh` now does it for you.**

The readme and the v0.1.1 notes told you to write `BMDPlugins.Path  ~/.local/share/BMDAudioPlugins`.
Three things wrong in one line: no `=`, a `~` that Resolve does not expand, and the directory
instead of the library file.

---

## v0.1.1 — 2026-08-26

**Fixes a hang when switching projects.**

Switching between two open projects could freeze Resolve with a black plugin window and no recovery.
It affected plugins that run in another process — Windows plugins through yabridge — and it was
reproducible rather than occasional. Resolve restores every saved effect inside one synchronous
chain on the main thread, so the thread that would answer yabridge was the thread waiting on it.

---

## v0.1.0 — 2026-08-26

**First release.** VST2, VST3 and CLAP plugins in Fairlight on Linux, with a prebuilt binary.
