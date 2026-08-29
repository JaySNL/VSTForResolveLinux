# Changelog

Every release of VST for Resolve on Linux, newest first. Dates are the day the tag was cut.

A claim in here is either measured or marked as not measured. Where a release said something that
later turned out to be wrong, the correction stays next to the original rather than replacing it.

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
