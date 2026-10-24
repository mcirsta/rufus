# Older Windows installers and legacy WinPE: reuse audit

Current implementation: [CP75 / Preview44 legacy ISO routes](linux-legacy-iso-routes.md).
The dated audit below is retained as historical evidence.

CP22k, 2026-10-02. Audit of the checked-out Rufus source and the Linux port after
CP22j. This checkpoint changes no production code and enables no additional
creation route. Modern Windows BIOS/dual NTFS/FAT32 remains supported as recorded
in [the support guide](linux-support.md). Older installers and XP-era WinPE need
separate eligibility and qualification; their existing boot code is reusable.

## Keep the source families distinct

| Source family | Original Rufus work | Existing Linux reuse / missing work |
| --- | --- | --- |
| Known Windows 7, 8 or 8.1 installer, plain BIOS/NTFS | `format.c:WriteMBR`, `WritePBR`, ordinary `ExtractISO`; no Win7 EFI repair without UEFI/Alt+E | Both original MBR choices, standard NTFS PBR, native formatter/mount, extraction and completion already exist. Shared WIM metadata recognizes these releases. The Linux Windows 10/11 eligibility gate currently refuses them. Add a bounded older-installer BIOS/NTFS route and qualify generated source/helper/handoff behavior before Qt enablement. |
| Older BOOTMGR installer with regular fallback EFI files | Ordinary copying, shared extra-partition rules, UEFI:NTFS on NTFS | Reuse the existing regular x64 fallback check and combined NTFS helper. Source-version admission and route qualification remain work; a version alone is not proof of a usable fallback. |
| Windows 7-style EFI image without a fallback boot application | `format.c:FormatThread` extracts image 1's `Windows/Boot/EFI/bootmgfw.efi` to `EFI/BOOT/BOOTX64.EFI` | Upstream wimlib, retained ISO/UDF WIM reading and native file extraction already exist. The missing operation is locating/checking that exact payload and installing it into the existing private mount. No host Windows API or new privileged worker is needed. |
| XP-era i386/amd64/minint WinPE | `wue.c:SetupWinPE`, original MBR masquerading and filesystem-specific PBR | Shared layout/options scanning exists. Native root-file preparation, the original SETUPLDR byte transformations and active-byte 0x81 support do not. Extract the transformations for both platforms and supply native file operations; ordinary modern-Windows copying cannot substitute for them. |
| BOOTMGR-only or WIM-based PE without an install archive; Vista; mixed loaders | Original Rufus has broader dispatch than the current qualified Linux routes | Keep separately tracked. The Linux install-metadata prerequisite is not met by a filename alone. Vista retains the original normalized unknown version; any later admission must use explicit raw-version/source evidence without changing that reporting rule. Do not route these through GRUB or invent a known version. |

## Already shared: metadata, scanning and filesystem decisions

`src/wim_metadata.c:NormalizeWindowsVersion` maps raw 6.1 to 7, 6.2 to 8 and 6.3
to 8.1. Its original reporting clears normalized major/minor for raw versions
through 5 and clears Vista's normalized major. `ScanWindowsImage` already reads
the first recorded installation archive and image 1 with upstream wimlib. A
complete known version requires all four metadata fields and nonzero normalized
major/build. It retains failure/unknown results instead of trying another archive.
These mechanisms need no second parser or host detection for older installers.

`src/image_file.c:CheckIsoWindowsFile` and `src/winpe_scan.c` already detect legacy
WinPE files and inspect OsLoadOptions. The AMD64 mask deliberately combines
i386's ntdetect.com/setupldr.bin with amd64/txtsetup.sif; it is not three files
in amd64. The minint layout has its own three-file mask. The SIF scan preserves
original directory priority, original-case paths, the selected UDF view and
case-insensitive /minint substring behavior. Unknown or unreadable options remain
pending, including on images with known install metadata.

`GetAllowedFileSystems`, `GetPreferredFileSystem` and `GetExtraPartitions` remain
the source of filesystem/default/extra-partition rules. Windows BIOS FAT still
needs Alt+E; plain BIOS NTFS omits UEFI:NTFS, even on EFI-capable media. Future
older-version support must supply those original rules and qualified native
geometry, rather than make another filesystem policy table. The current
`IsWindowsBiosImage` and UEFI `checkReport` deliberately admit only Windows 10/11.
Removing that restriction globally would also expose missing older EFI work.

The original `GetWindowsUnattendFlags` offers customization for Windows 10/11,
not Windows 7/8/8.1. Older ordinary creation must retain that behavior. XP-era
`SetupWinPE` is boot preparation and is unrelated to the existing Windows 11
boot.wim customization backend. No further customization installation matrix
belongs to this work.

## Windows 7 EFI repair is a source-content decision

The actual repair trigger is `HAS_WIN7_EFI(report)`: `has_efi == 1` and an
installation archive exists. Bit 0 records root bootmgr.efi; regular fallback
applications set other bits. The trigger does not test `win_version.major == 7`.
`FormatThread` applies it only for a UEFI target or Alt+E, after ISO extraction.
It uses the first recorded install archive, image 1 and the exact bootmgfw.efi
path. Copying root bootmgr.efi to BOOTX64.EFI would not reproduce it.

The Linux preflight currently requires an existing readable x64 fallback EFI
application for UEFI, enhanced NTFS with UEFI:NTFS, and enhanced EFI-capable FAT32.
That is why widening the version predicate cannot supply Windows 7 EFI support.
A later checkpoint should preflight the WIM payload through the retained source,
then reuse the existing wimlib extraction and native boot-file installation
operations inside the same helper. `windows_bootloaders.cpp` already does such
extraction/replacement for CA 2023 payloads; share its useful file operations if
needed, without conflating its Setup-image/CA policy with this install-image task.
Retained-source reading also avoids dependence on install.wim remaining on FAT
after WIM splitting. Validate the resulting boot application and source identity
before authorization, and retain final sync/unmount/exit behavior.

## XP-era WinPE needs the original transformations

`SetupWinPE` selects AMD64 before minint, otherwise i386. It obtains both
ntdetect.com and setupldr.bin from i386 for i386/AMD64 layouts, or minint for the
minint layout. It copies ntdetect.com to root and setupldr.bin as root BOOTMGR.
Without /minint, it copies the selected txtsetup.sif to root and inserts
`SetupSourceDevice = "\device\harddisk1\partition1"` after `[SetupData]`, keeping
the source SIF untouched. Existing `config_text.c` does token replacement; it
does not yet provide `parser.c:insert_section_data`'s section insertion operation.
Extract that relevant operation when needed rather than adding a general INI
library with different grammar.

The original loader transformation:

- Disables the Win2k3 CRC branch only for the exact two bytes at offset 0x2060.
- Rewrites the minint txtsetup/system32 paths to the selected i386/amd64 paths.
- Without /minint, changes matching rdisk(0) references to rdisk(1) and rewrites
  `$win_nt$.~bt` to the selected directory while retaining its following character.

Preserve valid-payload matching, order, bytes and length. These loops belong in a
small shared buffer operation called by both Windows and Linux file I/O, not a
second Linux patcher or a broad Windows API compatibility layer. Define short and
truncated input behavior when extracting: the original unsigned `size - 32` loop
bounds must not underflow for a short buffer, and replacement/logging must stay
inside the owned buffer. Focused byte fixtures can verify this without a real XP
executable or another Windows installation test.

The original minint early exit matters: a minint layout with /minint needs no
further loader patching. Other minint combinations can return failure; do not
silently interpret ambiguous combinations as an ordinary i386 source. Original
CopyFile calls also tolerate some copy errors. For Linux, mandatory generated
boot files must be present and verified before successful READY; record any
deliberate failure-handling difference instead of copying those unchecked errors.

Without /minint, `WriteMBR` forces the original Rufus MBR and active byte **0x81**,
overriding the ordinary Alt+A fallback. `res/mbr/mbr.S` uses the table's drive byte
for its disk swap/handoff. The Linux planner, serializer and readback currently
expect **0x80**. They need one explicit masquerading choice with exact readback,
not an incidental post-write byte patch. Qualify native partition discovery and
mounting with 0x81, as well as BIOS handoff and cleanup; ordinary 0x80 success
does not establish those behaviors.

## Reuse the records, but qualify the loader they search for

The original NTFS record already searches for BOOTMGR and legacy NTLDR. Both
original MBRs and this NTFS writer are reused unchanged on Linux. Plain older
BOOTMGR BIOS/NTFS needs no new boot code or formatter.

The FAT record choices are different. `WritePBR` selects the FAT32 PE record for
BOOTMGR, then the FAT32 NT record for legacy WinPE, in that order. The latter
searches for NTLDR; the former searches for BOOTMGR. The existing Linux
FAT_BOOT_WINDOWS adapter implements the PE choice, not the legacy NT choice.
Its FAT16 writer matches Windows's standard FAT16 branch, whose payload searches
for IO.SYS/MSDOS.SYS. Matching those original bytes alone cannot establish a
Windows installer boot chain. Keep Windows BIOS FAT16 and legacy FAT32 gated
until complete loader/file preparation and generated handoff are qualified; do
not relabel the PE record as a universal Windows record.

Original Windows NTFS finalization also remounts and runs CheckDisk, with a WinPE/
AIK comment. Linux installs the PBR before mounting, then syncs/unmounts on exit.
That preserves the required PBR-before-mount ordering, but metadata checks and
sync are not a CheckDisk replacement. Investigate the concrete legacy repair
requirement when qualifying that family; do not introduce a generic repair tool
or change NTFS provider after a corruption/write error merely to get success.

## Selected next step

**CP22l: known Windows 7/8/8.1 plain BIOS/NTFS preflight and helper qualification.**
Start with normalized 7.0, 8.0 and 8.1 installation metadata, root BOOTMGR and no
legacy/mixed BIOS loader. Use the existing explicit Windows request, original
MBR/PBR, NTFS providers, source retention, copying and one-helper completion.
Initially keep enhanced/EFI/FAT and GUI enablement separate so no missing repair
route is exposed by broadening a shared predicate. Extend existing small WIM and
ISO/UDF fixtures and helper tests; qualify native readback/generated BIOS handoff
before exposing older versions in Qt. Unknown versions stay unknown and refused.
Reuse the existing NTFS-provider and both-MBR qualification; one representative
older-source native creation/handoff is enough for this step, with version and
refusal combinations checked by focused fixtures. Do not create a new per-release/
provider Windows installation matrix.

Then implement the exact Windows 7-style EFI payload operation, and separately
extract legacy WinPE's shared loader/config transformations and add the original
0x81 MBR choice. FAT16 and legacy FAT32 need their own complete-chain qualification.
Windows To Go and other parity work remain tracked without a fixed overall order.
Reuse existing large-WIM and sole customization qualification. See
[the CP22k test record](testing-and-checkpoints.md#cp22k--legacy-windowswinpe-reuse-audit).


## CP22l1 implementation update

The audit above describes CP22k. CP22l1 admits known normalized 7.0/8.0/8.1 only
for plain Windows BIOS/MBR/NTFS without Alt+E through the existing preflight/helper.
Qt keeps the modern-only default. ISO/UDF and both original MBR choices pass
normal/sanitized generated-file tests. Only preflight implementation/header
changes; records/providers/helper/protocol/copying/Qt are reused unchanged.
Native combined-volume readback/generated BIOS handoff remains CP22l2, before
CP22l3 Qt enablement. Older EFI/FAT and XP-era WinPE remain separate. See
[the test record](testing-and-checkpoints.md#cp22l1--older-bootmgr-plain-biosntfs-preflight-and-helper).

CP22l2 supplies representative native NTFS creation/readback and generated BIOS
handoff: Windows 7-style ISO/UDF, both original MBRs and kernel/fallback mounting,
normally and sanitized. Root bootmgr.efi is copied with no fallback generated
for plain BIOS. Production hashes are unchanged. Qt enablement is CP22l3;
actual older Windows Setup, EFI repair and legacy WinPE remain separate. See
[the native record](testing-and-checkpoints.md#cp22l2--older-source-native-ntfs-creation-and-bios-handoff).

CP22l3 exposes that qualified plain BIOS/NTFS route for known 7.0/8.0/8.1 in Qt.
The existing source predicate, original MBR choices and shared filesystem/
customization rules are reused. Sources with EFI files still offer only the
qualified BIOS target; saved Alt+E remains for other images and is ineffective
here. Next is the source-content-driven EFI payload preflight described above,
then helper fallback installation/handoff before widening Qt choices. See
[the plan](linux-port-plan.md#cp22l3--older-bootmgr-plain-biosntfs-in-qt-2026-10-03).


## CP22m1 EFI repair lookup

The exact original repair lookup is now implemented independently of version
classification. It uses the retained source view, upstream wimlib and existing
bounded file reader; it returns validated x64 application bytes from image 1 of
the first install archive. Unknown metadata stays unknown. Existing fallback
header validation is shared, with original FindPeArch unchanged. ISO/UDF,
solid/pipable archives and refusal/cleanup cases pass normally and sanitized.
This component does not yet install EFI/BOOT/BOOTX64.EFI or broaden Qt/backend
choices. Next connect fresh helper preflight to atomic fallback installation,
then qualify native readback/EFI handoff before enablement. See
[the record](testing-and-checkpoints.md#cp22m1--windows-7-style-efi-payload-preflight).


## CP22m2 EFI repair installation

Fresh UEFI/Alt+E preflight now retains the checked first-install-image loader;
the helper installs EFI/BOOT/BOOTX64.EFI after normal copying and before
customization/final sync. Owned bytes avoid dependence on the copied install.wim
when FAT splitting removes it. The existing CA 2023 atomic writer and directory
operations are shared; no replacement PE parser, client payload field, privilege
flow or source reclassification. Ordinary BIOS still skips repair. Component/
copying/error/cancel and existing request/process suites pass normally/sanitized.
Older enhanced/UEFI/FAT and Qt gates remain. Native combined creation/readback/
generated EFI handoff is the next checkpoint. See
[the record](testing-and-checkpoints.md#cp22m2--efi-repair-payload-installation-in-the-helper).

## CP22m3 native EFI repair

The complete helper now passes generated native FAT32/ISO and kernel NTFS/UDF
repair jobs normally and sanitized. The ISO has only root bootmgr.efi; the first
install image supplies a different generated application. Independent readback
finds exactly the expected added fallback and preserves every source file.
OVMF executes that fallback directly on FAT32 and through original UEFI:NTFS
on NTFS. One authorization, existing progress/finalization and helper-exit
ordering pass without new production code. Known modern metadata isolates the
content trigger; older EFI/FAT policy and Qt exposure remain the next work.
Actual Windows Setup/BCD/Secure Boot is not qualified by this generated handoff.
See [the record](testing-and-checkpoints.md#cp22m3--native-efi-repair-creationreadbackhandoff).

## CP22m4 older EFI/FAT backend eligibility

Known normalized 7.0/8.0/8.1 now pass fresh FAT32/NTFS enhanced/UEFI preflight
through the existing records, native operations, x64 fallback validation and
content-driven repair. A shared known-version check replaces duplicate BIOS/
UEFI classification; unknown reporting and first-install-image identity remain
unchanged. Rufus's filesystem/extra-partition/split and customization rules still
apply. Legacy/mixed older sources and FAT16 stay gated. The existing Qt
preflight-result handler now explicitly retains plain older BIOS/NTFS, including
EFI-only sources that previously relied on backend refusal. Normal/sanitized
component and focused GUI regressions pass; older native EFI handoff follows
before broadening Qt choices. See
[the record](testing-and-checkpoints.md#cp22m4--bounded-older-efifat-eligibility).

## CP22m5 older native EFI repair

Known Windows 7 metadata now passes complete FAT32/ISO and kernel NTFS/UDF
native repair jobs normally and sanitized. Independent readback preserves all
source files and finds exactly the generated application added as the fallback.
OVMF executes it directly on FAT32 and through original UEFI:NTFS on NTFS.
One authorization, source retention, progress, final sync/cleanup and helper-exit
ordering pass with all production code unchanged. CP22m4 covers 7/8/8.1 and
regular fallback policy; actual Windows Setup/BCD/Secure Boot stays separate.
Older EFI/FAT32 Qt enablement is next. See
[the record](testing-and-checkpoints.md#cp22m5--older-native-efi-creationreadbackhandoff).


## CP22n1 shared WinPE preparation primitives

SetupWinPE now calls shared layout/copy planning and SETUPLDR buffer operations
in `winpe_setup.c`, retaining Windows file operations. Exact well-formed bytes
match a test-only baseline oracle, including dense markers and both architectures;
short/truncated/nonterminated input is bounded instead of inheriting unsigned
underflow or unchecked logging. The existing section-insertion loop calls the
shared `IsConfigSection` predicate; SIF encoding/output/file operations stay native.
Both original Windows build manifests register the new source/header; original
Windows compilation/execution still requires later real Windows validation.

All 64,996 loader/layout and 336 config checks pass normally and sanitized;
existing native config and ISO-preflight regressions pass, and Linux GUI/helper
builds succeed. This exposes no WinPE creation route. Native root-file preparation,
SIF writing, mandatory-file verification, 0x81 disk masquerading, record selection
and complete BIOS chain qualification remain separate. Setup's AMD64-first choice
must not be confused with the existing options scanner's i386-first priority.
See [the record](testing-and-checkpoints.md#cp22n1--shared-legacy-winpe-transformations).

## CP22n2 native WinPE root preparation

The new native component calls the shared WinPE plan/loader patches and shared
section matcher, preparing required root files in a retained private directory.
Case-ambiguous sources, source changes, existing destinations and I/O/verification
failures are explicit errors. Each file is verified/flushed before no-replace
publication; prior complete outputs may remain if a later operation fails.
Sources are never edited. This uses the existing worker cancellation/reporting
entry points, adds no callback bridge or privileged process, and is not enabled
in the GUI/helper yet.

The section adapter shares the config decoder, while retaining SIF CRLF and
1023 UTF-16-unit chunk semantics rather than ISO token replacement's LF output.
ASCII/no-BOM, UTF-8/BOM and UTF-16LE/BOM bytes are independently checked. Localized
no-BOM SIF remains gated until its code page can be selected without host guessing;
the Windows CRT's actual BOM/encoding round trip remains deferred to Windows.
[Microsoft's CRT documentation](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/fopen-wfopen?view=msvc-170)
supports the text-mode CRLF/LF translation and BOM-controlled Unicode streams;
it describes `ccs=UNICODE` without a BOM as UTF-16LE, while Rufus's parser comments
also describe ANSI inputs. Do not infer runtime equivalence from those comments
alone or claim Linux fixture tests qualify the original Windows CRT.

Native mandatory-copy/missing-section failures stop instead of inheriting
SetupWinPE's tolerated errors. Unsupported minint layouts stop before writes.
These are deliberate source-preservation/complete-boot-chain safeguards.
All five focused suites pass normally/sanitized and GUI/helper builds succeed.
Original 0x81 masquerading/records, native helper integration, BIOS handoff and
Qt exposure remain separate. See
[the record](testing-and-checkpoints.md#cp22n2--native-winpe-root-file-preparation-component).

## CP22n3 legacy WinPE records

The shared SetupWinPE plan now includes WriteMBR's exact full-family/no-/minint
masquerading predicate. Windows calls it while keeping the existing writer and
provider priority. The native MBR component reuses the original Rufus payload
with an explicit 0x81 variant; ordinary providers keep 0x80. libfdisk accepts the
stored label but reports only 0x80 as bootable, so readback checks the exact raw
0x81 before expecting its false library property. The prewrite table still
requires an active partition, and all extent/type/label checks remain.

Legacy FAT32 now calls the original NT provider and matcher through the existing
bounded prefix/tail/backup writer. FAT16 uses the original standard record;
NTFS's existing original provider is unchanged. No new payload or I/O engine.
Component fixtures compare original arrays, preserve surrounding/BPB/label bytes
and exercise corruption, partial writes, sync and cancellation. All 18 focused
suites are qualified normally/sanitized, with one incorrect non-bootable fixture
expectation corrected and rerun; 66,020 shared assertions and GUI/helper builds
pass. No WinPE helper or GUI route is enabled.

The legacy FAT32 NT payload looks for `NTLDR`, whereas SetupWinPE writes
`BOOTMGR`. The modern FAT32 and standard NTFS providers look for `BOOTMGR`.
This audited source/payload discrepancy requires generated BIOS handoff evidence;
it does not prove a Windows runtime bug. Preserve BOOTMGR's higher PBR priority
when both image markers exist. Resolve the legacy filename case before enabling
FAT32 or choosing a native alias/provider change. See
[the plan](linux-port-plan.md#cp22n3--legacy-winpe-boot-record-component-2026-10-03).

## CP22n4 BIOS observations and reuse decision

Generated USB boots confirm the legacy FAT32 lookup is NTLDR. Stock 0x81 MBR
masquerading plus the record's stored 0x80 drive ID stops earlier with Disk
error. Correcting only the fixture BPB to 0x81 reaches NTLDR is missing; a
fixture NTLDR copy of prepared BOOTMGR then reaches CS=2000/DL=81 and loads its
full >64-KiB tail. Original BOOTMGR code with a corrected drive also succeeds;
/minint retains DL=80 and an unchanged source loader. Six cases pass normally
and with sanitized native components. No real SETUPLDR/Windows Setup is tested.

Keep original record code and selection priorities. These findings justify
requiring explicit Linux drive/filename handling if a FAT32 route is introduced,
not an unconditional NTLDR alias or blanket PE-provider substitution. Existing
NTLDR source/collision handling still needs a source contract. Prefer testing
the already-used NTFS BOOTMGR record and 0x81 mapping first, then reuse the
existing native helper pipeline. All production files are unchanged this step.

The mount-free PE controls restore formatter FSInfo hints because the original
PE array overlaps five hint bytes and mcopy refuses its zero free count. Actual
mount/remount hint handling needs workflow qualification. Tiny emulated USB CHS
translation is a separate fixture issue; the runner uses sparse nine-GiB disks
with about 19 MiB allocated. None of these controls establishes real Windows
bootability or a Windows runtime defect. Full helper/Qt integration stays gated.
See [the plan](linux-port-plan.md#cp22n4--generated-legacy-bios-record-handoff-2026-10-03).

## CP22n5 unchanged NTFS handoff and source contract

The existing NTFS record accepts the original MBR's DL=81 directly and reaches
the generated prepared i386/amd64 BOOTMGR loader without any BPB correction or
NTLDR alias. /minint retains DL=80. Full tail loading and source/layout patches
are verified. An NTLDR-only control also works: the previous ASCII-only audit
missed its UTF-16LE lookup at NTFS array offset 520. That corrects the earlier
absence statement; no original NTFS payload is changed. Neither-loader control
produces BOOTMGR is missing. Five normal/sanitized cases and existing component
regressions pass after fixing the test fixture's parent-inode cleanup.

Use this unchanged NTFS chain for the first legacy helper integration. Reuse
the current BIOS/NTFS request, derive masks/minint/drive behavior from a fresh
source scan and shared plan, and validate complete tree, known options, unique
bounded mandatory files, SIF transformation and root destination collisions
before target writes. Keep unknown Windows versions unknown; do not route
legacy sources through modern installer/customization assumptions. Existing
reader/root-preparation/copying/finalization operations stay responsible for
source rechecks and one-helper cleanup. The full contract is in
[the plan](linux-port-plan.md#cp22n5--original-ntfswinpe-bios-handoff-2026-10-03).

No production files change and no helper/Qt route is enabled in this checkpoint.
The NTFS-3G library is only a mount-free test population/readback dependency;
production continues to prefer ntfsprogs-plus formatting and the native kernel
driver with NTFS-3G fallback. Actual SETUPLDR/Windows Setup, mixed/existing root
loaders, FAT32 drive/filename/FSInfo handling, FAT16 and localized SIF remain.

## CP22n6 — legacy source checks in the existing preflight

The new read-only `CheckLegacyWinPeImage` entry point shares the retained-source,
analysis/extraction and partition-fit implementation with modern ISO preflight.
It checks the proposed plain MBR/BIOS/NTFS legacy source using shared WinPE
plans, policy, options parsing, SIF editing and loader transformations. An
existing ISO/UDF metadata walker resolves unique mandatory inputs and rejects
case-ambiguous directories/files and generated-root collisions before any
target access. Prepared root sizes are reserved. i386 scan priority and AMD64
setup priority stay distinct, including when both families occur in an image.
No modern version is invented or required; no new copying/parser engine.

The original Windows implementation can overwrite existing root files. The
first Linux lane keeps those mixed/existing-loader cases gated until they are
qualified: overwriting an unrelated loader is unsafe for this unqualified
combination. Partial minint layouts and mixed installers likewise remain
refused. Required detector/loader files are bounded at the native root
component's 8 MiB limit, options at the shared 64 KiB bound; copied SIF must
support the existing insertion. All remain tracked restrictions, not scope
removals. Preflight proves only these source prerequisites, not bootability.

Normal/sanitized checks pass (490 preflight cases, 74 root cases, 66,020 shared
checks) and GUI/helper build. Original/shared production sources are unchanged.
The existing helper preflight still rejects legacy images; integrate and qualify
the complete helper NTFS job next, then expose it in Qt. See
[the plan](linux-port-plan.md#cp22n6--retained-legacy-winpe-source-preflight-2026-10-03).

## CP22n7 — complete helper lane

The existing Windows BIOS helper now explicitly accepts the qualified plain
legacy NTFS lane, using fresh source preflight before target access, existing
NTFS records/extraction and native root preparation. No client masks or another
worker/authorization. Default Qt eligibility remains gated until CP22n8.

Actual kernel-device execution exposed Linux's strict 0/0x80 DOS boot indicator
check: initial 0x81 prevented partition refresh. Keep 0x80 through formatting,
copying and private unmount; then use the existing MBR validator/writer to
install/read back/fsync the final original Rufus/0x81 record without a rescan.
The shared plan overrides a Win7 choice for masquerading; minint preserves that
choice. Finished media matches the original. This operation-order adaptation
also preserves the ordinary Linux parser limitation on replugged 0x81 media.
See the [kernel source](https://github.com/torvalds/linux/blob/master/block/partitions/msdos.c).

NTFS-3G no-replace rename failed with EINVAL, confirmed by a guest syscall trace.
The root component uses same-directory hard-link publication plus temporary-name
removal when rename flags/syscall are unsupported, preserving no-overwrite
semantics; ordinary errors, link failures and destination races remain failures.
No new disk/file-copy engine or installer dependency is added.

Generated i386/AMD64/minint helper jobs now pass exact source/root/record/metadata
readback and BIOS CS/DL/large-loader handoff across kernel NTFS and NTFS-3G,
with sanitizer coverage. A root-write failure proves FAILED after completed copy
and clean helper exit; no final masquerading record is installed. Existing modern
NTFS regression and 490/79/66,020 preflight/root/shared cases pass. Actual Windows
Setup and the remaining legacy variants remain unverified/tracked. See
[the plan](linux-port-plan.md#cp22n7--complete-legacy-winpe-ntfs-helper-job-2026-10-03).

## CP22n8 — Qt exposes the qualified NTFS lane

Qt and fresh source preflight now share one report predicate based on the
original WinPE setup plan. Explicit legacy opt-in selects the existing BIOS
Windows request, not a client-provided WinPE mask. GUI defaults and labels come
from the existing Rufus/native policy. Unknown version is not promoted to a
modern installer; modern options stay hidden. NTFS is the only qualified legacy
filesystem at present, with MBR/BIOS and no dual/UEFI customization. Saved
advanced preferences remain available for later modern sources.

The original ISO/DD policy/dialog is reused, including recommended ISO and
explicit DD selection. A valid DD choice survives unavailable legacy MBR/NTFS
formatting. Source root-file bounds/collisions are rechecked before authorization.
Generated complete Widgets/helper jobs pass normal/sanitized exact readback and
BIOS handoff, held-exit finalization and full READY. The small guest models caller
UID and authorization; actual Windows Setup/host polkit remain unqualified.
New production changes are confined to existing selection/preflight code; no
helper, shared Windows source or protocol change. FAT16/FAT32, localized/mixed
source variants and Windows To Go remain required work. See
[the plan](linux-port-plan.md#cp22n8--qualified-legacy-winpe-qt-creation-2026-10-03).

## CP22n9 — FAT reuse decision before expanding the route

The copy-first FAT audit confirms standard FAT16 DOS code cannot hand off the
prepared BOOTMGR, even with fixture drive mapping corrected. Existing WinPE
FAT32 NT code expects NTLDR; existing PE code loads prepared i386/AMD64/minint
BOOTMGR, with the original masquerading plan's drive ID accounted for. Reuse
that PE code for the complete BOOTMGR-only legacy source contract, with a small
Linux drive/FSInfo adaptation; preserve original arrays, existing provider
semantics and shared setup transforms. Do not create blanket NTLDR aliases or
pretend a DOS filename alias makes SETUPLDR a DOS loader. Mixed/existing loader
sources and a compatible FAT16 handoff remain unresolved.

Installing unchanged PE records after copying exposes a wrong free-cluster
summary because the array overlaps allocation hints. Successful generated BIOS
handoff is therefore insufficient to open the route. Preserve/recompute valid
hints and qualify final record/mount/unmount ordering before helper/Qt exposure.
Default Windows BIOS filesystem policy remains NTFS; advanced FAT selection
must not invent UEFI capability for legacy images.

Six new BIOS audit cases pass normally/sanitized, including expected FAT16 and
stock-drive failures; six existing BIOS cases and four component suites pass.
All 694 production files checked are unchanged. Actual Windows Setup and host
device lifecycle are unqualified here. See
[the plan](linux-port-plan.md#cp22n9--legacy-fat-record-and-metadata-audit-2026-10-03).

## CP22n10 — Reused PE code with narrow metadata adaptation

`InstallWinPeFatBootRecords` reuses the existing FAT32 PE provider and complete
FAT writer pipeline. After original writer verification/close, it preserves
each copy's allocation hints and writes the shared setup plan's 0x80/0x81 drive
mapping. No boot instructions, original arrays, other provider behavior or
shared setup code change. It preserves supplied metadata rather than scanning
the FAT or pretending to repair an invalid source volume. Caller source/volume
validation remains separate; the entry point does not implement FAT16 or
mixed/existing-loader support.

Normal/sanitized adaptation suites pass 152 fault/refusal cases, including
specific primary/backup drive and hint readback failures. Existing providers,
root preparation and shared setup regressions pass. Four generated BIOS jobs
per build pass exact files/records and independent allocation metadata checks,
including installation before copying, without a fixture hint/drive repair or
loader alias. Actual Windows Setup and mounted-device lifecycle remain
unverified. Production changes are confined to existing `fat_boot.c/.h`;
helper/Qt still gate legacy FAT routes. Next is full helper qualification. See
[the plan](linux-port-plan.md#cp22n10--native-legacy-fat32-bootmgr-record-adaptation-2026-10-03).


## CP22n11 — Native FAT32 lifecycle qualified before Qt exposure

The retained-device helper now reuses the regular-file adapter through the same
staged writer, source preflight, private vfat mount, extraction/root transforms
and one-helper cleanup. Explicit advanced FAT32 policy is checked freshly;
NTFS defaults and existing mixed-root NTFS behavior stay intact. FAT32 refuses
NTLDR/IO.SYS/MSDOS.SYS root collisions rather than changing their loader contract.
After successful sync/unmount, install adapted PE records and then the original
masquerading MBR where the shared plan requires it. Keep the required bootloader
phase for MBR validation before mounting; strict protocol remains unchanged.

Generated i386/AMD64-UDF/minint jobs, sanitized i386 success/failure, exact
file/record/hint readback, independent fsck and BIOS handoff pass. Root-write
failure leaves FAILED and no final records after clean teardown. Existing NTFS
legacy and modern FAT32 regressions pass. Only four existing Linux production
files change; no shared Windows/Qt/protocol changes. Qt advanced FAT selection
is next, with NTFS default and no inferred legacy UEFI support. Actual Windows
Setup, FAT16 and other legacy variants remain work. See
[the plan](linux-port-plan.md#cp22n11--complete-legacy-winpe-fat32-helper-job-2026-10-03).


## CP22n12 — Advanced FAT32 exposed through existing Qt policy

The existing selection controller now permits the qualified FAT32 legacy route
under Alt+E and freezes the advanced flag only for that filesystem; plain NTFS
requests retain their previous contract. Ordinary load defaults to NTFS;
toggling Alt+E preserves it until FAT32 is chosen, while a saved advanced
preference on new image load follows Rufus's shared FAT32 preference. No extra
Linux default policy or UEFI capability is inferred. Target/confirmation stay
BIOS, modern customization remains hidden, and FAT16 stays gated.

Normal/sanitized focused Widgets cases and full GUI/preflight regressions pass.
Complete generated Qt/helper/FAT32/readback/BIOS jobs pass with one simulated
authorization and held-exit READY checks. Only one existing production file
changes; helper/protocol/shared Windows code is unchanged. Actual Windows Setup
and other legacy variants remain work. Next audit mixed/existing root-loader
dispatch before further admission. See
[the plan](linux-port-plan.md#cp22n12--qt-advanced-legacy-winpe-fat32-creation-2026-10-03).


## CP22n13 — Existing root files and mixed-loader dispatch audit

The exact Windows sequence matters more than the presence of a familiar filename.
`SetupWinPE` uses `CopyFileA(..., TRUE)` for detector, SIF and loader. TRUE makes
an existing destination fail the copy; it does not replace that file, according
to [Microsoft's CopyFileA contract](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-copyfilea).
Rufus ignores the detector copy result, logs a failed SIF/loader copy and keeps
processing. Existing files therefore become inputs to later work, not merely
collisions that Windows always rejects. This is source/call-contract analysis;
actual Win32 file attributes, sharing and CRT execution remain deferred.

| Source/root case | Original Windows behavior | Current Linux behavior and next work |
| --- | --- | --- |
| Existing root NTDETECT.COM | Copy fails without replacement; existing detector is retained. A missing/copy-failed detector is also not checked here. | Fresh preflight and native component refuse EEXIST, including an identical detector. First extension: verify and retain an identical bounded regular file; keep mandatory-file/error checks and no-overwrite guarantees. Different detectors remain a separate compatibility decision. |
| Existing root TXTSETUP.SIF when `plan.copy_sif` | Directory copy fails; `insert_section_data` edits the existing root SIF, rather than substituting the directory SIF. | Refused before writes, even when identical. Future support must select/validate the root SIF's own content and encoding and use safe publication. It must preserve unrelated root content and source-directory files; equality alone does not implement the original edit. |
| Root TXTSETUP.SIF with `/minint` and no SIF copy | Root file is unused by this preparation step. | Already accepted and preserved, whether equal to or different from the directory SIF. No new production change is needed. |
| Existing root BOOTMGR | Failed copy is logged. Patch mode opens/patches that existing root loader; keep-loader minint can return success without checking whether copy supplied a loader. | Mixed-source report gate refuses it before native preparation; the component also refuses the destination. A modern BOOTMGR and a renamed SETUPLDR are not interchangeable. Future support needs an explicit existing-loader source contract, not unconditional replace/skip. |
| Root NTLDR alongside complete setup files | No root-copy destination collides. FAT32's original legacy NT record searches NTLDR; NTFS uses its original common record. | Plain NTFS preflight already accepts it; advanced FAT32 refuses NTLDR/IO.SYS/MSDOS.SYS combinations because its qualified PE provider loads prepared BOOTMGR. These gates must not be removed together. Source-native NTLDR handoff needs separate provider/content/drive qualification. |
| Root BOOTMGR or BOOTMGR.EFI plus complete legacy files | FAT32's `HAS_BOOTMGR` branch takes priority over `HAS_WINPE`, selecting PE records. BIOS extraction still calls SetupWinPE when full legacy files are reported. | Report stays mixed/unknown and refused by the qualified pure-legacy lane. Filename detection is not evidence of a supported modern installer or an EFI fallback; do not bypass metadata/source checks. |
| Syslinux/ReactOS plus legacy setup | With Alt+E Windows takes MBR priority and skips the native Syslinux branch; without it Syslinux may be installed. BIOS WinPE root fixup remains a separate post-extraction operation. | Existing exclusive loader routes refuse this combination. Supporting it needs the original MBR/PBR/installer precedence and both source contracts; selecting one predicate alone can omit required work. |
| GRUB/Grub4DOS plus legacy setup | Windows MBR priority with Alt+E precedes GRUB; otherwise the original MBR/SBR precedence applies. WinPE fixup is still conditioned separately. | Requires combined dispatch/records/resources qualification; do not route a mixed image to the pure WinPE helper merely because it contains setup files. |
| FAT16 with BOOTMGR/WinPE | Original standard FAT16 branch remains DOS code, searching IO.SYS/MSDOS.SYS. | Existing CP22n9 handoff audit still blocks advertising a prepared BOOTMGR chain. Root-collision support does not supply a compatible FAT16 loader. |

Original NTFS record disassembly provides a useful constraint: at offsets
0x3ae–0x3d6, it searches the seven-character BOOTMGR name at 0x202 first, then
uses five-character NTLDR at 0x25c if the first search returns zero. Both UTF-16
names are present in the original array. Mere string presence alone would not
prove that priority. FAT32 NT/PE and FAT16 filename offsets are also exported;
this static audit does not newly qualify a mixed BIOS boot or actual Windows
Setup. Existing generated BOOTMGR handoffs remain the runtime evidence.

Tests add 48 ISO/UDF source cases across NTFS/FAT32, i386/minint and equal/
different root detector/SIF/loader files, plus 12 native root cases. They prove
current classification/errors, unused-minint-SIF preservation, no target writes,
unchanged source bytes, source-directory preservation and no partial/staging
outputs after refusal. Full suites pass normally/sanitized: 566 source cases,
91 native root cases and 66,020 shared setup checks. All 694 checked production
files are unchanged. Evidence is at
`/home/marius/proj/lufus/.rufus-test-artifacts/cp22n13`.

Selected CP22n14: admit and retain an identical existing root detector through
fresh ISO/UDF preflight and the existing native root component. Validate before
any output, retain case/identity/bounds checks, recheck it across preparation,
and never rewrite it or conceal an I/O error. Qualify positive/refusal/race
cases and one complete existing helper/Qt/readback/BIOS job before expanding
that admission. Reuse current records, shared transformations and one-helper
completion. Existing SIF/BOOTMGR, localized no-BOM SIF, FAT16 and combined-loader
sources remain required work; no host code-page guessing or new customization
qualification is planned.

## CP22n14 — Existing identical root NTDETECT.COM (2026-10-03)

The first CP22n13 candidate is implemented. Original SetupWinPE leaves an
existing detector after its no-replace copy fails. For the qualified identical
case, Linux now preserves that result: fresh ISO/UDF preflight compares the
complete bounded selected-view files; native preparation retains the root input
and verifies it with the existing source reader/identity checks. It publishes
only the remaining files. No new file or loader engine, Windows conditional,
request/protocol, authorization or GUI change is introduced. Required root SIF/
BOOTMGR still need their distinct content contracts; their gates are unchanged.

Retention tests verify unchanged root spelling, bytes, inode and modification/
change times. Root/source changes, an identical replacement, duplicate case name,
removal, late mutation and read/cancel errors stop preparation. Different contents
(including equal-sized data), invalid size and links/special entries are refused.
No input is overwritten to hide incompatibility. The reused component retains
its no-replace publication for new files and removes failed staging files.

Normal/sanitized source/root/shared suites pass 594/106/66,020 checks. The normal
Widgets suite passes 975 cases (five optional existing fixtures skipped). One
existing complete Widgets/helper FAT32 i386 guest with NtDetect.Com in the source
root passes exact prepared-file/record readback, filesystem checks and generated
BIOS drive 0x81/tail handoff, with one simulated authorization and durable READY.
Actual Windows Setup/CRT and real-host polkit remain unqualified. Evidence and
next existing-root-SIF step: [CP22n14 plan](linux-port-plan.md#cp22n14--retain-an-identical-legacy-root-detector-2026-10-03).


## CP22n15 — Existing root TXTSETUP.SIF content (2026-10-03)

Linux now follows SetupWinPE's distinct existing-SIF behavior: edit the root
file's own content after the directory copy cannot replace it. Equality with
the selected directory SIF is not required. Fresh selected-view ISO/UDF preflight
validates that root input with the existing text adapter; native preparation uses
its existing array/reader/identity checks and verified writer for an atomic root
edit. Root spelling and ownership/permissions survive; unrelated settings,
repeated sections, encoding/BOM and the original section matcher are retained.
Directory inputs remain unchanged. Image-derived /minint policy is unchanged;
an unused minint root SIF is still preserved without editing.

Before commit, failed read/edit/write/verify/flush/close/metadata/rename and source
identity checks preserve the root file. After commit, a failed directory flush
or loader write can leave a complete SIF edit but fails the job. The new SIF
identity is retained and checked again during loader output; this is not a set-wide
transaction or an atomic pathname compare-and-swap for a shared directory. The
helper owns an exclusive private mount. New detector/BOOTMGR publication still
refuses replacement. No new patch/copy engine or shared Windows/GUI/helper flow.

Four synthetic near-capacity regressions exposed the first implementation's insufficient
staging reservation: final size growth was insufficient to hold both original
and edited root SIF before rename. Preflight now reserves the full edited file;
NTFS/FAT32 ISO/UDF controls reject ENOSPC before target access, with enough-space
controls accepted. Initial localized-byte test expectations were corrected to
separate malformed UTF-8 from unsupported valid no-BOM text, preserving policy.

Normal/sanitized suites pass 662 source cases, 143 root cases and 66,020 shared
checks; focused normal Widgets cases pass 34 including invalid-root-SIF stops
before authorization. One generated full Qt/helper FAT32 job and its rerun after
the space correction pass distinct-root-content/source preservation readback,
original records, fsck, sync/cleanup/exit/READY and generated BIOS handoff. Actual
Windows Setup/CRT/host polkit remain unqualified; no added customization matrix.
Root BOOTMGR classification/provider priority needs its own audit next; see
[CP22n15](linux-port-plan.md#cp22n15--edit-an-existing-legacy-root-sif-2026-10-03).


## CP22n16 — Preserve the existing BOOTMGR report/provider priority

The root name sets HAS_BOOTMGR_BIOS even when the bytes equal the selected
SETUPLDR. Keep this flag: original FAT32 WritePBR chooses its PE/BOOTMGR provider
before NT/NTLDR; native WinPE FAT32 already reuses that PE provider. Original
SetupWinPE still patches the existing root input (or retains it for minint)
after the no-replace copy fails. Its directory/SIF/masquerading plan does not
change. Original NTFS searches BOOTMGR first, then NTLDR. BOOTMGR.EFI remains
an independent EFI report and is outside this plain-BIOS extension.

Start future admission with full bounded equality to the unmodified selected
directory loader, not marker recognition, patched-output equality or report
flag removal. Equality preserves the existing copied-input contract, without
claiming executable authentication. Reuse native SIF replacement for patch mode
and existing source retention for keep mode; do not add another writer. All
production is unchanged this audit. New generated source classification/refusal
checks pass normally/sanitized; real Windows remains deferred. See
[the plan](linux-port-plan.md#cp22n16--existing-root-bootmgr-sourcerecord-contract-audit-2026-10-04)
for the exact contract, staging-space requirement and next implementation steps.


## CP22n17 — Reuse the native root writer for BOOTMGR

An identical unmodified root BOOTMGR now uses the retained SetupFile input and
shared PatchWinPeLoader, with the existing SIF atomic replacement path for patch
mode. Keep-minint skips publication and retains the original metadata/identity.
Both directory and retained/committed root inputs remain checked through
preparation. Differing/prepatched/opaque roots and bad kinds/links/case/bounds
remain refused. Source/Qt gates are unchanged until fresh ISO admission and
provider-preserving routing are qualified next. Native success/error/race tests
and normal/sanitizer suites pass. See
[the plan](linux-port-plan.md#cp22n17--native-identical-existing-root-bootmgr-2026-10-04).


## CP22n18 — Admit the identical root through the existing job

The complete plain-BIOS legacy candidate can now retain HAS_BOOTMGR_BIOS;
fresh source reads establish equality to the unmodified directory loader before
confirmation/authorization. The already qualified original PE/NTFS providers and
native writer handle the job. Full loader stage space is required for patch
mode, none for retained minint. The Qt choice filter allows pending Windows
version only for this complete legacy candidate, leaving the report/version and
Partial state intact. EFI/archive/other-loader and differing-root gates remain.
Normal/sanitizer source checks, focused/full Widgets and one complete generated
Qt/helper FAT32 job pass, including mixed-case root edits/readback and BIOS
handoff. See
[the plan](linux-port-plan.md#cp22n18--existing-root-loader-source-admission-and-qt-integration-2026-10-04).


## CP22n19 — Localized SIF input contract

The original insertion routine does not select an image code page. Its
`ccs=UNICODE` input, stream-level BOM handling and subsequent output-mode choice
must be checked in real Windows; Microsoft fopen/open descriptions disagree on
no-BOM defaults. Linux therefore retains ASCII/no-BOM and Unicode/BOM admission.
An NLS filename hint or valid UTF-8 sequence alone does not authorize legacy
transcoding. Future admission needs an explicit encoding passed to the existing
adapter and an existing codec. Keep this unresolved feature in scope without
adding host inference or a new parser. The existing `/minint` keep path already
preserves localized SIF bytes without decoding; source/native tests now prove
that distinction. All 694 production files are unchanged and focused normal/
sanitizer/locale checks pass. See
[the audit](linux-port-plan.md#cp22n19--localized-sif-encoding-contract-audit-2026-10-04)
for primary references, limits and the next differing-root-detector checkpoint.


## CP22n20 — Retain different existing detectors

The CP22n14 equality restriction is removed for root NTDETECT.COM. Original
SetupWinPE leaves the existing file after its no-replace copy fails; Linux now
retains different content with the same bounded reader and identity checks.
There is no transformation, replacement, authenticity claim or inferred version.
Native/source retain mandatory inputs, kind/link/case/size/error/cancel checks;
BOOTMGR equality and SIF encoding rules remain. Production logic shrinks by two
lines and one narrowed condition; no new adapter or helper flow. Normal/sanitizer
and focused Qt checks pass, including unchanged metadata, source bytes, exact
no-detector-stage budgeting and before-authorization refusals. The next checkpoint
uses the existing complete helper/readback recipe with differing detector bytes.
See [the checkpoint](linux-port-plan.md#cp22n20--retain-a-differing-root-ntdetectcom-2026-10-04)
for evidence and limits; actual Windows Setup compatibility remains deferred.


## CP22n21 — Full differing-detector creation

The existing complete Qt/helper i386 FAT32 recipe now qualifies CP22n20's differing
root detector alongside edited root SIF and matching root loader. Independent
readback preserves each detector's distinct source hash and all directory sources,
with exactly the expected root edits/files. Original records/FSInfo, the narrowly
allowed boot-label fsck notice, one simulated authorization and durable READY/
unplug assertions pass. Generated BIOS observes drive 0x81 and loader tail; it
never executes the synthetic detector and does not qualify Windows Setup. All
production hashes are unchanged. See
[the evidence and next loader step](linux-port-plan.md#cp22n21--complete-differing-detector-qthelper-readback-2026-10-04).


## CP22n22 — Reuse loader transformation for admission

Existing root BOOTMGR no longer needs raw equality in patch mode: its one-pass
shared transformation must equal the selected directory loader's one-pass result.
This admits ordinary prepatched and partially patched payloads while preserving
one original patch pass on the actual root. Keep-minint retains raw equality.
Original scan skips can make a second pass different, so neither patch success
nor a "repeat until stable" comparison supplies this contract. Native/source and
focused Qt tests cover accepted inputs, wrong-family/drive/tail and nonconverging
refusals, full staging space and existing identity/atomic-write faults. Windows/
shared production code is unchanged. Nonconverging roots remain parity work;
actual Windows Setup is still deferred. See
[the checkpoint](linux-port-plan.md#cp22n22--existing-loader-admission-through-one-shared-patch-pass-2026-10-04).


## CP22n23 — Full prepatched-loader creation

The existing complete Qt/helper i386/FAT32 guest now qualifies CP22n22 with a
prepatched root loader, differing retained detector and distinct root SIF.
Readback preserves the root loader hash and all directory inputs, retains both
detectors and verifies the SIF edit. Original records/FSInfo, the narrowly allowed
boot-label fsck notice, single authorization, held exit before READY and unplug
checks pass. Generated BIOS observes drive 0x81 and intact tail. All production
hashes remain unchanged; synthetic detector execution/Windows Setup stay deferred.
See [the evidence](linux-port-plan.md#cp22n23--complete-prepatched-loader-qthelper-readback-2026-10-04).


## CP22n24 — Mixed setup-directory plan reuse

Fresh Linux preflight now follows the already shared/native keep-loader plan
for complete i386/AMD64 with /minint and partial minint files, removing its extra
blanket rejection. Without /minint or without a complete family it still refuses.
Root SIF copying/loader patches are skipped; report/version and required-file
checks remain. Normal/sanitizer/source/native and 94 focused Widgets checks pass.
The broader Syslinux/GRUB/EFI mixtures still need the original independent MBR,
SBR, PBR and post-copy ordering, including Windows's Alt+E exception; their gates
are retained. The next complete guest checks the newly admitted keep-loader
combination before mixed-provider work. See
[the plan](linux-port-plan.md#cp22n24--follow-the-shared-plan-for-mixed-setup-directories-2026-10-04).


## CP22n25 — Complete mixed-directory keep-loader job

The existing Qt/helper i386/FAT32 guest qualifies the newly admitted partial-
minint combination with /minint, raw retained root loader, differing detector
and unused root SIF. Exact eight-file readback equals all source hashes. Win7
MBR and original PE records remain without masquerading; generated BIOS gets
drive 0x80 with intact tail. Single simulated authorization, held exit before
READY, sync/unmount/released claim and unplug checks pass. All production hashes
remain unchanged. Actual detector execution/Windows Setup remain deferred. See
[the evidence and shared-dispatch next step](linux-port-plan.md#cp22n25--complete-partial-minint-qthelper-readback-2026-10-04).


## CP22n26 — Reuse the independent provider choices

Original Windows MBR priority and Syslinux-versus-PBR operation choice now live
in the common format_policy C module, alongside the existing SBR selector.
Windows native calls/logging and Linux's current single-provider MBR adapter
consume the shared choices. Alt+E's Windows exception and independent GRUB SBR
selection remain distinct; BIOS SetupWinPE still runs at its original later
point. Frozen original-branch comparisons and normal native/request regressions
pass, with shared policy sanitizer checks. Mixed provider layouts remain gated
until their complete native sequence is composed. See
[the evidence and PBR byte-selection next step](linux-port-plan.md#cp22n26--share-the-original-boot-provider-dispatch-2026-10-04).


## CP22n27 — Reuse actual partition boot-record priorities

WritePBR's filesystem/boot priority now lives in the existing common C module,
separate from the Syslinux/PBR operation selector. Windows native writers and
existing Linux FAT/NTFS adapters consume it; final legacy BOOTMGR retains its PE
record/FSInfo/drive fixup. Frozen original-branch comparisons and independent
native byte/fault tests pass normally and with sanitizers. A complete generated
NTFS Qt/helper job passes exact readback, durable READY and BIOS drive 0x80.
Initial TCG timeout and successful diagnostic/unchanged retries are recorded;
actual Windows Setup remains deferred. Mixed providers still need a complete
native dispatch/config/post-copy audit before admission. See
[the checkpoint](linux-port-plan.md#cp22n27--share-the-original-partition-boot-record-choice-2026-10-04).


## CP22n28 — Syslinux files without Syslinux installation

The original Alt+E Windows exception selects Windows PBR installation for this
WinPE/Syslinux mixture; SBR selection remains independent. Extraction still
patches/creates/backups Syslinux configs before SetupWinPE. Fresh advanced FAT32
backend preflight now follows that shared operation choice, retaining report
flags and unknown versions; existing native extraction/preparation needs no new
writer. Real ISO/UDF functional byte comparisons, negative cases and normal/
sanitizer regressions pass. Qt's default gate remains until a complete simulated
mixed device job and selection/settings transitions are qualified. Enhanced NTFS
and other mixtures remain gated here. See
[the checkpoint](linux-port-plan.md#cp22n28--winpesyslinux-alte-backend-preparation-2026-10-04).


## CP22n29 — Complete Syslinux-data/WinPE helper sequence

A generated i386/Alt+E FAT32 USB now passes the complete backend helper sequence
without any production change. Exact ten-file readback includes patched Syslinux
data, generated config and patched backup alongside the independently prepared
WinPE SIF and preserved prepatched loader/detector. Native Syslinux installers are
absent and unused; unknown versions stay unknown. Independent original MBR/PE
records, zero SBR gap and final drive 0x81 reach the generated BIOS loader. Sync,
private unmount, released claim and helper/launcher exit precede client completion.
This does not admit the mixture in Qt or qualify Windows Setup/detector execution.
Mixed Alt+E NTFS remains gated and is next, before GUI settings/selection checks.
See [the checkpoint](linux-port-plan.md#cp22n29--complete-mixed-winpesyslinux-fat32-helper-job-2026-10-04).


## CP22n30 — Reuse the existing NTFS sequence under Alt+E

The shared original Alt+E exception also selects Windows PBR installation for
the WinPE/Syslinux mixture on NTFS. Existing native NTFS formatting/boot records,
fresh private mounting, ordinary config extraction and WinPE root preparation
already implement that sequence. Only the preflight prohibition on enhanced NTFS
is removed; no native writer, helper, Qt or Windows change. Mixed Alt+E-off,
UEFI:NTFS and other unsupported combinations remain gated. Functional every-file
ISO/UDF extraction/preparation and native formatted-file PBR/fault checks pass
normally and with sanitizers. The complete mixed NTFS kernel/helper/drive 0x81
job is next; Qt admission remains deferred. See
[the checkpoint](linux-port-plan.md#cp22n30--mixed-winpesyslinux-alte-ntfs-preflight-2026-10-04).


## CP22n31 — Complete NTFS mixture without another installer

A complete generated mixed WinPE/Syslinux NTFS helper job now passes with the
preferred kernel ntfs driver, ten exact file hashes, original Rufus/NTFS records
and generated BIOS drive 0x81. Neither Syslinux nor Extlinux is installed or used;
the original Alt+E Windows exception and existing extraction/WinPE path suffice.
Sync/private unmount/released access/helper exit precede backend completion.
All production code is unchanged. Qt admission and its settings/selection/READY
behavior are next; actual Windows Setup/detector execution remains deferred. See
[the checkpoint](linux-port-plan.md#cp22n31--complete-mixed-winpesyslinux-ntfs-helper-job-2026-10-04).


## CP22n32 — Qt uses the qualified original Alt+E route

Qt now supplies its saved Alt+E preference to the existing Windows candidate
predicate and retains it in NTFS requests. Loading the mixture with Alt+E follows
shared FAT32 preference; toggling preserves allowed selections. Unknown optional
Windows metadata must not hide valid DD with Alt+E off; the new hybrid tests catch
and fix that Qt gate. Root/provider flags and detected versions remain unchanged,
including the original GUI fallback diagnostic. Fresh preflight uses unknown
Syslinux version with no native installer selected.

Focused Widgets/source normal and sanitizer checks pass. One complete mixed NTFS
Widgets job passes independent ten-file/boot-record readback, one authorization,
held-exit Finalizing/full READY/unplug and generated BIOS drive 0x81. Existing
formatter/extractor/WinPE preparation and boot writers need no changes. Actual
Windows Setup/detector remains deferred. See
[the checkpoint](linux-port-plan.md#cp22n32--mixed-winpesyslinux-qt-admission-and-durable-ready-2026-10-04).


## CP22n33 — GRUB data follows the original independent dispatch

Alt+E does not suppress WriteSBR. The original Syslinux precedence does, so GRUB2/
GRUB4DOS files alongside the admitted Syslinux/WinPE mixture need only ordinary
copy/config preparation. Replace the blanket GRUB gate with the existing shared
SBR choice; keep actual SBR routes gated. No writer or protocol is added. Original
pre-write GRUB version lookup still prompts for a differing 2.12 module; No selects
no core for this Windows route, and Cancel aborts before authorization.

Source/Widgets normal and sanitizer checks plus one complete 14-file mixed NTFS
Qt/helper/readback/BIOS/READY job pass. The next missing piece is an independent
GRUB2 SBR beside a Windows MBR, since the native bounded core writer currently
requires a GRUB2 MBR. Actual Windows Setup remains deferred. See
[the checkpoint](linux-port-plan.md#cp22n33--reuse-the-sbr-free-winpesyslinuxgrub-path-2026-10-04).


## CP22n34 — Reuse the GRUB2 SBR writer with Windows records

WriteGrub2CoreImage now accepts the selected original Rufus, Win7 or masqueraded
Rufus MBR in addition to GRUB2, matching the independence of Windows WriteMBR and
WriteSBR. Reuse all existing bounded I/O, exact layout/record checks, flush and
full readback. Preserve MBR/active byte and every byte outside the core's actual
extent. Payload compatibility remains the caller's decision; a Windows MBR need
not execute the copied core. No new image request or helper/Qt route is enabled.

The old gate is reproduced; 643 placement/fault cases pass normally and under
sanitizers alongside existing MBR/ext/helper regressions. A baseline differential
helper reproduces twelve obsolete older-Windows refusal expectations, now
corrected with their existing readbacks retained and an explicit no-GRUB-core
assertion. Next, connect one coherent Windows-MBR/GRUB2-SBR route through the
existing fresh policy and core selection. See
[the checkpoint](linux-port-plan.md#cp22n34--independent-grub2-sbr-with-original-windows-mbrs-2026-10-04).


## CP22n35 — Retained GRUB2 SBR in the existing Windows NTFS job

The first backend route uses Alt+E and the shared keep-loader plan. Reuse Windows
operations and the existing optional core metadata/staging/owned snapshot. Fresh
analysis must independently select a GRUB2 SBR; reject mismatched or suppressed
cores before target access. The existing NTFS helper writes that core, installs
its Windows PBR, copies and prepares WinPE, then completes ordinary finalization.
No additional authorization, operation, installer flag or original policy change.

Source, request/process, retained core and native-helper regular-file checks pass
normally and under sanitizers, including both original Windows MBRs and ISO/UDF.
Unknown Windows metadata stays unknown. Qt, FAT32 and patch/masquerade SBR routes
remain gated; payload compatibility and actual boot/USB qualification remain
required. Next audit the existing Qt lookup/preflight ordering. See
[the checkpoint](linux-port-plan.md#cp22n35--reuse-windows-requests-and-retained-grub2-sbr-payloads-2026-10-04).
