# Desktop controls — CP72 / Preview 41

This batch implements the audited I01–I04 controls through the existing native
window, controllers and single helper. The user explicitly chose to track full
native translations separately (2026-10-23). I05 remains open; this batch adds
reusable original-catalog bindings and preserves filter patterns.

## Controls

| Action | Behavior |
| --- | --- |
| Alt+D | Clear known downloaded boot resources and policy cache files while idle. |
| Alt plus/minus | Change the saved job priority across the five original levels, including keypad keys. |
| Alt+. | Toggle saved USB enumeration diagnostics and libcdio informational logging; refresh discovery. |
| Ctrl+A | Select a visible log from any Rufus window, including when the main window owns focus. |
| Ctrl+Alt+D | Toggle saved explicit dark/light appearance while idle. |
| Bare Alt | Cycle active progress between percentage, transfer rate and estimated remaining phase time. |
| Alt+Y | Toggle forced native release selection. |
| Ctrl+Alt+Y | Toggle the advanced forced-release mode; log its native verification boundary when checking. |
| `VerboseUpdateCheck` | Original numeric setting: 0 disables extra update logs, 1 logs requests/results, 2 adds parameters and response metadata. |

The existing settings, cancellation and joined READY behavior remain in use.
Desktop shortcuts also work from the log window. Priority changes are permitted during a job. Cache and appearance changes wait
until media and update work is idle. Ordinary update force modes retain channel,
architecture and publisher eligibility and do not initiate a request themselves.

## Cache ownership and source preservation

Windows Alt+D deletes its entire `rufus_files` directory. Linux also keeps session
and bad-block logs there in portable mode. The native action therefore removes
known cache entries: GRUB/Syslinux resource files and signatures in their version
folders, `diskcopy.dll`, remote policy lists and DBX cache records. It preserves
logs, reports, settings and unrecognized files. Empty owned version folders are
removed. The current in-memory security policy remains available for this session;
future policy checks must obtain missing cache files normally.

Deletion uses retained directory descriptors, no-follow opens and owner/device
checks. A cache-root symlink or a child filesystem change is refused. A known
resource symlink can be unlinked without following its target. Selected image
and supplementary archive inodes are protected, including hard-link aliases.
Retained boot-resource selections are invalidated so a subsequent job checks or
obtains its payload again. An active operation never races cache deletion.

## Native scheduling

Windows stores `DefaultThreadPriority` relative to ABOVE_NORMAL and changes its
format worker. Linux preserves that setting encoding and applies the selected
level to each Rufus job worker, including the authorized preparation worker.
The GUI's scheduling remains independent.

| Windows level | Native best-effort I/O level | Authorized worker CPU nice |
| ---: | ---: | ---: |
| -2 | 7 | 10 |
| -1 | 6 | 5 |
| 0 | 4 | 0 |
| 1 (default) | 2 | -5 |
| 2 | 0 | -10 |

Unprivileged workers use reversible best-effort I/O priority. Linux CPU boosts
require privileges, and lowering CPU priority could prevent an unprivileged
worker from restoring its previous setting. The already authorized helper also
requests the mapped CPU nice value. Kernel permission failures are logged and
leave media completion dependent on its ordinary I/O result, as Windows's
unchecked thread-priority call does. No extra authorization or real-time policy
is introduced. A kernel's I/O scheduler determines how the hint is honored.

A bounded optional request field carries the initial choice. An exact validated
control frame updates an active worker through the same helper pipe. Coalesced
frames are handled individually; malformed complete controls accompanying the
initial request are refused before the worker starts. Child tools inherit the
worker policy when launched; an already running tool retains its launch policy.
Worker scope exit restores its previous native scheduling values.

## Appearance and progress

`DarkMode=1` forces dark and `DarkMode=2` forces light, matching Windows's saved
values. An absent setting uses the desktop defaults. Explicit choices use Fusion
so native styles cannot ignore the selected palette. Existing dialogs and new
dialogs receive the application palette. Linux applies this while idle without
restarting the window, preserving selected sources and controls.

Progress uses real operation-local byte counters for copying, verification,
hashing, capture and downloads. A phase change, counter reset or changed total
starts a new sample. Rates require at least 200 ms and positive progress;
unavailable measurements retain percentage. Terminal labels and bar positions
are unchanged: a displayed 100 percent never substitutes for final sync, cleanup
and helper exit. Bare Alt is distinguished from an Alt shortcut or mouse/menu
interaction.

## Advanced update behavior and catalog reuse

The exact Windows Ctrl+Alt+Y code bypasses **Authenticode timestamp errors** when
verifying a downloaded Windows executable (`pki.c` and `stdlg.c`). It does not
bypass the update HTTP Date check. Native updates use the previously documented
release-page/package-manager handoff; they do not download an Authenticode
executable. Ctrl+Alt+Y provides the native forced-release behavior and logs this
verification difference. HTTP dates, TLS, origins and package-manager trust
remain governed by their existing checks.

USB debug and priority messages reuse the original catalog templates. File
filter descriptions reuse original ISO/DD/VHD/VHDX/FFU/log labels while retaining
wildcard suffixes and `;;` separators. Mixed filters can retain an untranslated
native-only description. The existing 38-language catalog, inheritance and
placeholder checks remain authoritative. Full native-only diagnostic and UI
translations are I05, scheduled with final message coverage before release.

## Batched qualification

The focused group covers real worker I/O scheduling and an unchanged GUI thread,
actual key activation, saved values, palette propagation, log selection, cache
ownership/source/symlink preservation, real hash progress and mode changes,
validated helper commands with a real temporary FAT32 file job, update diagnostics
and forced-mode date refusal, catalog filter bindings and private-bus enumeration
logs. Normal and ASan/UBSan/LSan builds use the same group.

One full nine-suite Qt gate covers the shared helper/control lifecycle. Existing
media writer, claim/fault, boot and READY evidence is reused. This desktop batch
adds no media format or device class and needs no USB guest or Windows installation
matrix. Package inspection uses temporary stages without host installation.
Compact receipts and screenshots are under `../../.rufus-test-artifacts/cp72/`.
