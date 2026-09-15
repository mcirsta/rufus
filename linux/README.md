# Rufus Linux components

The Linux port uses **Qt6 Widgets** and follows Rufus's original Win32 dialog
layout, labels, control order and default visibility. The form uses standard
widgets, layouts and the desktop palette. Keep device/media logic in jobs and controllers.
[Qt Widgets documentation](https://doc.qt.io/qt-6/qtwidgets-index.html).

See the [Linux port implementation plan](../docs/linux-port-plan.md) for the
agreed architecture, reuse strategy and implementation phases.
The [testing and checkpoint plan](../docs/testing-and-checkpoints.md) defines
small delivery steps and how to test without physical USB drives.

CP2 adds read-only UDisks2 discovery to the Widgets shell, selection controller,
reporting component and functional tests. CP3b extracts shared post-scan image
policy and report definitions into `src/image_policy.c` and `src/image_report.h`;
the Windows caller now uses that policy. Portable C tests run with the Linux
suite and are available as standalone Windows tests. Read-only Qt image analysis
is connected in CP3d; CP8b later enables the narrow raw `.img` GUI workflow.
CP3c1 also extracts per-file classification
into `src/image_file.c`, with portable unit tests and a separate generated-ISO
fixture probe. See the [shared boundary](../docs/linux-image-analysis-boundary.md)
and [native Linux reuse choices](../docs/linux-native-reuse.md).

CP3c2 adds a shared read-only ISO9660 tree scanner and Rufus's existing string
array to the Linux build. Generated-image tests exercise this scanner directly.
CP3c3 also shares image-file buffer reads, preserving UDF preference and ISO
name options. CP3c4 shares Syslinux version and suffix recognition, including
tests through that reader. CP3c5 shares GRUB version/build and filesystem
inspection, including the existing label normalization. CP3c6 shares Shim and
systemd-boot banner diagnostics. CP3c7 shares config selection and Syslinux scan
policy, separating presence/detection from the selected installation version.
CP3c8 combines these stages into one shared ISO9660 metadata operation, with
label/size finalization and explicit complete, partial, failed and cancelled
results. CP3d connects this operation to SELECT with a concrete Qt worker,
cooperative cancellation and stale-result rejection. CP3e adds shared UDF traversal
and actual UDF image fixtures through that job. CP3f1/CP3f2 add embedded EFI/FAT
presence inspection through ISO9660 and UDF using Rufus's existing FAT reader.
CP3g1 shares WinPE options inspection. CP3g2 adds a native ordinary-WIM metadata
component and shares Rufus's Windows version normalization. CP3g3a shares the
existing EFI range reader, CP3g3b qualifies the upstream wimlib extension, and
CP3g3c connects Windows installation metadata to SELECT. CP3g3d qualifies the
unchanged reader extension against the upstream 1.14.5 release. CP3g4a adds native
single-file WIM extraction (16 MiB maximum) and shared PE architecture recognition.
CP3g4b shares standalone WIM/ESD signature recognition and qualifies retained
native-file ownership and Windows-compatible WIM lookup. That SELECT integration
is deferred while the first raw writing workflow is completed. CP5 supplies a
native copy/flush/verify operation tested on disposable files. CP4/CP7 add a raw
write plan and native authorized-descriptor handoff. CP8a connects the Widgets
workflow and worker. CP8b qualifies native USB storage and enables this narrow
raw-image workflow; CP9 boots the generated BIOS fixture in a separate VM.
CP9b qualifies real OpenWrt BIOS/UEFI raw images. CP10a/CP10b share ISOHybrid
classification and mode choices and test the original dialog in Qt. CP10c adds
ordinary ISO/UDF tree extraction into disposable directories. CP10d adds native
FAT32 formatting of a region in a disposable image. CP10e creates a GPT main
partition with shared Rufus placement arithmetic and formats that actual extent.
CP10f qualifies the extractor on mounted FAT32 in a disposable Linux USB guest,
including real filesystem failures and independent readback.
CP10g shares Rufus's filename substitutions with Windows and applies them to
Linux output paths while preserving original names for image reads.
CP10h replaces earlier colliding files and reuses directories as Rufus does,
while checking target types before changing existing entries.
ISO START remains gated until device-service integration and boot support are ready.
Secure Boot analysis
remains a separate increment; see the
[Windows-image boundary](../docs/linux-windows-image-metadata.md).
Windows validation is deferred to a
real Windows environment, as requested; it does not block Linux development.

## Build and test

The Linux port is strictly CMake-based, including dependencies compiled as part
of the port. Ninja is the preferred generated backend. The Windows application
continues to use Rufus's existing Visual Studio/MSBuild and MinGW/Autotools
builds; `linux/CMakeLists.txt` is the Linux entry point.

Requires CMake 3.21+, a C compiler, a C++17 compiler, Qt6 Core/Widgets/DBus 6.2+,
pkg-config and libfdisk 2.35+ development files (`libfdisk-dev` on Debian/Ubuntu),
`patch`, and the [upstream wimlib 1.14.5 source release](../docs/linux-wimlib-prototype.md#reproduction).
The application uses the extended upstream library for ordinary and embedded WIMs.
System wimlib development files are needed only for optional comparison tests
(`libwim-dev` on Debian/Ubuntu, `wimlib` on Arch).
Tests additionally require Qt6 Test, `dbus-daemon`, Python 3 and libarchive's
`bsdtar`, plus `genisoimage` or cdrtools `mkisofs` for UDF fixtures. FAT fixtures
require `mkfs.fat` from dosfstools and `mcopy`/`mmd` from mtools. Set
`RUFUS_MKISOFS`, `RUFUS_MKFS_FAT`, `RUFUS_FSCK_FAT`, `RUFUS_MDIR`, `RUFUS_SFDISK`, `RUFUS_MCOPY`
or `RUFUS_MMD` to absolute
executable paths when they are outside PATH. Image generators, checkers and
mtools are test dependencies; `mkfs.fat` also runs the native FAT32 formatting
component. The image reader requires native iconv and 64-bit file offsets; CMake
checks its platform features. UDisks2 must be available
on the system bus for real discovery. From the repository root:

```sh
cmake -S linux -B /tmp/rufus-linux-qt6-build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DRUFUS_WIMLIB_UPSTREAM=/tmp/rufus-cp3g3d-upstream/wimlib-1.14.5
cmake --build /tmp/rufus-linux-qt6-build
ctest --test-dir /tmp/rufus-linux-qt6-build --output-on-failure
```

Run the preview from a desktop session:

```sh
/tmp/rufus-linux-qt6-build/rufus-linux
```

Ninja chooses its default parallelism. `BUILD_TESTING=OFF` omits tests. Set both
`BUILD_TESTING=OFF` and `RUFUS_BUILD_GUI=OFF` to build only the Qt Core reporting
library. CTest runs Widgets offscreen and creates private D-Bus buses with
separate fixture processes; no host bus or device access is performed. The
test environment must allow local Unix sockets. No elevated privileges are needed.
Use `ctest --test-dir /tmp/rufus-linux-qt6-build --output-on-failure -L no-hardware`
to select tests that need neither hardware nor elevated privileges.

## Native raw writing component

`rufus_raw_write` supplies `WriteRawImage`: copy a retained source to an existing
regular file, flush, and compare the complete written range. It uses native
POSIX I/O and the existing source checks, with 2 MiB of buffers. Both descriptors
and offsets remain with the caller, the target tail is preserved, and failures
retain partial counts and any flush error. Cancellation still flushes written data.

`rufus_raw_target` retains an uncompressed `.img` source and the selected UDisks
identity. The first plan requires whole 512-byte sectors and the `55 AA` boot
marker; broader raw/container recognition remains deferred. It rejects mounted,
swap and known layered targets, source-on-target storage and changed selections.

`UDisksTarget` separates checking from authorization so a controller can confirm
the plan first. It requests native `OpenDevice("rw")` access with exclusive,
synchronous flags and interactive authentication allowed. The desktop's polkit
agent owns any password prompt. Fresh snapshots before and after authorization
and kernel descriptor checks precede ownership transfer. The block entry point,
`WriteRawImageToDevice`, uses the same copy/flush/verify loop and validates the
descriptor again. See [UDisks block operations](https://storaged.org/doc/udisks2-api/latest/gdbus-org.freedesktop.UDisks2.Block.html).

Tests use a private D-Bus, disposable files and test-only simulated kernel facts.
The optional [USB guest fixture](tests/vm/README.md) also tests native prompts,
kernel exclusivity, cancellation, unplugging and a separate BIOS boot. `rufus_qt_write` connects a concrete controller and `RawWriteJob`
to the existing form. SELECT recognizes this narrow `.img` path; START prepares
a fresh plan, requests confirmation, acquires the descriptor and starts the worker.
The existing C reporting functions deliver throttled copy/flush/verify updates.
Only complete verification and worker cleanup produce 100%/READY. Cancellation
still flushes written bytes, and logs retain partial counts and flush failures.

The application now supplies the system bus to `controller.writer()`, enabling
START for an eligible selected USB device and the supported raw `.img` input.
Polkit owns authentication; the GUI never receives a password or runs as root.
Normal functional tests still explicitly supply a private bus. The optional
CMake target `test_usb_guest` runs only in the guarded disposable VM and has no
simulated block calls. See its [recipe and limits](tests/vm/README.md).
Native tests cover full copy/flush/read-back, exclusive access, cancellation,
removal and post-write partition discovery. UDisks may change the medium's ID
when the new partition table appears, so the current UI conservatively clears
the selection and requires selecting the device again. It preserves the completed
operation's result. Automatic unmounting and physical USB qualification remain.

```sh
ctest --test-dir /tmp/rufus-linux-qt6-build -R 'rufus_raw_write|rufus_udisks_target' --output-on-failure
```

See the [checkpoint records](../docs/testing-and-checkpoints.md#cp5-raw-copy-flush-and-verification-on-disposable-files-2026-09-14).

## Native ISO/UDF extraction component

`rufus_iso_extract` supplies `ExtractIsoTree` for a caller-owned, private, empty
directory. It uses the shared `WalkIsoFiles`/`WalkUdfFiles` traversal and file
readers with Rufus's UDF-first and Rock Ridge/Joliet selection. The existing
root `ldlinux.sys` exclusion is shared with Windows; a nested loader is preserved.

The component retains and rechecks the source, validates the tree before writing,
creates output relative to the retained directory, and flushes files and
directories. Later colliding files replace earlier bytes; colliding directories
are reused. Existing files are checked as singly linked regular files before
truncation, and existing directories must open without following symlinks. Data
copying uses a fixed 1 MiB buffer plus the reader's existing 32 KiB cache. On a
file failure it removes the current created/truncated file; there is no rollback
of a replaced file. Other entries remain and the whole output must be treated
as incomplete. Links, special entries and incomplete/deep trees are
currently rejected. Output paths use Rufus's shared replacements for `* ? < > : |`,
including parent directories and failed-file cleanup. Source paths keep their
original names. Native filesystem lookup determines aliases such as FAT case
collisions. Counts describe processed source entries and bytes, including those
later replaced. Remaining filesystem restrictions, timestamps, config patching,
payload replacement and bootloader installation remain separate work.

The functional test generates real images and independently compares names,
directories and complete contents. It also exercises cancellation/retry, source
changes, shorter/empty replacements, directory merging, target-type conflicts,
short I/O, failed reads/writes/truncation/flushes/closes and descriptor cleanup.
No device, mount, root access or GUI writer is involved.

```sh
ctest --test-dir /tmp/rufus-linux-qt6-build -R '^rufus_iso_extract$' --output-on-failure
```

See [CP10c](../docs/testing-and-checkpoints.md#cp10c-native-isoudf-tree-extraction-2026-09-15).
The optional [FAT32 guest recipe](tests/vm/README.md#mounted-fat32-extraction)
also checks the component on an actual kernel-mounted filesystem. It uses the
same CMake probes and a disposable regular-file USB disk, without host mounts.

## Native FAT32 formatting component

`rufus_fat_format` supplies `FormatFat32Image`, a synchronous worker operation
for one region of a caller-owned regular file. It invokes a trusted absolute
`mkfs.fat` path with structured arguments and passes the retained descriptor to
the child. Offset, size, cluster size, volume ID and an already normalized ASCII
label are explicit. The current subset uses 512-byte sectors and cluster sizes
from 512 through 32768 bytes, with sufficient space for a standards-sized FAT32.
The dosfstools dependency must support `--offset` and `--mbr`; 4.2 is tested.

Offset and length are sector-aligned, including odd numbers of sectors. Since
mkfs.fat's explicit length uses whole KiB blocks, odd lengths skip its additional
track rounding; the final filesystem may retain or omit the last sector but
must fit inside the partition. The partition itself is never shortened to
accommodate the formatter. The adapter bounds diagnostics, handles cancellation and a 120-second
process timeout, terminates/reaps the child, and flushes even after a started
operation fails. Successful completion additionally requires unchanged target
size, the requested boot-sector fields and a readable root through Rufus's
existing FAT reader. dosfstools can round the volume down by less than one
63-sector track; the result reports the actual filesystem size. This is quick
formatting, without erasing old data or installing boot code.

Tests use guarded sparse images, inspect both FATs and backup metadata, run
`fsck.fat -n` on the extracted volume and list the original image with mtools.
They also check invalid requests, descriptor retention, failed tools/flushes,
cancellation, timeout and child cleanup. The component rejects block devices;
partition creation, native device authorization/formatting and mounting remain
separate work. The GUI remains unprivileged and receives no password.

```sh
ctest --test-dir /tmp/rufus-linux-qt6-build -R '^rufus_fat_format$' --output-on-failure
```

See [CP10d](../docs/testing-and-checkpoints.md#cp10d-native-fat32-formatting-in-image-files-2026-09-15).

## Native GPT image preparation

`rufus_partition_image` supplies `CreateGptImagePartition` for an existing,
exclusively borrowed regular file. Rufus's `GetPartitionStartOffset` and
`GetMainPartitionSize` now live in shared `src/partition_layout.c`; the existing
Windows `CreatePartition` calls them too. Linux delegates GPT serialization,
CRCs and identifiers to libfdisk. The initial layout keeps Rufus's 1 MiB start,
track-then-cluster rounding, backup-GPT reservation, Microsoft Basic Data type
and `Main Data Partition` name. It does not automatically choose an ESP type.

The adapter builds the table in memory, rechecks the expected image size before
writing, flushes even after an attempted write fails or is cancelled, and reads
the table back through a fresh library context. Recovery from a damaged GPT copy
is treated as failed write verification. Descriptor ownership and seek position
remain with the caller. No block device, kernel partition refresh or mount is used.

Functional tests independently decode primary/backup headers, entries and CRCs,
inspect the protective MBR and use `sfdisk` for an external table check. They
format the actual reported partition extent, verify both GPT copies are
unchanged, then inspect FAT32 with fsck and mtools. Cancellation, failed writes,
failed flushes, changed size, damaged table copies and descriptor cleanup are
covered. Tests require `sfdisk` from util-linux (`fdisk` package on Debian/Ubuntu).

```sh
ctest --test-dir /tmp/rufus-linux-qt6-build -R 'rufus_partition_layout|rufus_partition_image|rufus_fat_format' --output-on-failure
```

See [CP10e](../docs/testing-and-checkpoints.md#cp10e-shared-placement-and-native-gpt-images-2026-09-15).

## Shared ISO tree scan

`rufus_iso_scan` builds `src/iso_scan.c`, `src/iso_read.c`, `src/image_file.c`
and `src/strarray.c`, plus shared Syslinux version/selection, GRUB inspection and
EFI banner reporting code, plus `src/iso_analyze.c`, `src/iso_bootloaders.c` and
`src/embedded_efi.c`, `src/iso_file.c` and `src/winpe_scan.c`. The main Linux suite has
46 tests, or 47 with the optional system-wimlib comparator.
It links the bundled reader from `linux/cdio/`, using native stdio/iconv, and
the existing libfat sources through the `bundled_fat` target. The
comparison probe also uses this one library definition. CMake checks headers,
functions, time structures, byte order, iconv's signature and 64-bit `off_t`.
`DO_NOT_WANT_COMPATIBILITY` and `_FILE_OFFSET_BITS=64` propagate to consumers;
do not mix these headers with a system libcdio binary. Optical-device entry
points are disabled as in the original Rufus image path.

`ScanIsoFiles` takes an open ISO handle and concrete `ISO_SCAN_STATE` containing
caller-owned report/path arrays and scan options. It reads directory entries,
uses shared file rules, records original-case config/isolinux paths and counts
rounded 2048-byte blocks. Rock Ridge long names, firmware-symlink space allowance,
`live` to `casper` handling and deep-directory short circuits retain Rufus's
rules. `ScanUdfFiles` uses the same file accounting and consumes its UDF
directory iterator on every return. UDF parent entries are skipped explicitly;
an unreadable child or directory entry fails the scan. The Windows caller now
uses both shared scanners and keeps progress setup, extraction and platform-specific metadata
in `iso.c`. Its label/size and bootloader-content finalization now call shared
functions.

The caller supplies `IsIsoScanCancelled()`, a direct C entry point checked
between directory reads and entries. Windows queries its existing error status;
the test executable provides deterministic cancellation points. The Qt host
queries `QThread::isInterruptionRequested()` directly from the worker. This cannot interrupt an in-progress
library I/O call. No GUI handles or callback registry enter shared code.

A zero return means traversal finished, subject to `has_deep_directories`:
that flag and the negative deep-directory return mark a partial tree. Positive
returns mean error or cancellation; discard partial findings in those cases.
A complete tree is still a **partial image analysis**. It has no bootloader
content inspection, final media policy or bootability validation. Overlong tree
paths fail explicitly at the current 260-byte limit. Path-list allocation
failure also fails the scan instead of silently dropping required paths.

Run just this checkpoint's tests:

```sh
ctest --test-dir /tmp/rufus-linux-qt6-build -R 'rufus_iso_tree|rufus_strarray' --output-on-failure
```

The generated fixtures include a 39-file tree, mixed-case EFI and Windows paths,
config/isolinux array growth, old module thresholds, block rounding, firmware and
directory symlinks, RR/Joliet/plain names, long paths, deep directories, ordinary
invalid files and cancellation. `test_strarray` injects a failed `realloc` at
link time to check ownership and recovery in the actual shared implementation.
Both tests run with no devices, mounts or privileges. The test-only driver can
also inspect a regular ISO file with `iso_scan_probe IMAGE rr` (or `joliet` /
`plain`); its output explicitly distinguishes complete/partial trees and partial
image analysis. Existing bundled compiler warnings remain visible.

## Shared UDF/ISO9660 analysis operation

`AnalyzeIsoImage` takes an image path, its host-validated regular-file size and
concrete `ISO_ANALYSIS_OPTIONS`. It owns the reader handles, temporary paths and
buffers; its `ISO_ANALYSIS` result contains a report and counters with no heap
ownership. The existing direct C cancellation query and logging entry points
remain the only host hooks. There is no function-pointer or Qt callback layer.

The operation selects Rufus's initial Rock Ridge/Joliet view, scans the tree,
finalizes label/size data and runs shared Syslinux, embedded EFI/FAT, WinPE, BIOS GRUB
and EFI content inspection in Rufus's existing order. The embedded FAT stage
uses the selected UDF or ISO9660 view, with no fallback between them.
Content reads use the same name mask as
its ISO9660 tree. UDF is preferred for both tree and content. Only a failed UDF
open falls back to ISO9660; an unreadable UDF root fails explicitly. Windows scan
mode now follows this rule too, replacing its former fallback after root failure.
Extraction keeps its existing platform implementation.

The result identifies the selected `filesystem`. For UDF, a companion ISO view
supplies PVD size checks only; the scanner never traverses that view. Without a
PVD, `volume_size_known` is false and mismatch remains zero, which is not a
successful size check. Native UDF revision/allocation-layout coverage remains
limited to the tested fixtures.

Return values use `ISO_SCAN_OK`, `ISO_SCAN_PARTIAL`, `ISO_SCAN_ERROR` and
`ISO_SCAN_CANCELLED`. Failure/cancellation clears the result. Partial results
retain evidence plus `pending` bits for deep directories, unreadable
content, embedded EFI/FAT, Windows metadata, Secure Boot and a PVD size larger
than the file. Deep trees have no completed-tree flag, projected size or selected
bootloader version. An unknown successfully read version is an inspection result,
not a read failure; fallback origin remains explicit.

`ISO_SCAN_OK` means **completed UDF/ISO9660 metadata**, not a validated installation
plan. Raw/hybrid detection and final `ApplyIsoPolicy` media decisions are still
outside this operation, as are installer compatibility and boot tests. Pending
Secure Boot checks must never be interpreted as an unsigned/trusted verdict.
Individual library reads are synchronous; cancellation happens between them.
The existing reader's zero-length return groups empty files, read errors, size
limits and buffer allocation failures into unavailable content.

`FinalizeIsoScan` preserves original trailing-label trimming, signed PVD/file
size difference and the exact float-based 1% projected-size allowance. It checks
arithmetic limits. Firmware symlink extra blocks stay separate, as in Rufus.
`ScanSyslinuxBootloaders` and `ScanGrubAndEfiBootloaders` are shared with Windows;
Windows keeps its optional-read behavior while storage errors/cancellation fail
the scan. Neither the Win32 nor Qt form changes in this checkpoint.

```sh
ctest --test-dir /tmp/rufus-linux-qt6-build -R rufus_iso_analysis --output-on-failure
/tmp/rufus-linux-qt6-build/iso_analyze_probe IMAGE.iso rr none
```

The test-only probe returns 0/1/2/3 for complete/error/cancelled/partial. Twenty-two
disposable image/plain-file fixtures cover combined content, naming views,
unknown versions, unavailable fallbacks, pending stages, path limits and sizes.
Injected read/storage failures and cancellation check cleanup. A 49-check C
suite covers finalization, invalid arguments and UDF dispatch with API doubles;
actual UDF decoding is covered separately below. All twenty-six Linux suites pass normally
and with address, undefined-behavior and leak checks. See the
[checkpoint record](../docs/testing-and-checkpoints.md#cp3c8-shared-iso9660-finalization-and-coordinator-2026-09-14).

## SELECT and the Qt analysis job

SELECT now starts `ImageAnalysisJob`, a concrete `RufusJob` implementation on a
worker thread. `SelectionController` receives an owned `ImageAnalysisResult`
through a queued signal; it never interprets `finished()` as success. Each result
and log message carries a monotonically increasing selection request number.
Replacing a selection invalidates queued reports, requests interruption and
keeps only the latest replacement until the previous thread has stopped. Even
reselecting the same path gets a new request number.

The shared C code calls the existing reporting entry points and the direct
`IsIsoScanCancelled()` query. The job temporarily routes libcdio's own logging
API through that existing reporting path, restoring its handler/level on return
or exception. The process-wide RufusJob guard still rejects overlapping legacy
operations. The controller owns the thread and joins it on teardown; closing the
log window does not affect the operation.

The form uses Win32's analysis behavior: an indeterminate progress bar,
CLOSE changing to CANCEL, and disabled device/image/advanced selection controls.
The log remains available. Cancel or closing the main window during a scan
requests interruption and keeps the window open; after cleanup, CLOSE exits
normally. Cancellation is checked between synchronous library operations, so it
cannot impose a deadline on an in-progress read. Unexpected application teardown
joins the worker and may wait for that read.

Completed tree labels populate the existing Volume label field. Status and log
text distinguish completed UDF or ISO9660 metadata, partial analysis, cancellation and
failure. Partial reasons stay visible, including pending Secure Boot checks.
For these analysis-only inputs, START and media settings remain disabled; no new form controls or layout
changes are introduced. Device discovery is independent of read-only scanning.

Qt resources contain Rufus's existing v4/v6 Syslinux payloads as data. The shared
version parser obtains their fallback versions; no version numbers are hardcoded
or taken from the host's installer. An unknown image version stays unknown when
a fallback is inferred, and the log states that native compatibility is unchecked.
The job checks the selected file's type, size and modification time before and
after analysis. This is not a pinned image snapshot or write-time validation;
changes after completion require a rescan. Raw/hybrid detection and final media
policy are still outside this path.

```sh
ctest --test-dir /tmp/rufus-linux-qt6-build -R rufus_image_analysis_widgets --output-on-failure
```

Twenty-one Qt functional cases/data rows use real generated UDF/ISO9660 files, Widgets and the
shared C scanner. Link-time test gates exercise in-progress cancellation, a
successful result already queued after thread exit, rapid replacement, same-path
reselection, invalid replacements, file changes, exception/overlap recovery and
teardown. Embedded FAT presence, absence and unreadable results reach the same
controls. WinPE options use those controls for both filesystem views, including
unreadable results and cancellation. Both bundled Syslinux fallback families are checked. No production job
factory, test callback or simulation mode is added. Native desktop dialogs,
Windows rendering and real distribution boot behavior remain qualification work.

## Shared image-file contents

`ReadISOFileToBufferExt` in `src/iso_read.c` accepts image/internal paths and the
ISO extension mask. It preserves Rufus's UDF-first lookup: only a failed UDF
open tries ISO9660; missing UDF roots/files do not switch filesystem views.
Callers should use name options matching their scan when reading recorded paths.
The original Windows `ReadISOFileToBuffer` signature remains in `iso.c`, where
its small wrapper supplies preferences and retains log-level setup/restoration.
The shared reader has no GUI, Windows headers or application-global settings.

Successful reads return the original byte count and a `malloc`-owned buffer,
with an extra NUL byte at that count. Callers must `free` it. Binary data,
including embedded NULs, is preserved. The original limit is inclusive **1 GiB**.
Empty files, errors and oversize files return zero with a null buffer, preserving
the existing empty/error ambiguity. Directories are now rejected explicitly;
64-bit lengths are checked before narrowing/allocation. Each read is synchronous,
using the existing bulk libcdio call, without a new cancellation interface.

The `rufus_iso_contents` fixture compares entire byte streams, including final
partial blocks, RR/Joliet/plain names, an explicit eight-sector El Torito entry,
empty files and invalid paths/images. A fixture ending partway through file data
must return no buffer. `rufus_iso_read_contract` adds 30 scenarios and argument checks
using API doubles: UDF preference, cleanup, short/error reads, allocation failure,
the size boundary and recovery. Its linker rejects the test's 1 GiB allocation;
no large buffer or image is created. The UDF fixture suite below adds real disk
parsing. Run the ISO byte and API-contract tests with:

```sh
ctest --test-dir /tmp/rufus-linux-qt6-build -R 'rufus_iso_contents|rufus_iso_read_contract' --output-on-failure
```

## Real UDF fixtures

`rufus_udf_images` generates thirteen disposable UDF images with a native
`genisoimage` or `mkisofs`. They exercise exact binary reads, Unicode paths,
multiple directory sectors, nested directories, block rounding, Syslinux/GRUB/EFI
content, unknown versions, pending Windows/embedded-EFI checks and path limits.
A valid renamed UDF entry (CRC/checksum repaired) creates a different UDF view
from the companion ISO tree. Missing UDF files must not fall back to ISO content.
A copy with its ISO descriptors removed checks UDF operation without a PVD.

Read/storage failures and cancellation use link-time test gates. Seventeen reader
checks on a generated image cover short sector/file-entry reads, allocation
failure, directory bounds and cleanup. The native reader now requires full
sector reads, checks directory allocation/record boundaries, handles little-endian
UDF lengths and converts compressed 8-bit Unicode names to UTF-8.
The two additional Qt cases check UDF reports, pending Windows metadata,
cancellation after scanning and restart. No passwords, devices or mounts are used.

```sh
ctest --test-dir /tmp/rufus-linux-qt6-build -R 'rufus_udf_images|rufus_image_analysis_widgets' --output-on-failure
```

Both genisoimage 1.1.11 and cdrtools mkisofs 3.02a09 passed these UDF fixtures
locally. The Linux workflow installs genisoimage; the application needs neither
generator. These checks qualify generated metadata fixtures, not every UDF
revision, multi-extent payload, distribution image, native Windows run or boot.
See [CP3e](../docs/testing-and-checkpoints.md#cp3e-shared-udf-traversal-and-real-image-fixtures-2026-09-14).

## Shared embedded EFI/FAT inspection

`InspectEfiImage` in `src/embedded_efi.c` borrows the selected filesystem handle
and an embedded-image path. Exactly one of its ISO9660/UDF arguments must be
non-null; it never falls back between views. It separates Rufus's EFI
directory/name search from Windows `iso.c`; `HasEfiImgBootLoaders` remains a
wrapper there, now selecting UDF when present. Windows extraction keeps its
original FAT callback. The Linux coordinator calls the
shared inspection after Syslinux content checks, preserving the original
conditions that skip this stage when ordinary EFI/repair markers already exist.

The implementation reuses libfat's open/cache/directory/FAT-chain code and its
existing storage callback. A 32 KiB cache uses direct ISO sector reads or the
bundled UDF seek/block APIs, clamping read-ahead to the candidate file. It never
buffers the whole FAT image. The first
active MBR FAT partition and ISO-alignment rules are retained. Partition and
declared FAT lengths must fit the candidate. FAT sector sizes 512, 1024, 2048 and
4096 are accepted; fixtures exercise 512 and 4096. Existing global sector settings
are saved/restored, and callers must serialize libfat operations as before.

The directory search retains all eight architecture names and the original
RISC-V/LoongArch `~1` short-name aliases. Those aliases remain a heuristic, not
full long-name resolution. FAT32 directories use both cluster halves. Nonempty
regular directory entries count as presence; no executable bytes are validated.
The bundled reader now flushes directory-sector caches as traversal advances,
detects cycles with constant storage, rejects invalid successor clusters and
cleans up a cached boot sector on open failure. Its existing bytewise endian
helpers avoid unaligned integer access. A small Linux build adapter maps aligned
allocation to `posix_memalign`; no Windows API emulation is needed.

- Completed absence returns OK/false and clears embedded-EFI pending.
- Presence returns OK/true, sets the existing `has_efi = 0x8000` marker in the
  coordinator and leaves Secure Boot pending.
- Unreadable or unsupported FAT returns partial with embedded-EFI pending.
  Invalid arguments/context allocation failure return error; cancellation is
  explicit. Failed/cancelled analysis discards report fields.

CP3f2 adds UDF access to the same cache and directory search. Each cache fill
continues across recorded extents with explicit seeks; it does not assume that
file extents are physically adjacent. `udf_read_block` now stops at the remaining
extent or file boundary after a seek, validates allocation-descriptor bounds and
uses little-endian descriptor fields. Its return value is bytes, while its count
argument is blocks. The adapter uses recorded short/long descriptors in the file
entry and the first partition map. Sparse data, indirect/extended descriptors
and in-ICB data remain unsupported and leave embedded inspection pending when
encountered. UDF file position belongs to the borrowed handle; callers serialize
access, and subsequent operations must seek or open their file as usual.

El Torito load counts can be smaller than the full FAT image; those incomplete synthetic ranges
remain pending rather than trusting a FAT header to enlarge the readable range.
Secure Boot, extraction and OS-image boot behavior are separate work. For these inputs, START stays
disabled and the Widgets form is unchanged.

`rufus_embedded_efi` generates 27 container fixtures with native tools, including
FAT12/16/32, 4 KiB sectors, directory clusters above 65535, EOF read-ahead, MBR
bounds, truncated candidates, absent/empty/directory bootloader entries and UDF
coordination. Link-time read/allocation failures and cancellation check cleanup,
sector-state restoration and recovery. Corrected-reader-only directory-chain
fixtures check bounded failure. Even the 34 MiB FAT32 fixture reads fewer than
128 ISO blocks during inspection; each read is at most 16 ISO blocks.

```sh
ctest --test-dir /tmp/rufus-linux-qt6-build -R 'rufus_.*embedded_efi|rufus_image_analysis_widgets' --output-on-failure
```

`rufus_udf_embedded_efi` adds 16 generated UDF containers, including three
non-contiguous extents with short and long descriptors, distinct UDF/ISO views,
UDF without a companion ISO, Unicode/case-sensitive lookup and EOF padding.
Twenty-two direct range checks verify byte-exact reads and boundaries; injected
seek/read/open/storage failures and cancellation verify incomplete/cleared results.
The fixtures pass with both genisoimage and cdrtools mkisofs, including allocation
descriptors after extended attributes, normally and with sanitizers.
The Qt embedded-EFI case runs against both UDF and ISO9660 through the same form.
This qualifies embedded FAT streaming; the older whole-file buffer reader still
uses one UDF read and does not assemble multi-extent files.

All fixtures are ordinary temporary files. The workflow includes dosfstools and
mtools as test dependencies; the GUI does not invoke them for analysis. All 26
Linux suites pass normally and with address, undefined-behavior and leak checks.
See [CP3f2](../docs/testing-and-checkpoints.md#cp3f2-bounded-udf-access-to-embedded-fat-2026-09-14)
for the checkpoint limits and validation record.

## Shared WinPE options inspection

`ScanWinPeOptions` in `src/winpe_scan.c` selects the original i386/AMD64/minint
priority, reads the SIF path recorded by classification and shares the options
decision with Windows. `ReadISOFileToBufferLimited` checks its 64 KiB limit
before allocating or reading data. Existing callers of the earlier read API keep
their 1 GiB limit; both use the same UDF-first lookup and ownership rules.

`InspectWinPeOptions` uses the SIF line grammar from Rufus's token parser directly
on ANSI/UTF-8 bytes, preserving the first nonempty option value and `/minint`
substring rule. It accepts ordinary text without an on-disc NUL, one trailing
NUL or a UTF-8 BOM. Empty/oversized input, embedded NULs and UTF-16 remain
unavailable. The general parser's RTF grouping and update-data code stay separate.

The report's `winpe_options_known` distinguishes checked absence from unknown
options; `uses_minint` retains the existing decision. A complete WinPE-only scan
can clear Windows metadata pending. WIM/boot-manager metadata still keeps that
reason pending. Error/cancellation clears the analysis. The Win32-mapped Qt form
uses the existing status/log/label controls and keeps START disabled.

The component has 171 assertions, and 20 generated ISO/UDF containers cover
directory priority, original-case paths, differing filesystem views, limits,
pending WIM checks and cancellation. Both genisoimage and cdrtools mkisofs pass
the image suite. Two added Qt rows exercise real worker results and cancellation
for ISO9660 and UDF. All 26 suites pass normally and with ASan/UBSan/leak checks.

```sh
ctest --test-dir /tmp/rufus-linux-qt6-build -R 'rufus_winpe|rufus_iso_read_contract|rufus_image_analysis_widgets' --output-on-failure
```

CP3g2 implements ordinary native WIM metadata; CP3g3c now adds ISO/UDF access to SELECT.
Archive-format version and Windows version remain distinct. See the
[source map and next steps](../docs/linux-windows-image-metadata.md) and
[CP3g1 record](../docs/testing-and-checkpoints.md#cp3g1-shared-winpe-options-and-wim-boundary-2026-09-14).

## Shared ISO/UDF file ranges

CP3g3a moves the existing embedded EFI reader into `src/iso_file.c`, with a
concrete `ISO_FILE_READER` and open/read/close operations. EFI uses it through
the existing libfat sector adapter. It borrows the chosen ISO/UDF handle, retains
the fixed 32 KiB cache and supports exact unaligned/backward/multi-cache reads.
UDF data may span fragmented short/long extents; ISO data relies on the bundled
reader's contiguous-extent checks. A selected view never falls back to another.

Out-of-file ranges fail before I/O. Empty files accept a zero-byte read at EOF,
including cdrtools' empty-file representation. Physical reads are capped at 16
blocks, logical output excludes padding, failed refills invalidate the cache,
and cancellation is checked between calls and on cache hits. The caller owns
the destination buffer and discards it on failure. No per-read allocation,
mounts or whole-file staging are involved.

Tests compare 814 ranges from generated WIM/data files, including distinct ISO
and UDF views, UTF-8 names and fragmented UDF extents. The component adds 380
checks for cache behavior, 64-bit offsets, boundaries, faults, cancellation and
recovery. Run with:

```sh
ctest --test-dir /tmp/rufus-linux-qt6-build -R rufus_iso_file --output-on-failure
```

Extended upstream wimlib now reads through this transport in SELECT. The shared
scanner can retain exactly its selected filesystem handle through
`AnalyzeIsoImageWithSource`; the Windows stage borrows it before the worker closes
it. The standalone `AnalyzeIsoImage` wrapper retains its original close-on-return
behavior. See the [transport decision](../docs/linux-wimlib-prototype.md).

## Native WIM metadata

`rufus_wim_metadata` builds `src/wim_metadata.c` against native wimlib. Its
handle-based queries return archive facts, raw Windows fields, Rufus's normalized
version and explicit validity masks. Windows now calls the shared archive query
from `GetWimVersion` and the shared normalization from its existing XML reader.
The library decodes WIM/XML; there is no new XML parser or Qt callback interface.

`rufus_wim_file` supplies read-only Linux file opening and cooperative
cancellation through `IsIsoScanCancelled()`. The caller initializes wimlib before
workers and cleans up after them. Errors/cancellation release handles and clear
metadata. Unknown/invalid fields produce PARTIAL rather than a guessed version.
WIM indexes are 1-based; automatic reporting uses image 1, independently of the
archive's boot index. SELECT now queries the first recorded installation path
through the same selected filesystem. Unknown/failed WIMs remain partial, without
trying a later candidate. The existing status and log distinguish these results;
the form retains its Win32 layout. Successful WIM metadata does not clear pending
WinPE options, Secure Boot or other independent checks.

The two tests generate ordinary WIM files using native wimlib's public creation
API, including normal and solid formats, multiple/empty image lists, missing or
invalid properties and UTF-8 filenames. They also check cancellation, errors,
descriptor/handle cleanup, recovery and pathname replacement. All 28 CTest suites
pass normally and with ASan/UBSan/leak detection. No USB, mounts or root required.
These tests qualify metadata. CP3g4a separately qualifies bounded single-file
extraction using the same provider; Secure Boot and real Windows booting remain
unqualified. See the [extraction boundary](../docs/linux-windows-image-metadata.md#cp3g4a-bounded-single-file-extraction-and-shared-pe-recognition).

```sh
ctest --test-dir /tmp/rufus-linux-qt6-build -R rufus_wim --output-on-failure
```

For the optional independent system-library comparator, point `CMAKE_PREFIX_PATH` at its prefix, or
set `WIMLIB_INCLUDE_DIR` and `WIMLIB_LIBRARY` explicitly. Its shared-library
dependencies must also be discoverable at runtime. This workspace's verified
distro packages are extracted under `/tmp/rufus-cp3g2-wimlib/usr`; use:

```sh
export LD_LIBRARY_PATH=/tmp/rufus-cp3g2-wimlib/usr/lib
cmake -S linux -B /tmp/rufus-linux-qt6-build -DRUFUS_WIMLIB_UPSTREAM=/tmp/rufus-cp3g3d-upstream/wimlib-1.14.5 -DRUFUS_COMPARE_SYSTEM_WIMLIB=ON -DCMAKE_PREFIX_PATH=/tmp/rufus-cp3g2-wimlib/usr
cmake --build /tmp/rufus-linux-qt6-build
ctest --test-dir /tmp/rufus-linux-qt6-build --output-on-failure
```

See the [CP3g2 record](../docs/testing-and-checkpoints.md#cp3g2-native-wim-metadata-2026-09-14)
for dependency versions/checksums and limitations. No system package was installed.

## Standalone WIM/ESD source

`rufus_image_source` retains an ordinary Linux file and checks Rufus's shared
eight-byte WIM signature without moving the file offset. The suffix does not
control detection. Signature recognition is separate from archive decoding,
Windows version metadata and EFI architecture. In particular, unknown Windows
metadata stays unknown and wimlib's distinct pipable format is not a standalone
Windows-image candidate under Rufus's existing rule.

`OpenWimFileDescriptor` lets native wimlib open the retained inode through
`/proc/self/fd`. Free the WIM before closing the source. Check the descriptor and
current pathname against the original identity, size and timestamps before using
any results. Upstream requires a resolvable filename and can reject a file already
unlinked; never reopen the original pathname as a fallback. These are change
checks, not an immutable snapshot.

The component's generated-WIM tests select Windows-compatible case-insensitive
lookup, then query metadata and `Windows/Boot/EFI/bootmgr.efi` in image 1,
independently of the boot index. Missing EFI, unknown PE and unknown Windows
versions remain distinct. This foundation is not yet connected to SELECT.
The existing Widgets form and application wimlib initialization are unchanged.

```sh
ctest --test-dir /tmp/rufus-linux-qt6-build -R 'rufus_image_source|rufus_standalone_wim|rufus_image_file|rufus_wim_contract' --output-on-failure
```

See the [CP3g4b record](../docs/testing-and-checkpoints.md#cp3g4b-standalone-wim-classification-and-source-ownership-2026-09-14).

## Shared Syslinux version recognition

`src/syslinux_version.c` now owns `GetSyslinuxVersion`, with the existing public
signature and version macros in `syslinux_version.h`. It uses ordinary C and has
no Qt, Windows, file I/O or allocation dependency. The Windows image and embedded-
resource callers use the same implementation. Resource lengths remain unchanged;
the image caller includes the content reader's additional terminating byte.

The parser retains Rufus's marker/search rules, packed major/minor version and
suffix normalization. It checks that a banner terminates within the supplied
span and validates the numeric fields. Missing/malformed versions return unknown;
the normalized nonempty suffix points into the writable input buffer and must
be copied before that buffer is freed. It does not manufacture a fallback version.

The [46-case C suite](../tests/syslinux_version/README.md) and generated-ISO test
both run in the main Linux suite. Both bundled v4/v6 binaries are read as test
data under their existing notices. A temporary original-parser harness produced
the same results for the generated-image cases. Recognition remains independent
of the now-shared scan selection below; native installer capability checks are
still later work.

## Shared Syslinux scan selection

`src/syslinux_scan.c` selects a config, resolves candidate loader versions and
chooses the existing detected/embedded fallback version. These are three ordinary
C functions with explicit report and path arguments. File reads stay with the
caller; there are no new callbacks or allocation dependencies. Windows calls the
same functions through `ScanSyslinuxBootloaders` in shared scan finalization.

`HAS_SYSLINUX` now reads explicit configuration presence. The report separately
retains the chosen image loader's detected version/suffix, and the existing
`sl_version` fields remain the installation choice. Unknown detection stays zero
when a fallback is chosen; `sl_version_inferred` identifies that choice. An
unavailable fallback leaves the selection empty while preserving presence.

Rufus's shortest/latest-tie and openSUSE config preferences, conflicting-loader
prefix rules, family warnings and empty fallback suffix remain. The one path
error contract change rejects an overlong-only config list instead of keeping
the old placeholder path. See the [29-case policy suite](../tests/syslinux_scan/README.md).
Seventeen generated images exercise the shared traversal/read/parser/policy
chain. Sixteen supported-path fixtures also pass with the original selection
block in a temporary harness. No native installer or boot behavior is tested.

## Shared GRUB inspection

`src/grub_inspect.c` contains Rufus's version/build rules and filesystem marker
inspection. The functions take an explicit initialized report and filesystem
list; Windows now passes its existing state. The label sanitizer was moved from
`parser.c` into this module as a private function, preserving its rules. Existing
host `uprintf` reporting remains the only reporting dependency.

Version recognition retains the banner spellings, CentOS newline handling,
last-marker/first-file selection, zero-version exclusion, and ordered
`-nonstandard`, `-gdie` and GRUB 2.06 label suffixes. The input is a read-only
bounded span, including the reader's extra NUL. Filesystem names are copied to
the caller's original string array; allocation failure returns false with partial
results retained. `GetGrubFsMask` supplies the original FAT/exFAT/NTFS report bits.

The [component tests](../tests/grub_inspect/README.md) include 38 Linux cases;
the generated-ISO fixture covers eleven synthetic files and multiple-file
accumulation. The original functions passed the same image-fixture expectations
in a temporary Linux harness. No real GRUB boot, EFI structure validation or
installer behavior is implied.

## Shared EFI banner reporting

`src/efi_boot_info.c` contains Rufus's existing Shim/systemd-boot diagnostic.
Windows and Linux compile the same function; it keeps the first matching banner,
64-byte search margin, 32-byte version limit and existing log text. A precision-
bounded format string avoids modifying the input, and ctype receives unsigned
bytes. No allocation, Qt, global report or PE-reader dependency is required.

The input size is the actual byte count, without adding the reader's extra NUL.
No report flags or trust decisions are produced. PE/signature/revocation work
belongs to the separate Secure Boot path. See the
[34-case component tests and generated-image instructions](../tests/efi_boot_info/README.md).
Nine synthetic fixtures run in the main suite.
Optional tests also read two installed systemd-boot binaries through generated
images; the original routine produced the same fixture diagnostics. Real Shim,
PE validation and boot testing remain separate coverage.

## Widgets shell

[main_window.ui](qt/main_window.ui) maps the original controls from
[`IDD_DIALOG`](../src/rufus.rc) and the runtime layout in [ui.c](../src/ui.c).
See the [Win32 control mapping](../docs/linux-win32-ui.md) before changing the
form. Original icons are compiled directly from `res/`, including the supplied
16/24/32 pixel toolbar variants; retain their [notices](../res/icons/license.txt).

SELECT opens the desktop file picker. A readable, nonempty regular file appears
by name in Boot selection, with its full path and size in the tooltip. This is
file selection, not analysis or a claim that the image is bootable. Invalid
selections clear the previous image and show an explanation in the status bar.

The application starts discovery asynchronously and displays eligible devices
with name, path and capacity. It never automatically selects a target.
`SelectionController` preserves explicit selection across reorderings and name
changes, and clears it after removal, replacement, changed size/path, ambiguous
identity or discovery failure. Synthetic records live only in tests.

Both advanced sections expand/collapse, while unsupported operations remain
disabled. Image-specific options stay hidden until real analysis can determine
their applicability. The toolbar opens a reusable log window with Clear, Save
and Close; closing the main window also closes its dialogs. There are no media
jobs connected to the main window yet.

## Read-only device discovery

[UDisksClient](platform/udisks_client.h) receives an explicit `QDBusConnection`:
the application supplies the system bus, while tests supply a private bus with
no fallback. It calls ObjectManager `GetManagedObjects` asynchronously and
subscribes to interface/property notifications and service-owner changes.
Notifications coalesce over 25 ms. Obsolete replies cannot restore old records;
requests time out after five seconds and failures retry after two seconds.
Service restarts trigger a fresh snapshot. A lost bus clears discovery and
asks the user to restart the preview.

The initial filter accepts whole USB block devices with media available, both
removable hints set, a valid device path and positive 64-bit capacity. It excludes
partitions, optical, read-only, system/ignored and missing-drive records. These
are UDisks metadata hints, not proof that a disk is safe to erase. In particular,
`MediaRemovable` is not a universal USB-stick versus fixed-disk classifier; some
flash devices may be omitted. Fixed USB storage and the disabled **List USB Hard
Drives** option need a later, tested policy. See the UDisks
[Block](https://storaged.org/doc/udisks2-api/latest/gdbus-org.freedesktop.UDisks2.Block.html)
and [Drive](https://storaged.org/doc/udisks2-api/latest/gdbus-org.freedesktop.UDisks2.Drive.html)
contracts. Mounted-device/system-use checks remain required before any write.

Selection identities belong to this client session. They combine observed
object lifetime with path, size, device/drive identifiers and detection times.
Removal invalidates the identity immediately, even if identical metadata
reappears at the same path before the next snapshot. Relevant property changes
also invalidate selection while refreshing. These IDs are not persistent device
identifiers and do not replace authoritative revalidation before writing.

Discovery opens no device nodes and requests no authorization. UDisks2/polkit
authorization and native prompts remain CP7 work. The opt-in host smoke probe
uses this same production reader and prints only a count or error:

```sh
/tmp/rufus-linux-qt6-build/udisks_probe --system
```

It is built with the tests but never registered with CTest. It does not require
a USB device or root privileges.

## Reporting boundary

`rufus_qt_reporting` supplies C-linkage implementations of four existing Rufus
entry points, using their signatures at upstream commit
`18ae93bf37f94764d77216cff4168e0549a14c4d`:

| Entry point | Qt signal |
| --- | --- |
| `uprintf(format, ...)` | `RufusJob::logMessage(QString)` |
| `uprintfs(message)` | `RufusJob::logMessage(QString)` |
| `UpdateProgress(operation, percent)` | `RufusJob::progressChanged(int, float)` |
| `_UpdateProgressWithInfo(operation, messageId, processed, total, force)` | `RufusJob::progressWithInfo(int, int, quint64, quint64, bool)` |

The Linux C header declares these functions without including Qt or Windows.
It does not replace `src/rufus.h` and does not make Windows-dependent source
files portable. Future Linux targets select this library instead of the
original reporting implementations; do not link both definitions. Additional
entry points such as `wuprintf` are outside this initial subset.

Derive a concrete worker from `RufusJob` and implement its protected `execute()`
method with the operation's ordinary code and explicit inputs. Move the job
to a worker thread and connect the thread's `started` signal to `run()`, which
owns serialization, reporting scope and completion. Do not call `execute()`
directly. Use queued connections from job signals to named GUI member functions
or widget slots. The tests show the complete worker lifetime and real Widgets
connections. This uses one virtual call per operation, without generic task
injection, lambdas or a new callback registry.
[Qt worker-object pattern](https://doc.qt.io/qt-6/qthread.html#details).

The operation runs once. It must check any C error returns and throw an
appropriate C++ exception at the C++ call site to report failure. The adapter
converts exceptions to `failed(QString)` and always emits `finished()` after
flushing reports. `finished` means the job terminated; successful media
creation and validation remain the operation's responsibility. Keep the job
alive until it finishes and then delete it through the owning thread's normal
QObject cleanup. Closing a window should disconnect its consumers while the
application's job controller retains ownership of ongoing work.

Rufus still uses process-global state. A mutex rejects overlapping or nested
jobs rather than running them concurrently. During execution, a scoped
thread-local pointer routes existing context-free C reporting calls to the
active job. It points to a worker object, never a widget, and is cleared before
job completion, including on exceptions. Calls made by additional library
threads do not inherit this scope: use an explicit library callback/context
when integrating such a library. Outside a scope, logs go to Qt's diagnostic
output and progress updates are ignored.

`QString` owns queued text, so there is no cross-thread `malloc`/`free`
agreement and no fixed 4096-byte message buffer. Progress counters remain
64-bit integers. Ordinary intermediate progress is coalesced at a 50 ms
interval; control events, forced updates, resets, discrete steps and final
reports are preserved. The most recent pending samples are flushed in source
order on a boundary or job termination. No worker event loop timer is needed
while inside a blocking C call.

Progress values preserve upstream meaning: negative percentages indicate
discrete steps, and the `OP_INIT` info payload is not a byte count. A future
GUI controller must map operations to its own presentation; it must not treat
every percentage as whole-job completion or every info payload as bytes. This
component does not recreate Windows progress weighting, ETA or localization.

Cancellation remains cooperative and operation-specific. A blocking C call
cannot receive a queued cancel slot until it returns. Future jobs should check
thread interruption in their work loop or a supported library callback; this
adapter alone does not make an arbitrary C operation cancellable.

## Coverage

Eight behavioral cases compile a caller as C and deliver its notifications to
Qt Widgets on the GUI thread. They check UTF-8 and long messages, ownership
after buffer mutation and sender destruction, 64-bit counters and upstream
progress controls, coalescing and phase order, failure/completion ordering,
serialization, one-shot execution, recovery after exceptions, and widget
destruction before delivery.

The window suite adds 20 behavioral cases/data rows covering real keyboard and
mouse actions, file-picker acceptance/cancellation, image errors, simulated
device replacement, log saving/clearing, Win32 control order and advanced
sections. A third CTest entry checks large-font layout again at 2× scaling.
Native platform file dialogs are used in the application; tests force the Qt
dialog for reproducibility.

The discovery suite adds 31 behavioral cases/data rows against a separate Qt
D-Bus fixture process. It covers filtering, 64-bit metadata, duplicate names,
selection and disabled START in the real window, hotplug and reused paths,
property/media changes, stale replies, service loss/reappearance, malformed
replies, errors/retry, timeout, bus disconnection and a responsive log window
while discovery is pending. Each case gets a fresh private bus and rejects
service calls beyond read-only discovery. Physical/virtual USB, authorization,
media-write and boot validation remain later checkpoints.

[Linux CI](../.github/workflows/linux-qt6.yml) builds and runs `no-hardware`
tests on Ubuntu 22.04 and 24.04. The workflow is prepared locally; it has not
yet run on GitHub.
