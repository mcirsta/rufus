# Native Syslinux NTFS reuse — CP20i–n

CP74 extends this original reuse boundary with source-matched Syslinux 5/6
resources and old-family 3/4 image installation through the same NTFS adapter.
The explicit standalone v4 choice remains FAT-only. See the
[ISO/EFI resource guide](linux-iso-efi-resources.md); the CP20 measurements below
record the original pinned v6 qualification.

Audited and probed 2026-09-29. **Use upstream 6.04-pre1 extlinux as the candidate
NTFS provider.** It installs the existing, hash-pinned Rufus v6 payloads on both
the preferred kernel `ntfs` driver and `lowntfs-3g`. Both simulated USBs boot to
the bare Syslinux prompt. CP20j–k add a checked adapter, Windows file attributes
and provider packaging. CP20l connects the standalone v6 helper and qualifies its
complete lifecycle. CP20m enables the standalone choice in Qt.

## Reuse boundary

Windows permits standalone Syslinux v6 on MBR/NTFS. `src/syslinux.c` writes the
loader, obtains its NTFS file extents through `NtfsSectGetFileVcnExtent`, calls
the original Syslinux patcher, and overlays boot code while preserving the NTFS
BPB. v4 remains FAT-only. The native extlinux source performs the same kind of
operation using Linux file mapping and the identical v6 loader/VBR/companion.

| Operation | Native source / observed result | Linux integration |
| --- | --- | --- |
| Build | Eight original upstream C files plus the existing pinned payload generation. Modern glibc needs `sys/sysmacros.h` for `major`/`minor`; CMake supplies it with `-include`. No upstream file or Windows code is edited. | CP20k moves the provider into the shared CMake build as `rufus-extlinux`, packaged in `SyslinuxRuntime`. Only the mapping observer stays test-only. |
| Device identity | `extlinux/main.c:open_device` checks filesystem type and mount `st_dev` against partition `st_rdev`. `--device /proc/self/fd/3` works with an inherited, retained partition descriptor. | Keep the helper's validated descriptor and private mount. Do not use request-supplied paths or native device discovery to replace our validation. |
| Mounted access | Native `O_RDWR | O_SYNC` reopen succeeds. Competing `O_EXCL` opens of the partition and whole disk fail with `EBUSY` before and after installation. | Reuse the existing lifecycle: release the whole-disk exclusive descriptor for the mount, let the mounted filesystem own the claim, then unmount and reacquire the disk. This is not holding an extra whole-disk `O_EXCL` claim while mounted. |
| File mapping | `libinstaller/syslxcom.c:sectmap` tries FIEMAP, then FIGETBSZ/FIBMAP. Kernel NTFS succeeds through FIEMAP. NTFS-3G returns `EOPNOTSUPP` for FIEMAP and succeeds through FIBMAP. | Reuse native mapping and Rufus's patcher. The checked adapter still needs bounded maps, extent/geometry checks, and exact loader/ADV/VBR readback. No new NTFS filesystem parser is justified by this probe. |
| Files and boot code | Both produce a 69632-byte padded loader/ADV and the matching 122656-byte companion. All 136 mapped loader sectors match raw partition reads. Loader contents are identical between drivers. BPB, original MBR, alignment/trailing gaps and backup boot sector stay unchanged. | Reuse existing payload descriptions, process supervision and completion rules. The FAT directory/chain reader does not apply to NTFS. |
| Sync and completion | The native tool uses global `sync()` and closes its device. The guest then syncs/unmounts and successfully remounts each disk read-only through the kernel NTFS driver. | Native exit is not sufficient for READY. Keep our checked sync, child cleanup, unmount, disk reacquisition/final sync and helper-exit gate in one authorized job. |

The kernel mount presents boot files as mode 0444; the NTFS-3G mount uses the
existing synthetic `umask=0077` policy and presents 0700. This does not prove
Windows's hidden/system/read-only attribute parity. CP20k implements and checks
those flags through the common NTFS xattr ABI below, independently of POSIX mode.

## ntfsprogs-plus checker incompatibility

The installed ntfsprogs-plus 1.0.0-3 `ntfsinfo` rejects the resulting VBR with
`Boot sector: Bad jump.` Its [upstream volume check](https://github.com/ntfsprogs-plus/ntfsprogs-plus/blob/main/libntfs/volume.c#L645)
accepts `EB 52 90` or `EB 5B 90`, while the original Rufus/Syslinux payload uses
`EB 58 90`. That jump enters the boot code at byte 90. Windows's existing
`syslinux_make_bootsect(..., NTFS)` copies the same three bytes. This is also a
compatibility issue for that existing media, not evidence of an invalid NTFS
allocation map or an unclean journal.

The investigation records the rejection rather than treating the original
volume as passing this checker. For metadata inspection only, it replaces
those three bytes with the formatter's original jump in a disposable volume
copy: clean volume flags then pass. It restores the actual Syslinux bytes
before independent 7-Zip file readback. Native kernel remounts and both BIOS
boots use untouched media; boot snapshots leave backing hashes unchanged.
Downloaded upstream source blobs and hashes are retained with the evidence.

Keep the original boot bytes and preferred formatter. Do not alter boot code,
repair the test media, switch drivers, or add a checker fork merely to satisfy
this restriction. A future checked adapter must verify the actual Syslinux
VBR/payload directly; it cannot require this version of `ntfsinfo` to accept the
final VBR. A separate upstream checker correction can be proposed later.

## Qualification and next step

The final probe uses two disposable 64-MiB MBR disks, 4096-byte clusters,
ntfsprogs-plus formatting, Linux **7.2.7-arch1-1**, **NTFS-3G 2026.7.7**, QEMU TCG
and SeaBIOS. It uses the production mount options, explicitly disables mount
helper dispatch for the kernel case, and asserts the actual filesystem magic.
No physical device, host mount, installation, authorization prompt or Windows
customization test is involved. This proves a bare loader, not OS boot or the
production helper's cancellation/error behavior. Native upstream executables
are not claimed to be sanitizer-clean.

See the [probe recipe](../linux/probes/syslinux/README.md#ntfs-extlinux-investigation--cp20i).
Logs, file/boot hashes, source audit and screenshots remain under parent
`.rufus-test-artifacts/cp20i-*`; disposable disks and guest trees are removed.

## CP20j — checked native adapter

Completed 2026-09-29. `InstallNativeSyslinuxNtfs` now wraps the candidate in the
existing supervised runner. It borrows the retained partition/directory and
checks the device number, disk sequence, capacity, writable mount root, NTFS
geometry, serial and initially identical primary/backup sectors before launch.
It requires an empty root, a caller-controlled mount path and the trusted
provider; it does not authorize, mount, discover devices or select a driver.

The original Rufus patcher, pinned resource descriptions, reset ADV and exact
payload/VBR comparison are now shared with FAT in `syslinux_payload.cpp`.
NTFS supplies its root path to the patcher, as native extlinux does. Verification
reads both installed regular files without following symlinks, checks sizes and
identity, validates their sector maps, compares mapped raw bytes with file
contents, rejects shared/duplicate sectors and checks the v6 companion hash.
It preserves the original BPB and backup VBR; it does not parse or recreate NTFS.

The bounded Linux mapping adapter uses the same FIEMAP and FIGETBSZ/FIBMAP APIs
as upstream. It does not import upstream `sectmap` into the worker: that routine
can accept holes and falls back after arbitrary FIEMAP errors. The verifier
instead checks extent counts, logical coverage, alignment, flags, physical bounds
and overflow, and falls back only for an unsupported ioctl. Like upstream, it
rounds the final extent when kernel NTFS reports the companion's exact EOF.
The unchanged external installer continues to use its original mapper.

After any started child, the worker completes filesystem sync and a final raw
device sync, even on cancellation/failure. It drops clean raw-device cached
pages before readback: repeated native fixture installs exposed stale raw reads
despite successful filesystem/device flushes. The sector mismatch disappeared
after this invalidation, with exact payload checks still enabled. Invalidation
failure also fails the operation. Unmount, whole-disk reacquisition/final checks
and READY remain the caller's responsibility.

Qualification: **48 cases per driver**, in normal and ASan/UBSan builds, using
the existing two 64-MiB simulated USBs and unchanged native installer. Cases cover
identity/preconditions, provider failure, cancellation of a running process
group, sync errors/EINTR, invalidation failure, damaged loader/ADV/companion/VBR,
symlinks/hardlinks/directories, invalid mappings and both valid map interfaces.
Every case checks borrowed fd state, fd leaks and child reaping. Both final
volumes unmount, remount read-only, pass independent 7-Zip readback and boot to the
bare Syslinux prompt. The guest matches disks by USB serial and compares guest
hashes with host readback; `/dev/sda` enumeration order is not an identity.

Six existing FAT installation suites pass normally and under ASan/UBSan after
the shared verifier extraction. This does not claim the external upstream
installer is sanitizer-clean. The `ntfsinfo` jump restriction described above
still applies; the actual boot bytes remain unchanged. Only 512-byte sectors,
4096-byte clusters and bare-loader native guests were qualified here.

Main Linux CMake compiles the adapter; no helper request, GUI eligibility or
installed extlinux provider is added yet. Windows build/source is untouched.
Compact evidence is under parent `.rufus-test-artifacts/cp20j-qualified` and
`cp20j-asan`; completed test media and guest trees are removed.

**Next: CP20k.** Qualify Windows hidden/system/read-only file attributes on both
NTFS drivers and promote the native provider into the main CMake/package build.
Then integrate with the existing single-authorization helper lifecycle and GUI
selection in a separate checkpoint. Source-backed NTFS ISO support remains
separate from this standalone-v6 operation.

## CP20k — Windows file attributes and packaged provider

Completed 2026-09-29. Windows's `src/syslinux.c` creates `ldlinux.sys` with
read-only, hidden and system attributes, then copies the standalone v6
`ldlinux.c32` normally. The adapter applies that distinction, preserving other
flags and clearing NORMAL when protecting the loader. Both drivers expose the
four-byte, CPU-endian `system.ntfs_attrib` interface: see the
[NTFS-3G attribute documentation](https://github.com/tuxera/ntfs-3g/wiki/Using-Extended-Attributes#ntfs-attributes)
and [kernel NTFS implementation](https://github.com/torvalds/linux/blob/v7.2/fs/ntfs/ea.c).
No driver fork, POSIX-mode substitute or second privilege request is needed.

After provider success, the adapter rechecks mount identity, opens each expected
regular file without following symlinks and checks its device, size and link
count. It reads, updates and reads back the attributes before filesystem/device
flushes. Full payload verification checks the flags again after those flushes.
Unsupported attributes, missing data, failed or ignored writes and mismatched
readback fail the operation. Started failures/cancellation still flush.

The normal and ASan/UBSan/LSan workers each pass **59 cases per driver**. Eleven
new cases exercise attribute failures, short reads, unrelated-bit corruption,
post-flush readback and cancellation. Both CPU-endian and big-endian xattrs agree
before and after kernel read-only remount. Independent 7-Zip file records show
`RHS`/ordinary files for kernel NTFS and `RHSA`/`A` for NTFS-3G: the driver's
archive bit is preserved. Both final disks in each build boot to the bare prompt.

`rufus-extlinux` now comes from the main/shared CMake recipe, including builds
with tests disabled. It shares the existing generated v6 payloads and original
release source; the duplicate investigation-only target is removed.
`SyslinuxRuntime` installs both v6 executables and their shared license/notice.
Staging checks verify exact contents, ordinary permissions, binary identity and
version. The NTFS guests use the staged normal provider from a path containing
spaces. Existing v4/v6 package tests, including four FAT boots, still pass.

Evidence: parent `.rufus-test-artifacts/cp20k-final`, `cp20k-final-asan`, package
and build logs. Disposable disks, guest trees and staging trees are removed.
Qualification remains 512-byte sectors/4096-byte clusters on the two native
drivers; the upstream executable itself remains unsanitized. The known
ntfsprogs-plus jump-check limitation and original boot bytes are unchanged.

**Next: CP20l.** Connect explicit standalone v6/MBR/NTFS to the existing helper:
preflight the fixed provider before writes, format/mount/install in one job,
then unmount, reacquire/check and flush the disk before success. Qualify native
failure/cancellation/cleanup and original progress. GUI enablement follows;
source-backed NTFS ISO compatibility remains separate work.


## CP20l — standalone NTFS helper lifecycle

Completed 2026-09-29. The explicit `prepare-syslinux6-ntfs-device-v1` request
uses the existing one-authorized-helper job. It has no source, customization,
installer path or caller-supplied NTFS serial. v4/NTFS, source-backed NTFS
Syslinux, SFD and incompatible requests remain rejected.

The shared MBR planner accepts NTFS with the original Syslinux bootstrap, one
active type-07 partition and no UEFI:NTFS partition. Before writes, the helper
checks the selected fixed `rufus-extlinux` path, mkntfs and native mount support.
It reuses existing NTFS formatting and mount ownership, borrows the retained
partition/directory for the checked adapter, then performs the same cleanup as
other mounted jobs. That shared cleanup now explicitly flushes the reacquired
whole disk too, even after cancellation or an earlier error. Nothing is ejected.

The client expects mounting before bootloader work and the original two discrete
copy steps after verified installation. All started mount attempts finalize;
cleanup/sync failures remain failures, and a result alone cannot complete the
job before helper exit. The FUSE child stays owned by the same helper.

Normal and ASan/UBSan/LSan qualification passes **16 kernel-NTFS jobs and 15
NTFS-3G jobs per build**, followed by independent file/attribute/boot-record
readback and four bare BIOS boots. NTFS-3G is selected by omitting kernel NTFS
support from that guest, with no production override. The extra kernel case
injects a mount error and proves failure without switching to the available
fallback. Every case audits one authorization launch, process/fd cleanup,
disk-claim release, progress and exit ordering. The authorization launcher and
UDisks inventory are guest fixtures; actual block devices, mounts, formatter,
installer, helper and client are exercised in the offline VM.

The reused FAT Syslinux and FreeDOS helper matrices also pass after the shared
cleanup change. No Windows source or further customization qualification is
involved. Compact evidence is under parent `.rufus-test-artifacts/cp20l-*`;
disposable media and guest trees are removed, including failed setup attempts.
See the [helper recipe](../linux/tests/vm/README.md#standalone-syslinux-on-ntfs).

**Next: CP20m.** Expose standalone v6/MBR/NTFS in the Qt choice with the existing
Windows filesystem/default/cluster/progress behavior and fixed-provider
preflight before authorization. Keep v4 and image-backed NTFS eligibility
separate. No new host-setting detection or privilege path is needed.


## CP20m — standalone NTFS in Qt

Completed 2026-09-29. The native eligibility filter now permits NTFS for explicit
standalone v6/MBR. Qt reuses Rufus's filesystem ordering/default marker, existing
filesystem preference and 4/8/16/32/64-KiB cluster list (4 KiB default). v4 remains
FAT-only, and selecting it from v6/NTFS restores a supported FAT/default cluster.
The existing controller checks mkntfs and the fixed `rufus-extlinux` before and
after destructive confirmation, before authorization. FAT tools are unnecessary.

The mount preparation displays the same “Installing Syslinux 6.04...” status as
the bootloader phase. The original twenty-one NTFS progress slots and two copy
events produce positions 53052 and 56172 out of 65535; finalization remains at
62414 until clean helper exit. Short format results fail, as for other NTFS
format-only jobs. Successful READY remains full after unplugging.

Normal and ASan/UBSan/LSan focused Widgets runs each pass 74 results, alongside
format/request/window checks. Selection, installation and READY screenshots were
inspected. Compact evidence is in parent `.rufus-test-artifacts/cp20m-*`.
These are simulated Qt/protocol tests; CP20l remains the native helper and BIOS
qualification. Windows source/build, privileges and the native installer are
unchanged. Source-backed NTFS Syslinux and wider image-version support follow
separately, without inferring an unknown version from the user's filesystem choice.


## CP20n — ISO extraction and native NTFS installation

Completed 2026-09-29. One matching 6.04-pre1 ISO fixture now passes the shared
extractor followed by the unchanged upstream native installer on kernel NTFS
and NTFS-3G. Both outputs boot through the generated root redirect and copied
configuration to the existing BIOS marker, normally and with sanitized
extraction/format/scan code. This is a component sequence, not a newly enabled
helper or GUI operation.

| Boundary | Windows Rufus | Reuse decision |
| --- | --- | --- |
| Ordering | `src/format.c` calls `InstallSyslinux`, remounts, then `ExtractISO`. | Keep Linux's established FAT ordering: extract into an empty private root, then install. This preserves the extractor's empty-target contract and avoids admitting pre-existing boot files into general extraction. NTFS can install while the same private mount retains the device claim. The helper must still finish sync/unmount/claim checks. |
| Loader | `src/iso.c` skips root `ldlinux.sys`, protecting the newly installed/patched loader. | The shared extractor already skips it. The native installer creates the real loader after extraction; the source's dummy loader never reaches the target. |
| Companion | For image mode, Windows gets the companion from the source tree; the standalone-v6 branch separately copies its bundled companion. | Native extlinux always installs the pinned root companion. Reuse the existing source checker: a root companion that will be replaced must match exactly, while preserved companions follow the existing version policy. Do not broaden unknown-image inference. This fixture contains matching root and selected-directory companions. |
| Config search | Windows passes the selected config directory into the original patcher and applies shared backup/redirect rules during extraction. | Reuse `CheckNativeSyslinuxSource`'s comparison with native root lookup. The fixture covers the `boot/x86_64/loader` override, `syslinux.org` backup, root redirect and label patch; arbitrary conflicting lookup paths are not admitted by this probe. |

Two production gaps were found in the NTFS-3G extraction path and fixed locally:

- `config_patch.cpp` now checks the temporary file's owner/group/mode before
  changing them. NTFS-3G's fixed mount permissions already match but redundant
  metadata setters can return `EOPNOTSUPP`. Required changes still fail on error;
  no unsupported-operation error is ignored.
- `iso_extract.c` uses a hard link followed by unlink only when
  `RENAME_NOREPLACE` is unavailable (`EINVAL`, `ENOSYS`, `EOPNOTSUPP`). Link
  creation cannot overwrite a backup. Other rename errors stop immediately.
  If unlink fails, both names retain the original bytes and the operation fails;
  that incomplete target is not resumed. No check-then-overwriting rename is used.

The normal/sanitized component tests cover matching/required metadata,
metadata-stat failure, each unsupported rename response, existing backup,
link/unsupported-link/unlink errors, original-content preservation and cleanup.
The real NTFS guest uses rename; NTFS-3G uses one link and one unlink. Both
reject repeated extraction into the populated root before writing anything.
Independent NTFS readback verifies all eight expected copied/generated files,
all 136 mapped loader sectors, BPB/backup and disk gaps; all four configured
BIOS boots pass. Source ISO hashes remain unchanged. The known ntfsprogs-plus
jump check is normalized only on the disposable metadata-readback copy.

**Next: CP20o.** Extend the checked NTFS adapter to accept an explicitly validated
image job after extraction. Reuse retained-source identity/version/config checks,
reject an existing root loader, verify any replaced root companion, and retain
the current attribute/raw-payload verification and synchronization. Standalone
jobs must still require an empty root. Qualify that adapter before adding the
source-backed NTFS helper request and Qt eligibility. This checkpoint invokes
upstream extlinux directly after extraction; it does not qualify image-mode
helper lifecycle, provider attributes or all possible ISO/module combinations.
Evidence: parent `.rufus-test-artifacts/cp20n-*`; all generated media/guest trees
are removed. No Windows source, host privilege setup or customization test changes.


## CP20o — Checked NTFS image adapter

Completed 2026-09-29. `InstallNativeSyslinuxNtfs` now accepts explicit `BT_IMAGE`
requests with the retained file, pathname, selected ISO/UDF view, analysis and
Rock Ridge setting used for extraction. The existing shared source checker
compares version/build/payload and native/Windows config lookup; there is no new
NTFS version policy. Unknown detection only becomes a qualified inferred pair
through the existing exact companion check, never through selecting NTFS.

`BT_SYSLINUX_V6` remains source-free and requires an empty root. Image mode expects
successful checked extraction into a fresh private NTFS mount. It rejects any
existing root `ldlinux.sys`, including directories and dangling symlinks. Native
extlinux's root companion replacement is allowed only when the existing file is
ordinary, single-link, on the same device and byte-identical to the pinned module;
absence is allowed. Other copied files remain the caller's extraction/readback
responsibility. This is not an API for arbitrary existing volumes.

The source is checked before compatibility checks, before the installer starts
and after final verification. The established attribute, mapped-file, VBR/backup,
flush and child-reaping path serves both modes. The caller still owns the mount,
device claim, source-on-device checks, extraction and final unmount/reacquisition
before READY. No privilege path, copy engine or installer algorithm was added.

Native geometry now describes FAT and NTFS directly: 512-byte sectors, FAT's
32-bit mtools byte offset, NTFS's 32-bit hidden-sector field. An explicit FAT-only
guard prevents the FAT adapter from accidentally accepting an NTFS selection.
The public provider lookup, helper request and Qt image choices remain closed
for NTFS image jobs until lifecycle integration is qualified.

Qualification uses `syslinux_ntfs.py --adapter --iso`: 83 cases per driver,
including 24 image-specific cases plus the existing 59-case adapter matrix.
The populated-root case must now succeed without changing the unrelated tree;
in standalone mode it still returns `ENOTEMPTY`. Missing source fields, unsupported
selection/version, incomplete/wrong analysis, stale source before/between/after
installation, reserved paths, root companion type/link/size/content and matching
inferred-pair cases all pass. Failures preserve borrowed descriptor state, reap
children and perform the expected syncs. Readback verifies all eight extracted
or generated files, both installed payloads, Windows attributes, 136 mapped
loader sectors, BPB/backup, disk gaps and clean read-only remount. Four image BIOS
boots reach the configured marker; four standalone regression boots reach the
bare prompt, across the two drivers and normal/sanitized builds.

Shared NTFS source checks pass 39 layout cases, 8 faults, a symlink and 2 UDF-view
cases per build. FAT source/adapter and eligibility regressions also pass. This
still qualifies one native image layout, 512-byte sectors and 4-KiB clusters;
it does not claim arbitrary module compatibility, physical firmware coverage or
a complete source-backed helper lifecycle. No Windows customization test was
added. Temporary media/guest trees are removed; evidence is in parent
`.rufus-test-artifacts/cp20o-*`.

**Next: CP20p.** Integrate this contract into the existing authorized NTFS helper
and qualify its full extraction/progress/cancellation/cleanup flow before Qt.


## CP20p — Complete NTFS image helper lifecycle

Completed 2026-09-29. A source-bearing NTFS Syslinux operation now runs through
the existing client and single authorized helper. It requires the retained source
identity, uses the same preflight as FAT and accepts neither provider paths nor
installer versions from the request. Native MBR/NTFS acquisition and formatting
now admit this explicit image selection as well as standalone v6; v4 stays
FAT-only. The common provider lookup supplies the packaged extlinux executable.

The helper keeps the private mount and partition descriptor through extraction,
installation, attribute/raw-payload verification and error-path sync. It then
uses the existing unmount/FUSE wait, whole-disk reacquisition/identity checks and
source recheck before returning. Successful completion still waits for helper
exit. Extracting is the image copy phase through installation; only the actual
shared extractor supplies its block counters. No standalone copy steps are
injected into that denominator. The GUI's image selection remains closed here.

The existing mounted-copy body is now `copyImage`, used by ordinary ISO jobs
and this NTFS route. It owns the same shared extraction, source checks, copied
byte count and optional Windows customization call. Mount ownership and final
cleanup remain with each caller. No new copy engine, installer implementation or
authorization mechanism was added.

The complete image guest runs 25 kernel-driver and 24 absent-kernel fallback jobs
per build, normally and under ASan/UBSan/LSan. It verifies exactly one launch,
monotonic copy counts, no image-mode discrete steps, expected phases, source-byte
preservation, unchanged whole disks on preflight failures, released claims,
removed private mounts, no leaked client descriptors/children, and completion
after the held launcher exits. Cases include missing native tools, stale identity,
companion/config mismatch, extraction/installer failure and cancellation, source
changes after copy/installation, no-op/corrupt providers, attribute/read/sync
errors, unmount/removal failures, and known/inferred compatible successes.
A kernel mount error remains an error even when NTFS-3G is available.

All four image outputs pass independent NTFS file/attribute/BPB/backup/MBR/gap
readback and configured BIOS handoff. Normal standalone v6 regressions pass 31
jobs and two bare boots; the existing FAT image helper passes five jobs and its
configured boot. Native coverage remains 512-byte sectors/4-KiB clusters with
the generated v6 image, not every image or physical firmware. Logs/manifests and
readback evidence are under parent `.rufus-test-artifacts/cp20p-*`; all generated
media/guest trees are removed. No additional Windows customization test was run.

**Remaining layout boundary:** Windows's shared extra-partition rule requires
UEFI:NTFS for an EFI-capable ISO on NTFS, even when Syslinux supplies BIOS boot.
CP20p checks and rejects that combination before writes. CP20q must connect the
existing embedded payload and MBR extra-partition path and qualify both boots
before Qt enablement. Do not silently accept the image as BIOS-only or reinterpret
an unknown version from the filesystem selection.

## CP20q — Required UEFI:NTFS partition on Syslinux image jobs

The source-backed request now supports the original extra-partition policy.
`prepare-ntfs-syslinux-uefi-device-v1` explicitly requests the embedded loader
alongside the Syslinux image. The existing `uefiNtfs` request flag represents
this layout as well as standalone UEFI:NTFS; `usesUefiNtfs()` feeds the existing
layout, formatter and client phase handling. The helper recomputes
`GetExtraPartitions` from the retained image and rejects both missing required
partitions and unnecessary additions before writes.

The existing MBR layout now admits NTFS/Syslinux with UEFI:NTFS. It retains
Rufus's main-partition active flag, original Syslinux MBR and random disk
signature. The helper passes those same values to the existing checked embedded
payload writer before mounting/copying the ISO. There is no new partitioner,
bootloader payload, copy engine, authorization path or production progress
protocol. Standalone Syslinux still has no extra partition.

The additional firmware path uses the existing x64 fallback-loader validation;
a malformed or missing `/EFI/BOOT/BOOTX64.EFI` fails before formatting. This is
qualification of the current x64 route, not full EFI-architecture parity. BIOS
and UEFI use the same generated USB output, and the original UEFI:NTFS binary
loads the copied EFI application from the main NTFS partition. Secure Boot and
physical firmware compatibility are not established by that fixture.

Qt eligibility remains closed until CP20r. Its request preparation, preflight,
progress, confirmation and completion code must treat `syslinux && uefiNtfs` as
an ISO job, rather than a standalone loader job. Compute that flag from shared
Rufus image policy, retain the ISO/DD dialog and FAT preference, and validate
filesystem/layout changes before enabling START.


## CP20r — Qt NTFS image selection and completion

The Qt image route now offers NTFS alongside FAT16/FAT32 when shared Rufus policy
and native geometry allow it. FAT32 remains the image preference; selecting NTFS
does not infer or substitute a Syslinux version. ISOHybrid sources retain the
original ISO-recommended/optional-DD choice. DD does not need the native installer
or formatter.

`GetExtraPartitions` selects the image's UEFI:NTFS flag before preflight and
freezes it with the job. Native cluster choices use the same layout reservation
as the helper. `isStandaloneUefiNtfs()` prevents an image with that flag from
entering standalone-only preflight, confirmation, progress or completion paths.
There is no new transport, writer, payload, mount path or progress algorithm.

The GUI retains the image path, checks the selected installer and formatter
before authorization, and uses the original NTFS image progress weighting:
16% preparation, 81% copy and three finalization slots. Halfway through extraction
it displays `Copying ISO files: 50.0%` with the overall bar at 56.5%; the first
finalization slot is 98%. Successful READY still requires cleanup and helper exit.
Short copy, wrong layout, sync/cleanup errors and cancellation cannot report
success. The check now shares formatter availability across source-free and ISO
jobs, including the second check after user confirmation.

CP20r's functional GUI tests use generated ISOs, private D-Bus, simulated device
facts and a protocol helper fixture. They complement CP20p/q's native helper,
readback and firmware qualification; they are not additional real-device or
firmware runs. Other image versions, EFI architectures and geometry qualification
remain pending. No further Windows customization installation test was added.
