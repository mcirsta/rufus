# Windows BIOS and dual boot: reuse audit

CP22a, 2026-10-01. This is an audit of the checked-out Windows dispatch and
existing Linux components. It enables no additional GUI route. Windows runtime
validation and further Windows customization installation tests remain deferred.

CP22b extends the existing MBR planner/writer to NTFS with either original Windows
bootstrap, plain or with trailing UEFI:NTFS. CP22c adds the explicit frozen
Windows request and shared BIOS preflight. CP22d1 integrates the existing NTFS
helper and checks file-backed success/fault/lifecycle behavior. CP22d2a qualifies
native combined creation/readback with both NTFS drivers. CP22d2b qualifies
generated BIOS/EFI handoff and both Rufus MBR prompt choices. CP22e enables the
qualified Windows BIOS/dual NTFS route in the existing Qt GUI. FAT dual and
legacy Windows routes remain separate work.

## What is already reusable

| Operation | Windows source | Existing Linux implementation / required change |
| --- | --- | --- |
| Main filesystem and extra-partition policy | `src/rufus.c:SetAllowedFileSystems`, `SetFSFromISO`; `src/format.c:FormatThread` | `GetAllowedFileSystems`, `GetPreferredFileSystem`, `GetExtraPartitions` in `src/format_policy.c` are already shared. Supply the actual BIOS target and frozen dual-mode flag; do not copy these rules. |
| MBR payload | `src/format.c:WriteMBR`, `src/ms-sys/br.c:write_rufus_mbr` / `write_win7_mbr` | `linux/platform/partition_image.c:SetMbrCode` includes the original 440-byte arrays and signature handling. CP22b extends the same planner to NTFS with either Windows bootstrap, with or without UEFI:NTFS. No bootstrap or layout arithmetic is duplicated. |
| NTFS partition boot record | `src/format.c:WritePBR`, `src/ms-sys/ntfs.c:write_ntfs_br` | `InstallNtfsBootRecord` in `linux/platform/ntfs_boot.cpp` already calls the same unchanged ms-sys routine through bounded memory I/O. `PrepareDeviceTarget::installNtfsBoot` provides the retained partition/whole-disk checks. CP22d1 extends its recognized-job gate to checked Windows requests and reuses the same I/O; native creation and generated firmware handoff are qualified in CP22d2a/b. |
| NTFS formatting and mounting | Windows formatters and volume APIs | Existing native mkntfs, preferred kernel `ntfs`, absent-kernel NTFS-3G fallback, partition verification and private mount lifecycle. No second provider or privileged process is needed. |
| ISO tree / large files | `src/iso.c:ExtractISO` | Existing shared analysis/extraction and native NTFS copying. The ordinary root `bootmgr` sets `HAS_BOOTMGR_BIOS`; do not require it to be a normal EFI PE application. |
| UEFI:NTFS payload for dual mode | `src/drive.c:CreatePartition`, trailing embedded image write | Existing shared reservation, MBR serialization and `WriteMbrUefiNtfsImage`. Reuse the unchanged embedded payload, with exact metadata/payload validation. |
| Windows customization and WIM metadata | `src/wue.c`, shared unattended content and native WIM adapters | Existing Linux metadata/options/customization implementation. Adding BIOS does not justify duplicating it or repeating the real customization qualification. |
| Copy progress and completion | `src/ui.c`, shared progress policy | Existing BT_IMAGE NTFS progress allocation and helper-exit completion gate. Retain one authorization, cancellation, sync and private-mount cleanup. |

The 4,136-byte NTFS record is already the original Windows bootstrap; it is not
a Linux-specific GRUB record despite having been integrated for GRUB jobs.
The source contains BOOTMGR and legacy NTLDR filename searches. This establishes
payload provenance, not support for every Windows release. The adapter preserves
the formatter's BPB/volume serial and the original backup sector, flushes partial
writes even on failure/cancellation, and verifies its result.

## Windows decisions that must remain distinct

For an ordinary Windows image containing a BIOS boot manager, `GetExtraPartitions`
omits UEFI:NTFS on a BIOS target unless dual mode is enabled. Plain BIOS media
therefore contains one active NTFS partition even if EFI files are also copied.
EFI-capable dual-mode NTFS media contains that active partition plus the inactive
trailing EFI/FAT UEFI:NTFS partition. FAT dual mode copies its EFI files directly
and does not need UEFI:NTFS. Ordinary installation is separate from Windows To Go;
its ESP/MSR rules must not be introduced here.

`WriteMBR` selects Rufus's MBR by default. `use_rufus_mbr` starts true and Alt+A
switches between it and the Windows 7 fallback. Preserve both payloads and that
user choice. `res/mbr/mbr.S` contains the USB key prompt/timeout and disk handoff;
replacing it with a generic Linux MBR would alter behavior. Its prompt is
conditional on finding another bootable disk, so a later boot fixture should
cover that condition explicitly rather than assuming the prompt is always shown.

`SetPartitionSchemeAndTargetSystem` normally prefers GPT for EFI-capable Windows
images, respecting an explicit supported choice. Alt+E enables enhanced dual
UEFI/BIOS mode, refreshes partition/filesystem controls and prefers MBR for such
Windows images. The Windows MBR target is BIOS (or UEFI-CSM) without dual mode,
and BIOS or UEFI with dual mode. Above 2 TiB, normal device refresh forces GPT;
unsupported native geometries remain gated. Do not infer these choices from the
Linux host.

Windows BIOS image mode excludes FAT without Alt+E. Dual mode permits FAT subject
to shared size/compatibility policy; the single oversized install.wim case uses
the existing split-WIM path. FAT16 uses the original standard PBR; FAT32 with a
Windows boot manager uses `write_fat_32_pe_br`, not the standard DOS PBR.
CP22f extends `fat_boot.c` with original Windows FAT records, including both
FAT32 PE prefixes/tails and physical BIOS drive-ID handling. Generated file/fault
qualification covers the record component; FAT layout/request/helper integration
and native/firmware qualification are still required. Do not enable it by merely
copying the ISO.

The Windows boot-manager branch takes precedence over native Syslinux in dual
mode on a mixed Windows/Syslinux image. `WriteMBR` also jumps to its Windows
selection first in that case. `WriteSBR` has its existing independent secondary
loader precedence; ordinary modern Windows needs no GRUB/Syslinux SBR. Initially
reject mixed/unqualified bootloader combinations at preflight and retain them
in the backlog rather than routing them through the Linux installer by accident.

## Current Linux gaps

- CP22e resolves Qt MBR selection/dispatch for qualified modern Windows NTFS
  images. Its shared BIOS eligibility function also supplies helper preflight.
  The existing controller dispatches an explicit Windows request instead of
  treating all non-Syslinux MBR images as GRUB; no new writer class is added.
- CP22e extends `NativeFileSystemClusterSizes` with the original Windows
  bootstrap choice and actual shared-policy extra-loader reservation. Existing
  Syslinux/GRUB geometry remains unchanged; Windows FAT is still gated.
- CP22c resolves the request-boundary gap: explicit Windows BIOS/dual operations
  retain source, firmware/dual-mode and MBR selection without reinterpreting an
  old operation. `usesUefiNtfs` and `isStandaloneUefiNtfs` distinguish the actual
  reservation from plain BIOS and blank-loader requests. CP22d1 integrates helper
  dispatch, including plain-BIOS bootloader phases in the existing client and
  mandatory copied-file validation. CP22d2a qualifies native creation/readback;
  CP22d2b qualifies the generated firmware handoffs.
- CP22d1 extends `prepareNtfs`'s standard-PBR branch and device gates to Windows,
  leaving GRUB-core installation specific to GRUB. The same flow writes optional
  UEFI:NTFS and performs extraction/customization/cleanup. It generates one disk
  signature and retains it for layout/payload validation. File-backed tests and
  native kernel/FUSE creation/readback pass. Generated BIOS/x64 EFI handoffs and
  both original MBR prompt choices pass in CP22d2b; Windows Setup is not qualified.
- CP22c resolves the BIOS report-check gap: known Windows 10/11 installation
  metadata and the BIOS marker are required, while only dual mode validates the
  actual fallback EFI loader. Shared policy checks the reservation. Existing GPT
  requirements remain unchanged; this preflight does not prove Windows boot.
- CP22e implements persistent Alt+A and refreshes Alt+E using the qualified
  BIOS/dual NTFS helper. Windows defaults/target labels and explicit GPT choices
  are retained; active scans/jobs keep their frozen settings. FAT BIOS/dual
  records are qualified in CP22f; the request/helper and native handoff still
  need integration before that filesystem choice is exposed.

## Legacy and adjacent work stays in scope

`WriteMBR` uses active byte 0x81 and forces Rufus MBR for legacy WinPE without
/minint. `src/wue.c:SetupWinPE` copies ntdetect.com and txtsetup.sif, supplies
SetupSourceDevice, copies setupldr.bin as BOOTMGR and patches loader paths,
CRC behavior and disk masquerading. Plain modern-Windows copying cannot replace
this. Audit the byte/config transformations for shared extraction with native
file access when implementing that separate checkpoint; avoid a broad Win32
compatibility layer.

Windows 7 EFI media can need `Windows/Boot/EFI/bootmgfw.efi` extracted from the
first installation image as EFI/BOOT/BOOTX64.EFI. Use the upstream WIM reader and
existing extraction facilities, not host Windows tools. Other architectures,
Windows To Go, standalone WIM/ESD, old-BIOS compatibility partitions and remaining
advanced options retain their separate backlog entries.

Windows remounts after changing the NTFS PBR, remounts before exit and calls
CheckDisk for NTFS ISO media. The Linux helper already installs the PBR before
creating its private mount, then syncs and removes that mount. Reuse that order;
do not add a final read/write desktop remount or advertise sync as a full
CheckDisk equivalent. Legacy WinPE/AIK repair requirements need investigation in
their own qualification; current metadata checks alone do not establish parity.

## Small implementation checkpoints

1. **CP22b — NTFS Windows MBR layout.** Extend the existing planner to accept
   original Rufus/Windows 7 bootstrap on NTFS, with and without the trailing
   UEFI:NTFS image. Reuse layout, serializer, clearing, identity and readback.
   Qualify exact payloads, active/type/signature/extent fields, boundaries,
   refusals, write/sync/cancel errors on generated files. No GUI enablement.
   Completed in CP22b; see [the component record](testing-and-checkpoints.md#cp22b--original-windows-mbrs-on-ntfs).
2. **CP22c — frozen request and preflight.** Add the explicit source-backed
   Windows BIOS/dual request to the existing protocol. Preserve retained source,
   boot choice and dual-mode policy; reject incompatible settings/layouts before
   authorization. Start with known modern installers on NTFS, leave unsupported
   combinations explicit, and regress existing GPT Windows and Linux requests.
   Completed in CP22c; see [the request/preflight record](testing-and-checkpoints.md#cp22c--windows-biosdual-request-and-preflight).
3. **CP22d — helper and native handoff.** Reuse NTFS preparation, original PBR,
   optional embedded UEFI:NTFS, extraction/customization and final cleanup within
   one helper. First qualify files/faults, then simulated USB with the preferred
   kernel driver and absent-kernel fallback. A generated BOOTMGR handoff fixture
   can check the BIOS chain without another Windows installation/customization
   matrix; test the original MBR prompt when a bootable second disk is present.
   CP22d1 completes helper integration and generated-file/fault/process checks.
   CP22d2a completes native creation/readback with both NTFS providers and original
   MBR choices. CP22d2b completes generated BIOS/x64 EFI handoff and the Rufus
   MBR key/timeout choices with another bootable disk. CP22e enables Qt selection.
4. **CP22e — Qt NTFS selection.** Enable the qualified BIOS/dual route and Alt+A/
   Alt+E behavior, with exact Windows defaults/target labels, manual choices,
   confirmation, shared progress and READY after helper exit. Keep GPT unchanged.
   Completed in CP22e; see the support guide and checkpoint record.
5. **CP22f and later — FAT dual and legacy paths.** Extend the existing FAT boot-record
   adapter with the original Windows record choice and reuse split-WIM copying.
   Qualify before enabling. Implement WinPE/Windows 7 and other retained parity
   groups separately; their absence is not a scope reduction.

CP22a's existing-component checks and exact limits are in
[the checkpoint record](testing-and-checkpoints.md#cp22a--windows-biosdual-boot-reuse-audit).


## CP22f — Original Windows FAT boot records

2026-10-02. Reuse the unchanged original standard FAT16 and FAT32 PE writers in
the existing adapter. `WritePBR` moves its second FAT32 record by six sectors;
therefore its separate sector-12 payload has a sector-18 backup. The adapter
stages each original record on bounded memory and writes only the two prefixes
and two tails, avoiding overlapping full-span writes. Insufficient reserved
space fails before writes. BPB/serial, directory label, FAT/data and gaps survive;
the original BIOS drive ID, boot label and PE FSInfo hint-byte resets match exactly.

Component/fault tests precede request/helper integration and native boot tests.
Qt still exposes only qualified Windows BIOS/dual NTFS. No Windows payloads,
Windows build configuration or device/privilege lifecycle changed. Next is the
existing FAT32 MBR layout and frozen request/preflight extension, followed by
helper/native handoff and Qt enablement. See [the test record](testing-and-checkpoints.md#cp22f--original-windows-fat-boot-records).


## CP22g — Windows FAT32 layout and request/preflight

2026-10-02. The Windows BIOS protocol has a source-backed FAT32 operation, with
frozen MBR choice/volume identity/settings and no UEFI:NTFS payload. Original
Rufus/Windows 7 MBR and the existing FAT32 planner/serializer are reused without
production modifications. Native cluster choices use that same layout.

Preflight follows shared Rufus Alt+E filesystem policy, validates the x64 fallback
EFI application for enhanced EFI-capable FAT media, and shares the existing
single conventional install.wim splitting check with GPT. Directory collision,
FAT file limit and required split-space checks remain in the ordinary extractor.
Tests cover ISO9660/UDF, both MBR choices, known/unknown/older Windows, mixed
loaders, EFI failures/cancellation, source changes and split/refusal policy.

The helper refuses this component-qualified FAT request before source/target
opening until CP22h installs the original FAT records before mounting/copying.
Qt remains NTFS-only for Windows BIOS/dual. Native/firmware qualification and GUI
enablement follow; this step does not create a bootable FAT Windows USB.


## CP22h — Windows FAT32 helper integration

2026-10-02. The temporary helper refusal is removed. The existing FAT helper
operation selects CP22f's original Windows PE record adapter after formatting,
checks MBR/signature/layout before and after it, then uses ordinary extraction
and the existing split-WIM/customization/final-cleanup paths. The BIOS disk
signature uses the existing generator. This adds no independent FAT/Windows
writer, serializer, split implementation or privilege lifecycle; one production
file changes by +15/-14 lines.

Generated-file/fault tests cover both original MBR choices and BIOS-only/dual
sources, boot record/backup failures, partial-write cancellation, failed sync,
readback corruption, changed MBR/source, split collisions and cleanup failures.
The actual split check uses a small empty WIM with oversized-file report facts;
its metadata is independently reopened/verified. Copy progress follows original
`WimProgressFunc` compressed-resource arithmetic rather than counting WIM metadata
as copied streams. No multi-gigabyte split or Windows installation test is added.

This qualifies helper sequencing and failure handling on ordinary files. Its
mount stand-in is a separate directory. Native FAT filesystem/firmware handoff
is CP22i; Qt enablement is CP22j. Until then the GUI remains NTFS-only for Windows
BIOS/dual. See [the test record](testing-and-checkpoints.md#cp22h--windows-fat32-helper-integration).


## CP22i — Native Windows FAT32 and firmware handoff

2026-10-02. Extend the existing native Windows/MBR test recipe rather than adding
another writer or device lifecycle. The unchanged helper formats/mounts native
FAT32, installs original PE records, copies ISO/UDF files and completes cleanup.
Independent mtools/fsck readback checks source files and executable records.
Exclude only live FSInfo free/next-cluster hints from post-copy payload comparison;
kernel allocation updates the primary hints. Reuse the precise original NO NAME
label diagnostic accepted for the standard ms-sys records. BIOS boots verify the
original Windows chain and multi-cluster BOOTMGR loading beyond 64 KiB. EFI boots
reuse the existing application oracle directly on FAT, without UEFI:NTFS. Rufus
MBR prompt/key/timeout choices retain their original behavior with a second disk.

One buffered QMP response could be hidden from select() until emulator exit;
the Windows boot runner now uses unbuffered pipes, retaining its prompt assertions.
Native and sanitizer checks pass without production/source/payload changes.
This qualifies generated handoff, not Windows Setup/BCD, Secure Boot, physical
firmware or actual polkit. Qt FAT32 enablement follows in CP22j. Existing large-
WIM and sole customization qualification are reused without another matrix.


## CP22j — Windows FAT32 BIOS/dual in Qt

2026-10-02. Reuse the qualified FAT32 helper unchanged. Two existing Qt files
admit FAT32 for the original MBR choice and describe enhanced EFI-capable FAT
confirmation correctly, for +4/-3 production lines. Shared Rufus Alt+E policy,
valid filesystem defaults/preselection, native geometry, frozen requests,
existing WIM splitting and final cleanup continue to supply the behavior.

Fourteen additional GUI scenarios and seven normal/sanitized suites pass. A
read-only real Windows 11 ISO preview confirms MBR/FAT32 and BIOS or UEFI without
starting a job. GUI fixtures cover requests/lifecycle; CP22i supplies native
creation/readback/handoff. No new writer, compatibility layer, privilege path
or customization qualification is added. Modern FAT32 BIOS/dual is now exposed;
legacy Windows/WinPE/FAT16 remain the next reuse audit and separate parity work.
See [the support guide](linux-support.md#windows-bios-and-dual-ntfsfat32-creation).
