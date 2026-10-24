# Qt / Win32 GUI behavior audit

> **Historical baseline:** the CP12–14 control table below is not the current
> supported-feature list. CP42/51/58/59/61/67/68/71 have since delivered
> persistence, old-BIOS fixes, Windows To Go, settings/logging, runtime validation,
> language/update actions and advanced device controls. The
> [2026-10-23 audit](linux-parity-audit.md) and
> [completion register](linux-parity-register.json) define current open work;
> [support](linux-support.md) defines delivered routes. Retained historical
> “disabled,” “hidden” and “remaining” statements below are superseded there.

CP12n, updated through CP14e on 2026-09-24. Compare the original event handlers and control updates, not
just whether a helper can create media. This is a source comparison with Qt
interaction tests. Native Windows execution and a visual comparison on Windows
remain deferred as agreed.

The [support table](linux-support.md) summarizes current creation routes,
runtime requirements and the boot evidence behind them. This audit tracks the
interaction details and remaining differences.

## Implemented image-creation workflow

| Interaction | Windows reference | Qt behavior / evidence |
| --- | --- | --- |
| No image | `EnableControls`, `UpdateImage` in `src/rufus.c` | Empty READY, SELECT available, START disabled. Empty device dropdown remains enabled. No invented boot/format choices. |
| Device selected, no image | `PopulateProperties`, `SetFileSystemAndClusterSize`, `EnableControls` | Shows supported native filesystem/cluster defaults but leaves both fields disabled. START remains disabled; discovery does not authorize or write. Empty/removed targets clear the lists. |
| Initial device discovery | `GetDevices` in `src/dev.c` | Eligible devices sorted by increasing capacity; the smallest is selected on the first populated list, including arrival after an empty startup. Selection alone neither authorizes nor writes. Equal-capacity devices keep discovery order. |
| Device refresh | `GetDevices`, `WM_DEVICECHANGE` | Keeps the selected identity across cosmetic updates, reordering and arrival of a smaller device. Linux deliberately requires reselection after identity loss instead of Win32's first-drive fallback; see below. |
| File picker cancelled | `IDC_SELECT` | Keeps the previous valid image and device; tested with a boot-marked raw image. |
| ISO/UDF scanning | `ImageScanThread`; `ExtractISO` in `src/iso.c` | Locks selection/format controls, offers CANCEL, keeps Log usable. Marquee now has a visible “Scanning image...” caption; Qt's native busy bar otherwise omits its format text. |
| Successful load | `ImageScanThread`, `SetProposedLabel` | Basename in Boot selection, short “Using image: filename”, proposed ISO label, empty READY, focus on START when enabled. Detailed scan information stays in tooltip/log. |
| New image | `ImageScanThread`, `UpdateImage` | An accepted new ISO resets the user-edited label; rejection preserves a manual edit. Resets cluster selection, recomputes filesystem defaults, clears old analysis and source identity, updates Windows options. Late results from the previous scan cannot restore stale controls. |
| Volume-label proposal | `GetDriveLabel`, `SetProposedLabel`, `SizeToHumanReadable` | Uses the existing filesystem label before image selection, or Rufus's decimal capacity suggestion. Loaded ISO labels take precedence; manual edits survive device refresh/change and rejected images. Raw DD labels remain empty. Discovery needs no mounts or device opens. |
| Failed/cancelled scan | `ImageScanThread` failure path and final `MSG_210` | Clears filename, size, retained source and image-specific settings; restores disabled device filesystem/cluster defaults, retaining an allowed manual filesystem preference. Notification with details; “Failed to scan image” in the lower bar and empty READY. CANCELLED/FAILED write states are separate from rejected selection. |
| Complete scan without a recognized boot method | `ImageScanThread` boot-method check | Rejects at load, clears selection, shows “Unsupported image” and “No image selected”. Incomplete analysis is still reported as incomplete, not reclassified as proven absence. |
| Policy requires DD | `ApplyIsoPolicy`, `EnableBootOptions` | Shows Win32's ISOHybrid explanation at load; disables label/partition/target/filesystem/cluster controls and clears quick-format/extended-label checks. START goes directly to DD confirmation. |
| Hybrid supports ISO and DD | `BootCheckThread`, shared `GetImageWriteChoices` | Keeps format controls at load. Offers ISO as recommended default and DD at START. Selecting DD does not require a native Syslinux installer. |
| Windows installation ISO | `UpdateImage`, `ToggleImageOptions` in `src/ui.c` | Shows “Image option” / “Standard Windows installation”. Windows To Go is visible but disabled, with an explanation of the missing Linux backend. Switching to a raw image removes the row and shrinks the form. |
| Partition/target dropdowns | `SetPartitionSchemeAndTargetSystem`, `EnableControls` | Before image selection, disabled fields preview the existing table (MBR for raw/unknown), with BIOS/UEFI targets for MBR and UEFI for GPT. Image loading replaces the preview with supported native creation choices. Qualified choices stay enabled even with one item; both disable during work and without a selected target. |
| Syslinux MBR firmware target | `SetPartitionSchemeAndTargetSystem` in `src/rufus.c` | CP12p qualifies Alpine MBR/FAT32 under BIOS and UEFI. Regular EFI files produce “BIOS or UEFI” in the dropdown and confirmation; BIOS-only images retain “BIOS (or UEFI-CSM)”. GPT still shows “UEFI (non CSM)”. |
| Device / filesystem changes | `SetFileSystemAndClusterSize`, `IDC_FILE_SYSTEM` | Device changes retain a manual filesystem preference if allowed and retain the edited label; cluster selection returns to the new target's default. Shared Rufus filesystem and cluster policies still choose supported defaults. |
| Filesystem display | `SetFileSystemAndClusterSize`, `MSG_030` | Linux uses “FAT32” at every supported size, by the user's decision: its native formatter does not switch at 32 GiB. “(Default)” marks the first native format supported by the target, before image filtering; it does not follow manual selection or the ISO's preference. Cluster labels already use Win32's units and default marker. |
| Existing filesystem | `PopulateProperties`, `SetFileSystemAndClusterSize` | Before image loading, preselects the detected NTFS or known FAT16/FAT32 if native geometry permits. Manual choice takes precedence; unsupported/unknown formats use native defaults. Refresh updates only idle automatic choices. Loading an ISO follows shared image policy; metadata changes cannot reset its filesystem or custom cluster size. |
| START declined before creation | `UM_FORMAT_START` / `aborted_start` | Empty READY, no authorization or write. Retry is possible. Cancelling an active write remains CANCELLED and waits for cleanup. |
| Completed write cancellation | `UM_FORMAT_COMPLETED`, `MSG_041` | DD and ISO lower-bar text uses “Operation cancelled by the user”, preserving the full incomplete-media diagnostic in tooltip/log. Normal and enlarged-scale checks ensure the message fits beside the clock. The yellow CANCELLED bar and cleanup gate remain unchanged. |
| Creation starts | `UM_TIMER_START`, `ClockTimer`, `UM_FORMAT_START` | Focus on CANCEL; elapsed `hh:mm:ss` begins with the actual worker/helper job, not while scanning or choosing settings. Qt uses its monotonic clock. |
| Progress and completion | Shared `progress_policy`; `UM_FORMAT_COMPLETED` | Existing operation percentages retained. Timer continues through sync/cleanup and stops only at the terminal state. Full green READY and a single completion notification still require helper/worker exit. Unplugging afterward preserves success. |
| Log, advanced sections, keyboard, scaling | `IDD_DIALOG`, `src/ui.c` | Existing expand/collapse order, log lifetime, keyboard navigation and enlarged-font/scale tests retained. Windows option row tested at normal and enlarged scale. |

## Explicit limits and follow-up work

This does not mean the entire Windows feature set is available. Keep these
distinctions visible in future checkpoints:

- Windows To Go, persistence, Windows BIOS/dual FAT16, older EFI/FAT and remaining legacy routes, other bootloader-only
  modes and filesystems beyond the qualified FAT16/FAT32/NTFS routes need native
  implementations. The persistence row is still hidden; this is an outstanding
  GUI/feature gap, not a claimed match to `ToggleImageOptions`.
- Language/settings actions, old-BIOS fixes and runtime UEFI validation
  are still unavailable controls. CP17d3 enables List USB Hard Drives/Alt+F with
  its original tooltip/status and existing erase confirmations. Quick format is fixed by the implemented creation route.
  Do not enable a control whose requested operation would be ignored.
- Existing-device filesystem discovery now supplies type/version from the same
  volume as its label. Preselection is limited to supported NTFS and known FAT16/FAT32;
  the broader target matrix is not fully reproduced. Windows's forced Large FAT32 mode has no separate
  Linux equivalent: Linux uses its native FAT32 formatter at every size.
  Linux retains the conservative removable-device eligibility from CP2.
  CP14c supplies existing partition-table metadata for the disabled idle preview;
  its updates do not replace the selected physical device or choose a write mode.
  CP14d adds existing filesystem labels and capacity-based label proposals;
  CP14b covers native filesystem/cluster defaults only.
- Label discovery uses the first filesystem by partition offset, including
  unmounted media, because Linux has no Windows drive-letter ordering. Whole-disk
  filesystems use their own label. An unlabeled first filesystem gets a capacity
  proposal even if a later filesystem has a label. CP65 adds mounted `autorun.inf` extended labels and Multiple Volumes captions;
  discovery does not mount media or request privileges merely to obtain them.
  See [device discovery](linux-device-discovery.md) for matching and fallback rules.
- Default markers consider Linux's implemented native FAT16/FAT32/NTFS formats.
  CP15f marks FAT16 on eligible small GPT/UEFI targets, while a suitable EFI ISO
  still prefers FAT32 when available, through Rufus's shared policy. Existing
  FAT16 can be preselected before image loading. CP15j adds the qualified
  MBR/Syslinux FAT16 route, including FAT16-only small targets. A Syslinux image
  still prefers FAT32 when both fit. Manual filesystem choices survive partition
  and target changes when allowed; a successful new scan reapplies image policy.
  If the image filters out the target's default format, no remaining item gains
  a replacement marker. For example, a Windows image requiring NTFS on an ordinary
  FAT32-capable USB displays plain “NTFS”, as on Windows.
- After a selected device disappears or changes identity, Win32 falls back to
  the first listed drive. Linux clears the target and requires explicit
  reselection, including after a UDisks restart or failed snapshot. A path can
  now describe different media, so an old selection/confirmation must not bind
  to the replacement. Discovery also must not reset a completed READY state.
  This preserves Linux's existing invalidation behavior; only initial selection
  is automatic. Active-job cancellation and final cleanup gates are unchanged.
- Syslinux MBR now describes BIOS/UEFI for regular EFI payloads after CP12p's
  Alpine boot qualification. Secure Boot and uncertain embedded-boot analysis
  remain explicit pending checks. Other distributions, architectures and physical
  firmware are not qualified by that one fixture. These limitations must not
  disappear behind a READY caption.
- Confirmation includes the concrete native layout and label; Cancel is the
  default destructive-confirmation action. Linux authorization and cleanup
  messages describe the actual helper lifecycle. Root authorization remains
  once per creation, through the existing unprivileged GUI approach.
- Native Linux themes can draw controls differently. The source comparison,
  offscreen rendering and keyboard tests do not establish pixel identity with
  Windows or qualification on every desktop environment.

Continue closing these gaps in bounded feature checkpoints. No new Windows
customization qualification is required: the previously agreed single case
still defines that scope. See the [checkpoint evidence](testing-and-checkpoints.md)
and [port plan](linux-port-plan.md).


## CP16e — Non-bootable GPT/exFAT

The source comparison uses `src/rufus.c` SetBootOptions/SetPartitionSchemeAndTargetSystem,
SetProposedLabel, EnableControls and IDC_BOOT_SELECTION, `src/format.c`'s
ToValidLabel call, and the already shared `src/format_policy.c` and
`src/progress_policy.c` implementations. Non bootable precedes Disk or ISO image;
image mode remains the initial choice. Switching to non-bootable retains the
loaded image, disables SELECT and image-specific options, uses the device's
proposed label unless manually edited, and shows BIOS or UEFI. Switching back
restores the image and its eligible ISO/DD choices. A new image selection returns
to image mode. Busy jobs cannot change mode or settings.

The initial supported non-bootable route is GPT/exFAT only. Windows offers other
filesystems, MBR and SFD, and can prefer MBR. This is a temporary qualification
boundary, not completed selection parity or a permanent GPT preference. Additional
routes must get their own complete helper/sync tests before appearing in the UI.
Quick format is selected; full format, bad-block tests and extended label/icon
creation remain unavailable. exFAT is not added to ISO filesystem choices.

Win32 normalizes exFAT labels with its FAT-style ToValidLabel flag. Qt calls the
same shared algorithm and shows the effective label before authorization. The
native adapter retains its Unicode-capable lower-level interface; the GUI follows
Rufus's spelling. Cluster choices preserve whole-disk defaults but exclude values
whose native metadata does not fit the actual GPT extent. Enhanced Windows install
preferences cannot alter a non-bootable request or reset its cluster selection.

The existing creation controller supplies asynchronous preflight, destructive
confirmation, one authorized helper, cancellation, progress and completion. It
uses the original non-bootable allocation (ten exFAT filesystem steps, no copy
slots). The native CLI has no intermediate progress callback; completed stages
advance to their shared boundaries. The first finalization step is 15/16 of the
bar (61439/65535), never full READY. Success requires the entire planned partition
to be formatted, zero copied bytes, sync/cleanup success and helper exit. FAILED
and CANCELLED do not notify success; unplugging after completion preserves the
full READY state and its single notification.

## CP16g — Non-bootable GPT/FAT16 and FAT32

CP16g extends CP16e's mode to CP16f's qualified FAT jobs. Compare Win32
SetFileSystemAndClusterSize, its selection of an existing/manual filesystem,
and InitProgressBar in `src/ui.c`. The same shared allowed-filesystem and cluster
rules now populate FAT16, FAT32 and exFAT for non-bootable GPT. The first supported
format carries the Default marker; a valid existing/manual choice can select a
different format. FAT16 disappears at 4 GiB and FAT32 at 2 TiB; native geometry
also removes unusable cluster choices. Keep the user-approved plain FAT32 label
at all capacities. Non-bootable NTFS and MBR/SFD still need their own complete
qualifications, so the list remains temporarily narrower than Windows's.

The retained image is excluded from policy, preflight and the source-free
request. Confirmation names Non bootable, GPT, the chosen filesystem, normalized
label and cluster size. Device/provider checks repeat before authorization.
FAT16/FAT32 use Win32's five filesystem steps: the first finalization step is
10/11 (59577/65535). At 32 GiB and above FAT32 uses Win32's analog-format weighting,
with its first finalization step at 99% (64879/65535). The native CLI supplies no
intermediate progress callbacks, so progress advances at operation boundaries.
Full READY and its single notification still require successful sync, matching
partition extent, zero copied bytes and helper exit. Native FAT's bounded
track-rounding tail is allowed; exFAT still requires the full partition length.

Functional Widgets cases cover both FAT formats with retained Windows images,
ordinary and simulated large targets, normalized confirmation, one helper,
result-before-exit waiting, unplug after success, denial, sync/cleanup failures,
wrong layout, omitted finalization, cancellation and missing formatter before
confirmation or authorization. Window cases cover default ordering, existing
formats, unsupported NTFS fallback, manual selection and capacity limits.


## CP16n — Non-bootable MBR/GPT selection

Win32 references are `SetPartitionSchemeAndTargetSystem` and
`IDC_PARTITION_TYPE` in `src/rufus.c`. Non bootable defaults to MBR even when the
existing device has GPT. An explicit choice is remembered, including activating
the already-selected entry. Selecting a disk above 2 TiB updates that preference
to GPT; returning to a smaller disk retains it. Both MBR and GPT display BIOS or
UEFI. Manual filesystem and label choices survive table/device changes when
valid; cluster choices are recomputed using native geometry. Switching back to
an image restores its supported image routes and does not turn plain MBR into
a Syslinux job.

Linux now offers MBR with FAT16, FAT32, NTFS and exFAT through CP16k/l/m's
qualified helpers. Confirmation shows the selected table, filesystem, normalized
label and cluster size. Preflight repeats the selected layout before confirmation
and authorization. The helper result must match that exact extent; receiving a
GPT extent for an MBR job fails. Existing Win32 progress weights and completion
semantics are unchanged. Table changes are blocked while checking/writing; full
READY and its one notification require successful sync/cleanup and helper exit.

One explicit limit differs: Win32 still lists MBR above 2 TiB and handles its
limitations later; Qt omits that entry because the current 512-byte-sector MBR
backend cannot represent the full target. Keeping only GPT prevents offering an
unusable full-device format. SFD remains unavailable until its backend is ready.
This does not add exFAT ISO extraction or another Windows customization test.


## CP17a — ISOHybrid DD without the preparation helper (historical)

Superseded by the 2026-09-27 [consolidation](linux-consolidation.md): all DD
jobs now use the matching helper. Missing helpers gate creation; ISO/DD choices,
recommendation, progress and completion behavior remain.

The original ISO/DD policy and dialog are unchanged: ISO remains recommended
and selected when both modes are offered; DD-only images skip the dialog and
disable format controls. START now reflects availability of each offered mode.
DD uses UDisks and works even with a missing/non-executable ISO helper. Selecting
ISO in that situation reports a missing-helper error without switching to DD
or requesting authorization. The image's ISO formatting choices remain visible
before mode selection. ISO-only and non-bootable jobs still require their helper.

Helper availability is checked before ISO/non-bootable preflight and again after
confirmation, including removal while a dialog is open. DD retains source-change
rejection across mode selection, normal destructive confirmation, cancellation,
UDisks denial handling and READY only after sync/verification/completion. No new
privilege or write backend is introduced. This fixes a Linux dependency gate;
there is no corresponding external-helper dependency in the Windows UI.


## Full parity scope (2026-09-25)

All outstanding Windows features and conditional GUI behaviors remain in scope,
including SFD and advanced/legacy choices. The [feature backlog](linux-feature-parity.md)
tracks them; checkpoints can be implemented in any dependency-compatible order.
Do not enable placeholder controls before their operation works. The existing
Windows-regional-settings exception and single-customization test limit remain.


## CP16r — Non-bootable SFD/FAT32 selection

Traced `SetPartitionSchemeAndTargetSystem` and `IDC_PARTITION_TYPE` in
`src/rufus.c`, `SetComboEntry` in `src/ui.c`, and `sfd_name` in `src/drive.c`.
Qt uses the same MBR, GPT, Super Floppy Disk order and BIOS or UEFI target for
Non bootable. It defaults to MBR, remembers explicit selection across device and
boot-mode changes, falls back to the first supported entry without losing the
preference, and resets the preference to GPT above 2 TiB. ISO/DD modes do not
offer SFD. Returning to an ISO with a remembered SFD choice falls back to the
first offered partition entry; it must never send an SFD image-creation request.

The temporary qualification difference is filesystem availability: only FAT32
has a complete native SFD route, so selecting SFD filters to FAT32 and omits SFD
on capacities that cannot fit it. Windows permits other filesystems. This gate
prevents exposing an unsupported destructive job; FAT16/NTFS/exFAT remain planned
implementation work. MBR/GPT retain all four qualified filesystems, and returning
to either restores a still-valid explicit filesystem preference. SFD cluster
choices use the entire device at offset zero, without GPT/MBR space deductions.

The confirmation explicitly says Non bootable — Super Floppy Disk, FAT32, with
normalized label and cluster size, and keeps Cancel as default. Formatting and
finalization use the existing Rufus progress policy, including large FAT32
weighting. Completed READY requires helper exit, successful final sync and the
whole-device result. Sync/protocol failures and cancellation never show full
success; unplugging after success preserves the completed state and does not
repeat the notification. No automatic eject or mount is added.


## CP16t — FAT16 alongside FAT32 for SFD

The existing SFD selector now offers FAT16 and FAT32 using original Rufus
filesystem order and cluster policy, filtered by native whole-volume checks.
Small FAT16-only media can select SFD; overlap offers FAT16 (Default) then FAT32,
with a valid discovered/manual filesystem taking precedence. Unsupported NTFS
or exFAT preferences fall back without being overwritten and return when switching
to MBR/GPT. Explicitly choosing that already-displayed fallback now records the
choice, matching IDC_FILE_SYSTEM's selected_fs assignment in Win32. Automatic
fallback still does not create a user preference.

Tests cover 3/16/64 MiB, 4095 MiB, the last sector below 4 GiB and the 4-GiB policy
limit without allocating large images. Near 4 GiB the native whole-device FAT16
cluster count exceeds its limit; unlike a smaller MBR/GPT partition, that extent
cannot offer FAT16. FAT32 remains available. Original non-bootable defaults,
remembered partition choice and BIOS or UEFI target are unchanged. FAT16 uses the
existing confirmation text, normalized labels, progress weighting, cancellation,
one launcher and full READY only after successful helper exit. No new GUI control
or helper lifecycle is introduced. NTFS/exFAT SFD are still pending qualification.


## CP20d — advanced standalone Syslinux 6

`SetBootOptions` puts the embedded Syslinux versions under advanced drive
properties. Qt now offers the qualified **Syslinux 6.04** FAT path there, after
FreeDOS and before UEFI:NTFS, with Windows's MBR-only layout and
**BIOS (or UEFI-CSM)** target. The provider remains the pinned 6.04-pre1 payload.
The original shared filesystem/cluster policies supply FAT16/FAT32 defaults;
unsupported capacities disable START. Windows's v4 choice and v6 NTFS support
remain pending provider qualification, not removed from scope.

The retained image survives standalone selection. Its Windows options disappear,
it does not enter the source-free job, SELECT can replace it, and collapsing
advanced properties returns to image mode. Explicit filesystem/partition
preferences survive temporary restrictions. No new host-setting detection occurs.
Confirmation names the standalone loader and format settings and defaults to
Cancel. The one-helper path checks tools both before confirmation and before
authorization. The installation caption is `Installing Syslinux 6.04...`, without
invented timed progress. The shared Windows allocation keeps three copy slots
with two actual events. Finalization and helper exit precede full green READY;
sync/cleanup failures remain FAILED and unplugging after success preserves READY.
The screenshots cover selection, installation and completion; normal/2× scaling
and protocol-fixture tests are recorded in CP20d's testing record.


## CP22e — Windows BIOS and dual NTFS selection

Known Windows 10/11 installers with root BOOTMGR offer MBR/NTFS through the
existing writer. EFI-capable Windows images default to GPT/UEFI; a manual MBR
selection shows BIOS (or UEFI-CSM). Alt+E refreshes the partition and filesystem
controls, defaults qualified Windows images to MBR, and shows BIOS or UEFI for
EFI-capable dual mode. Explicit GPT remains selectable. FAT BIOS/dual creation
is gated until the original Windows FAT records are integrated; GPT FAT32/WIM
splitting remains available by selecting GPT explicitly after Alt+E.

Alt+A persists the original Rufus/Windows 7 MBR choice. Both shortcuts retain
Win32's 3.5-second messages and are blocked during scanning or active creation.
Plain BIOS confirmation names BIOS, even when EFI files are copied; dual names
BIOS or UEFI. Windows eligibility is shared with fresh helper preflight and the
native geometry check uses the actual Windows bootstrap/extra partition. No
additional writer, privilege launcher, progress engine or completion path is
introduced. BIOS bootloader status names the partition boot record. Full READY
still waits for sync, cleanup and helper exit; copying/results alone do not finish.


## CP22l3 — Older BOOTMGR plain BIOS/NTFS selection

Known normalized Windows 7.0/8.0/8.1 installers with root BOOTMGR and no mixed/
legacy loader now offer the qualified plain MBR/NTFS route. Original shared
filesystem and extra-partition policy supplies NTFS without UEFI:NTFS. Alt+A,
confirmation, progress and completion use existing Windows BIOS paths; original
customization rules offer no Windows User Experience/edition chooser here.

Temporary Linux limitation: Windows Rufus also supports older EFI/FAT and the
Win7-style WIM payload repair, which are not yet implemented/qualified on Linux.
Qt therefore exposes only BIOS/MBR/NTFS for these sources, including those with
regular fallback EFI files. It does not offer a GPT choice that fresh preflight
would refuse. A saved Alt+E preference is retained for later eligible images,
but is ineffective for this request; Alt+E reports the limitation without
changing the saved preference. This is an explicit capability restriction,
not a claim that Windows's full older-installer defaults are already reproduced.
Native MBR geometry gates START; device replacement still requires reselection.
EFI/FAT enablement follows separate payload/preflight/helper/handoff work.


## CP22m6 — Older EFI/FAT32 choices and content-driven targets

The temporary older BIOS/NTFS-only gate is removed for known Windows 7/8/8.1.
As in `SetPartitionSchemeAndTargetSystem`, Qt uses `IS_EFI_BOOTABLE`, including
root-only Windows EFI layouts, for the GPT choice, image default and dual target
text. These sources now default to GPT/UEFI unless an explicit supported choice
exists; Alt+E defaults BIOS-capable sources to MBR, permits FAT32 and names
BIOS or UEFI when EFI-capable. BIOS-only sources retain MBR/BIOS. The same fix
applies to modern installers with Rufus's content-driven repair layout. Plain
MBR remains NTFS/BIOS; GPT FAT32/NTFS and explicit GPT after Alt+E remain selectable.

Alt+E and Alt+A persist normally and stay blocked during scans/checks/active
jobs. Confirmations use the same EFI predicate as selection. UEFI-only installers
need no BIOS BOOTMGR and continue through GPT; older FAT16 remains hidden on
both targets until separately qualified. Fresh preflight still owns version,
source, x64 loader, repair and mixed/legacy validation. Older images do not gain
the modern Windows User Experience/edition dialog. Existing helper, progress and
READY-after-sync/cleanup/exit behavior are reused.


## CP23b — Image checksum action and optional hashes

`IDC_HASH` / `HashThread` / `HashCallback` now have an ordinary Qt action. The icon
is enabled for an accepted idle image, without depending on USB/helper eligibility.
MD5, SHA1 and SHA256 are default; Alt+H toggles the saved EnableExtraHashes/SHA512
choice and status message. The selected source and algorithms are frozen before
running, with the same busy exclusions as scans/writes. CANCEL bypasses a data-loss
confirmation, discards pending success and keeps controls blocked until joined
cleanup. No read-only checksum action launches pkexec.

A GUI-thread window-modal dialog follows joined success, preserving basename title,
monospaced read-only/selectable fields, SHA512 hint and OK/Escape. Hexadecimal wraps
without changing copied bytes. Font size follows the application; field width stays
within the screen, with scrolling available for narrow large-font content.
Progress reserves full green READY and one beep/alert for joined success, including
when a result has already reached the GUI queue. Failures show an error dialog;
cancellation uses the existing Linux status instead of Windows' additional
informational modal notification. Windows' per-algorithm throughput remains a
qualification item. Native source changes are detected instead of relying on
Windows file-share exclusion. See CP23b's record for tests and screenshots.

## CP35 — save-device action

The normally visible save icon beside Device is enabled for an idle selected
USB independently of input-image selection. It offers VHDX (default), fixed
VHD and GPT-only FFU, remembers the chosen format, and participates in the
ordinary control lock, cancellation, clock, status and completion flow. One
read-only UDisks authorization covers capture and consistency checks. READY
requires durable output publication, all closes and the joined worker. Existing
output remains until commit. A disconnected source after success does not
clear READY or repeat the notification. CP36 adds automatic source unmount and ISO export; see the support guide for capture format limits.


## CP36 — filesystem export and automatic unmount

The save picker offers ISO when discovery identifies a filesystem. VHDX stays
the default, and the last chosen format is remembered. Mounted sources use one
source-helper authorization for the complete job. ISO exports the whole-disk
filesystem or first partition filesystem by offset, matching Linux label
discovery. Busy sources report a close-files-and-retry diagnostic. The normal
cancel, control lock, progress, READY and post-success unplug behavior applies.
Cleanup failures retain FAILED and preserve the previous output. The source
is left unmounted without automatic eject or remount.


## CP37 — read-only capture sources

Discovery and the save action accept eligible read-only USB devices. The Device
list adds `[Read-only]`, START explains the save action in its tooltip, and
format controls remain disabled. A known read-only capability change keeps the
physical selection; a transition to read-only invalidates pending/active raw or
ISO creation without cancelling an independent save. Unknown state still
invalidates discovery. Image selection remains usable. Capture permissions do
not propagate into default write preflight or the native kernel write checks.


## CP48 — bad-block testing, preview 17 (2026-10-20)

Five Windows pattern modes and default fake-capacity detection now run before
creation, including DD. Alt+B saves the explicit detector preference. Reports
and Abort/Retry/Ignore stay within one authorized job and joined READY.
Partial FFUs needing existing base bytes refuse destructive testing before target
access. Qualification groups native/sanitizer cases, combined guests, one Qt gate
and inspected packages. See the support guide for exact coverage and remaining
Windows parity work.


## CP49 — extended labels and icons, preview 18 (2026-10-20)

The default-checked appearance option now writes the original Rufus icon and
Windows UTF-16LE autorun label metadata, preserving image-owned autorun files.
The job freezes original Unicode text independently of filesystem normalization.
FAT16/FAT32/NTFS/exFAT and SFD use checked private mounts, one authorization and
joined READY. DD/ext disable the effective option and retain the user's choice.
One normal/sanitizer corpus, focused GUI checks, combined guests, one Qt gate
and inspected packages qualify delivery. See the support guide for exact limits;
CP65 later adds existing mounted extended-label discovery; other parity groups remain active.


## CP59 desktop preferences and logs

The [desktop guide](linux-desktop-settings.md) records portable INI selection,
cache/report storage, startup preferences and instance wait, saved advanced
panels and full-session automatic/persistent logs. Ctrl+L/P and Alt+R/U use their
original Windows meanings. A visible 2,000-block tail does not truncate exports.
Normal/portable settings share the existing controller paths; the GUI stays
unprivileged. Later batches enable policy controls (CP63), language selection
(CP67) and native application release checks (CP68); their dedicated guides
record the current behavior and remaining work.


## CP67 — application language selection and catalog reuse (Preview 36)

The [language batch](linux-localization.md) reuses the unchanged original
38-language catalog and shares quoted-argument parsing with Windows. The native
selector, saved `Locale`, name/numeric CLI choice, base fallback, checked menu,
explicit uppercase and RTL layout work together. Live switching retains the
controller, device/image/filesystem, user labels and complete session log.
Unmatched native-only text remains English and translation coverage stays open.

One grouped normal/sanitizer parser/catalog/UI qualification, one combined
networkless diskless guest route as UID1000, one full nine-suite Qt gate and
inspected native/TGZ packages use default Ninja parallelism. Existing writer,
helper, boot, fault/polkit and agreed customization evidence is reused; no host
media is mounted or written. Strict external-catalog transaction/bounds and
Qt relabeling differences are documented. The complete Windows parity goal
remains active across the other unresolved groups.


## CP68 — native application release checks (Preview 37)

The [application-update batch](linux-application-updates.md) groups native release
catalog parsing, numeric/channel/architecture eligibility, scheduled and manual
checks, beta/forced discovery, cancellation, deferred notifications and explicit
release-page/package-manager handoff. It preserves existing Secure Boot consent
and timestamps, keeps notes as plain text and reports an empty native catalog
separately from an up-to-date release. Publishing and distribution installation
remain delivery work; Windows executables are excluded from native offers.
The stale-translation diagnostic and redundant-language-event dialog lifetime
repair are included in the same application-controls batch.

One grouped normal/sanitizer qualification, combined diskless loopback-only
guests as UID1000, live public metadata, one full nine-suite Qt gate and inspected
native/TGZ packages reuse existing writer/helper/boot/fault/polkit/customization
evidence. No host media or installation is touched. Complete Windows parity
remains active across the remaining feature and distribution groups.


## CP69 — ISO read options and timestamps (Preview 38)

The [ISO controls batch](linux-iso-options.md) groups Alt+I/J/K/S/T: session
ISO/Joliet/Rock Ridge and size checks, saved timestamp preservation, coherent
selected-source analysis/preflight/extraction and the explicit MS-DOS 64 KiB
cluster override. Physical capacity and actual filesystem bounds remain enforced.
Ordinary ISO9660/UDF copies preserve access/modification times when enabled;
creation time uses native filesystem behavior. Directory times are applied after
children so copying cannot immediately overwrite them. See the guide for the
Windows behavior, Linux differences and qualification limits.


## CP70 — NTFS compression and indexing (Preview 39)

The [NTFS options batch](linux-ntfs-options.md) groups session Alt+N compression,
saved Alt+Q indexing, frozen NTFS-only options, native formatter defaults and
boot-file compression exceptions. Larger clusters continue uncompressed with
a diagnostic. Compressed file creation preselects the qualified NTFS-3G provider
before writes; ordinary NTFS creation keeps the kernel provider. No write or
corruption error switches drivers. The guide records native directory-default
restoration, provider evidence and remaining UDF/ReFS indexing-hint work.
