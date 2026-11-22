# The first scan, in detail

Timings, the scan cache and the crash-recovery files. What a user needs is in the
[README](../README.md#the-first-scan).

Reading a plugin's name means opening it, and opening a Windows VST3 through yabridge starts a
Wine host. Measured here: `dlopen` 0 ms, **`ModuleEntry` 327–352 ms**, `ModuleExit` 26–29 ms,
`dlclose` 0 ms. On a tester's machine the same step takes one to three seconds per plugin. With a
few hundred plugins that is minutes, once.

Three things make that once bearable:

- **`fxbridge-scan` does it outside Resolve**, with a progress bar. `build.sh` runs it for you
  after it installs. A plugin that faults while being read then takes down a command rather than an
  edit session.
- **Eight modules are opened at once.** Independent Wine hosts, so they start alongside each other:
  2,910 s serial against 1,232 s parallel over the same 21 modules here. `FXBRIDGE_SCAN_THREADS`
  changes the number.
- **Nothing learned is thrown away.** The cache is written after every module, so Ctrl-C is safe
  and the next run continues where the last one stopped. A cached start reads the file and opens
  nothing: **0,003 s** here, against 2,910 s cold.

The cache key is the size and modification time of the binary inside each bundle, so a plugin you
update or install is read again and the rest are not.

**Plugins that stopped a start** — `~/.local/share/BMDAudioPlugins/fxbridge-scan-crashed.txt`. You
do not write this one. Reading a plugin's name runs that plugin's code inside Resolve, so a plugin
that hangs or faults there takes the start down with it — and without this, every later start would
open the same module, in the same order, and stop in the same place.

The scan notes which module it is about to open and rubs the note out when that module answers. A
note still there at the next start names a module that was open when the last one ended. **The
first time proves nothing** — a start closed by hand blames whatever happened to be open — so that
module goes to `fxbridge-scan-suspect.txt` and is opened again. Only a module that is open at the
end of a second start is written to the crashed file and skipped, and the log says so both times.

A scan that runs all the way through clears the suspect list, because everything in it answered.
Delete a line from the crashed file to have that plugin opened again, or delete the file to try all
of them.

**Nothing learned is thrown away.** The cache is written after every module rather than at the end
of the scan, so a start that stops halfway keeps every answer it already had. The next start begins
where that one stopped. This matters most with a large Windows collection: each module costs about
one to three seconds through yabridge, so a first scan of a few hundred of them is minutes long —
but it only has to happen once, and it no longer has to happen in one sitting.
