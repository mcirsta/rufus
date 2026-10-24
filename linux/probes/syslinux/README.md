# CP12a–g native Syslinux qualification

## NTFS extlinux investigation — CP20i

CP20k promotes the original investigation provider into the shared native build
as `rufus-extlinux`. It is present with `BUILD_TESTING=OFF` and is packaged in
`SyslinuxRuntime`; CP20l connects the standalone NTFS helper while GUI enablement
remains separate. The guarded
guest-only observer `syslinux_extlinux_facts` stays test-only. The provider
compiles unmodified extlinux sources and the existing pinned v6 payloads;
it does not run upstream Make/Autotools or rebuild BIOS resources.

```sh
cmake -S linux/probes/syslinux -B /tmp/rufus-extlinux-probe -G Ninja \
  -DSYSLINUX_SOURCE_DIR=/path/to/syslinux-6.04-pre1
cmake --build /tmp/rufus-extlinux-probe --target syslinux_extlinux_facts
cmake --build /tmp/rufus-linux-qt6-build --target rufus-extlinux
python3 linux/tests/vm/syslinux_ntfs.py \
  --build /tmp/rufus-linux-qt6-build --probe-build /tmp/rufus-extlinux-probe \
  --upstream /path/to/syslinux-6.04-pre1 --kernel 7.2.7-arch1-1 \
  --ntfs-3g-package /path/to/ntfs-3g.pkg.tar.zst \
  --ntfs-library-package /path/to/libntfs-3g.pkg.tar.zst \
  /tmp/rufus-extlinux-ntfs
```

The main build supplies existing MBR/NTFS formatter probes. Host requirements
also include mkntfs/ntfsinfo, 7-Zip, sfdisk, QEMU/SeaBIOS, and the existing guest
bootstrap tools. The selected kernel needs the new `ntfs`, `fuse` and `nls_utf8`
modules. The NTFS-3G packages are unpacked only inside the guest, never installed
on the host. The recipe removes its disks, guest trees and firmware copies after
success. If interrupted, retain logs/hashes before removing those disposable
files manually. Do not keep failed fixture images after finishing an audit.

Both drivers install/map the loader, keep competing exclusive opens blocked,
remount and boot. The [NTFS reuse audit](../../../docs/linux-syslinux-ntfs.md)
records the checker incompatibility, exact scope and remaining integration work.

### Checked NTFS adapter and attributes — CP20j–k

Build `syslinux_ntfs_probe`, `mbr_image_probe` and `ntfs_format_probe` in the main
Linux build, then add `--adapter` to the command above. The guest runs 59 cases
per driver through the real adapter/runner and unchanged provider, then verifies
the final media independently and boots both bare loaders. Invalid maps and
I/O/sync/cancellation faults are injected only by the guarded guest worker.
USB serials identify backing images independently of Linux enumeration order;
guest hashes must match the corresponding host readback. CP20k checks Windows
attributes through both xattr byte-order interfaces before and after remount,
plus independent 7-Zip file-record readback. Eleven extra cases cover attribute
I/O, unsupported/missing attributes, short reads, ignored/incorrect writes,
final readback and cancellation; errors still flush before returning failure.

The recipe defaults to `--build`'s `syslinux/rufus-extlinux`. Pass
`--installer '/path/to/stage/opt/rufus package test/libexec/rufus-extlinux'`
to qualify the staged package at a relocated path, as CP20k does.

For ASan/UBSan, build those same three targets in the sanitizer tree and pass
that tree as `--build`; use `--installer` to select the normal staged provider
and keep `--probe-build` pointing to the normal mapping observer. The guest
enables leak detection and halts on sanitizer errors. The upstream executable
is not claimed to be sanitizer-clean.
Use a new disposable output directory per run. The qualification neither
installs a host provider nor enables NTFS in the application UI. CP20l separately
qualifies the complete [helper lifecycle](../../tests/vm/README.md#standalone-syslinux-on-ntfs).

## Earlier FAT qualification

This standalone CMake probe builds the upstream **6.04-pre1 mtools installer**
as a native host executable. CP12g shares its build recipe with the main Linux
project. It is not selected by the privileged helper yet. CP12b adds a small read-only
pair check to the Linux component build; the probe tests it with shared Rufus
selection and actual extraction output. BIOS remains disabled in Qt.
CP12c runs the installer in a dedicated file-only worker through the existing
command runner's new process-group entry point. No Wine, mount, root authorization,
physical device or Windows ISO is needed.
CP12d adds retained-source eligibility before creating test media and extends
the extracted-ISO qualification to eight BIOS volume boots.
CP12e routes those boots through the concrete file adapter, including independent
installed-loader/VBR verification and explicit temporary-directory cleanup.
CP12f adds two whole-disk boots using Rufus's Syslinux MBR payload, shared
partition placement, native FAT formatting and the same installer adapter.
CP12g tests that same route using the main build's staged `rufus-syslinux` package.

## Source and payloads

Use the unmodified `syslinux-6.04-pre1.tar.xz` from the
[upstream release directory](https://www.kernel.org/pub/linux/utils/boot/syslinux/Testing/6.04/).
The archive SHA-256, checked against that directory's `sha256sums.asc`, is:

```text
3f6d50a57f3ed47d8234fd0ab4492634eb7c9aaf7dd902f33d3ac33564fd631d
```

The checksum was checked over HTTPS; the manifest's PGP signature was not
authenticated. Preserve the archive's source and license notices. CMake does
not download dependencies or run upstream Make/Autotools. It compiles the ten
upstream C files listed by `mtools/Makefile`, and uses upstream `bin2c.pl` to
embed the three release payloads. It does not rebuild the BIOS payloads.

The release's `bios/core/ldlinux.sys` and `.bss` are byte-for-byte identical to
Rufus's existing `res/syslinux/ldlinux_v6.sys` and `.bss`. CMake checks that
equality and pins their hashes plus the companion `ldlinux.c32` hash. A different
payload fails configuration. Use the verified source archive; these payload
checks do not authenticate arbitrary installer source supplied by the caller.
The release's prebuilt installer needs a 32-bit runtime; this CMake build is
native x86-64 on the qualification host.

## Run

Requires CMake/Ninja, C/C++ compilers, Qt6 Core development files, Perl,
Python 3.11+, dosfstools, mtools, NASM and `qemu-system-x86_64`.
Set `SYSLINUX_SOURCE_DIR` to the extracted release:

```sh
cmake -S linux/probes/syslinux -B /tmp/rufus-syslinux-probe -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DSYSLINUX_SOURCE_DIR=/path/to/syslinux-6.04-pre1
cmake --build /tmp/rufus-syslinux-probe
ctest --test-dir /tmp/rufus-syslinux-probe --output-on-failure
```

The FAT test runs four cases sequentially and removes their files, including on test
assertion failures. It uses at most a 128 MiB FAT image plus a same-size check
copy, with 64 MiB RAM for each of two short TCG boot tests. No network or KVM is
needed. The cases are:

- Root installation on 64 MiB FAT32, with 512-byte clusters.
- `/boot` installation on 128 MiB FAT32, with 1024-byte clusters.
- Installation into a FAT32 region at a 1 MiB offset, preserving bytes before
  and after the volume. This case has no partition table and is not booted.
- Rejection of a valid 4096-byte-sector FAT16 volume, with its full hash unchanged.

All three installations check the BPB, installed loader size, exact companion
module, existing config/fixture/ordinary-file contents, guards and temporary
installer-config cleanup. Upstream changes the primary VBR but leaves the backup
VBR unchanged, as Rufus's installer does. The test verifies that fact, then
updates only the backup VBR **in a separate check copy** and requires clean
`fsck.fat -n`. It does not claim that the unmodified volume passes fsck without
the expected backup mismatch, and does not silently repair the booted image.

The two volume-at-zero cases boot the actual installed VBR and loader. Syslinux
loads its matching `ldlinux.c32`, reads the test configuration and executes its
`BOOT fixture.bs` entry. The existing `linux/tests/vm/boot.asm` fixture prints a
serial marker and exits QEMU through `isa-debug-exit`. The test requires the
Syslinux 6.04-pre1 banner followed by that marker and the expected exit status;
the backing image's complete SHA-256 must be unchanged after the VM exits.
These are BIOS volume boots, not MBR integration or Linux-distribution boots.

## Result and integration boundary

All four cases passed on 2026-09-21; the final run took 2.29 seconds. The CMake build uses GCC
16.2.1; packed-member warnings remain in unmodified upstream code. The four
existing shared Syslinux CTest suites also pass. Two additional small generated
ISOs use the release's real `isolinux.bin` and a loader without a version banner:
the first detects `6.04/pre1`; the second retains unknown detection and selects
numeric 6.04 with an empty suffix and `INFERRED=1`. Their same-source companion
module does not turn that fallback into a claim about arbitrary unknown images.

The native route works for this pair, with constraints to resolve before use:

- The installer embeds a fixed `.sys`/`.bss`/`.c32` set. `--version` prints only
  `6.04`, omitting `pre1`; it cannot establish compatibility or accept Rufus's
  dynamically selected payloads. A distribution's different 6.04 build is not
  interchangeable based on its numeric version alone.
- `--directory` moves **both** loader files there. Rufus keeps `ldlinux.sys` at
  the root, patches the config base directory, and handles companion modules
  separately. Root installation with Rufus's existing shared config redirect
  is a candidate to test; do not adopt the subdirectory layout just for convenience.
- The mtools installer supports 512-byte sectors; its byte-offset option is
  32-bit. FAT16/12 installation, NTFS, large offsets, other architectures,
  Syslinux v4 and other release/module sets are not qualified here.
- It invokes mtools through internal shell commands, uses a temporary MTOOLSRC,
  and calls global `sync()`. Some directory/attribute failures are only warnings.
  The authorized helper still needs controlled tool/environment selection,
  descriptor ownership, bounded child-process cleanup, independent postchecks
  and its own final sync. CP12c qualifies process handling through a separate
  runner entry point on ordinary files; privileged integration and postchecks
  remain separate work. No privileged execution was added in these checkpoints.

## CP12b selection and extracted-ISO boots

`CheckNativeSyslinuxPair` in `linux/platform/syslinux_native.cpp` accepts only
the selected 6.04/pre1 build, or Rufus's inferred numeric 6.04 fallback with an
empty suffix, plus the exact qualified `ldlinux.c32` size/hash. It checks FAT32,
512-byte sectors and an aligned offset representable by the native installer.
It never edits the image report, guesses a detected build, launches a host tool
or writes media. Its success is a pair/geometry check, not complete preflight.
The caller must use the executable built with the pinned payloads and check
every module it would replace; the check does not authenticate an arbitrary
executable claiming the same version.

The default CTest selection entry exercises 23 cases through shared
`SelectSyslinuxConfig`, `GetSyslinuxVersion`, `SelectSyslinuxCandidate`,
`SelectSyslinuxVersion` and `GetIsoSyslinuxConfig`. These cover known/inferred
selection, wrong numeric/build versions, absent/truncated/different same-size
modules, unsupported filesystems/sectors/offsets, offset representation limits,
no config, a preserved root config and the openSUSE override. All input report
bytes must remain unchanged by the pair check. The 32-bit boundary cases check
representability only; no large-offset installation is claimed.

For the additional ISO/boot test, provide an existing configured main Linux
build with its real extraction, scanning and source-check probes, plus `bsdtar`
and `mkisofs` on PATH (or set the standalone cache variable `SYSLINUX_MKISOFS`):

```sh
cmake --build /tmp/rufus-linux-qt6-build --target iso_extract_probe syslinux_scan_probe syslinux_source_probe syslinux_install_probe mbr_image_probe fat_format_probe
cmake -S linux/probes/syslinux -B /tmp/rufus-syslinux-probe \
  -DRUFUS_LINUX_BUILD_DIR=/tmp/rufus-linux-qt6-build
cmake --build /tmp/rufus-syslinux-probe
ctest --test-dir /tmp/rufus-syslinux-probe --output-on-failure
```

Four small ISOs cover `/isolinux/isolinux.cfg`, its unknown-version counterpart,
an existing `/syslinux.cfg`, and `/boot/x86_64/loader/isolinux.cfg` overriding a
root config. The real scanner agrees with the selected report. The real extractor
skips the source root `ldlinux.sys`, preserves existing root config or generates
the shared `CONFIG`/`APPEND` redirect, and creates `syslinux.org` on override.
Its output is copied to a 64 MiB FAT32 file and the native installer runs at root.
Every extracted file is read back byte-for-byte. All four boot through upstream
`menu.c32` and its actual companion libraries to the relative `fixture.bs` entry.
This tests module lookup and working-directory behavior as well as the boot entry.
The VM uses a snapshot; complete backing hashes remain unchanged. Each case and
its check copy are removed before the next case.

**Native difference:** image-mode Rufus keeps the image's modules where they
were extracted and patches the loader's config base directory. This native
root installation additionally puts the matching `ldlinux.c32` at the root.
That gives the unchanged native loader its verified bootstrap companion before
Rufus's shared config redirect changes directories. Existing image modules stay
intact. Any existing root companion must match before the native installer may
overwrite it; the qualification checks this separately and rejects a conflict
before making media. This extra bootstrap file lets us retain the upstream native
installer and loader without replacing the image's module set with host files.

This establishes the four tested layouts, including one unknown-version fallback;
it does not establish every config search path, `extlinux.conf` precedence,
arbitrary module sets or a distribution boot. Full source-layout preflight must
handle those cases before enabling the route. Helper descriptor ownership,
controlled process execution/cancellation, final sync and MBR integration remain
the next checkpoints. No privilege or GUI behavior changed in CP12b.

## CP12c retained target and process cleanup

`syslinux_command_probe` is a disposable regular-file worker. It calls
`RunInstallerCommand` in the existing `format_command.c`; the ordinary FAT/NTFS
formatter entry point keeps its previous behavior. The new entry point requires
the worker to enable Linux child-subreaper adoption before starting the job. It
rejects automatic child reaping and requires exclusive ownership of the group's
exit statuses. It does not change these process-wide settings inside a Qt thread.

The child gets the retained file at descriptor 3, standard streams, a complete
caller-supplied environment and private working directory. All other descriptors
are closed. The probe supplies only its selected mtools directory on PATH and
private HOME/TMPDIR, without inheriting the user's mtools or loader environment.
The unchanged installer reopens `/proc/self/fd/3`; the pathname is never used
again. The upstream internal shell calls and global `sync()` remain unchanged.

The runner uses the existing 120-second timeout, 500-ms TERM/KILL grace period,
bounded diagnostics and final descriptor sync. It signals the whole process group,
adopts and reaps descendants, and treats an installer returning with children
left behind as a failure. The leader's PID stays reserved until group cleanup
starts. Unrelated child statuses are not consumed. Cleanup waits for actual exit;
it cannot promise a hard deadline for a process stuck in kernel I/O. These trusted
tools must not daemonize or leave the group. This is not a sandbox for arbitrary
programs. The caller removes the private temporary directory after return,
including an MTOOLSRC left by an interrupted installer.

The default `rufus_syslinux_native_command` CTest covers 21 process cases and
three real-installer cases: root installation, a 1-MiB offset with the pathname
replaced after opening, and cancellation inside a substitute mcopy with a writing
grandchild. Faults cover exit/signal failure, orphaned writers, TERM-resistant
children, closed/noisy output, timeout, clock failure, sync failure/EINTR,
pre-cancellation, unrelated children, invalid worker setup and a missing program.
At every final sync the fault harness verifies that no job children remain.
It checks descriptor counts, flags, original file position and retained-file
identity. Native success also checks the BPB, exact companion bytes, guard regions
and fsck on the separately normalized copy described above. Media are removed.

The four ISO-layout boots now use this worker too. On 2026-09-21 all four
standalone CTests passed in 11.51 s; FAT/NTFS formatter regression suites passed
in 4.86 s. The command-worker cases also pass ASan, UBSan and leak detection;
the upstream native installer remains the normal, uninstrumented executable.
Full source-layout eligibility, an adapter with independent postchecks, device
ownership, MBR integration and distribution boot qualification are still needed.
BIOS remains disabled in Qt; no production helper invokes this installer yet.

## CP12d initial source-layout eligibility

`CheckNativeSyslinuxSource` in `linux/platform/syslinux_source.cpp` borrows the
analysis's selected ISO/UDF handle. It uses the existing shared tree walker and
bounded file reader, retaining only boot-path entries and their parent names.
CP12d originally checked the selected-directory companion and any root companion
against the pinned pair. CP12l corrects this for known image versions: like Win32,
select by the detected version/suffix and preserve the distribution's non-root
module, which may be rebuilt from that same release. An unknown/inferred version
still needs the exact selected companion; any overwritten root companion must
also match exactly. Those checks reject wrong sizes before allocating/reading
contents and read only the 122656-byte pinned companion. Installed payload
verification remains exact in all cases. This is an eligibility check, not a
claim that arbitrary same-version distribution modules will boot.
It does not change the report, infer a detected version, open media or run tools.
The caller must retain/check source identity and perform ordinary extraction
preflight as well; the source probe demonstrates that ownership sequence.

Initial lookup follows the qualified loader's order: its start directory,
`/boot/syslinux`, `/syslinux`, then root, trying `extlinux.conf` before
`syslinux.cfg`. The adapter compares Win32 Rufus's patched start with the native
root start, resolving the shared root redirect and generated EFI config copy.
It permits equal routes and rejects different first configs with both paths in
the diagnostic. It does not attempt to interpret every command in those configs.
In particular, an additional `extlinux.conf` is not automatically an error.

The Win32 source has a root-config edge case: `InstallSyslinux` shortens cfg_path
only when the last slash is after its first byte. For a root config it passes
the whole filename as the loader's start directory. Upstream directory lookup
then fails and searches the fallbacks before root. The checker accounts for this
source-level behavior; it does not change Windows code or claim Windows runtime
validation. The normal root fixture still works when no earlier fallback wins.

Reserved file/directory conflicts, critical-path symlinks, case collisions,
renamed boot paths and an occupied required `syslinux.org` backup are rejected.
The selected redirect path must use supported unquoted ASCII tokens. Trailing-dot
and trailing-space spellings of boot paths are rejected: kernel VFAT ignores
trailing dots and disallows trailing spaces, while mtools handles names
differently. See the [kernel filename implementation](https://github.com/torvalds/linux/blob/master/fs/fat/namei_vfat.c).
The key used to detect those spellings never renames source or output files.

With `RUFUS_LINUX_BUILD_DIR` configured, `rufus_syslinux_native_source` covers
39 source layouts, one actual Rock Ridge symlink, eight fault/identity cases
and two UDF-view checks. These include wrong/missing/oversized companions,
known/inferred selection, case preservation/collisions, config precedence,
backups, cancellation and failed reads. A UDF-only companion rename must be
rejected even though the untouched companion ISO view still contains that file.
Every call preserves the original image hash (or the retained original in the
intentional pathname-replacement case), analysis bytes and descriptor count.

The ISO-layout CTest now runs eligibility before extracting or creating media.
Its eight BIOS boots include the previous four cases, selected-directory
`syslinux.cfg`, root `extlinux.conf`, an unused fallback config and the shared
EFI config copy. Each uses the supervised installer, complete file readback,
VBR/BPB checks and unchanged backing hashes after snapshot boot. Tests remove
their images and trees. A concrete file adapter with independent postchecks,
device/helper ownership, MBR integration and distribution boot qualification
remain before BIOS is exposed in Qt.

## CP12e file-image installer adapter

`InstallNativeSyslinux` (originally `InstallNativeSyslinuxImage`) combines the checked ISO/UDF source, formatted FAT32
region and CP12c process runner. It requires a dedicated child-subreaper worker,
a retained regular O_RDWR target, a trusted pinned native installer and trusted
mcopy/mattrib tools. The caller must own a newly formatted, unmounted target
populated through checked shared extraction. This is not an arbitrary-volume
repair/reinstall entry point. The adapter refuses existing root `ldlinux.sys`
and checks any root companion before launching. It creates a private directory,
supplies a complete environment, then checks source identity again after return.
Child exit/reaping and sync precede verification and directory removal; any
failure leaves an incomplete target, with no rollback or successful READY.

Verification uses the existing formatted-image checks and bundled libfat for
directory lookup and FAT-chain traversal. Reads stay inside the declared volume;
file size bounds memory, and expected cluster counts bound chain traversal.
It rejects cycles, short/extra chains, directories and cross-linked boot files.
Rufus's existing `syslinux_patch`, `syslinux_reset_adv` and
`syslinux_make_bootsect` reconstruct the expected full loader/ADV and primary VBR
from the actual sector map and original BPB. Patch templates come only from the
hash-pinned compiled resources, never from disk. Even a sector-per-extent map
fits the pinned loader's 192 slots. The backup VBR must remain unchanged and the
root companion must match the qualified hash. This does not read back every
copied ISO file or certify arbitrary config commands; the caller/test does that
separately, and actual boot qualification remains necessary.

UBSan found the original x86 unaligned integer dereferences in `syslxint.h`.
The six 16/32/64-bit accessors now use memcpy without changing the algorithm or
adding platform branches. All 24 byte-alignment/width checks pass. GCC 16.2.1
at `-O2` emits single loads/stores for those helpers, with no memcpy calls.
Windows runtime validation remains deferred. The standalone upstream native
installer remains unchanged and uninstrumented; the adapter, Rufus patcher and
packed-field probe pass ASan/UBSan/leak checks without sanitizer exceptions.

`rufus_syslinux_native_install` runs 32 cases covering successful root/offset/
larger-cluster installation, preflight rejection without writes, false success,
corrupt loader/module/VBR/BPB/backup/FAT chains, read/sync failures, changed
source/target, cancellation before/during/after installation and failed private
directory cleanup. It verifies descriptor counts/flags/position, analysis bytes,
source content hashes, guards, child reaping and cleanup. The permission-based
cleanup fault requires an ordinary unprivileged test user. The eight extracted
ISO boots now use this adapter, retaining complete copied-file readback, fsck on
the normalized copy and snapshot backing-file hash checks. Each case removes
its images; only small logs/manifests and build inputs are retained.

Next qualify MBR whole-disk layout, then native-installer packaging and the
existing authorized helper's device ownership. BIOS remains disabled in Qt.

## CP12f MBR whole-disk images

`PlanSyslinuxMbrLayout`, `CreateSyslinuxMbrImage` and `CheckSyslinuxMbrImage` extend
the existing Linux partition component. The narrow file-only path creates one
active FAT32 LBA partition (type 0x0c), with Rufus's usual 1-MiB start and shared
track/cluster rounding. It supports 512-byte sectors and disk capacity through
2 TiB; the upper bound is tested by planning only, without a large fixture.
The caller supplies a per-job disk signature. Libfdisk serializes the DOS table;
the adapter inserts the exact `src/ms-sys/inc/mbr_syslinux.h` payload used by
Win32 `WriteMBR`/`write_syslinux_mbr`. Syslinux does not need a secondary boot
record in this path, as in Win32 `WriteSBR`.

The existing 440-byte payload's SHA-256 is
`4746f74bc9b9d3d579c41988a4a29bb7ac932ad1c70470ea779ea161eb799b64`.
It also matches the pinned release's `bios/mbr/mbr.bin`. The original array and
shared placement functions remain untouched. The native library supplies CHS
fields; this checkpoint qualifies the usual LBA boot path, not CHS-only firmware
or the optional old-BIOS fixes.

The API borrows an exclusively owned regular O_RDWR descriptor and checks exact
size/flags, preserves its offset and flushes after every attempted write. Readback
checks boot bytes, marker, disk signature, reserved/unused entries, active flag,
type and extent. Failure/cancellation after `started` leaves incomplete metadata.
Existing non-DOS labels are refused instead of leaving a GPT backup behind during
conversion. DOS replacement clears the old table entries; data/old filesystem
metadata beyond sector zero is not erased. Preparing previously used media and
block-device ownership remain separate full-job work before helper/Qt enablement.

The main build's `rufus_syslinux_mbr_image` CTest makes 57 planner/create/check
calls. It independently decodes the sector, compares the original boot payload,
uses sfdisk inspection and verifies that every byte after sector zero stays
unchanged. Cases include replaced DOS tables, rejected GPT conversion, malformed
geometry, retained-path replacement, descriptor checks, interrupted/partial I/O,
cancellation, sync failure and corrupted boot/table fields. Read-only validation
must reject damage without repairing it. All cases pass ASan/UBSan/leak checks.

The standalone `rufus_syslinux_native_mbr` CTest builds 65-MiB and 129-MiB disks
with 512/1024-byte clusters and nested/root configs. It composes MBR creation,
the existing native formatter, shared extraction copied into FAT through mtools,
and the file-image installer. Both whole disks boot from MBR through FAT32,
Syslinux and menu to the fixture's serial marker. All copied files, loader,
companion, BPB, MBR and alignment gaps are checked; fsck uses the separate
normalized-backup copy. QEMU snapshot boot leaves backing hashes unchanged.
The eight previous volume boots now also use the existing native formatter.
All temporary media and extracted trees are removed.

CP12g below handles native-installer packaging. Device preparation and the
authorized job, followed by a real distribution boot, remain before GUI enablement.

## CP12g shared native build and packaging

`linux/syslinux/CMakeLists.txt` now owns the existing native build. The main
project selects it with `RUFUS_SYSLINUX_UPSTREAM`; this standalone harness retains
its `SYSLINUX_SOURCE_DIR` option and includes the same directory. Both build
`syslinux/rufus-syslinux` with the same unmodified upstream sources and pinned
payloads. The old `syslinux_native_probe` executable name is retired.

The [private package instructions](../../syslinux/README.md) describe provenance,
dependencies and the `SyslinuxRuntime` install component. It installs only the
private executable, upstream license and build/payload notice, with ordinary
permissions and no new authorization action. The executable remains optional
while BIOS is disabled; existing UEFI-only builds need no new source dependency.

The main `rufus_syslinux_package` CTest stages the component under DESTDIR at a
relocated prefix containing spaces. It checks the exact installed file set,
permissions and executable hash, then uses that staged executable for both CP12f
whole-disk boots through the existing file adapter. Independent installed-byte
checks, full file readback, fsck on a separate normalized copy and unchanged
snapshot backing hashes still apply. Separate altered `.bss`, `.sys` and `.c32`
inputs must fail configuration with the payload diagnostic. All temporary trees
are removed. The test passes in **5.35 s**; all seven standalone suites pass in
**27.51 s** after moving to the shared recipe.

No source/payload, runtime policy, Windows build, GUI or helper behavior changed.
The package has only been staged/tested locally, not installed on the host or
qualified in CI. Next connect MBR preparation and native device ownership to the
single authorized job, including old-metadata cleanup and final sync, then
qualify a representative distribution before enabling BIOS in Qt.

## CP12j native block-descriptor qualification

`InstallNativeSyslinux` now also accepts an explicit prepared MBR layout
and expected disk sequence. The default remains regular-file-only. Native mode
reuses `CheckSyslinuxMbrImage` and the formatter's BPB/libfat validation, checking
device/capacity/sector/access facts, partition extent and sequence before and
after the existing installer runner. The whole-device exclusive claim remains
held through the unchanged upstream installer and its mtools children. The same
Rufus patcher verifies the installed loader/ADV/VBR; source rechecks, group exit,
sync and private-directory cleanup remain part of success.

The [minimal guest recipe](../../tests/vm/README.md#syslinux-mbr-device-preparation)
composes native MBR preparation, formatting, mounted shared extraction and
installation on a simulated USB. Ten cases pass normally and with ASan/UBSan/leak
checks; a child-side competing exclusive open is refused during mcopy. Independent
copied-file readback, filesystem checks and a whole-disk Syslinux/menu boot pass.
The 32 file-adapter cases and both earlier MBR boots remain green. The native
installer itself stays unmodified and uninstrumented. The complete authorized
job and a representative distribution boot remain before GUI enablement.


## CP12l real distribution and known-version selection

The complete CP12k helper is now exercised with the pinned Alpine 3.22.5 ISO by
`mbr.py --alpine`. Alpine reports 6.04-pre1, but its rebuilt source companion is
115416 bytes (SHA-256 `eb5da0d2b08bda3d3055a37bcf4fca700edbf916378b97fdb4a7eb3fefaf561e`),
which differs from the upstream 122656-byte module. Requiring byte identity for
this preserved non-root file was stricter than Windows Rufus's version/suffix
selection (`src/rufus.c`, before `InstallSyslinux`).

`CheckNativeSyslinuxSelection` now checks that selection separately from
`CheckNativeSyslinuxPair`'s exact-byte check. Known 6.04-pre1 source modules stay
unchanged; no Alpine-specific hash allowlist was introduced. Unknown/inferred
sources still require an exact selected companion, and any root companion the
native installer would replace must match its pinned payload. Post-install
verification still checks the exact root companion, loader/ADV and VBR. Existing
path/config/identity/cancellation rules remain. Version eligibility alone is not
a general compatibility or bootability guarantee.

The distribution test verifies the entire copied file inventory and contents,
MBR/FAT consistency, then boots the created USB through SeaBIOS/Syslinux to Alpine
userspace. It adds only a serial-console argument through the bootloader prompt;
ISO/USB configs are untouched. The live system checks BIOS mode, release,
architecture, USB mount, modloop and boot-file hashes before powering off, and
the snapshot's backing hash remains unchanged. See the
[VM instructions](../../tests/vm/README.md#syslinux-mbr-device-preparation).
GUI BIOS enablement is the next separate checkpoint.


## CP15h FAT16 file-only installer (2026-09-24)

The adapter now accepts explicit FAT16/FAT32 selection. FAT16 uses the existing
formatter readback and libfat fixed-root/chain traversal; it has no FAT32 backup
VBR field. The same pinned 6.04-pre1 executable installs the loader, and the
original Rufus patcher verifies every installed loader/ADV/VBR byte against
trusted templates. Version/suffix and companion checks apply to both FAT types;
unknown source detection still requires the compatible pinned pair.

The main Linux CMake build registers `rufus_syslinux_native_selection`,
`rufus_syslinux_fat16_install` and `rufus_syslinux_fat32_install` when the upstream
source is configured. Selection has 25 cases, and each installer suite has 34.
FAT16 exercises 16 MiB root/offset volumes, larger clusters and a 512 MiB volume
with 64 KiB clusters. It substitutes that final case for FAT32's backup-VBR fault;
all shared corruption/cancellation/cleanup cases remain. Independent fsck checks
the actual FAT16 VBR; only FAT32 needs its known backup mismatch normalized on a
copy. Every fixture is removed afterward. No boot or native-device claim follows
from this installation/readback checkpoint.

For sanitizer builds, instrument our adapter, reader and Rufus patcher while
using the qualified normal-build external executable, as in CP12e:

```sh
cmake -S linux -B /tmp/rufus-linux-qt6-sanitizers   -DRUFUS_SYSLINUX_TEST_INSTALLER=/tmp/rufus-linux-qt6-build/syslinux/rufus-syslinux
ctest --test-dir /tmp/rufus-linux-qt6-sanitizers --output-on-failure   -R '^rufus_syslinux_fat(16|32)_install$'
```

This cache option changes only the executable passed to installer tests. It does
not change the packaged binary or production paths. An empty option uses that
build's executable. A fully instrumented upstream executable reports unaligned
loads/stores in its original `libinstaller/syslxint.h` and `libfat/ulint.h`, plus
exit-time allocations from its mtools main path. The diagnostic log is retained;
these upstream findings remain unresolved. No sanitizer exception is added.
Production device/helper/GUI Syslinux paths remain FAT32-only pending CP15i.
