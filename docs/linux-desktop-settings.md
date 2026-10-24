# Desktop storage, startup and logging

CP59 groups the original desktop preferences, portable storage, startup switches
and log actions. The Qt GUI remains unprivileged.

## Settings and portable files

Normal builds retain the existing per-user `Rufus/Linux.ini` preferences under
Qt's user configuration directory. An existing `rufus.ini` beside the executable
selects portable storage. `--portable` creates that file; Rufus's original
portable filename marker also works: a lowercase `p`, or the first uppercase
`P` unless it starts `PH` (the original `ALPHA` exception). Directory names do
not select portable mode.

Portable mode uses that INI exclusively and puts downloaded boot components,
automatic logs and bad-block reports in the adjacent `rufus_files` directory.
Existing Windows numeric preferences, case-insensitive keys and the original
Windows key names are accepted. Existing Linux previews' alternate MBR and
Windows User Experience key names remain readable; portable writes use the
Windows spellings. Unknown INI keys are retained. Only implemented preferences
affect the application; a Windows INI does not enable unfinished Windows
features. The title shows `(Portable)`.

This selects portable application data. The native package still requires its
system libraries and matching installed helper/polkit policy, as described in
the package guide. The helper path stays fixed: allowing an arbitrary writable
portable directory to select a privileged executable would bypass the trusted
system policy. The generic archive is an install tree, not a bundled standalone
Windows-style executable.

An unavailable adjacent or explicitly requested portable INI stops startup with
its actual error. Windows can fall back silently to its registry in this case;
the Linux GUI refuses so that a requested portable session cannot unexpectedly
write preferences or logs into the user's profile. The portable directory must
also allow creating/replacing files to save preferences and logs.

## Startup

- `-i PATH`, `--iso PATH` or one positional path selects an image.
- `-x`, `--extra-devs` initially enables the existing **List USB Hard Drives**
  control. It retains source/system-device protection and ordinary confirmation.
- `-f FILESYSTEM`, `--filesystem FILESYSTEM` prefers a filesystem when eligible.
  Original case-insensitive names and numeric indices are accepted. Unsupported
  or unavailable preferences use the ordinary image/device defaults. A manual
  choice remains possible. This does not enable an unavailable native provider.
- `-g`, `--gui` retains GUI startup, already the Linux default.
- `-w TICKS`, `--wait TICKS` waits for the running instance to exit. Each tick is
  100 ms, matching the original Windows mutex loop. `/W` selects 150 ticks (15 s).
  Linux validates a nonnegative, bounded timeout before waiting.
- `--portable` selects the storage described above.
- `-h`, `--help`, `--help-all`, `-v`, `--version` work without a graphical session.
  Help/version do not acquire the instance lock or create portable settings.
  Qt's GUI platform/style options remain available for normal GUI startup.

The Linux application uses one instance per user in a private runtime directory.
The original Windows mutex is global. This native per-user boundary follows
Linux desktop sessions and keeps each user's preferences and logs separate;
the existing device claims still protect a physical target across users.
A second instance either waits as requested or reports that Rufus is running.
This does not send a new image to the existing process.

## Controls and logs

Expanded advanced drive and advanced format panels are restored and saved using
`ShowAdvancedDriveProperties` and `ShowAdvancedFormatOptions`. Existing expert
and writing preferences use the selected settings file too.

- **Ctrl+L** toggles the log window; the toolbar log button opens it.
- **Ctrl+P** toggles and saves `PersistentLog`. At the next startup, a persistent
  session begins with a timestamp. Earlier sessions are kept on disk and are
  not loaded into the visible session, matching Windows.
- **Alt+U** toggles and saves `UseProperSizeUnits` for the device list. Nominal
  decimal capacity/rounding is the original default; the alternate display uses
  1024 bytes per KB. Confirmation and progress sizes retain their existing units.
- **Alt+R** deletes the normal per-user settings. Like Windows's registry reset,
  it leaves current choices and the portable INI intact.

On an accepted idle close, Rufus saves `rufus.log` in its application files
folder: the existing per-user data folder's `files` directory, or portable
`rufus_files`. The default replaces the previous session. Persistent mode
retains it and appends the new session. A failed atomic replacement preserves
the previous log and reports the error on stderr. Logging failures never turn
a finished media job into a failure or prevent closing.

The visible text keeps the latest 2,000 blocks. A private temporary spool retains
the entire session, up to the original approximately 2 GiB log limit, for both
manual **Save** and automatic saving. **Clear** clears both the session and its
visible tail. Temporary spools are removed on normal teardown. Saved text uses
native UTF-8 and LF endings. Atomic file replacement is a Linux improvement over
the original direct append/write: a failed export must preserve earlier logs.

CP63 adds policy-update controls, CP66 native image downloads, CP67 language
selection/catalog reuse and CP68 native release checks with consent, beta and
forced-discovery controls. See their dedicated guides for exact behavior and
remaining delivery/translation work.
