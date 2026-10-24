# GRUB2 reuse audit — CP21a

Audited 2026-09-29 against this checkout. The exact source and payload hashes are
recorded in parent `.rufus-test-artifacts/cp21a-boot/manifest.json`.

## Decision

Reuse Rufus's existing GRUB2 MBR and selected `core.img`, with the existing Linux
partition/format/helper infrastructure and a small bounded boot-record adapter.
The ordinary Rufus route does not need a host `grub-install`, a new GRUB source
build or a Windows API emulation layer. This recommendation follows the code
and the file-backed boot probe below; it does not yet enable GRUB in Qt.

GNU's native installer is useful for a general GRUB installation, but it generates
a core image and installs GRUB files. That is a different operation from placing
Rufus's already-selected bytes while preserving an ISO's module tree. Rebuilding
with the host's version would also lose Rufus's selected version, prefix and
patches. See the primary [grub-install documentation](https://www.gnu.org/software/grub/manual/grub/html_node/Invoking-grub_002dinstall.html).
No host GRUB utilities are installed here, and none were installed for this audit.

## What Windows actually does

| Part | Windows implementation and behavior | Linux treatment |
| --- | --- | --- |
| Boot choice | `src/rufus.c:SetBootOptions` offers advanced **Grub 2.14**, from `res/grub2/grub2_version.h`. `SetPartitionSchemeAndTargetSystem` disables GPT and UEFI for this standalone choice. | Keep those labels and eligibility rules; do not add native GRUB's BIOS-on-GPT behavior to this option. |
| Filesystems | Shared `GetAllowedFileSystems` allows FAT16/FAT32/NTFS and ext2/3/4 for standalone GRUB2. It does not offer exFAT just because the embedded core contains its reader. | Reuse the shared mask, intersected with qualified native format/layout support. NTFS and ext support remain required follow-up work, not exclusions. |
| MBR | `format.c:WriteMBR` selects Syslinux before GRUB2 for an image containing both. `ms-sys/br.c:write_grub2_mbr` writes the original **432-byte** array and boot signature; disk signature and partition table survive. | Reuse that payload/function and preserve the existing writer's identity/layout checks. Do not treat it as a 440-byte Syslinux payload. |
| Secondary boot record | `format.c:WriteSBR` writes the selected core at byte 512. It checks that the complete payload fits before the first partition. Syslinux takes precedence; GPT uses the protective message instead. | Use the retained whole-disk descriptor. Share portable placement/bounds logic when integrating, keeping resource acquisition and GUI decisions outside it. |
| Partition boot records | The subsequent `WritePBR` still writes Rufus's standard FAT16/FAT32/NTFS records; FAT32 updates its backup too. ext2/3/4 return success without that write. | Reuse the original ms-sys routines through the existing bounded boot-record I/O approach. GRUB bypassing the PBR does **not** remove this Windows parity obligation. |
| Image content | Shared inspection records version/build/prefix information; normal ISO extraction preserves modules and applies existing configuration/label fixes. | Keep that path. Avoid copying unrelated host modules or generating a replacement `grub.cfg`. |
| Standalone content | The GRUB2 choice writes boot records but does not install a module tree, configuration or OS. This differs from the later Grub4DOS branch, which copies `grldr`. | A bare GRUB rescue prompt without `normal.mod` is expected. Do not describe it as an installed OS or working menu. |
| Progress/finish | Boot-record work belongs to `OP_FIX_MBR`; the generic standalone progress allocation reserves copy slots even though GRUB2 supplies no module-copy events. | Reuse the original allocation/events; no invented copy percentages. One authorized job, final sync, cleanup and helper exit before READY. |

The selected MBR is already prepared to load sector 1. The embedded core's first
sector then loads sectors 2 onward. This matches GNU's distinction between
[`boot.img`, `diskboot.img` and `core.img`](https://www.gnu.org/software/grub/manual/grub/html_node/Images.html).
These prepared positions are why the existing Rufus payload does not require
running another installer to choose disk extents.

## Payload provenance and version behavior

`res/grub2/readme.txt` records GNU GRUB 2.14 with the linked image-base revert
patch and `--enable-boot-time`. Its `grub-mkimage` command selects the fixed prefix
`(hd0,msdos1)/boot/grub` and modules `biosdisk fat exfat ext2 ntfs ntfscomp part_msdos`.
`boot.img` has the existing hard-disk BIOS workaround at offset 0x66. Windows
actually writes `src/ms-sys/inc/mbr_grub2.h`; the probe checks its first 432 bytes
against `boot.img`, rather than assuming the two files agree.

The resources are existing boot-time binaries. Consuming them from the CMake
build adds no autotools build, native GRUB package or host configuration probe.
Their recorded upstream build recipe is provenance, not a new Linux build step.

For BIOS image mode, `src/rufus.c:BootCheckThread` handles a nonempty detected version
that differs from the embedded one. It first looks for a cached
`grub-<version>/core.img`, then offers the original Yes/No/Cancel download choice.
Signed downloads try the full name and progressively shorter prefixes at dots
and dashes. Failed/declined downloads or unreadable cached data fall back to the
embedded core; Cancel stops preparation. The current code logs payload-database
membership when writing; that log is not a general compatibility proof.

Keep that selection policy separate from installation and from the detected
version. An empty detection currently uses the embedded fallback on Windows;
do not rewrite it as a detected 2.14 or choose a host installation's version.
`src/grub_inspect.c` already preserves `-nonstandard` for `/boot/grub2`, debug
build suffixes and the special 2.06 label suffix. Those distinctions matter to
the image's external `normal.mod` and other modules. A working embedded 2.14
bare loader does not qualify unrelated versions, prefixes or patched module sets.
Full image enablement must include that existing choice/fallback behavior and
representative matching-source tests, rather than silently advertising all GRUB ISOs.

## Executable evidence

The standalone CMake [probe](../linux/probes/grub2/README.md) compiles the original
`src/ms-sys/br.c` with ordinary bounded stdio callbacks. It uses no Win32 handle
or `FAKE_FD`. The small file-only harness places the unchanged core at the same
offset as `WriteSBR`; this harness is not linked into the application.

For both FAT16 and FAT32, existing native MBR/formatter probes prepare a 64-MiB
regular file. The test seeds the gap and metadata, invokes the original MBR
writer, and independently compares the entire resulting file. Only bytes 0–431,
the boot signature and the selected core range may change. The MBR disk ID,
partition entry, remaining gap and filesystem contents must remain identical.
Empty/undersized/oversized payloads and a 63-sector old-BIOS gap are refused
without changing the file. The current core is 42,742 bytes, so that old gap
cannot fit it; Windows already reports insufficient SBR space for this case.

SeaBIOS boots the generated image as emulated USB storage, through the original
MBR and core. Guest commands verify `prefix`, `root`, and listing of a known
file on each FAT volume. Snapshot boot must leave the backing-file hash unchanged.
The missing `normal.mod` and rescue prompt are expected for this bare-loader test.
No module/menu/kernel/OS handoff, NTFS/ext volume, UEFI, Secure Boot or physical
firmware compatibility is claimed. GNU likewise explains that the core normally
loads the remaining modules from the filesystem in its [image documentation](https://www.gnu.org/software/grub/manual/grub/html_node/Images.html).

Normal and ASan/UBSan/LSan runs cover the host reuse harness, including the original
MBR routine. Firmware and the prebuilt core are not sanitizer-instrumented.
No privileges, host mount, device node or bootloader installation are needed for
the normal probe. Generated media is deleted on success and failure; compact
screens, commands, manifests and hashes remain in the evidence directory.

## Next small checkpoints

1. **CP21b — bounded boot-record adapter on files.** Reuse the GRUB2 MBR array
   in the existing MBR writer and extract/reuse the portable SBR placement rule.
   Accept already-selected immutable payload bytes. Validate identity, sector
   size, capacity and the embedding gap before writes; add partial-I/O,
   cancellation, flush and readback failure tests. Generalize the existing
   FreeDOS PBR staging just enough to call the original standard FAT routines;
   do not create a GRUB filesystem writer.
2. **CP21c — complete standalone helper job.** Add only the necessary explicit
   request selection to the existing format/helper lifecycle. Qualify FAT16/32
   simulated-device jobs, cleanup and representative bare boots before adding
   the advanced Qt option. Extend through native NTFS and ext backends in
   separate testable steps. Do not drop the existing exclusive descriptor or
   request a second authorization.
3. **Source-backed GRUB images.** Separate the existing version/cache/download
   decision from GUI code into shared policy where needed. Retain version/build
   and prefix distinctions, original Syslinux precedence, configuration fixes,
   ISO/DD choices and required UEFI:NTFS combinations. Qualify matching modules
   and a real configuration/boot handoff before exposing each route.

No production GRUB installer, GUI option or Windows-source modification is part
of CP21a. This checkpoint settles the reuse direction and the next adapter
boundary with runnable evidence.

## CP21b — bounded adapters implemented

The existing partition writer now supports `MBR_BOOT_GRUB2` using the original
432-byte array. `WriteGrub2CoreImage` reuses its target/layout checks and transfer
loop for the selected immutable payload. It writes exactly at byte 512, refuses
overflow into the first partition, flushes partial work after failure/cancellation,
reads back the complete payload and rechecks the MBR and target. Memory remains
bounded to the existing 64-KiB buffer size. This preserves the small `WriteSBR`
placement rule directly; a separate shared abstraction for that arithmetic
would add no substantial reuse. Windows code and payloads remain untouched.

The FreeDOS staging adapter is now named `fat_boot` and explicitly selects
`FAT_BOOT_FREEDOS` or `FAT_BOOT_STANDARD`. Both use the original ms-sys routines,
the same BPB/backup validation, staged writes, sync and readback. The existing
FreeDOS helper selects its previous records explicitly; there is no second PBR
writer or change to FreeDOS payloads. Standard device jobs and GRUB core device
writes remain gated until the next helper checkpoint.

Normal and sanitized regular-file FAT16/FAT32 outputs boot through the production
MBR/core and retain working directory reads after the standard PBRs are installed.
Refusal/fault tests and independent full-file comparisons cover the adapter
boundaries. This completes CP21b; CP21c still needs the complete standalone helper
job, simulated block-device lifecycle and eventual Qt exposure. NTFS/ext and
source-specific modules/version selection remain follow-up work.


## CP21c — complete standalone helper

The request protocol accepts only source-free `prepare-grub2-fat16-device-v1`
and `prepare-grub2-fat32-device-v1` operations. They require the existing native
MBR/FAT target and reject caller payload paths, source images, DOS/Windows
settings, UEFI:NTFS and other boot selections. The same helper loads the original
core from its compiled resource before disk writes, formats through the existing
native formatter, then calls the bounded core and standard FAT adapters with the
retained whole-device descriptor. Each stage retains existing identity checks,
flush and readback. The shared finalizer flushes the whole disk before completion.

The device gates from CP21b are removed for these qualified adapters. A new
installer, formatter, filesystem mount or privilege session was unnecessary.
The unprivileged process client requires the bootloader and finalization phases,
rejects copy events/nonzero copied bytes, and still joins helper exit. The
original generic copy-slot allocation remains unchanged even though this
standalone option copies no files. A bare rescue prompt is expected until the
user supplies modules/configuration; that is the Windows standalone behavior.

The original standard PBR also leaves `NO NAME` in its BPB while retaining the
requested directory volume label. Independent validation reuses the existing
FreeDOS check that accepts precisely this fsck diagnostic and rejects any other
repair. No production bytes were changed to satisfy a formatter expectation.

The next checkpoint is the advanced Qt choice for this qualified FAT route.
Native NTFS/ext boot records and image-specific payload/module/version handling
remain separate required work.


## CP21d — Qt selection of the qualified standalone route

The advanced Widgets boot list now uses the original `GRUB2_PACKAGE_VERSION`
label, `Grub 2.14`. The existing selection controller fixes GRUB2 to MBR/BIOS,
intersects Rufus's filesystem policy with native format and helper support,
and offers FAT16/FAT32 only. An earlier image selection stays available for
switching back to Image, but never enters the standalone request. No host GRUB
package, host settings or additional authorization is involved.

The existing ISO-write controller handles the source-free job. It retains
confirmation, immutable options, Rufus progress allocation, cancellation and
completion only after the helper exits. Normal and sanitizer Widgets tests cover
these transitions. This does not qualify the Windows standalone NTFS/ext paths
or any image-backed GRUB installer; those still require native boot-record,
module/version/prefix and representative boot work before exposure.


## CP21e — NTFS MBR/core file boundary

The shared MBR planner now permits the original GRUB2 bootstrap with an active
NTFS partition, in addition to the already-qualified FAT types. It still
rejects exFAT GRUB2 and an extra UEFI:NTFS payload for this standalone choice.
The existing MBR writer supplies partition type 0x07 and the retained core writer
uses its unchanged byte-512 embedding and readback path. The extension is a
narrow filesystem/boot-code eligibility rule; it does not select host GRUB.

Regular-file layout/preparation and core fault tests run for NTFS in normal and
sanitized builds. They do not claim an NTFS filesystem boot yet. Windows also
writes its standard NTFS PBR after formatting; adapting that original ms-sys
routine is the next file-level checkpoint before complete helper and Qt work.


## CP21f — original NTFS PBR staged on native files

Rufus's `ms-sys/ntfs.c:write_ntfs_br` replaces its 11-byte jump/OEM prefix and
4,052 bytes from offset 0x54: **4,136 primary bytes** in total. The BPB between
those ranges survives. A 512-byte sector-only adapter would silently omit most
of Rufus's bootstrap, so the Linux writer stages and verifies the full record.
It does not rewrite the backup NTFS boot sector; that follows Windows's
`WritePBR` behavior. The original payload array and writer remain unchanged.

The existing NTFS formatter's primary/backup, geometry and MFT readback is
checked before staging. The bounded ms-sys memory callbacks formerly inside
the FAT adapter are shared with NTFS, without target descriptors or Win32
`FAKE_FD`. The target writer owns exact-length transfer, partial-write flush,
readback, backup preservation, cancellation and retained-descriptor checks.
Tests compare the complete native-formatted file with the original Rufus byte
arrays and independently read the resulting label and NTFS metadata. Simulated
block-device/helper integration and a representative BIOS boot remain next.


## CP21g — complete standalone MBR/NTFS helper

The versioned source-free `prepare-grub2-ntfs-device-v1` request now joins the
existing GRUB2 MBR/core writer, native `mkntfs`, and Rufus's original NTFS PBR.
It excludes the UEFI:NTFS payload and ISO extraction, retains one authorized
helper process and the whole-disk claim throughout formatting and boot writes,
and uses the existing final flush before READY. The Qt filesystem list stays
FAT16/FAT32-only for GRUB2 at this checkpoint.

On a disposable QEMU USB disk, the complete job passes independent MBR/core/PBR
byte checks, native NTFS label/metadata reads and a bare BIOS boot to GRUB
rescue with the NTFS partition selected. Eight further guest jobs inject write,
sync, readback and cancellation faults at the core and NTFS PBR stages. The
client requires the bootloader/finalization phases, accepts no copy events, and
waits for the single launcher to exit. This qualifies the helper route; Qt
exposure and its Widgets behavior are the next checkpoint. Ext filesystems and
source-backed GRUB remain later work.


## CP21h — NTFS in the advanced Qt choice

The advanced Widgets GRUB2 choice now offers NTFS beside FAT16 and FAT32 when
the MBR layout and native NTFS cluster sizes are valid. The existing selection
controller can retain an explicit NTFS choice or preselect an existing NTFS
volume. The existing source-free controller performs native formatter preflight,
normalizes the NTFS label, shows the exact MBR/NTFS confirmation, launches the
same single helper and reports READY only after its final result and exit.

No new Qt boot workflow or host GRUB detection is involved. The protocol-only
Widgets fixture accepts the distinct NTFS operation and checks that it carries
no source, UEFI:NTFS payload or copy events. Normal and sanitized selection,
creation, refusal and cancellation tests cover the new choice; the complete
normal Widgets regression suite also passes. Ext and image-backed GRUB remain
separate checkpoints.


## CP21i — ext-family audit and MBR/core layout

The shared GRUB2 filesystem mask includes ext2, ext3 and ext4, but Windows
`SetFileSystemAndClusterSize` actually assigns a cluster choice to ext2 and
ext3 only, in advanced format mode on disks of at least 256 MiB. Both receive
Rufus's sole `0x100` "Default" sentinel. Partition placement uses that value;
`FormatPartition` later turns it into block-size zero so `FormatExtFs` chooses
the real filesystem block size. `FormatExtFs` itself maps an ext4 request back
to ext3, so exposing ext4 as a distinct Linux choice now would claim behavior
that the Windows UI does not supply.

Windows puts type `0x83` in the active MBR entry, writes the same GRUB2 MBR and
embedded core as for FAT/NTFS, and skips `WritePBR` for ext. Its bundled
`ext2fs` formatter makes ext2 or ext3 with selected feature flags and a journal
for ext3. On Linux, the native e2fsprogs formatter is the appropriate backend
to qualify; copying the Windows I/O manager or adding an ext PBR writer would
add code without preserving useful behavior. Native formatter options and
superblock readback still need independent qualification.

The existing Linux MBR planner now accepts only GRUB2/ext2 or GRUB2/ext3 with
the original sentinel and size floor, emits type `0x83`, and reuses the original
432-byte MBR and bounded embedded-core writer. A sparse-file test checks both
tables, exact payload placement and rejection of ext4, incompatible bootstrap,
cluster size, device size and UEFI:NTFS. No ext formatter, helper protocol or
Qt filesystem option is enabled at this checkpoint.

Next, qualify `mke2fs` on a borrowed partition-sized descriptor, including
cancel/flush/readback and the ext2/ext3 feature/label differences. Then connect
the existing single helper and prove a representative BIOS boot before offering
the choices in Widgets. Source-backed GRUB remains separate.


## CP21j — native ext2/ext3 formatter on files

The new file-only adapter calls a trusted absolute
[`mke2fs`](https://man7.org/linux/man-pages/man8/mke2fs.8.html) through the existing
formatter supervisor with a borrowed partition-sized descriptor. It uses the
same Rufus size-profile defaults for block size, inode size and bytes per inode,
with five percent reserved blocks. It explicitly selects ext2 or ext3 and the
features Rufus sets in its bundled `ext2fs` formatter; ext3 alone gets a journal.
The command disables discard and initializes inode tables before success.
Readback checks the native superblock's type, geometry, features, journal,
label and clean state. The native creator OS field is Linux, rather than the
Windows value written by Rufus on Windows; that reflects the native formatter
and does not change GRUB's filesystem selection.

Disposable regular files pass native `dumpe2fs` and read-only `e2fsck` checks
for both formats, including the 512-MiB block/inode transition, UTF-8 label and
reformatting an ext3 volume. Refusal, false success, corrupted readback, child
failure and sync fault cases pass normally and under sanitizers. The adapter
does not yet accept a block descriptor. The existing helper protocol and Qt
still reject ext; complete device ownership, helper integration and BIOS boot
are the next checkpoints.

## CP21k2 — native ext2/ext3 GRUB2 USB jobs

The Linux helper now accepts explicit source-free GRUB2/ext2 and GRUB2/ext3
operations. It reuses Rufus's original MBR/core writer and performs no ext
partition boot-record write. The formatter receives a verified partition
descriptor through the existing one-child supervisor. Unlike `mkntfs`,
`mke2fs` opens the partition exclusively and fails while our whole-disk claim
is held; the existing FAT/exFAT handoff releases that claim only during the
formatter, then reacquires it and checks the MBR and target identity. This is
still one authorized helper launch.

Disposable QEMU USB disks passed native ext2 and ext3 filesystem readback and
bare BIOS boot to GRUB rescue. The ext3 guest also exercised missing and
failed formatters, cancellation, table corruption, competing reacquisition,
final-sync failure/cancellation and original core write faults. Qt's advanced
filesystem list remains the next checkpoint; image-backed GRUB is separate.


## CP21l — advanced ext2/ext3 Qt choice

The standalone Grub 2.14 Qt route now includes native ext2/ext3 behind Win32's
advanced-format expansion gate and 256-MiB minimum, with one **Default** cluster
entry. It uses the original shared filesystem mask and already-qualified
MBR/core/helper implementation. Ext4 and source-backed GRUB remain separate
work; no host GRUB installer or additional disk writer was introduced.

Win32 skips `ToValidLabel` for ext, then `static_strcpy` stores at most 15 bytes
plus a terminator in the 16-byte volume name. Qt preserves case/punctuation and
shows that effective name in confirmation. At a split UTF-8 boundary it keeps
whole characters, a documented correction rather than reproducing malformed
text. `mke2fs` availability is checked before and after confirmation, before
launching the one authorized job.


## CP21m — shared image core-selection policy (2026-09-30)

The existing shared inspection/format files now also own the portable decisions
from `BootCheckThread` and `WriteSBR`; no new policy module or platform switch
was needed. Windows calls `NeedsGrub2CoreLookup` for the BIOS/MBR lookup gate,
`GetGrub2DownloadVersionLength` for the original full-name/dot/dash search order,
and `GetSecondaryBootType` for secondary boot-record precedence. Linux's existing
shared libraries build the same code. The download-name helper no longer
temporarily edits `img_report.grub2_version`; neither helper changes detection
or certifies that a selected payload matches external modules.

The Windows cache, prompt and I/O behavior remains in its existing call site:

| Condition/action | Original outcome to retain on Linux |
| --- | --- |
| UEFI target or GPT layout | Skip this version lookup; bootable GPT SBR uses the protective message. |
| No GRUB2, empty detection, or version exactly equal to the embedded version | No cache/download lookup; use the normal embedded record if GRUB2 is selected. Empty detection stays empty. |
| Known different version, full-name cache opens | Read that file without prompting. Positive length and a complete read select its bytes. |
| Opened cache is empty, allocation fails or read is incomplete | Fall back to embedded bytes; do not prompt again. |
| Cache cannot open | Show the existing Yes/No/Cancel version-mismatch warning. |
| No | Use embedded bytes without rewriting the detected version. |
| Cancel | Stop preparation without writes; Linux must cancel before launching its authorized job. |
| Yes | Create/use the directory named for the **full detected name** and attempt signed downloads in the shared prefix order. Store successful `core.img` there, even when a shorter server name supplied it. |
| Every ordinary candidate fails, or installed bytes cannot be read completely | Use embedded fallback, preserving detection. A final missing/invalid signature also sets a fatal Windows error; CP21r handles it explicitly before authorization. |
| Syslinux and GRUB2 both present | The lookup gate can still prompt; Syslinux subsequently takes boot-record precedence. GRUB2 otherwise overrides Grub4DOS. |

A shortened server lookup name is not a detected version, selected ABI or
compatibility proof. Keep the full detected name, chosen server name and actual
payload identity separate when implementing native cache/download handling.
The Windows dual BIOS/UEFI Windows-MBR override also remains outside this SBR
helper; sharing this helper does not generalize all primary-MBR selection.

The 144-case GRUB inspection suite checks lookup gating, suffix/prefix order,
missing/unterminated names, immutable reports and existing build detection.
The format-policy suite adds 160 boot-choice/flag/layout precedence cases and
two standalone null-report cases. Both pass normally and with ASan/UBSan/LSan.
Generated ISO fixtures also pass detection-to-lookup checks, including unknown,
embedded-version, nonstandard/debug/2.06-label and first-usable-file behavior.
The Linux GUI/helper build and nearby window/format-option tests pass. No
network download, cache filesystem implementation, source-backed GRUB job or
Windows executable validation is claimed by this policy checkpoint.

## CP21n — bounded unprivileged cache snapshot (2026-09-30)

`linux/platform/grub2_cache.cpp` adds one reader to the existing
`rufus_prepare_source` library. It uses the shared BIOS/MBR image lookup gate
and reads only `grub-<full detected name>/core.img` beneath a caller-supplied
absolute files directory. It does not create directories or search shortened
cache names. Unknown and embedded-version images skip lookup; Syslinux does
not suppress this earlier lookup. The image report remains unchanged.

Its result distinguishes **NotNeeded**, **Missing** (open failed),
**Unusable** (an opened file could not supply acceptable bytes) and **Ready**
(a complete owned snapshot). Ordinary open/read failures retain the original
prompt-versus-embedded-fallback distinction. No prompt is implemented yet.
Existing native source identity checks compare the retained descriptor and
pathname, including inode, size, mtime and ctime. Replacement, modification,
disappearance or cancellation stops selection with `ESTALE`/`ECANCELED`, rather
than silently switching payloads. That deliberate input-change refusal follows
the port's existing frozen-input contract; Windows's cache read lacks it.

The reader accepts regular files, following a stable symlink once as Windows
`fopen` does. It reads with close-on-exec/nonblocking open flags and bounded
64-KiB chunks. The accepted size is one sector through the planned embedding
gap, excluding the MBR; the supported partition start must be sector-aligned
and at most 1 MiB. A short first sector cannot contain the required BIOS core
loader, and an oversized file cannot fit the already-qualified native writer.
Both are rejected before allocation/read. A FIFO or directory is rejected
without attempting content I/O. Short reads and `EINTR` are handled explicitly.

The returned SHA-256 identifies the retained bytes; it is not a signature,
version detector or module-ABI check. Later cache changes cannot alter the
owned snapshot. Tests use the original 42,742-byte embedded resource as an
I/O fixture under a synthetic cache name; they do **not** claim it matches
that name's GRUB modules. Windows does not authenticate existing cached cores
in this path; its signed downloader remains a separate operation.

All 48 Qt test cases pass normally and under ASan/UBSan/LSan, alongside source
identity and shared GRUB inspection regressions. The GUI/helper builds pass.
This reader is not connected to image creation yet: no download, privileged
job, target write or source-backed boot was performed. CP21o will audit the
original detached-signature format/key and native verification reuse before
adding network selection. Payload transfer and one compatible image/core/module
boot remain subsequent checkpoints.


## CP21r — connected unprivileged core selection (2026-09-30)

`linux/qt/grub2_core_selection.cpp` connects the existing bounded cache reader,
signed Qt Network fetcher and atomic writer in the existing Qt analysis library.
It uses the shared lookup/prefix helpers; there is no second version detector,
cache format, cryptographic verifier, storage framework or privilege mechanism.
The caller supplies its captured ISO identity, files directory, target gate/gap
and FILES_URL root. Results distinguish Embedded, Cache and Download; the full
detected report is copied unchanged, and the chosen server name stays separate.
An Embedded result has no bytes and requests the existing bundled resource.

A missing full-name cache emits one `downloadRequired` signal for the original
MSG_115/116 Yes/No/Cancel warning. A usable cache supplies owned bytes without a
prompt; opened-unusable inputs fall back without another prompt. Existing caches
remain unsigned as on Windows. No selects embedded fallback, Cancel stops
preparation, and Yes tries shared full-name/dot/dash candidates. Failed payloads,
signatures or saves are never selected. Candidate exhaustion falls back; a
successful install is read back once, with ordinary read failure falling back
without restarting the search. The original `DownloadToFileOrBuffer` resets
ErrorStatus for each request, so a rejected signature does not permanently
poison a later valid candidate. Rejected bytes are never written to cache.
The final missing/invalid signature sets Windows ERROR_BAD_SIGNATURE even while
the GRUB search branch logs embedded fallback. Linux preserves that fatal
signature outcome explicitly as EBADMSG before authorization, rather than
carrying a global error into partition/format operations. Last ordinary not-found
exhaustion still falls back. The prior Windows error can be cleared by another
request or conditional format code; its actual runtime effects remain deferred
to Windows validation.

The native flow retains the existing ISO descriptor/identity while prompting
or downloading. It checks before requests, before storing and before completion;
source change or explicit image/target invalidation stops with no selected bytes.
Cancellation stops current networking and produces one completion, without
starting a helper. Cached/verified results own their bytes, so later cache
changes do not modify them. Successful completion makes the object idle before
emitting, allowing immediate reuse or deletion. Destruction silently aborts.

Two deliberate safeguards extend Win32's unchecked preflight: changed ISO input
stops preparation, and installed readback must equal the verified downloaded
snapshot. Stable substituted cache bytes stop with ESTALE rather than becoming
the selected loader. Neither guard is a module-compatibility proof. Source/core
identity will still need checking in the existing authorized helper. Ordinary
cache failures preserve the original fallback behavior; atomic writes and HTTPS
downgrade refusal retain the documented CP21p/q differences.

The 53-case selection suite uses real loopback HTTP and temporary source/cache
files. It qualifies unsigned/valid/empty/short caches, unknown/embedded/GPT/UEFI/
non-GRUB/unterminated gates, lookup before Syslinux precedence, No/Cancel, all
candidate success positions and exhaustion, bad-signature recovery to a valid candidate or ordinary not-found exhaustion,
final missing/invalid signature rejection,
save/readback/substitution faults, changed ISO input at three stages, logical
invalidation, cancellation at prompt/payload/signature, invalid starts and
reentrant completion/deletion/reuse. Seven focused CTests pass normally and
under ASan/UBSan/LSan; GUI/helper builds pass. No physical USB, authorized job,
external download, compatible-image boot or Windows executable was used.
Evidence is in parent `.rufus-test-artifacts/cp21r`; disposable builds were cleaned.

This is the tested selection component and warning contract. The actual main
window warning/creation integration remains pending; image-backed GRUB is still
disabled. CP21s freezes the selected payload into the existing request/helper
preflight, with bounded identity/embedding checks before writes. Main-window
wiring and one compatible image/core/module/configuration boot follow before
advertising the creation route. Unknown detection remains unknown throughout.


## CP21s — frozen core handoff and helper preflight (2026-09-30)

`PrepareImageProcess::start` now optionally accepts the owned selected bytes.
It stages a read-only `core.img` in a private temporary directory before the
existing single pkexec launch. The unchanged 4 KiB JSON limit carries only path,
size and SHA-256; partial metadata and non-GRUB operations are rejected. The
file remains until helper exit, including cancellation/destruction, and is
removed before completion signals. Legacy calls still use the embedded core.

The existing helper plans the embedding gap and loads selected bytes before
opening a target. It requires a caller-owned, readable, ordinary single-link
file, refuses final-component symlinks and checks frozen size/hash. Cache and
helper reads now share one bounded 64 KiB reader using the existing descriptor/
path identity checker, EINTR/partial-read handling and cancellation. Successful
preflight retains an owned array; the unchanged `WriteGrub2CoreImage` never
reopens the cache or staged file. Identity is not authentication or proof of
compatibility; existing unsigned-cache policy is preserved.

The 42-case native suite covers bounds, ownership, file types, mutation/hash,
read faults, cancellation and borrowed-descriptor seek preservation. Six cases
run the actual helper/server/worker in an unprivileged test probe: test-only UID
wrappers admit preflight, and a target.open wrapper blocks **all** target access.
Invalid cores stop before that gate; valid cores reach it and stop. A disposable
sentinel file remains unchanged. This does not acquire real privileges.
The 80-case GRUB request suite includes strict metadata and private staging
lifetime tests across FAT16/FAT32/NTFS, including helper success, error, cancel,
destruction, denied/dismissed authorization and failed launch. Eight focused
CTests pass normally and under ASan/UBSan/LSan, with the original 151-case core
writer regression; production GUI/helper builds pass. Compact evidence is in
parent `.rufus-test-artifacts/cp21s`; disposable builds were removed.

This completes the transfer/preflight component only. Actual main-window
warning/selection wiring, image-backed GRUB job integration and one compatible
image/core/module/configuration boot remain before enabling that route. No
physical USB, real authorization, external download or Windows/customization
qualification was performed. Unknown detection remains unknown.


## CP21t — GRUB warning and START integration

Completed 2026-10-01 after resuming the saved 2026-09-30 checkpoint. The controller
uses the existing core selector before confirmation. The Widgets warning follows
Rufus's original Yes/No/Cancel behavior, including Cancel as the default and escape
button (Win32 notification resource/handler checked). Original download URLs are
shared through `src/download_urls.h`; the Windows values and RUFUS_TEST behavior
remain unchanged. Linux uses a writable per-user files cache because an installed
application directory normally requires privileges; the warning shows that path.
No second authorization route is introduced.

Download progress uses the shared Rufus percentage policy. Payload 100% while
signature validation is pending is still Downloading, not READY. Selection changes,
unplugging and cancellation close the warning and prevent helper launch. All
18 new GUI scenarios pass normally and under ASan/UBSan/LSan. They cover warning
choices/keys, invalidation, signed download, cache reuse, missing/bad payloads,
held payload/signature cancellation and excluded versions/partition/write modes.
The supported mixed Syslinux/GRUB route retains original shared Syslinux/SBR
precedence and launches only one simulated helper after confirmation.

The full Widgets suite passes normally (647 passes, two optional real-ISO skips,
about 141 seconds) and under sanitizers (same results, about 263 seconds), without
sanitizer findings or suppressions. Five neighboring suites and the focused GRUB
Widgets entry pass in both builds. GUI/helper builds pass. The previous 180-second
whole-suite timeout was insufficient; its limit is now 600 seconds. Per-case
waits remain bounded and unchanged. `rufus_grub2_widgets` runs the 18 new scenarios
independently with a 30-second limit. The two skipped checks require explicitly
provided Windows/Alpine ISO paths; no new GRUB scenario was skipped.

Logs, CTest transcripts and source hashes are retained in parent
`.rufus-test-artifacts/cp21t`; disposable builds and private fixtures are cleaned.
No real USB, authorization, external binary download, Windows customization test
or complete image-backed GRUB boot is claimed. Real Windows validation is deferred.
Pure image-backed GRUB creation remains disabled. Accepted core bytes are retained
but deliberately not sent to a Syslinux job.

**Next: CP21u.** Extend the existing prepare request and image checks to represent
an image-backed GRUB job, keeping source-free GRUB behavior and strict rejection
of unrelated options. Reuse shared boot selection and existing image extraction,
core staging/preflight and writer. Qualify the request boundary and helper order
before connecting/enabling the route. Then prove one compatible image/core/module/
configuration boot. Unknown detection stays unknown; a signature or standalone
boot alone does not establish image module compatibility.


## CP21u — GRUB ISO request and read-only preflight

Completed 2026-10-01. The existing GRUB2 operation and source identity fields now
represent either a standalone job or a retained FAT16/FAT32/NTFS ISO. No parallel
request, downloader or writer was added. Optional frozen core metadata uses the
existing bounded field. Source-free classification now distinguishes a GRUB ISO
from a standalone boot-record job; malformed/incomplete sources, customization,
DOS/Syslinux flags, incompatible layouts and ext ISO requests remain rejected.
Existing source-free GRUB/ext jobs are unchanged.

Read-only GRUB preflight shares the Syslinux BIOS report checks, Rufus's filesystem
and extra-partition policy, retained source checks, tree/config validation and
capacity accounting. Shared `GetSecondaryBootType` enforces original Syslinux/SBR
precedence. BIOS GRUB images do not require an x64 fallback EFI application.
Unknown version detection remains empty. Preflight success establishes extraction
prerequisites, not core/module compatibility or bootability. NTFS images needing
the UEFI:NTFS extra partition and ext ISO extraction remain pending, not removed
from parity scope.

The launcher and actual helper explicitly reject this unfinished image-backed
write route. The helper gate precedes source/core/target access. Two added actual
helper probe cases demonstrate ENOTSUP with no target gate reached, including a
missing staged core and nonexistent source. GUI support remains disabled.

Nine focused CTests pass normally and under ASan/UBSan/LSan without suppressions:
135 ISO preflight cases, 83 GRUB request cases, 44 core/helper cases, 66 process,
45 FreeDOS request, 71 Syslinux request, 53 selector and 20 focused Widgets cases
(including init/cleanup), plus 631 shared policy checks. New cases include six
real small ISO/UDF GRUB trees across three filesystems, policy refusals, stale
sources, unknown detection, source/core round trips and blocked write entry.
GUI/helper builds pass. The shared SBR declaration's existing C-linkage guard was
corrected; the Widgets test no longer needs its manual include wrapper. Windows
runtime validation remains deferred; no Windows algorithm changed here.

Compact logs, transcripts and source hashes are retained in parent
`.rufus-test-artifacts/cp21u`; disposable builds and synthetic fixtures are removed.
No real USB, authorization, external download, Windows customization qualification
or image-backed GRUB boot was performed. Full Widgets regression was qualified in
CP21t; this checkpoint uses the focused GUI/request/policy regressions above.

**Next: CP21v.** Reuse the existing FAT/NTFS source mount/extraction and GRUB record
writer inside the single helper. Correct image-job phase/copy accounting, retain
source/core snapshots before writes, and test helper ordering, rejection and
cleanup with simulated targets. Keep GUI creation disabled until a compatible
image/core/module/configuration boot has passed. Do not infer compatibility from
a signature, cache path, shortened version or the standalone boot qualification.


## CP21v — GRUB ISO helper integration (2026-10-01)

GRUB ISO FAT16/FAT32/NTFS jobs now share the existing retained-source extraction
and copy finalization. The helper repeats shared image/SBR policy and source
checks before writes, snapshots the selected core, and installs original boot
records before copying. The existing launcher distinguishes standalone zero-copy
jobs from image jobs, requiring copied data and every ordered phase before clean
completion after child exit. No second formatter, extractor, job layer or prompt.

The [CP21v test record](testing-and-checkpoints.md#cp21v--grub-iso-helper-integration)
describes 49 simulated helper jobs, 36 additional launcher cases and normal/
sanitized regressions. Test-only regular files/directories qualify real writer/
extractor calls and orchestration; they do not establish complete mounted-media
bootability. GUI creation stays disabled until CP21w qualifies a compatible
ISO/core/module/configuration boot. NTFS requiring UEFI:NTFS and ext ISO copying
remain pending; unknown versions are not promoted to known ones.


## CP21w — compatible GRUB ISO creation and boot (2026-10-01)

The existing native MBR helper guest now creates a GRUB 2.12 FAT32 image and
boots its copied normal.mod/config/kernel/initramfs into Linux userspace. It uses
the checked-in signed Rufus core and a fully pinned Ubuntu 2.12-1ubuntu7 module
set as test data, without a new production dependency or GRUB installation.
Normal and sanitized native creation, complete independent file/record readback
and BIOS USB boots pass. The
[test record](testing-and-checkpoints.md#cp21w--compatible-grub-iso-native-creation-and-boot)
contains identities, fixture limits and evidence.

The native run found a small helper reporting error: image-backed GRUB had the
standalone event classification, hiding copy percentages. Source-free GRUB alone
now has that classification; ISO copying uses the ordinary shared counters.
The existing simulated FAT16/FAT32/NTFS tests enforce the final block total.
No Windows algorithm or progress calculation changed.

This proves the tested core/module/configuration combination. It does not imply
compatibility from shortened versions/signatures, add host detection or promote
unknown versions. GUI enablement is CP21x; NTFS/UEFI:NTFS and ext ISO paths remain
pending. Native Windows and physical-media validation remain separate.


## CP21x — GRUB ISO Qt creation (2026-10-01)

The GUI now selects the existing BIOS/MBR/FAT32 ISO job using shared Rufus SBR
precedence. Its existing unprivileged core selection forwards retained bytes
through the previously qualified private handoff; Syslinux precedence keeps those
bytes out of Syslinux jobs. Cache changes after selection cannot change the job.
Download warnings/fallback decisions and unknown image versions remain unchanged.
ISO copying uses BT_IMAGE progress weighting, and READY waits for cleanup and
helper exit. The GUI offers only the qualified FAT32 ISO route; FAT16/NTFS/ext
remain pending. This adds no GRUB installer dependency or second writer.

See the [plan](linux-port-plan.md#cp21x--grub-iso-qt-start-progress-and-ready) and
[test record](testing-and-checkpoints.md#cp21x--grub-iso-qt-start-progress-and-ready).
CP21w's exact native compatible fixture remains the boot evidence; GUI protocol
tests do not certify additional module/payload or firmware combinations.


## CP21y — native FAT16 GRUB ISO boot (2026-10-01)

The same compatible GRUB ISO/core/module fixture now boots from native MBR/FAT16
media, normally and under sanitizers. The production formatter, original boot
records, helper, extractor and core handoff need no changes. Only the existing
test recipe is parameterized, with independent FAT16 geometry and all-file
readback. The default FAT32 recipe also passes. FAT16 GUI enablement is the next
checkpoint; unknown versions and remaining NTFS/ext/firmware combinations stay
pending. See the [test record](testing-and-checkpoints.md#cp21y--native-fat16-grub-iso-creation-and-boot).


## CP21z — FAT16 GRUB ISO Qt selection (2026-10-01)

The existing GRUB ISO GUI route now offers FAT16 and FAT32 where both Rufus's
shared filesystem policy and native MBR geometry allow them. The eligibility
check also accepts small FAT16-only targets. This restores the original first
supported filesystem default for small BIOS targets; FAT16 is absent at the
4-GiB boundary. Image-specific preferences still apply. When a GRUB image has
no filesystem preference, rescanning it preserves a valid manual format choice,
as Win32 does. An explicit FAT32 choice therefore remains FAT32.

Both formats reuse the existing controller/helper lifecycle, selected-core
snapshot, shared copy progress and READY-after-sync/cleanup/helper-exit behavior.
No formatter, installer, compatibility layer or production dependency is added.
CP21w/y supply native creation, complete readback and BIOS boot evidence; this
checkpoint checks the Qt workflow using private D-Bus and file-backed fixtures.
NTFS/ext GRUB ISO choices remain pending qualification/enablement; arbitrary
core/module ABIs, physical media, real polkit and new UEFI combinations are not
certified by these fixtures.


## CP21aa — native GRUB ISO on NTFS (2026-10-01)

The existing helper now has native compatible-fixture qualification for plain
BIOS/MBR/NTFS GRUB ISO creation, with the preferred new kernel NTFS driver and
NTFS-3G fallback, normally and under ASan/UBSan/LSan. Production code is unchanged.
All 308 ordinary files, original MBR/core/PBR, NTFS geometry/clean metadata and
expected label patch pass independent readback. GRUB loads copied configuration
and boots copied Linux kernel/initramfs into userspace reading the USB marker.
One simulated authorization, sync/cleanup/helper-exit and resource audits pass.
The existing FAT32 recipe also passes after fixture extension.

NTFS GUI enablement follows in CP21ab. EFI/UEFI:NTFS and ext GRUB ISO combinations
remain pending; this does not certify arbitrary distribution payload ABIs,
physical media or actual polkit. The existing formatter/mount/extraction/boot
record paths are reused without a new production provider or abstraction. See
[the test record](../docs/testing-and-checkpoints.md#cp21aa--native-grub-iso-on-biosmbrntfs)
for evidence and [the VM recipe](../linux/tests/vm/README.md#grub-iso-on-ntfs-cp21aa)
for reproduction. Temporary builds/media/package trees are removed after export.


## CP21ab — BIOS/MBR/NTFS GRUB ISO Qt choice (2026-10-01)

The qualified plain BIOS GRUB ISO route now offers NTFS in Qt alongside native
FAT16/FAT32. It uses the existing controller/helper, Rufus filesystem policy,
NTFS formatting/mount providers and selected-core snapshot. When image policy
excludes FAT, NTFS becomes the default; eligible manual filesystem and cluster
choices pass unchanged into confirmation and the frozen helper request.

NTFS copy percentages use the original BT_IMAGE allocation: at 50.0% file copy
the tested GUI bar is 37027/65535, compared with 35061 for FAT. Successful READY
requires complete copy, the confirmed partition extent, complete NTFS format
bytes, sync/cleanup and helper exit. No new installer, writer, job abstraction
or production dependency is added. CP21aa retains the native creation/readback/
BIOS-boot evidence with the preferred kernel driver and NTFS-3G fallback.

GRUB ISO NTFS remains unavailable on MBR when the image is EFI-capable, since
Rufus requires its UEFI:NTFS combination there. Existing MBR/FAT and GPT/UEFI
choices remain available under their usual policy. Direct callers also reject
the unqualified combination before authorization. EFI/UEFI:NTFS and ext GRUB
ISO integration remain pending parity work; unknown versions stay unknown.


## CP21ac — GRUB/NTFS with UEFI:NTFS backend (2026-10-01)

The existing request/helper/MBR/raw-payload paths now support source-backed
GRUB/NTFS with Rufus's required UEFI:NTFS partition. The ISO remains retained,
shared partition policy and x64 fallback-loader validation run before writes,
and both bootloaders use one progress stage in the same authorized helper.
Regular-file integration checks compare the retained core, full bundled payload,
partition geometry and copied configuration, and cover payload failure,
cancellation and helper-exit ordering. No new production provider or abstraction
is added. Native combined creation/readback and BIOS/x64 UEFI boot qualification
remain CP21ad; the Qt EFI-capable GRUB/NTFS option remains gated until that passes.
See [the port plan](linux-port-plan.md#cp21ac--grub-iso-ntfs-with-uefintfs-backend-integration).


## CP21ad — native GRUB/NTFS + UEFI:NTFS qualification (2026-10-01)

The pinned compatible GRUB ISO passes production combined MBR/NTFS creation,
complete independent readback, BIOS boot into Linux userspace and x64
UEFI:NTFS handoff to the copied EFI test application. Both the preferred kernel
NTFS driver and absent-kernel NTFS-3G fallback pass normally and under
ASan/UBSan/LSan, with one simulated authorization and complete helper cleanup/
exit. All 309 files, original boot records and bundled payload match readback;
snapshot boot leaves backing media unchanged. The ordinary FAT32 BIOS recipe
also passes. Production hashes match CP21ac; no new production code is added.

Qt enablement follows in CP21ae; the EFI-capable GRUB/NTFS gate remains for now.
This fixture does not qualify arbitrary distribution modules/EFI loaders,
Secure Boot, other EFI architectures, physical media or real polkit. Compact
evidence is retained and disposable builds/media/package trees are removed.
See [the test record](testing-and-checkpoints.md#cp21ad--native-grub-ntfs-with-bios-and-uefintfs-handoffs).
