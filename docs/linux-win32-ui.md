# Win32 GUI mapping

The user requires the Linux UI to follow Rufus's Win32 GUI exactly. Use
[`IDD_DIALOG` in rufus.rc](../src/rufus.rc) and runtime control creation/layout
in [ui.c](../src/ui.c), at upstream revision
`18ae93bf37f94764d77216cff4168e0549a14c4d`, as the source of truth. Initial values
and visibility also depend on `InitDialog`, `SetBootOptions` and `UpdateImage`
in [rufus.c](../src/rufus.c). Lufus's older form does not govern this layout.

Keep the sections, labels, relative positions, field pairings, toolbar order
and default visibility. Use Qt layout sizing for fonts and DPI. Native Qt
painting, desktop window decorations and platform file pickers differ from
Windows; this checkpoint does not claim pixel-identical Win32 rendering.

## CP1 control mapping

| Win32 control/behavior | Qt form or implementation |
| --- | --- |
| Drive Properties, Format Options, Status | Same captions, 14-point semibold headings and horizontal rules; no enclosing group boxes. |
| `IDC_DEVICE` | `deviceCombo`, full-width while advanced drive options are collapsed; capacity order and initial smallest-device selection match Win32. Selection uses device identity; invalidated targets require reselection. |
| `IDC_SAVE_TOOLBAR` | `saveDeviceButton`, original save icon, shown with advanced drive options; disabled until implemented. |
| `IDC_BOOT_SELECTION` | `bootCombo`, showing `Disk or ISO image (Please select)` initially and the chosen filename afterward. No separate image-path row. Other boot modes await media support. |
| `IDC_HASH_TOOLBAR`, `IDC_SELECT` | Original hash icon, then SELECT at the right of the same row. Checksums remain disabled. |
| Image option and persistence controls | Present in the form in the original position; hidden until image analysis determines applicability. |
| Partition scheme / Target system | Side-by-side labels and combos. Disabled pending analysis/planning. |
| Advanced drive toolbar | Show/Hide advanced drive properties, initially collapsed; expands without animations. |
| Advanced drive options | List USB Hard Drives enabled at CP17d3, with Alt+F, original warning tooltip and temporary status. Old BIOS fixes and runtime UEFI media validation remain disabled, in original order. |
| Volume label | Full-width text field; displays the completed scan label in CP3d. Editing stays disabled. |
| File system / Cluster size | Side-by-side labels and combos aligned with partition/target fields. Disabled. |
| Advanced format toolbar | Show/Hide advanced format options, initially collapsed. |
| Advanced format options | Quick format and extended label/icon files initially checked; bad-block checkbox and pass count below. Disabled. |
| `IDC_PROGRESS` | Progress bar in the Status section, with READY as the idle caption. START requires a supported raw image and an eligible selected device. |
| `IDC_MULTI_TOOLBAR` | Original language, information, settings and log icons in that order. Only log is currently enabled. |
| `IDC_START`, `IDCANCEL` | START and CLOSE on the lower right; CLOSE becomes CANCEL during analysis or writing. |
| Runtime status bar | Bottom status line showing device discovery and image selection/analysis state; full text is also available as a tooltip. |
| `IDD_LOG` | Read-only log with Clear, Save and Close. Contents survive closing/reopening the dialog; saving uses an atomic file write. |

The title identifies this as a Linux preview. Raw `.img` writing has passed
virtual USB qualification. The visible disabled fields preserve the original
GUI while formatting, extraction and additional image modes remain unavailable.
The current form is [main_window.ui](../linux/qt/main_window.ui); named C++
methods handle selection, dialogs and expansion. Compile original icons
directly from `res/` and retain their [license notices](../res/icons/license.txt).

CP2 feeds the existing device combo and status/log areas from read-only UDisks2
discovery; it does not change the form, control order or advanced defaults.
The initial conservative removable-USB filter is documented in the
[Linux README](../linux/README.md#read-only-device-discovery). At CP2, fixed USB listing
and all media operations remained unavailable. CP17d3 now enables explicit fixed
USB listing and carries that policy through the qualified creation routes; see
[the current support limits](linux-support.md).

## CP3d analysis behavior

`EnableControls` and the progress messages in `src/rufus.c` govern the runtime
behavior: analysis uses a marquee progress bar, disables device/image/advanced
selection controls, and changes CLOSE to CANCEL. The log remains available.
Cancelling through the button, Escape or main-window close requests cooperative
interruption and leaves the main window open. CLOSE is restored after cleanup.

Completed scan labels use the existing Volume label field; report details and
partial reasons use the existing status/log areas. The bar returns to READY for
a completed metadata scan, INCOMPLETE for a partial report, or FAILED/CANCELLED.
Linux-specific incomplete/backend messages are necessary because media writing
and some image checks remain unimplemented. For these analysis-only inputs, START and media settings stay
disabled. CP3e uses the same controls for UDF, identifies UDF in the completed
status text, and keeps Windows/embedded-EFI checks pending. CP3f1 uses the same
status/log areas for embedded FAT inspection on ISO9660: found EFI leaves Secure
Boot pending, completed absence clears the embedded check, and unreadable FAT
stays incomplete. CP3f2 applies the same outcomes to UDF embedded images. The `.ui` form, widget
order and advanced-section defaults are unchanged.

## CP8a raw writing behavior

The form and control order remain unchanged. A supported raw `.img` uses DD
mode: partition/target/filesystem/cluster and label editing stay disabled, and
Quick format / extended-label checks are cleared, following `EnableBootOptions`.
With an explicitly configured writer, START prepares the plan before showing
Rufus's `MSG_003` destructive-device warning. The dialog includes the retained
source filename/size and target identity. OK/Cancel and default Cancel match
`IDD_NOTIFICATION` / `NotificationCallback`. Text uses plain-text rendering.

Acquisition and writing disable selection/advanced controls while leaving the
log usable. CLOSE, Escape and window close request cancellation. During writing,
the `MSG_105` Yes/No warning defaults to No, with the `MSG_049` cancellation title.
The window stays open while cancellation joins the worker and flushes copied
bytes. A finished operation closes an obsolete cancellation dialog.

Copy and read-back each account for half the progress range; flush uses marquee
progress. The bar reaches 100%/READY only after complete verification and worker
cleanup. Failed/cancelled runs retain their counts and never display completion.
Status and log messages distinguish writing, flushing, verification and errors.
Twenty functional cases operate this path with a private bus and disposable
storage. CP8b qualifies the same Widgets/controller against native guest USB
storage and a separate KDE/polkit prompt. The application now supplies the system
bus to its writer. START becomes available for the narrow supported raw `.img`
workflow; the form layout is unchanged.

## ISOHybrid mode selection decision (2026-09-14)

Keep the existing `BootCheckThread` / `SelectionDialog` behavior. For a hybrid
where Rufus offers both modes, show `MSG_274`/`MSG_275` and the existing choices:

- `Write in ISO Image mode (Recommended)` is the default (`MSG_276`).
- `Write in DD Image mode` remains selectable (`MSG_277`).

Preserve Cancel handling, DD-required images, persistence restrictions and the
conditional ISO-to-ESP choice when their corresponding backend features land.
Do not change this policy for Linux, Arch or CachyOS. The recommendation is
Rufus's general usability choice; it is not a distribution's endorsement or
proof that a native extraction installer is compatible.

CP10a adds candidate/policy diagnostics to the existing status/log. CP10b adds
`ImageModeDialog`, a Qt Widgets implementation of the ISOHybrid selection dialog,
and shared C choice ordering used by the Windows caller and Linux selection
controller. The original question icon, message, exclusive radio choices and
OK/Cancel layout are retained with native Qt styling. Tests compare English
captions directly with `rufus.loc`, exercise mouse/keyboard selection and all
three cancellation routes, and check enlarged fonts at normal and 2× scale.

The first radio remains selected and OK is the default action. With the ESP
option the order is ISO, ISO → ESP, DD; otherwise it is ISO, DD. A shared resolver
maps the selected row to the operation, so the second row cannot accidentally
mean DD in the three-choice dialog. Cancel, Escape and window close return no
operation. Pure/DD-required images and persistence cases produce one policy
choice and do not require this dialog. The separate Windows ISO-to-ESP-only
prompt is outside this checkpoint.

The dialog component is exercised through tests but is not yet opened by
application START. Writing `.iso` files remains unavailable until native ISO
execution is implemented and qualified. Choosing a dialog option alone does not
create a write plan, authorize a device or start I/O.

## Verification

Functional Qt tests check captions, ordering, paired alignment, initial values,
collapsed/expanded sections, file selection, log controls, keyboard navigation,
and layout with enlarged fonts at normal and 2× scale. CP3d adds generated-ISO
analysis, partial results, cancellation and lifecycle tests through the same form.
CP3e adds actual UDF report/cancellation/restart checks. CP3f1 adds generated FAT
presence/absence/unreadable results and cancellation after inspection, bringing
Qt analysis coverage to 18 behavioral cases without changing the form. CP3f2
runs the embedded-EFI case for both UDF and ISO9660 (19 cases/data rows total).
CP3g1 adds WinPE options for both views (21 cases/data rows total), showing READY
for completed WinPE-only metadata and Windows metadata pending when its options
are unreadable. Cancellation clears the result through the same controls.
Screenshots of collapsed
and expanded forms were inspected against the source layout. A live Windows
comparison and native desktop theme/dialog checks remain manual qualification;
do not describe the offscreen tests as proving those.

Add image-specific options and additional modes at their backend checkpoints,
retaining the original conditional layout and interaction. Do not introduce a
new summary panel, rearrange the fields, or redesign the workflow while adding
Linux service support.


## CP18e — standalone UEFI:NTFS behavior (2026-09-28)

`SetBootOptions` adds UEFI:NTFS only in advanced drive mode and restores the
image selection when that section is collapsed. Qt follows that behavior while
retaining an already loaded image. `EnableControls` keeps SELECT enabled in all
idle modes, including Non bootable and UEFI:NTFS; selecting a new image switches
back to image mode. No additional summary panel or form field is introduced.

`SetPartitionSchemeAndTargetSystem` defaults standalone UEFI:NTFS to GPT and
remembers explicit MBR/GPT choices, with UEFI (non CSM) as its only target. An
inapplicable prior SFD preference falls back to MBR as the first allowed entry.
Above 2 TiB, native MBR cannot represent the whole device, so Qt retains its
existing GPT-only gate. `GetAllowedFileSystems` supplies the NTFS/exFAT mask;
native geometry filters cluster sizes using the same reserved loader extent as
the helper. The default filesystem marker is determined before boot filtering,
so it need not appear beside either offered filesystem. Existing/manual format
selection and the original cluster defaults apply.

The label follows `SetProposedLabel`: an ISO label applies only in image mode;
standalone selection uses the device/proposed label unless manually edited.
Windows image options and ISO/DD dialogs remain hidden for a standalone job.
The confirmation names UEFI:NTFS, the selected partition scheme, filesystem,
normalized label and cluster size, without claiming an image will be copied.
Shared progress uses BT_UEFI_NTFS, with success only after helper exit.

The window and simulated-device lifecycle tests cover these transitions at
normal scale, with focused 2×-scale checks and a retained offscreen screenshot.
Source tracing and Qt fixtures do not replace a future live Windows comparison
or physical-device/desktop authorization qualification.


## CP19e — normal FreeDOS choice (2026-09-29)

`SetBootOptions` offers FreeDOS outside advanced drive properties. Qt now does
the same for the qualified backend; expanding/collapsing that section preserves
FreeDOS. `SetPartitionSchemeAndTargetSystem` restricts it to MBR and BIOS
(or UEFI-CSM). Shared Rufus rules supply FAT16/FAT32 eligibility and native
geometry filters the clusters. Source ISO/Windows options are not applied;
SELECT still works, and a retained ISO can be selected again.

The explicit Linux difference is the user-requested DOS settings dialog before
authorization: choose from Rufus's own keyboard/code-page tables, default US/437,
without Linux host detection. Cancel stops preparation, accepted settings are
frozen, and the ordinary erase/partition warnings still precede one helper job.
The main form acquires no extra fields or summary panel.

Windows's `format.c` advances one preceding DOS copy step and `ExtractDOS`
advances five more; MSG_230 is “Copying DOS files...”. Qt consumes all six through
the shared original progress policy, preserving its discrete weighting and float
rounding. The caption does not add an ISO-style percentage. Both ordinary and
large FAT32 progress are covered. READY remains full only after successful sync,
cleanup and helper exit, and remains full if the completed USB is unplugged.
Offscreen workflow and normal/scaled window tests do not replace future live
Windows/desktop comparison or physical-device qualification.
