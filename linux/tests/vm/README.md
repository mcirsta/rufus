# Native USB guest qualification

This optional test runs the actual Qt Widgets, UDisks discovery, polkit helper and C
writer inside a disposable Linux guest. It uses real guest block devices without
linker wrappers. The normal CTest suite never runs it on the host.

The current offline recipe uses installed Arch/CachyOS package metadata and
matching archives from `/var/cache/pacman/pkg`. Build the executable on the same
system so its Qt and C++ runtime match the guest packages. It needs Python 3.11+,
bsdtar, kmod, cpio, OpenSSL, mkfs.ext4, mkfs.fat, NASM, QEMU x86, debugfs, and
SeaBIOS at `/usr/share/qemu/bios-256k.bin`. The
installed guest dependencies include UDisks2, polkit, the KDE authentication
agent, Qt6, Xvfb, xdotool and DejaVu fonts. Missing cached dependencies are errors;
the recipe never downloads packages or installs anything on the host.

```sh
cmake -S linux -B /tmp/rufus-linux-qt6-build -DRUFUS_BUILD_VM_TEST=ON
cmake --build /tmp/rufus-linux-qt6-build
python3 linux/tests/vm/prepare.py --build /tmp/rufus-linux-qt6-build \
    --kernel 6.18.50-3-cachyos-lts /tmp/rufus-usb-qualification
python3 linux/tests/vm/run.py /tmp/rufus-usb-qualification /tmp/rufus-usb-run
```

Use an installed kernel release with its matching modules. Both output paths
must be new directories. The recipe records package versions and SHA-256 hashes,
the kernel release, and hashes of the executable, kernel, initramfs and disk
images in `manifest.json`. The runner checks the prepared images before use and
records its QEMU command. Allow roughly 7 GiB for preparation and one run; the
unpacked package tree can be removed after preparation. KVM requires access to
`/dev/kvm`; `--accel tcg` selects software emulation, subject to the same timeout.
Only KVM is qualified so far.

Each run attaches two newly created regular-file disks: the guest system and a
256 MiB emulated removable USB disk with serial `RUFUS-TEST-USB`. There is no
network, host filesystem sharing, host block-device attachment or USB passthrough.
Guest root setup assigns ownership and starts the services. The test itself runs
as UID 1000, verifies that direct raw writes are denied, and refuses to start
unless the fixture marker, QEMU DMI product and kernel command line all match.
It also checks the selected USB serial, path and capacity before starting.

The guest has fresh accounts and a public disposable password, `rufus-fixture`.
Its polkit rule only chooses the fixture administrator; pkexec policy
still requires authentication. xdotool enters the password into the separate
native KDE agent. Rufus neither receives the password nor runs as root. Xvfb
provides a real X11 surface for the Widgets and native prompt; this is not a
complete desktop session or a claim of visual parity with Windows. The native
agent uses its software renderer under Xvfb; the Rufus frontend uses Widgets.

The tests dismiss authentication, authenticate successfully, write a generated
17 MiB BIOS/FAT16 image, flush and verify it, and wait for native partition
discovery. QEMU throttles writes to 2 MiB/s for the following fault cases: a
competing guest mount must report busy while the writer owns the disk, then
succeed after cancellation releases it; unplugging through QMP during a write
must end as failure and clear discovery without reporting 100%. Reattaching the
same USB at the same device and UDisks paths must produce a new identity. The old
selection and retained write plan must remain rejected, without authentication
or writing; the user must select the newly discovered device.

The host independently compares every written byte and the untouched target
tail. It saves the successfully verified output before the cancellation/removal
cases, then boots that saved disk read-only in a second SeaBIOS VM. The original
`boot.asm` fixture prints `RUFUS_RAW_BOOT_OK` to COM1 and exits QEMU with status 33.
This proves that the generated fixture reaches its BIOS entry point. It does not
qualify an OS installer, physical USB durability or Windows.

Results include `serial.log`, `qemu.log`, `authentication.png`, `complete.png`,
`mount-error`, `verified-usb.raw`, `boot.log`, the retained `bios.bin`, its hash
and QEMU version in `firmware.json`, and the manifest/command records.
A missing assertion, timeout, changed input hash or missing boot marker fails
the run. No VM remains running after runner failure.

## Optional real Linux images

The extended run writes the official OpenWrt 25.12.5 x86/64 ext4 combined BIOS
and UEFI images through the same Widgets workflow. These are bootable raw disk
images. Download and decompress them during fixture preparation; this does not
add compressed-image support to the application.

```sh
python3 linux/tests/vm/openwrt.py /tmp/rufus-openwrt-downloads
python3 linux/tests/vm/prepare.py --build /tmp/rufus-linux-qt6-build \
    --kernel 6.18.50-3-cachyos-lts --openwrt /tmp/rufus-openwrt-downloads \
    /tmp/rufus-openwrt-guest
python3 linux/tests/vm/run.py /tmp/rufus-openwrt-guest /tmp/rufus-openwrt-run
```

`openwrt.py` downloads two pinned archives over HTTPS and checks their published
SHA-256 hashes. Preparation verifies them again before decompression and records
their URLs, release, compressed hashes and raw hashes in the manifest. Existing
archives allow offline preparation. The sources and checksums are published in
the [official release directory](https://downloads.openwrt.org/releases/25.12.5/targets/x86/64/).
No signature verification is claimed. Allow roughly 10 GiB for this extended
preparation and run. In addition to the baseline dependencies, the download
needs curl and UEFI boot needs `/usr/share/edk2/x64/OVMF_CODE.4m.fd` and
`OVMF_VARS.4m.fd`.

After each verified write, the guest observer saves the closed device before the
next write can begin. The host compares the complete written range against the
source and checks that the tail is unchanged. The UEFI source is slightly
shorter, so its tail is compared against the preceding BIOS result. The official
image bytes, including partition tables, are preserved without repairs.

Each saved output boots as emulated USB in a separate VM with SeaBIOS or OVMF.
The test enters its serial shell, reads `/etc/openwrt_release`, checks `uname -m`
and `/sys/firmware/efi`, and requires an exact output marker for the expected
release, architecture and boot mode before powering off. QEMU uses a temporary
snapshot for first-boot writes; the saved disk hash must remain unchanged.
Firmware copies, hashes, QEMU commands and serial logs accompany both disk
snapshots. This establishes userspace boot for these two images and firmware
configurations. Secure Boot, other distributions and OS installation remain
separate qualification work.

## Mounted FAT32 extraction

`fat_extract.py` is a separate, smaller guest for the ISO/UDF extractor. It
reuses the existing CMake test probes, generated-image helpers and kernel/module
recipe. It needs no desktop packages, package cache, system disk or authentication
agent. The current recipe uses an Arch/CachyOS kernel and glibc layout, including
the `gconv` modules used for UDF names. Host requirements are Python 3.11+, Bash,
coreutils, kmod, cpio, QEMU x86, mkisofs, mkfs.fat, fsck.fat and mtools.

Build the normal CMake test targets first (`BUILD_TESTING=ON`).
`RUFUS_BUILD_VM_TEST=ON` is not needed for this guest.

```sh
cmake --build /tmp/rufus-linux-qt6-build
python3 linux/tests/vm/fat_extract.py --build /tmp/rufus-linux-qt6-build \
    --kernel 6.18.50-3-cachyos-lts --mkisofs /usr/bin/mkisofs \
    --accel tcg /tmp/rufus-fat-extract
```

Use a new output directory and the actual installed mkisofs path. Allow about
400 MiB. This focused recipe is qualified with TCG software emulation; it also
accepts `--accel kvm`. It has a 180-second guest timeout and kills/reaps QEMU on
timeout. It never downloads dependencies or mounts anything on the host.

The existing GPT and FAT32 components prepare a fresh 64 MiB regular-file disk.
QEMU exposes only that disk as emulated USB, with no network, host filesystem
sharing or device passthrough. The guest checks PID 1, DMI and command-line
markers before running the tests. It mounts the partition through Linux vfat
with `utf8=1,shortname=mixed`.

Twenty baseline extraction cases cover Rock Ridge, Joliet and UDF names/content;
empty and long-name entries; root-loader exclusion; shared filename substitutions in files and nested
directories; file replacement and directory merging after name/case collisions;
shorter and empty replacements; a character outside Rufus's substitution rule;
injected write/flush failures and cancellation/retry; a genuinely full filesystem;
a read-only mount; recovery after unmount/remount; and root Syslinux config keep,
backup and backup-collision behavior. The config patch source is copied separately.
Four additional calls to `config_patch_probe` check native text edits on FAT32:
UTF-8 BOM label replacement, UTF-16LE Unicode preservation, an unchanged CRLF
file, and malformed UTF-8 rejection. The unrelated `boot.cfg~` must survive and
there must be no leftover temporary files. The fault wrappers belong
to the existing test probe. Successful extraction, collision handling, invalid-name rejection, ENOSPC
from a filled filesystem and EROFS from the read-only mount use real kernel I/O.
Failure/cancellation fixtures use a name requiring conversion, checking cleanup
of the resulting `bad_name.txt`. The UDF name fixture restores original characters
in valid directory records so it tests Rufus's conversion, not mkisofs's own
substitutions.

After clean unmount and guest shutdown, the host copies the partition extent,
runs `fsck.fat -n`, and reads every output through mtools. It checks exact names,
file bytes, empty failure directories and the final contents of colliding files
and merged directories. A FAT case collision keeps the first entry's spelling
while the later source supplies its bytes. Counts include every processed source
entry, even if its output is later replaced. Both GPT copies and all bytes outside
the partition must remain unchanged.
Results include `results.json`, `manifest.json` with input hashes,
`qemu-command.json`, `serial.log`, `format.log`, `fsck.log`, `usb.raw`,
`volume.raw` and the independent `readback` tree.

This qualifies the extraction component, shared filename substitutions, native
config-file edits and ordinary file/directory collision behavior on FAT32.
It does not establish bootability or the complete ISO writing workflow. ISO START
remains gated; boot qualification uses the separate authorized UEFI fixture.

CP10s adds four integrated extraction cases: the seven-token label fix on ISO,
Red Hat repo and original-filename netinst handling on UDF, and malformed config
rejection during preflight with an empty destination. These bring the fixture to
28 result cases (24 extraction jobs plus four standalone adapter operations).
Readback checks complete trees, unchanged configs and absence of temporary files.
CP10t adds FreeNAS UTF-8/UTF-16 volume-reference edits, an EFI Syslinux copy after
patching, preservation of an existing mixed-case EFI config, and a partial-copy
failure that must preserve the source and remove the incomplete duplicate. The
fixture now has **32 result cases** (28 extraction jobs and four standalone edits).

## Retained GPT block descriptors

This smaller guest qualifies the GPT adapter's borrowed block descriptor. It
uses the same initramfs recipe, with two fresh 64 MiB regular files attached as
USB disks: one with 512-byte logical sectors, one with 4096-byte sectors. Nothing
is mounted or opened as a block device on the host. There is no network, desktop
root filesystem, package download or physical USB requirement.

```sh
cmake -S linux -B /tmp/rufus-linux-qt6-build -DRUFUS_BUILD_VM_TEST=ON
cmake --build /tmp/rufus-linux-qt6-build
python linux/tests/vm/gpt.py \
  --build /tmp/rufus-linux-qt6-build \
  --kernel 6.18.50-3-cachyos-lts --accel tcg \
  /tmp/rufus-gpt-guest
```

Use an installed kernel with its matching modules and a new output directory.
The host needs the dependencies of the existing minimal FAT guest plus `sfdisk`.
The CMake-built `test_gpt_guest` requires guest root, the fixture marker and either
the direct-init DMI/PID checks or the dedicated authorization guest's DMI/caller
checks described below. The init
script also refuses direct host execution before any mounts. Tests are never
registered as host CTest entries. A sanitizer build can use the same recipe;
the guest enables address/leak and undefined-behavior failure reporting.

Twelve GPT requests cover native identity/capacity/sector/read-only checks, a read-only
descriptor, refusal of the unqualified two-partition UEFI:NTFS device layout,
cancellation before and after writing, failed flushing and success.
Each checks write/flush counts, retained flags and seek, and descriptor cleanup.
The caller's `O_EXCL` claim survives libfdisk and readback. The host independently
verifies both GPT headers/tables/CRCs, the protective MBR, Rufus placement and
every byte outside the GPT metadata and formatted partition. The rejected 4K
disk must be byte-identical.

The 512-byte guest also executes stock `mkfs.fat` with the retained descriptor
inherited by the child. Its second exclusive open fails with EBUSY. This is an
expected compatibility result. The next phase explicitly refreshes the guest
partition table, releases the exclusive descriptor, invokes stock mkfs.fat on
`/dev/sda1` with FAT32, cluster-size, hidden-sector, volume-ID and label options,
then reacquires exclusive access. The guest verifies device identity/capacity
and byte-identical GPT copies. A second invocation on a read-only mounted
partition must fail with EBUSY. The host independently checks the requested FAT
fields and backup boot sector, runs fsck.fat and lists the empty volume with
mtools. The production FAT adapter still rejects devices; the privileged-job
integration is the next step.

Artifacts include `manifest.json`, `results.json`, both `usb-*.raw` disks,
`serial-*.log`, `qemu-*.log`, `volume.raw`, `fsck.log`, `mdir.log` and the exact
`qemu-command-*.json` invocations. This guest tests the low-level adapter and CLI
handoff as guest root. It does not integrate desktop authorization, automount
services, hot-unplug during handoff, or ISO bootability. Authorization will cover
the complete creation job once, including subsequent tools and cleanup.

## One authorization for the helper

`helper.py` runs the same GPT/FAT32 C fixture through `pkexec` from an unprivileged
Qt test, using a real KDE authentication agent. It reuses the full desktop package
recipe above and adds dosfstools. The host needs its installed-version package
cache and the existing GPT inspection tools, plus `mkfs.ext4` and `debugfs` from
e2fsprogs. Nothing is downloaded or mounted on the host.

```sh
cmake -S linux -B /tmp/rufus-linux-qt6-build -DRUFUS_BUILD_VM_TEST=ON
cmake --build /tmp/rufus-linux-qt6-build
python linux/tests/vm/helper.py --build /tmp/rufus-linux-qt6-build \
  --kernel 6.18.50-3-cachyos-lts --accel tcg /tmp/rufus-helper-guest
```

Use a new output directory and allow about 3 GiB of disk space. QEMU receives a
regular-file root disk and a fresh 64 MiB emulated USB, with no network or host
device passthrough. The current Arch/CachyOS, KDE/X11 recipe is qualified with
TCG and accepts KVM. VM execution has a 480-second timeout. The guest-only policy
and restored packaged pkexec setuid mode are never installed on the host.
The Qt and C executables refuse direct host invocation before any authorization
request or block access; they are not host CTest entries.

The policy uses `auth_admin`, not a cached authorization. An independent root
D-Bus monitor counts one authorization request for each of three scenarios:
native prompt dismissal; an approved complete GPT/format/mount/readback/cleanup
run; and failure with the agent stopped. The Qt caller stays UID/EUID 1000 and
cannot open the device; pkexec's helper and its formatter child are UID/EUID 0.
The public fixture password is entered only into the separate KDE dialog.
Missing-agent failure has no internal terminal-agent fallback or retry.

KDE Escape can produce pkexec's generic denial code 127 rather than its distinct
dismissal code 126. The dismissal test accepts either, requiring no helper entry.
The three requests belong to three separate test jobs: the approved job uses one
authorization for every media phase. Production cancellation during a running
root job remains separate work; an ordinary-user process cannot rely on signalling
that helper, and this fixture's supervisor stops the disposable VM on timeout.

After shutdown, host checks independently inspect both GPT copies and FAT32
metadata, run fsck/mtools, and verify surrounding bytes. `debugfs` extracts the
authorization log and native-dialog screenshot without a host mount. Artifacts
include `manifest.json`, `results.json`, `qemu-command.json`, `serial.log`,
`authorization.log`, `helper-authentication.png`, `system.raw`, `usb.raw`,
`volume.raw`, `fsck.log` and `mdir.log`. The same recipe accepts sanitizer builds
and rejects sanitizer diagnostics in the serial log.

This qualifies a single native launch of the existing media fixture. Production
helper/controller integration and active-job cancellation are covered by the
separate mode below. Handoff hot-unplug/automount and other desktop sessions remain.

## Syslinux MBR device preparation

`mbr.py` qualifies `PrepareSyslinuxMbrTarget` on two disposable 65-MiB USB disks
in small initramfs guests. Each starts with two GPT partitions and nonzero data.
The 512-byte guest checks bad identity/capacity, cancellation, read-only media
and descriptors, and refusal of GPT by the table-only writer. Explicit preparation
clears Rufus's disk/new-partition ranges and creates one active FAT32 LBA MBR.
Independent host readback checks exact MBR bytes and cleared ranges, preserving
the middle of the disk. Kernel refresh must remove the old second partition and
publish the correct new extent; a second exclusive opener remains refused.
The 4K-sector guest must reject the request with the complete disk hash unchanged.

```sh
cmake -S linux -B /tmp/rufus-linux-qt6-build -DRUFUS_BUILD_VM_TEST=ON
cmake --build /tmp/rufus-linux-qt6-build --target test_mbr_guest partition_image_probe
python3 linux/tests/vm/mbr.py --build /tmp/rufus-linux-qt6-build \
  --kernel 6.18.52-1-cachyos-lts --accel tcg /tmp/rufus-mbr-guest
```

Add `--handoff` to qualify `PrepareDeviceTarget` formatting/mounting with an
explicit Syslinux MBR table. Build `test_mbr_handoff_guest` and `udisks_fixture`
instead of `test_mbr_guest`. This mode uses one 64-MiB file as a USB disk, real
`mkfs.fat`, the kernel FAT driver, block ioctls and a private mount namespace.
The existing fixture provides UDisks properties over a private real D-Bus;
this is not a UDisks daemon, desktop automount or polkit qualification.

```sh
cmake --build /tmp/rufus-linux-qt6-build --target test_mbr_handoff_guest udisks_fixture
python3 linux/tests/vm/mbr.py --build /tmp/rufus-linux-qt6-build \
  --kernel 6.18.52-1-cachyos-lts --accel tcg --handoff /tmp/rufus-mbr-handoff
```

Thirteen cases cover unsupported MBR/NTFS, failed snapshots, changed MBR and
both GPT copies, lost exclusive access, formatter cancellation, mount and sync
errors, and successful GPT/MBR handoffs. Mount cleanup must remove its temporary
directory, return sync errors and reacquire the exclusive whole-disk claim;
descriptor counts must remain unchanged. The final case fills FAT32 through the
disk's last sector, proving those bytes are filesystem data rather than a GPT
backup. Independent host checks decode the MBR, run `fsck.fat -n` and use mtools
to verify every byte of the 65,026,560-byte file. The runner retains its hash and
diagnostics and deletes disk, partition and readback copies even on failure.
ASan/UBSan builds use the same recipe with leak detection and halt-on-error;
sanitizer diagnostics in serial output fail the run. The guest-only executable
refuses an ordinary host launch before opening any disk.

Use `--install` instead of `--handoff` for CP12j's native block-installer cases.
Configure the optional `RUFUS_SYSLINUX_UPSTREAM` dependency and build
`rufus-syslinux`, `test_mbr_handoff_guest` and `udisks_fixture`. Pass the same
qualified upstream source for the generated ISO's modules:

```sh
python3 linux/tests/vm/mbr.py --build /tmp/rufus-linux-qt6-build \
  --kernel 6.18.52-1-cachyos-lts --accel tcg --install \
  --upstream /path/to/syslinux-6.04-pre1 /tmp/rufus-syslinux-device
```

`--installer /path/to/rufus-syslinux` selects a separately built qualified native
executable, useful when testing a sanitized adapter against the unchanged native
installer. This mode needs mtools, bsdtar and nasm on the host. It generates a
small nested-config ISO, then composes real MBR preparation, FAT formatting,
mounted shared extraction, sync/unmount, claim reacquisition and native installation
inside the guest. The private fixture bus has the same boundary described above.

Ten cases cover implicit file-mode refusal of devices, wrong device/capacity/disk
sequence/extent/volume ID, changed MBR after installation, cancellation, sync
failure and success. A test mcopy wrapper attempts another exclusive open from
inside the installer process group before executing the real mcopy. The original
claim must still block it. Every case checks descriptor flags/position, private
directory cleanup and absence of extra live or zombie children. Production code
contains no test bypasses; the installer and payload sources remain unchanged.

The host independently reads back every expected copied/generated file, checks
the MBR/alignment gap, and runs fsck on a separate copy with only the known backup
VBR mismatch normalized. The original backup VBR must remain unchanged. It then
boots the created whole disk through Syslinux/menu to `RUFUS_RAW_BOOT_OK`, using
a QEMU snapshot in the test directory and requiring an unchanged backing hash.
Logs, commands and hashes remain; ISO trees, disks, readback copies and guest
images are removed on success or failure. This qualifies a component composition,
not a real distribution or the application's authorized START route.

Use an installed kernel with matching modules and a new output directory. As
with the GPT guest, QEMU has no networking, passthrough or host-directory sharing;
only ordinary image files are attached. The native test refuses host invocation
before opening a block node. All eight block requests pass locally; no privilege
prompt or root process is needed on the host. The runner removes guest images,
initramfs trees and disks even on failure, retaining logs/manifests/results.
This is a partition-preparation checkpoint, not a helper/formatter/mount or
privileged Syslinux-installer test. Linux treats clearing/readback/sync failure
as fatal, including the backup-GPT area where Win32 permits a warning.

For CP12k's complete production-helper job, use `--helper` and also build
`rufus-prepare-helper`:

```sh
PYTHONPATH=linux/tests python3 linux/tests/vm/mbr.py \
  --build /tmp/rufus-linux-qt6-build \
  --kernel 6.18.52-1-cachyos-lts --accel tcg --helper \
  --upstream /path/to/syslinux-6.04-pre1 /tmp/rufus-syslinux-helper
```

This mode uses 1 GiB of guest RAM for the larger sanitized helper/initramfs. It
stages the native executable at the helper's compiled installation path (read
from the Ninja build) and requires the compiled mtools directory to be `/usr/bin`.
No host installation is performed. The fixture bus listens at the guest's real
system-bus socket because the production helper discards D-Bus environment
overrides. The test client supplies the fixture caller UID and retains the stdin
pipe through the job; this tests the root helper, not a real polkit prompt.

Five jobs cover a missing installed executable, an incompatible source companion,
cancellation while mcopy is blocked, and success with known and unknown detected
versions. Refusals hash the entire disk to prove no writes occurred. Cancellation
closes the normal protocol pipe during finalization and checks the disk is still
exclusively held until cleanup. Every job requires a single final result, correct
phase order, monotonic copy progress, helper exit, released mounts/claim, removed
temporary directories and no leftover descendants/descriptors. The final disk's
copied/generated files, MBR, alignment gap, FAT and boot are checked independently
as above. Source ISOs and all guest disks/staging are removed afterwards. This is
a generated boot fixture; distribution qualification and GUI BIOS enablement
are still separate checkpoints.

CP12l adds `--alpine /path/to/alpine-standard-3.22.5-x86_64.iso` in place of
`--helper` for the complete real-distribution path. It uses the existing
[Alpine ISO pin](alpine_iso.py), a 512-MiB simulated USB with 4-KiB clusters, and
2 GiB of RAM in the creation guest. The ISO is hash-checked before use; it is
never modified. The helper still runs its actual protocol, source/device checks,
formatter, extractor, installer and cleanup with fixture UDisks discovery.

```sh
PYTHONPATH=linux/tests python3 linux/tests/vm/mbr.py \
  --build /tmp/rufus-linux-qt6-build \
  --kernel 6.18.52-1-cachyos-lts --accel tcg \
  --alpine /path/to/alpine-standard-3.22.5-x86_64.iso \
  --upstream /path/to/syslinux-6.04-pre1 /tmp/rufus-alpine-syslinux
```

`syslinux_alpine.py` independently checks the complete file inventory, 122 ISO
files (including the virtual El Torito entries), the generated root config and
native root companion, exact MBR/geometry, alignment gap, BPB/backup VBR and
filesystem consistency. A second VM boots the resulting whole USB disk through
SeaBIOS with no source ISO, direct kernel or initrd attached. It selects Alpine's
`lts` entry through the Syslinux keyboard prompt, adding only
`console=ttyS0,115200` for observation. The media/config bytes remain unchanged.
The live system confirms release, architecture, BIOS mode, its `/dev/sda1` FAT
mount, modloop and six boot-file hashes, then powers off. The backing disk's hash
must be unchanged by the snapshot boot.

The boot observer uses QMP over pipes and QEMU's documented two-way serial FIFOs,
so it needs no host socket listener, root or graphical desktop. Startup, console
interaction and process cleanup are bounded; pipes and generated media are
removed on failure too. The fixture is deliberately a known Syslinux 6.04-pre1
source: it does not qualify other releases, arbitrary modules or a physical USB.
GUI START/authorization and real UDisks/desktop automount remain separate tests.

CP12p extends this same `--alpine` run with the existing Alpine UEFI verifier.
After BIOS succeeds, a fresh OVMF VM boots the same MBR/FAT32 USB with no source
ISO or direct kernel/initrd attached. It checks actual UEFI mode, the Alpine
release and architecture, the mounted USB/modloop and all six EFI/GRUB/kernel
boot-file hashes. Both firmware results must preserve the backing disk hash.
This adds `readback.uefi_boot` to `results.json` and `alpine-boot/` evidence beside
`alpine-bios/`. Unlike the BIOS observer's pipes, the existing UEFI observer uses
a local Unix serial socket, so the host test environment must permit that socket.
The firmware copies/hashes and QEMU command are recorded. Secure Boot enforcement,
other architectures and physical firmware are not qualified by this test.

## Installed runtime and Widgets

`installed.py` stages the complete CMake installation into a disposable guest,
including the actual policy without rewriting its helper path. Configure a
separate build with the distribution prefix `/usr`, the existing upstream wimlib
and Syslinux sources, and `RUFUS_BUILD_VM_TEST=ON`. Build `rufus-linux`,
`rufus-prepare-helper`, `rufus-syslinux`, `test_prepare_device_guest`,
`test_usb_guest` and `check_usb_mount` with Ninja's default parallelism.

```sh
PYTHONPATH=linux/tests python3 linux/tests/vm/installed.py \
  --build /tmp/rufus-installed-build \
  --kernel 6.18.52-1-cachyos-lts --accel tcg \
  --alpine /path/to/alpine-standard-3.22.5-x86_64.iso \
  --upstream /path/to/syslinux-6.04-pre1 /tmp/rufus-installed-guest
```

Use a new output directory. In addition to the common cached guest packages,
the fixture needs dosfstools, mtools and hivex. It creates a sparse 4-GiB system
disk and a 512-MiB USB, with no network, host disk or shared directory attached.
Allow about 7 GiB while the package tree and system image coexist.

The test first launches the installed executable as UID 1000, observes its main
window and closes it, with zero authorization requests. Creation runs through
the actual MainWindow/SelectionController in the QtTest guest harness, connected
to real UDisks and the installed production helper. This is a separate test from
external input automation of the installed executable. Both jobs choose the
default ISO mode and confirm MBR/FAT32/Syslinux preparation. Each needs exactly
one fresh native KDE authentication prompt with the uncached production policy.
The first job cancels during copying and checks cleanup; the second finishes
with a full READY bar after helper exit. The GUI remains unprivileged throughout.

The existing Alpine verifier checks every copied file, native boot payload,
partition layout and filesystem, then boots that USB through SeaBIOS and OVMF
into live Alpine userspace. The OVMF observer needs a local Unix serial socket.
Logs, package/input/installed-file hashes and startup, native
prompt, cancellation and completion screenshots remain in the output directory.
The runner removes guest disks, extracted trees and generated boot files even on
failure. Desktop automount, physical unplugging, Secure Boot enforcement and
arbitrary distributions are outside this fixture. CP12o ran the original BIOS
version; CP12p qualifies both firmware paths through `mbr.py --alpine` without
repeating the installed-GUI/authorization guest.

## Production device preparation

`helper.py --device` runs `rufus-prepare-helper` and its actual Qt process controller
with real authorization and root-side UDisks/kernel validation:

```sh
cmake --build /tmp/rufus-linux-qt6-build
python linux/tests/vm/helper.py --device --build /tmp/rufus-linux-qt6-build \
  --kernel 6.18.50-3-cachyos-lts --accel tcg /tmp/rufus-device-guest
```

The same offline guest recipe, regular-file USB and host readback tools apply.
Allow about 3 GiB during preparation; this mode removes the unpacked package copy
after building `system.raw`. The guest installs the generated production policy
with its helper path adjusted to `/opt/rufus`, plus a guest-only policy for the
separately linked cancellation probe. Both disable authorization caching.

Nine independent jobs each make exactly one authorization request: cancel while
the real dialog is open; reject changed disk sequence; reject changed identity;
reject an ordinary-file request sent to the elevated helper; cancel an active root
formatter; kill its ordinary-user GUI caller; complete native GPT/FAT32 preparation;
refuse a mounted target; and fail without retry when the agent is absent. The root
observer verifies whole-disk hashes for the initial refusals and mounted refusal.
The slow formatter ignores TERM, so cancellation must use the existing grace-period
KILL and reap path. No fault override is compiled into the production helper.

The final disk is independently checked for GPT/FAT32 metadata, backup copies,
fsck/mtools results and preserved surrounding bytes. The artifacts listed above
are retained for normal and sanitizer runs. The changed-sequence test modifies
the expected identity; it does not simulate physical removal during the ownership
handoff. This is the preparation component, not application START or ISO boot.

## Authorized FAT16 preparation

`helper.py --fat16` runs the same nine preparation/authorization scenarios on a
64 MiB virtual USB, with 2 KiB FAT16 clusters. It uses the existing GPT worker,
mkfs.fat partition-descriptor handoff and disk reacquisition; authorization is
still once per job. The host verifies both GPT copies, FAT16 type, cluster size,
hidden sectors, volume ID/label, fsck.fat, mtools and surrounding bytes.

```sh
python3 linux/tests/vm/helper.py --fat16 --build /tmp/rufus-linux-qt6-build \
  --kernel 6.18.52-1-cachyos-lts --accel tcg /tmp/rufus-fat16-device-guest
```

Build with `RUFUS_BUILD_VM_TEST=ON` first. Without `--extract` or `--uefi`, this
qualifies device preparation only. Add `--uefi` for the generated EFI fixture:

```sh
python3 linux/tests/vm/helper.py --fat16 --uefi \
  --build /tmp/rufus-linux-qt6-build --mkisofs /path/to/mkisofs \
  --kernel 6.18.52-1-cachyos-lts --accel tcg /tmp/rufus-fat16-uefi-guest
```

The combined run checks changed/unsupported/root-owned sources, cancellation,
caller death and mount/cleanup failures before creating the final EFI USB. Each
of the nine jobs requests authorization once. Final creation uses ISO preflight,
the production helper and its existing vfat mount/copy/finalization path. Host
readback checks every copied file; OVMF boots the result and the missing-payload
negative control described below. FAT16 Syslinux and GUI START remain outside
this fixture. `--fat16 --extract` runs the component copy tests without booting.
Keep logs, manifests, authentication screenshot and `results.json`, then remove
the disposable guest disks, copied trees and boot files.

## Authorized NTFS preparation

`helper.py --ntfs` uses the same nine preparation/authorization scenarios with
two GPT partitions, 4 KiB NTFS clusters and a Unicode label. The guest needs the
cached installed `ntfsprogs-plus` package and kernel `ntfs3`/`nls_utf8` modules.
No host installation or mount is needed. CP11e adds five boot-payload jobs, for
14 independent requests in this mode; each still authorizes exactly once.

```sh
PYTHONPATH=linux/tests python linux/tests/vm/helper.py --ntfs \
  --build /tmp/rufus-linux-qt6-build --kernel 6.18.50-3-cachyos-lts \
  --accel tcg /tmp/rufus-ntfs-helper-guest
```

The cancellation probe verifies that the helper retains its exclusive disk
claim when launching mkntfs. The production helper formats the main NTFS
partition while holding that claim. The root observer then mounts it read-only
with NTFS3 and independently attempts formatting through the same `/proc/self/fd`
adapter path, without force. Both the formatter and subsequent helper request
must refuse it, preserving the whole disk hash. This checks native mkntfs's
mounted-device refusal in addition to the helper's UDisks check.

The guest-only `prepare_boot_probe` additionally cancels after a 4 KiB boot write,
kills the ordinary caller at that point, reports a flush failure, corrupts a byte
before readback, and completes a checked write. It hashes every byte outside the
payload before/after each operation, including all NTFS bytes, both GPT copies
and unused boot-partition padding. It also confirms the disk claim is retained.
The production helper then completes formatting and payload writing, using the
embedded resource without a source-tree asset in the guest. The controller must
observe the bootloader phase. Temporary resource copies must be removed after
normal completion, failure and caller death.

Host readback checks both GPT copies/entries/attributes, native NTFS geometry,
Unicode label, backup boot sector and clean metadata with ntfsinfo/ntfslabel and
ntfsck. `--ntfsinfo`, `--ntfslabel` and `--ntfsck` override those tool paths.
Host comparison requires exact bundled payload bytes and preserved sentinels
everywhere else outside the formatted main partition and GPT, including unused
UEFI:NTFS padding. Add `--extract` for the shared extraction scenarios described
below. Windows Widgets and boot options remain separate.

## Authorized device extraction

`helper.py --extract` selects a separate set of nine jobs in the same production
helper/controller guest. It creates ordinary ISO fixtures using mkisofs and adds
a guest-only `prepare_extract_probe`, linked with mount/write/flush/unmount fault
wrappers. No fault controls or additional authorization requests enter production.

```sh
cmake --build /tmp/rufus-linux-qt6-build
python linux/tests/vm/helper.py --extract --build /tmp/rufus-linux-qt6-build \
  --mkisofs /tmp/rufus-test-deps/usr/bin/mkisofs \
  --kernel 6.18.50-3-cachyos-lts --accel tcg /tmp/rufus-extract-guest
```

The first three jobs reject changed source metadata, an unsupported symlink in
the ISO tree and a root-owned source. The root observer checks that these leave
the entire USB unchanged. The next jobs cancel during an actual FAT file write
and kill the ordinary GUI caller during that write. While the copy is paused,
the root observer verifies that the temporary FAT mount exists only in the helper's
namespace, with `nodev,nosuid,noexec`. EOF must release it without another prompt.

Three more jobs inject mount, filesystem flush and unmount failure. The controller
must report failure, including the explicit flush/cleanup error. An unmount failure
can leave an empty `/run/rufus-*` directory when namespace exit releases the mount;
no recursive removal or lazy detach hides this failure. The last job uses the
production helper with stock tools and completes preparation and extraction.

Each job makes exactly one real polkit request with caching disabled. After shutdown,
the host checks GPT/FAT metadata and preserved surrounding bytes, runs fsck, copies
the FAT contents out with mcopy and checks every filename, byte and empty directory
against the generated source. This includes UTF-8/long names, Rufus substitutions
and the root-loader exclusion. Artifacts also include `fixtures/` and `copied/`.
This qualifies ordinary tree copying; a bootable ISO workflow remains separate.

## Authorized NTFS extraction and fallback

`helper.py --ntfs --extract` runs those same nine extraction jobs against NTFS,
including the unchanged embedded UEFI:NTFS payload. Use a kernel with the new
`ntfs` driver (7.1 or newer) and provide matching NTFS-3G and libntfs-3g package
archives for the disposable guest. They are unpacked there and hashed in its
manifest; nothing is installed on the host.

```sh
PYTHONPATH=linux/tests python linux/tests/vm/helper.py --ntfs --extract \
  --build /tmp/rufus-linux-qt6-build --mkisofs /path/to/mkisofs \
  --ntfs-3g-package /path/to/ntfs-3g.pkg.tar.zst \
  --ntfs-library-package /path/to/libntfs-3g.pkg.tar.zst \
  --kernel 7.2.5-1-cachyos --accel tcg /path/to/new-ntfs-extraction-guest
```

Repeat with `--ntfs-fallback --kernel 6.18.50-3-cachyos-lts` to require the real
NTFS-3G fallback. This flag changes the fixture's expectations, not the production
helper's selection. The fallback uses the package's `lowntfs-3g` with
`ignore_case` and `no_detach`. A guest-only shell wrapper injects its startup
failure; all other calls execute the packaged binary. Kernel mount failures use
the existing linked probe. The root observer verifies the selected filesystem
type, private mount flags and absence of leftover FUSE daemons or payload
temporaries after the jobs.

After successful extraction, the guest independently remounts read-only using
`ntfs3` and exports the tree to `ntfs-tree.tar`. That remount must preserve the
whole-disk hash. Host checks compare every filename, file byte and empty
directory against the source, plus native NTFS metadata, both GPT copies, the
exact boot image and all padding/sentinels outside the main filesystem. Repeat
both provider runs with the sanitizer build. This fixture qualifies ordinary
extraction and lifecycle behavior. The large-file variation below checks the
complete ISO-to-NTFS path; Windows Setup remains a separate checkpoint.

### Files beyond 4 GiB

Add `--large-file iso` or `--large-file udf` to `helper.py --ntfs --extract`.
These select two jobs: FAT32 must reject the large file without changing a disk
byte, then the production NTFS helper completes creation with one authorization.
Run both views with the new kernel driver and with `--ntfs-fallback` on the older
kernel. The same commands accept the sanitizer build directory.
Prefer `--accel kvm` and check `/dev/kvm` in the environment that actually launches
QEMU. A tool sandbox may hide that device even when the host can use it.

The fixture contains a **4 GiB + 1 MiB + 123-byte** file, distinct markers in
every MiB, an unaligned tail, ordinary small files and empty directories. The
source is sparse, but extraction reads and writes the whole file normally.
mkisofs uses ISO level 3 for the large ISO9660 file. The UDF variation changes
only the UDF filename, with its descriptor checksum repaired, so a silent switch
to the ISO9660 view fails validation. Reader probes check ranges across 1, 2
and 4 GiB before starting the VM.

The guest uses a 6 GiB simulated USB and an 8 GiB sparse system disk. Its root
observer checks the production helper's private mount and selected driver. The
controller must report the exact copied byte count, all seven phases and clean
finalization. Independent readback through read-only `ntfs3` hashes every byte
of the large file and checks its exact size. The small tree is exported and
compared on the host as above. GPT, boot payload, native NTFS metadata, unchanged
source images, surrounding sentinels and unchanged disk bytes during readback
are also checked.

Whole-disk equality checks use BLAKE2 in this large fixture to reduce CPU
emulation overhead. The extracted file and host input manifests retain SHA-256.
Test deadlines are extended only for the large fixture; production timeouts
and the ordinary guest jobs are unchanged.

Artifacts include `fixtures/large-file.json`, `fixtures/large-ranges.log`,
`large-file.sha256`, `large-file.size` and the usual manifest/results. To avoid
duplicating large data, the checked temporary `volume.raw` is removed; `usb.raw`
retains its complete bytes. The large file is hashed in place instead of also
exporting it in the tar archive. Allow roughly 16 GiB of host storage per run
and up to 90 minutes for a software-emulated guest. NTFS-3G copying is particularly
slow under CPU emulation; its individual copy job allows one hour. This qualifies streaming
large installation-sized files; it does not itself qualify a WIM or Windows boot.

When space is limited, run one large guest at a time. After a successful run,
keep the logs, `results.json`, input manifests and checksum reports; the generated
`system.raw`, `usb.raw` and `fixtures/LARGE.iso` can be removed once no guest or
host verification is using them. The fixture source and recipe reproduce these
files. Failed runs should retain their disks until the failure is investigated.

## UEFI boot after authorized extraction

`helper.py --uefi` selects one production preparation/extraction job and then boots
its resulting USB image. It reuses the same desktop authorization guest and native
kernel/module recipe. Additional local inputs are systemd's installed
`/usr/lib/systemd/boot/efi/systemd-bootx64.efi`, the x86-64 OVMF firmware used by the
OpenWrt tests, gzip/cpio and mtools (`mdel` for the negative control).

```sh
cmake --build /tmp/rufus-linux-qt6-build
LD_LIBRARY_PATH=/tmp/rufus-test-deps/usr/lib python linux/tests/vm/helper.py --uefi \
  --build /tmp/rufus-linux-qt6-build --mkisofs /tmp/rufus-test-deps/usr/bin/mkisofs \
  --kernel 6.18.50-3-cachyos-lts --accel tcg /tmp/rufus-uefi-iso-guest
```

The source ISO contains the stock EFI loader, a loader entry using local paths,
the native kernel, a small initramfs and a payload checksum manifest. It is a
generated file-tree fixture with no optical boot catalog or distribution installer.
The shared Rufus scan must recognize its EFI fallback path and complete tree.
The production helper then partitions, formats, mounts, copies, flushes and
unmounts with **one** uncached authorization. Host readback checks GPT/FAT metadata,
preserved surrounding bytes, fsck and every copied file hash/directory.

A fresh OVMF VM receives only that USB disk and fresh firmware variables: no
direct QEMU kernel/initrd, creation-guest disk, source ISO or network. Linux must
confirm UEFI and its kernel release, mount the USB FAT partition read-only,
verify the extracted payload hashes and power off. A second snapshot boots a copy
with the required marker deleted; it must explicitly report the missing payload
and fail. Both backing files must stay unchanged during boot. QEMU's temporary
snapshot files stay in the corresponding output directory.

The source loader entry includes `rufus.fixture_label=RUFUS_UEFI`. The shared
label passes must change it to `rufus.fixture_label=RUFUS\x20TEST` for the formatted
USB. Independent readback verifies that exact config and the source's unchanged
bytes; the booted kernel verifies the changed command-line value. The checksum
manifest deliberately describes the expected USB config, alongside unchanged
payload files. `RUFUS_UEFI_ISO_PATCHED_LABEL` records this boot check.
The fixture also includes an EFI `isolinux.cfg` and a GRUB `set` volume reference.
The helper must duplicate the patched EFI config, patch the FreeNAS reference and
keep the original root `syslinux.cfg`. Independent host readback and checksums in
the booted system verify all those files. The boot remains systemd/UEFI; the added
config files do not imply execution or qualification of Syslinux or FreeNAS.

Artifacts add `uefi-fixture.json`, `source-scan.log`, `uefi-boot/`,
`uefi-missing-payload/` and boot results in `results.json`. Each boot directory
records the exact QEMU invocation, copied firmware hashes and serial output.
Repeat with the sanitizer build to exercise the instrumented production helper
and controller; the booted Linux kernel itself is the normal installed kernel.

This qualifies the existing 512-byte-sector GPT Basic Data/FAT32 layout (or
FAT16 with `--fat16`) on x86-64 OVMF. EFI presence alone does not establish distribution compatibility: label/UUID
references, config fixes, native installers, other firmware and Secure Boot remain
separate qualification. Add `--widgets` to exercise application ISO START as below.

## Widgets START through UEFI boot

`helper.py --uefi --widgets` uses the same generated payload and independent
readback/boot checks, but drives `MainWindow` and `SelectionController`. It keeps
live UDisks discovery connected while selecting the device/image, editing the
label, pressing START and accepting the destructive confirmation. It types
`rufus test`, verifies `RUFUS TEST` in confirmation and independently checks that
normalized label in the filesystem and patched boot configuration. The guest runs
as uid 1000, requires zero authorization requests before confirmation and exactly
one uncached request for the complete helper job, and checks READY/100% only after
the helper exits successfully. Its random planned FAT volume ID is independently
checked against the written boot sector.

```sh
python linux/tests/vm/helper.py --uefi --widgets \
  --build /tmp/rufus-linux-qt6-build --mkisofs /path/to/mkisofs \
  --kernel 6.18.50-3-cachyos-lts --accel tcg /path/to/new-widgets-guest
```

The output directory must not exist. Prefer persistent storage with room for the
3 GiB creation disk and unpacked package tree. Keep the build unchanged throughout
the run: the harness checks input hashes after creation. Extra artifacts are
`iso-confirmation.png`, `iso-complete.png` and the
`RUFUS_ISO_WIDGETS_PASSED REQUESTS=1` marker in `serial.log`.
Repeat with the sanitizer build. The generated ISO has only the ordinary ISO
choice; separate private-bus Widgets tests cover hybrid ISO-default/disabled-ESP/DD
selection, stale inputs, errors and cancellation without extra authorization.

## Real Alpine ISO extraction and UEFI boot

`helper.py --alpine <iso>` uses the official Alpine standard 3.22.5 x86-64 ISO.
Download it from the [Alpine release directory](https://dl-cdn.alpinelinux.org/alpine/v3.22/releases/x86_64/).
The fixture checks the complete image against its pinned
[published SHA-256](https://dl-cdn.alpinelinux.org/alpine/v3.22/releases/x86_64/alpine-standard-3.22.5-x86_64.iso.sha256):

```text
8844f664dffbf50964ba33fe67658dd5d61c8f02d88e4febd17cf2c4334983c4
```

```sh
cmake --build /tmp/rufus-linux-qt6-build
LD_LIBRARY_PATH=/tmp/rufus-test-deps/usr/lib python linux/tests/vm/helper.py \
  --alpine /tmp/rufus-alpine-source/alpine-standard-3.22.5-x86_64.iso \
  --build /tmp/rufus-linux-qt6-build --mkisofs /tmp/rufus-test-deps/usr/bin/mkisofs \
  --kernel 6.18.50-3-cachyos-lts --accel tcg /tmp/rufus-alpine-guest
```

Use a new output directory and allow roughly 4 GiB of temporary space for one run.
The same cached desktop packages, native mkfs/mtools and OVMF prerequisites apply.
The guest receives a 512 MiB emulated USB and runs one production helper job with
one uncached authorization. The target FAT label deliberately differs from the
source ISO label. Original image boot configurations and loaders remain unchanged;
the helper also generates the root `syslinux.cfg` redirect using Rufus's shared
rule. No native Syslinux/GRUB installer is launched.

Before the job, bsdtar independently supplies the expected ordinary file hashes
and directory names. Rufus's bundled reader also exposes two virtual El Torito
files under `[boot]`; the fixture independently reads the pinned ISO's boot
catalog and hashes their referenced byte ranges. All 122 source files and nine
directories must match after copying them back with mcopy, including dotfiles,
packages, configurations and the original boot payload. GPT/FAT metadata, fsck
and bytes outside the prepared partition are checked separately. An independent
expected string verifies the additional root `syslinux.cfg`, pointing to
`/boot/syslinux/syslinux.cfg` with `/boot/syslinux/` as its APPEND directory.

The boot VM receives only the written USB and fresh OVMF variables, with no source
ISO, direct kernel/initrd, injected boot arguments or network. The stock Alpine
initramfs provides its serial login. The test logs in as the live image's root user,
checks the installed Alpine release and architecture, actual UEFI boot, the mounted
USB's FAT filesystem, hashes of the original boot files and the mounted squashfs
modloop. It then powers off and verifies that the backing disk remained unchanged.

Artifacts include `alpine-source.json` (source identity, labels, file hashes and
boot-catalog ranges), `source-preflight.log`, the usual authorization/readback
evidence and `alpine-boot/` (firmware hashes, invocation, guest checks and serial
log). Repeat with the sanitizer build for the instrumented creation path. This
tests Alpine's UEFI live boot; BIOS, installation, Secure Boot and distribution
images requiring configuration changes are separate work.

The pinned EFI executable embeds a search for `alpine-std 3.22.5 x86_64` in addition
to the separate on-disk `grub.cfg`. Because the FAT label is `RUFUS TEST`, GRUB
prints `error: no such device` before proceeding through the selected USB's menu
and completing boot on OVMF. The test retains this warning; it neither patches
the executable nor suppresses its output. See Alpine's
[image construction recipe](https://raw.githubusercontent.com/alpinelinux/aports/3.22-stable/scripts/mkimg.base.sh).

## Native NTFS formatting and large files

`ntfs.py` formats a disposable 6 GiB partition-sized file using the built
`ntfs_format_probe` and the native `mkntfs`. It verifies the filesystem with
`ntfsinfo`, `ntfslabel` and optionally `ntfsck -n`, then attaches that file as
the only emulated USB in an offline Linux guest. The fixture needs the usual
QEMU/kernel/initramfs prerequisites plus the kernel's `ntfs3` and `nls_utf8`
modules. It does not need a desktop root filesystem or physical USB.

```sh
LD_LIBRARY_PATH=/home/marius/proj/lufus/.rufus-test-artifacts/deps/usr/lib \
  python linux/tests/vm/ntfs.py --build /tmp/rufus-linux-qt6-build \
  --kernel 6.18.50-3-cachyos-lts --accel tcg --fsck /usr/bin/ntfsck \
  /tmp/rufus-ntfs-guest
```

Use a fresh output directory and allow at least 6 GiB of temporary disk space.
The guest deadline is five minutes because NTFS3 may zero the full 5 GiB gap
through software-emulated USB; this is separate from the formatter's timeout.
`--fsck` is optional for providers without a read-only `ntfsck`. Other formatter
and metadata-tool paths can be supplied with `--mkfs`, `--info` and `--label`.
Repeat with the sanitizer build and the usual ASan/UBSan environment.

The PID-1/DMI/command-line-guarded guest mounts with NTFS3, writes distinct marker
blocks at the beginning and 5 GiB offset of a Unicode-named file, cleanly unmounts,
remounts read-only and checks its 5 GiB + 512-byte length, both markers and zero
blocks at 2/4 GiB. The local kernel allocates the intervening space. This tests
large file offsets and persistence; it does not perform Rufus ISO extraction or
hash every byte of a Windows installation WIM. Host tools recheck clean metadata
after shutdown. No host filesystem is mounted or host block device opened.

Artifacts include the formatter diagnostics, kernel/initramfs/executable hashes,
before/after image hashes, exact QEMU command, serial log and results JSON. This
fixture directly boots a Linux kernel to test storage. UEFI:NTFS handoff, Secure
Boot and Windows Setup boot are separate checkpoints.

## UEFI:NTFS firmware handoff

`uefi_ntfs.py` combines the native GPT, NTFS formatter and bounded payload writer
on a disposable 256 MiB disk. It uses the original `res/uefi/uefi-ntfs.img` bytes
and the installed `systemd-bootx64.efi` to launch a generated Linux EFI tree from
NTFS. Requirements are those of `ntfs.py`, plus systemd's EFI loader and the x64
OVMF files used by the existing UEFI fixtures. No host mounts, package installs
or physical USB devices are used. Allow about 2 GiB of temporary space.

```sh
LD_LIBRARY_PATH=/home/marius/proj/lufus/.rufus-test-artifacts/deps/usr/lib \
  python linux/tests/vm/uefi_ntfs.py --build /tmp/rufus-linux-qt6-build \
  --kernel 6.18.50-3-cachyos-lts --accel tcg --fsck /usr/bin/ntfsck \
  /tmp/rufus-uefi-ntfs-guest
```

The output directory must not exist. Repeat with the sanitizer build and usual
ASan/UBSan environment. Native tools verify NTFS metadata, and an independent
parser plus sfdisk check both GPT copies. The payload bytes and untouched boot
partition padding are compared directly. A guarded preparation guest mounts
only the emulated main partition and copies the generated files; all bytes
outside that partition must remain unchanged.

Firmware qualification runs separately with only the prepared USB and fresh
OVMF variables, without `-kernel`, `-initrd`, a source ISO or networking. Serial
output must show UEFI:NTFS, and the booted Linux guest must verify actual UEFI
boot, expected kernel, NTFS mounting and SHA-256 hashes of every fixture file.
A second image missing the required payload marker must boot but report failure.
Firmware runs use snapshots and leave the verified backing images unchanged.

Artifacts include source/tool/kernel/initramfs hashes, formatter and payload
write output, partition extents, firmware copies/hashes, exact invocations,
serial logs, the final disks and a results JSON. The logs identify bundled
UEFI:NTFS 2.8 and NTFS driver 1.9. Secure Boot is disabled. This is boot-chain
qualification; production-helper NTFS extraction is tested separately above, while Windows Setup installation
remain separate work.

## FAT32 installation-WIM splitting

`wim_split.py` qualifies the production helper with one real Windows ISO whose
conventional `sources/install.wim` exceeds FAT32's file limit. It sets the separate
enhanced-installation request (Win32 Alt+E), with zero customization flags, and
requires one real polkit authorization. It does not enable the GUI preference
or perform another Windows installation/customization trial.

Build the normal device-guest targets plus `iso_write_check_probe`,
`wim_split_source_probe` and `wim_split_verify`. The last target requires
`RUFUS_COMPARE_SYSTEM_WIMLIB=ON` and links the external system library, without the
Rufus reader or splitter adapter. If optional native libraries live in a local
dependency directory, set `LD_LIBRARY_PATH` for the recipe so it can copy their
runtime dependencies into the guest. The installed guest kernel needs 9p and
9pnet_virtio support in addition to the existing USB/FAT32 modules.

```sh
python3 linux/tests/vm/wim_split.py \
  --build /tmp/rufus-linux-qt6-build --kernel 7.2.6-1-cachyos --accel kvm \
  --image /path/to/Windows11.iso \
  --source-manifest /path/to/prior-creation/windows-source.json \
  /path/to/new-fat32-split-test
```

The prior ordinary-creation manifest comes from `helper.py --windows` below. The
recipe hashes the whole source ISO before reusing its independent 7-Zip file
inventory. It exports the ISO's parent directory read-only with QEMU 9p and mounts
that share read-only in the guest; no installation-sized source copy is needed.
Only the named ISO is used. The guest uses a 3 GiB system image and 16 GiB sparse
simulated USB; the recipe requires 24 GiB free space before starting.

After helper exit, sync and private-mount cleanup, readback write-protects both
disk and partition, runs `fsck.fat -n`, and mounts FAT32 read-only. Every ordinary
file must retain its SHA-256 and every directory must match the source. Each SWM
must fit FAT32 and their total must fit preflight's reservation. System wimlib
decompresses and verifies every image/resource across all parts. Image XML,
metadata hashes and resource hashes/sizes must match the original archive read
directly through the ISO reader. GPT copies and guard bytes are checked separately.
A whole-disk checksum must remain unchanged across verification.

On success, the recipe saves logs, inventories, checksums, input versions and the
authentication screenshot, then removes its disks and boot files. A failure
retains the fixture for diagnosis; clean it up after saving the relevant evidence.
The source ISO is hashed again and preserved. This is media-creation/data-integrity
qualification, not Windows Setup consumption of SWM files or physical USB testing.

For an immediately following Setup boot check, add `--keep-usb-for-boot`. The
recipe still removes its system disk and boot files after successful verification,
but retains `usb.raw` and records its purpose in `retained-usb.json`. Boot it with:

```sh
python3 linux/tests/vm/windows_setup.py --boot-only \
  --usb /path/to/new-fat32-split-test/usb.raw /path/to/new-fat32-setup-boot
```

This mode attaches no installation disk and limits the VM to ten minutes. Observe
Setup, record screenshots and any read-only WinPE checks, then shut down without
installing Windows. Reaching Setup or listing its editions proves boot and metadata
discovery; it does not prove that Setup can apply every resource across all SWM
parts. The independent system-wimlib verification above covers their data integrity.
Remove the retained USB and boot VM overlay/state after exporting evidence. Keep
the source ISO. Do not turn this check into another customization trial.

## Real Windows 11 creation and observed installation

`helper.py --windows` uses the actual Widgets selection, preflight, confirmation
and one native polkit authorization to create GPT/NTFS + UEFI:NTFS media. Supply
a local Windows 11 x64 ISO; no Windows image is downloaded. This fixture uses a
16 GiB guest system image and a 16 GiB simulated USB. KVM is recommended for the
installation-sized workload. The existing NTFS guest dependencies are required,
plus host 7-Zip for an independent UDF source reader.

```sh
PYTHONPATH=linux/tests python3 linux/tests/vm/helper.py \
  --windows /path/to/Windows11.iso --build /tmp/rufus-linux-qt6-build \
  --kernel 7.2.5-1-cachyos --accel kvm \
  --ntfs-3g-package /path/to/ntfs-3g.pkg.tar.zst \
  --ntfs-library-package /path/to/libntfs-3g.pkg.tar.zst \
  /path/to/new-windows-creation
```

The host records every source file's SHA-256 through 7-Zip's UDF reader, checks
Rufus's preflight count/size and records the ISO hash. The Qt test requires known
Windows 11 metadata, automatic NTFS selection, successful cleanup and exactly
one authorization. It clears the Windows User Experience checkboxes for the
ordinary baseline. After helper cleanup, the verifier write-protects the disk and
main partition at the block layer and checks their read-only status. A separate
read-only mount through the kernel NTFS3 driver
hashes every created file. Names, directories and hashes must match the source.
The whole-disk checksum must remain unchanged during readback and match the
final host image; `created-disk.checksum` preserves the pre-readback value.
The host also checks both GPT copies, partition extents, the unchanged bundled
UEFI:NTFS bytes and padding, and clean NTFS metadata. Temporary reference-tree
and partition copies are deleted; manifests, logs, screenshots and `usb.raw`
remain. This stage does not boot Windows.

Once verification completes, retain the logs, manifests, checksum inventories and
screenshots, then remove `system.raw`, temporary partition copies, generated
boot/initramfs files and extracted reference trees. Keep `usb.raw` only when it
is needed for the next Windows Setup qualification. After that qualification,
remove its completed installation disk and USB overlay too. User-provided ISOs
are inputs and must be preserved. Do not retain successive installation-sized
fixtures after their checkpoints have finished.

Add `--windows-privacy` to the same command to select only **Disable data
collection (Skip privacy questions)** through the real Widgets dialog. The guest
also checks that regional copying is disabled and that no authorization has
occurred while choosing options. It saves `windows-options.png`. Independent
NTFS3 readback must show all original files unchanged, plus exactly
`sources/$OEM$/$$/Panther/unattend.xml` and its new parent directories. The host
parses the answer file and requires the original AMD64 oobeSystem privacy
settings, with no extra customization elements. `windows-customization.json`
records the checked answer-file hash. Authorization must still occur once.
CP11n passed this privacy-only media creation/readback test. It fulfills the
single customization qualification requested by the user. Its effect inside
installed Windows remains unverified and deferred; do not run more
customization VM/installation tests unless requested.

The following runner documents the ordinary installation baseline already
qualified in CP11k. It uses the verified USB through a writable copy-on-write overlay
and a new sparse 80 GiB install disk. Windows PE refused the hardware-write-
protected NTFS fixture with a write-protection error despite a clean CHKDSK;
the overlay permits normal mount behavior without changing verified source bytes.
It needs QEMU, KVM, `qemu-img`, `swtpm` and x64 OVMF in
`/usr/share/OVMF/x64`. Use `OVMF_CODE.secboot.4m.fd` with SMM and secure
pflash, plus a fresh `OVMF_VARS.4m.fd`. The ordinary CODE build lacks Secure
Boot capability and Windows 11 refuses it. The capable build remains in Setup
Mode without enrolled keys; Secure Boot enforcement is disabled for this baseline.
The runner supplies four vCPUs, 8 GiB RAM and a fresh virtual TPM 2.0. No source
ISO, physical disk, host TPM, network or host shared directory is attached.

```sh
python3 linux/tests/vm/windows_setup.py \
  --usb /path/to/new-windows-creation/usb.raw /path/to/new-windows-setup
```

The runner exposes a local `qmp.sock` for screenshots and input, records its
commands/firmware hashes and supervises both QEMU and the TPM for up to one hour,
including interactive Setup (ten minutes with `--boot-only`, without an install
disk). The following installation steps describe CP11k's existing baseline and
do not apply to a boot-only check.
Observe Setup, install to the new virtual disk, detach the USB and record evidence
of the installed OS boot. The baseline endpoint is the installed Windows
first-use region screen (OOBE), with its version and `C:\WINDOWS` recorded;
account setup and activation are outside this fixture. Reuse `run.py`'s QMP
command function for control. Save screenshots and exact commands as test
evidence; process exit alone does not
mean installation passed. Shut down through Windows or QMP when finished. The
runner checks that the verified USB hash is unchanged after shutdown.

The runner appends `process-status.jsonl` records when the VM starts and exits,
and before and after cleanup. They retain both child PIDs, exit codes, timestamps
and any active exception. SIGTERM unwinds through cleanup and still exits with
status 143. Another SIGTERM or Ctrl+C during cleanup is deferred until both
children have been reaped; the existing five-second stop deadline then SIGKILL
fallback applies to each child. An interrupted run is never a successful test
return, and these process records do not replace boot screenshots or USB hashes.

`ctest --test-dir <build> -R '^rufus_vm_supervision$' --output-on-failure` tests
this lifecycle with tiny Python processes and a local Unix socket. It requires
no QEMU, Windows ISO, disk image, root or KVM. CP11ai also checks normal and
interrupted shutdown with diskless QEMU/TPM. No new Windows installation or
customization run is needed for supervisor changes.

The ordinary recipe remains uncustomized. The optional privacy recipe adds an
answer file only on the created USB; neither recipe modifies the source ISO.
The privacy creation/readback test is the one customization qualification in
scope; additional installation and per-option customization tests are deferred.


## FAT16 MBR/Syslinux device qualification (CP15i)

The existing MBR fixture accepts `--fat-bits 16` with `--handoff`, `--install`
or `--helper`. It uses a 64 MiB disposable USB disk and 2 KiB clusters. FAT32
remains the default. FAT16 table-only and optional Alpine modes are not part of
this fixture extension.

```sh
cmake -S linux -B /tmp/rufus-linux-qt6-build -DRUFUS_BUILD_VM_TEST=ON
cmake --build /tmp/rufus-linux-qt6-build
PYTHONPATH=linux/tests python3 linux/tests/vm/mbr.py \
    --build /tmp/rufus-linux-qt6-build --kernel 6.18.52-1-cachyos-lts \
    --accel tcg --helper --fat-bits 16 \
    --upstream ../.rufus-test-artifacts/deps/syslinux-6.04-pre1 \
    /tmp/rufus-fat16-helper
```

Use an installed kernel with matching modules and a new output directory. Replace
`--helper` with `--install` for the 11 installer/ownership fault cases. Use
`--handoff` for the 13 formatter/mount cases; that mode needs no upstream tree.
The helper runs five jobs, including preflight refusals and cancellation.
Successful installer/helper outputs receive independent filesystem/file/MBR
readback and a generated BIOS boot. FAT16 has no backup VBR; its fsck check uses
an unmodified volume copy. FAT32 keeps the documented backup normalization on a
copy. All guest/media files are removed automatically; logs and manifests remain.

For instrumented builds, pass `--installer` pointing to the qualified normal
`rufus-syslinux` executable, as in CP12e/CP15h. Our helper, adapters and linked
Rufus code remain instrumented; no sanitizer suppression is used. This does not
clear the upstream executable's recorded alignment and exit-time leak findings.
Return `RUFUS_BUILD_VM_TEST` to OFF after qualification. Qt FAT16 MBR exposure is
a separate checkpoint.


## Non-bootable GPT/exFAT helper qualification (CP16d)

The existing offline MBR/handoff fixture also runs GPT/exFAT jobs. It needs no
ISO or Syslinux source. `--exfat` tests CP16c's partition adapter and exclusive
claim handoff (512-byte and rejected 4-KiB sectors); `--exfat-helper` tests CP16d's
complete client/helper operation on a 64-MiB emulated USB with 128-KiB clusters.

```sh
cmake -S linux -B /tmp/rufus-linux-qt6-build -DRUFUS_BUILD_VM_TEST=ON
cmake --build /tmp/rufus-linux-qt6-build --target test_mbr_handoff_guest prepare_exfat_probe rufus-prepare-helper udisks_fixture partition_image_probe
python3 linux/tests/vm/mbr.py --build /tmp/rufus-linux-qt6-build --kernel 6.18.52-1-cachyos-lts --accel tcg --exfat-helper /tmp/rufus-exfat-helper
cmake -S linux -B /tmp/rufus-linux-qt6-build -DRUFUS_BUILD_VM_TEST=OFF
```

Use a new output directory and an installed kernel with matching modules.
The helper recipe requires the compiled formatter path `/usr/bin/mkfs.exfat`.
The isolated guest supplies private UDisks properties and a root-only `pkexec`
stand-in, counts one launcher invocation per job, and delays launcher exit to
check that receiving a result cannot publish READY. This is not a desktop polkit
prompt test. Five preflight refusals hash the entire disk; the remaining jobs
cover native formatter failure/cancellation, final-sync failure/cancellation,
EINTR recovery and success. Final-sync fault wrappers are guest-only and not
linked into the production executable. Independent host inspection checks GPT,
boot copies/checksums, partition offset, cluster size, Unicode label and native
`fsck.exfat -n`/`dump.exfat` output. All disposable media and guest trees are
removed automatically; logs, manifests and hashes remain. No mount, copied-file
or boot qualification is claimed for this non-bootable format job.

## Non-bootable GPT/FAT16 and FAT32 helper qualification (CP16f)

The same source-free fixture now accepts `--fat-helper --fat-bits 16` or
`--fat-helper --fat-bits 32`. Build the targets shown above; this route also
requires the configured formatter path `/usr/bin/mkfs.fat`. No ISO or native
Syslinux payload is needed. For example, while VM targets are enabled:

```sh
python3 linux/tests/vm/mbr.py --build /tmp/rufus-linux-qt6-build --kernel 6.18.52-1-cachyos-lts --accel tcg --fat-helper --fat-bits 16 /tmp/rufus-fat16-helper
python3 linux/tests/vm/mbr.py --build /tmp/rufus-linux-qt6-build --kernel 6.18.52-1-cachyos-lts --accel tcg --fat-helper --fat-bits 32 /tmp/rufus-fat32-helper
```

Each run exercises ten jobs: missing formatter, bad label, invalid cluster,
changed disk sequence, formatter failure/cancellation, final-sync
failure/cancellation, interrupted-sync recovery and success. Four preflight
refusals verify the entire disk is unchanged. Every job checks one authorization
launcher, phase ordering, joined exit, released claims and process/descriptor
cleanup. Cancellation waits for both the worker marker and the client phase,
since their delivery is asynchronous. The shared guest-only final-sync probe
keeps its existing `prepare_exfat_probe` build target; its faults now apply to
both FAT and exFAT jobs and never enter the production helper.

Host readback independently verifies both GPT copies, FAT type, geometry,
cluster size, hidden-sector offset, label, serial, permitted track-rounding tail,
and FAT32's backup boot sector. `fsck.fat -n` must report a clean filesystem and
`mdir` must find no files. Formatting creates no bootloader and this checkpoint
makes no boot claim. Disposable disks, partition copies and guest trees are
removed automatically. Run `--exfat-helper` as the shared finalization regression.

## Non-bootable GPT/NTFS helper qualification (CP16h)

The same guest recipe accepts `--ntfs-helper`, requiring the configured native
formatter `/usr/bin/mkntfs` and host `ntfsinfo`, `ntfslabel`, `ntfsck` readback
tools. It uses the installed ntfsprogs-plus tools; no NTFS mount driver, source
ISO or desktop session is needed. Build the same guest targets listed for
CP16d/f, then run with a new output directory:

```sh
python3 linux/tests/vm/mbr.py --build /tmp/rufus-linux-qt6-build --kernel 6.18.52-1-cachyos-lts --accel tcg --ntfs-helper /tmp/rufus-nonbootable-ntfs
```

Ten source-free jobs cover missing formatter, invalid label/cluster, changed
disk sequence, formatter failure/cancellation, final-sync failure/cancellation,
interrupted-sync retry and success. The four preflight refusals hash the entire
disk before/after. Guest-only formatter wrappers verify the retained whole-disk
claim before every attempted formatting command; the client also checks it
while cancellation is pending. One authorization launcher is counted per job,
and completion must wait for launcher exit. Native metadata readback confirms
one GPT data partition, exact extent, NTFS 3.1, primary/backup boot sectors,
cluster size, partition-offset metadata, Unicode label and clean volume flags;
`ntfsck -n` must succeed. No mount/copy or OS boot qualification is claimed.

Before those jobs, one existing bootable NTFS preparation request verifies its
four original phases, including bootloader, and compares the written UEFI:NTFS
payload byte-for-byte against the repository resource. The fixture seeds both
kernel partition nodes and supplies their usual udev links. The new non-bootable
jobs then replace that layout with a single partition. Adapter checks also
reject a boot partition for non-bootable NTFS and a missing boot partition for
bootable NTFS before opening a device. The minimal guest includes `/tmp` for
Qt's existing embedded-payload temporary file. It does not change production
paths or bypass host authorization. Disks, copied volumes and guest trees are
removed automatically, including after failure; logs, manifests and hashes stay.


### Non-bootable MBR/FAT16 and FAT32 helper qualification (CP16k)

This extends CP16f's source-free guest with the plain MBR helper operations.
Build `rufus-prepare-helper`, `prepare_exfat_probe`, `test_mbr_handoff_guest`,
`udisks_fixture` and `partition_image_probe` with `RUFUS_BUILD_VM_TEST=ON`.
Use an installed kernel with matching modules and a new output directory:

```sh
python3 linux/tests/vm/mbr.py --build /tmp/rufus-linux-qt6-build --kernel 6.18.52-1-cachyos-lts --accel tcg --mbr-fat-helper --fat-bits 16 /tmp/rufus-mbr-fat16-helper
python3 linux/tests/vm/mbr.py --build /tmp/rufus-linux-qt6-build --kernel 6.18.52-1-cachyos-lts --accel tcg --mbr-fat-helper --fat-bits 32 /tmp/rufus-mbr-fat32-helper
```

Each run executes twelve jobs on one disposable 64-MiB USB: missing formatter,
bad label, invalid cluster, wrong disk sequence, formatter failure/cancellation,
final-sync failure/cancellation, MBR tampering during formatter handoff, competing
claim on reacquisition, interrupted sync and success. The existing guest-only
`prepare_exfat_probe` supplies the handoff/sync faults; production contains no
test switches. Four refusals preserve the entire disk. The first writing job
proves both old GPT headers were cleared before formatting. Every job checks
one authorization launcher, phase order, result-before-exit waiting, claim release
and descendant/descriptor cleanup. Final success independently checks the zeroed,
inactive MBR, partition type/extent, FAT metadata/serial/label, fsck and an empty
mtools listing. No ISO or Syslinux installer is staged.

Run the same commands against a sanitizer build for ASan/UBSan/leak detection;
the guest enables these checks. The script retains logs/manifests/results and
removes its disks, extracted volumes, kernel and guest trees even on failure.
Restore `RUFUS_BUILD_VM_TEST=OFF` afterward. These are device-helper formatting
checks, not OS boots or GUI MBR qualification. NTFS/exFAT MBR and Qt exposure
remain subsequent checkpoints.


### Non-bootable MBR/NTFS helper qualification (CP16l)

Build the guest targets listed for CP16k with `RUFUS_BUILD_VM_TEST=ON` and the
configured native mkntfs at `/usr/bin/mkntfs`. The host readback uses `sfdisk`,
`ntfsinfo`, `ntfslabel` and `ntfsck`. Run with a fresh output directory:

```sh
python3 linux/tests/vm/mbr.py --build /tmp/rufus-linux-qt6-build --kernel 6.18.52-1-cachyos-lts --accel tcg --mbr-ntfs-helper /tmp/rufus-mbr-ntfs-helper
```

Eleven source-free jobs cover missing formatter, invalid label/cluster, wrong
disk sequence, formatter failure/cancellation, changed MBR, final-sync
failure/cancellation, interrupted sync and success. NTFS retains its exclusive
disk claim throughout the formatter; its guest wrapper checks the claim before
every attempted command. There is no formatter claim handoff/reacquisition case.
One existing GPT bootable request first checks its UEFI:NTFS payload byte-for-byte.
The MBR jobs then replace that layout: preflight refusals preserve the whole
disk, and the first writing job checks both old GPT headers are cleared. Adapter
checks refuse bootable MBR/NTFS and an unexpected boot partition before access.

Each job counts one authorization launcher, waits for its exit after the helper
result, and checks phases, released claims and descendant/descriptor cleanup.
Independent host readback verifies one inactive type-0x07 MBR partition, zeroed
boot code/signature, exact volume extent, NTFS 3.1, primary/backup boot sectors,
cluster/offset metadata, Unicode label and clean flags; `ntfsck -n` must pass.
Repeat with a sanitizer build for ASan/UBSan/leak checks. `--ntfs-helper` retains
the GPT regression recipe. Neither recipe boots an OS or tests customizations.
Temporary media, copied volumes and guest trees are removed on success or
failure; evidence remains. Restore `RUFUS_BUILD_VM_TEST=OFF` afterward.


### Non-bootable MBR/exFAT helper qualification (CP16m)

Build the CP16k guest targets with `RUFUS_BUILD_VM_TEST=ON` and the configured
native formatter at `/usr/bin/mkfs.exfat`. Readback uses `sfdisk`, `fsck.exfat`
and `dump.exfat`. Use a fresh output directory:

```sh
python3 linux/tests/vm/mbr.py --build /tmp/rufus-linux-qt6-build --kernel 6.18.52-1-cachyos-lts --accel tcg --mbr-exfat-helper /tmp/rufus-mbr-exfat-helper
```

Thirteen jobs use one disposable 64-MiB USB: missing/old formatter, bad label,
insufficient metadata space, wrong sequence, formatter failure/cancellation,
MBR tampering, competing claim on reacquisition, final-sync failure/cancellation,
interrupted sync and success. Five refusals preserve full-disk hashes. The first
writing job proves both old GPT headers were cleared. Adapter checks reject
bootable MBR/exFAT before access. Existing guest-only faults and the authorization
stand-in check one launch, phase order, result/exit waiting, released claims and
cleanup of descriptors, descendants and temporary mounts. No production fault
switch or alternative authorization path is introduced.

Independent host checks verify one inactive type-0x07 partition with zeroed
boot code/signature, exact table/volume extent, matching main/backup boot regions,
exFAT checksums, 128-KiB clusters, partition-offset metadata, Unicode label and
absence of a partition table inside the formatted volume. Native `fsck.exfat -n`
and `dump.exfat` must pass. Repeat against a sanitizer build for ASan/UBSan/leak
checks; `--exfat-helper` retains GPT regression coverage. No ISO, installer,
filesystem mount or OS boot is involved. Disks, copied volumes and guest trees
are automatically removed; logs/manifests/results remain. Restore
`RUFUS_BUILD_VM_TEST=OFF` afterward. Qt MBR selection is a separate checkpoint.


### SFD/FAT32 native device handoff (CP16p)

Build `test_mbr_handoff_guest`, `udisks_fixture` and `partition_image_probe` with
`RUFUS_BUILD_VM_TEST=ON`, then run the existing isolated guest recipe:

```sh
python3 linux/tests/vm/mbr.py --build /tmp/rufus-linux-qt6-build --kernel 6.18.52-1-cachyos-lts --accel tcg --sfd /tmp/rufus-sfd-handoff
```

Use a new output directory; repeat with the sanitizer build for ASan/UBSan/leak
checks. The backend runs as root only inside a fixture-guarded QEMU guest, using
a private D-Bus/UDisks snapshot service and one disposable 64-MiB USB. The recipe
also boots a 4096-byte-sector fixture and verifies that refusal leaves every
byte unchanged. No host USB, host mounting or host privilege changes occur.

The 13 handoff cases cover an uncleared GPT, invalid extent/cluster, a busy old
partition, cancellation before and after formatting, formatter and sync failure,
a competing exclusive claim, altered FAT BPB, nested MBR entries and success.
Each starts with actual GPT kernel partitions; SFD preparation clears on-media
metadata, and format must explicitly refresh the kernel before handing the whole
disk to mkfs.fat. The command wrapper checks the claim is released; the caller
checks it is reacquired, including on failure/cancellation. Bad metadata after
the formatter's own readback must fail the final whole-disk readback. A retained
competing claim must never be bypassed. Unsupported bootable and NTFS SFD
requests are also rejected; filesystem expansion is separate work.

After successful guest shutdown, native fsck.fat/mtools and independent BPB,
backup, label/ID, hidden-sector and empty-MBR-entry checks inspect the whole disk
at byte zero. The 67,108,864-byte device contains a 67,092,480-byte filesystem,
within the existing native FAT formatter's sub-track rounding allowance. The
host inspection records the resulting hash before removing the image.

Run `--handoff` as the shared MBR/GPT regression; its 13 cases include partition
table preservation, mount failures/cleanup, a full FAT volume and retained disk
claims. Both modes clean kernels, initramfs trees and disposable disks even on
failure, retaining logs/manifests/results. Restore `RUFUS_BUILD_VM_TEST=OFF` after
building/running the probes. This checkpoint does not exercise a polkit prompt,
helper protocol, GUI or a bootable SFD image: source-free helper integration is
CP16q, with the existing one-authorization/finalization rules unchanged.


### SFD/FAT32 complete helper job (CP16q)

With `RUFUS_BUILD_VM_TEST=ON`, build `test_mbr_handoff_guest`, `udisks_fixture`,
`partition_image_probe`, `rufus-prepare-helper` and `prepare_exfat_probe`:

```sh
python3 linux/tests/vm/mbr.py --build /tmp/rufus-linux-qt6-build --kernel 6.18.52-1-cachyos-lts --accel tcg --sfd-helper /tmp/rufus-sfd-helper
```

Repeat with the sanitizer build and a new output directory. The existing guest
uses a disposable 64-MiB USB with 512-byte sectors, private UDisks properties and
a guest-only pkexec stand-in. The real client/helper protocol and native block
operations run; host devices and real polkit are not involved. Every job checks
one launcher, ordered phases, result-before-exit waiting, released claims and
cleanup of descriptors, descendants and temporary mounts.

The twelve cases cover missing formatter, invalid label/cluster, stale device
sequence, formatter failure/cancellation, injected partition entries, competing
claim on reacquisition, final-sync failure/cancellation, interrupted sync and
success. Preflight refusals preserve every disk byte. The first writing job
clears both GPT headers; writing jobs remove stale kernel partition children.
Successful results must describe the entire device at byte zero. Host readback
checks FAT BPB/backup, label/ID, zero hidden sectors, empty partition entries,
fsck.fat and mtools. Use `--mbr-fat-helper` for the existing shared FAT helper
regression. Fixtures are removed automatically; logs/manifests/results remain.
Restore `RUFUS_BUILD_VM_TEST=OFF` after qualification. Qt selection is CP16r;
this test does not expose the control or add a bootable SFD path.


### SFD/FAT16 native handoff and complete helper (CP16s)

Use the CP16p/CP16q guest targets with `RUFUS_BUILD_VM_TEST=ON`. The same fixtures
and fault cases now accept FAT16 without a separate formatter or helper:

```sh
python3 linux/tests/vm/mbr.py --build /tmp/rufus-linux-qt6-build --kernel 6.18.52-1-cachyos-lts --accel tcg --sfd --fat-bits 16 /tmp/rufus-sfd-fat16-handoff
python3 linux/tests/vm/mbr.py --build /tmp/rufus-linux-qt6-build --kernel 6.18.52-1-cachyos-lts --accel tcg --sfd-helper --fat-bits 16 /tmp/rufus-sfd-fat16-helper
```

Repeat with the sanitizer build and fresh output directories. Both use a
64-MiB simulated USB with 2048-byte FAT16 clusters. Handoff runs thirteen cases
plus unchanged-media refusal for 4096-byte logical sectors. The complete helper
runs twelve jobs, auditing one launcher, cancellation/final sync, process exit
and claim/descriptor/child cleanup. Native mkfs.fat and the existing FAT16
validator are reused; final SFD validation checks FAT16 and empty partition
entries after reacquiring exclusive access.

Independent host readback checks the FAT16 BPB, short/long sector count, cluster
range, volume ID/label, both identical FAT copies and zero hidden sectors, with
no surviving GPT tail or actual partition entries. fsck.fat and mtools must pass.
Regular-file SFD tests additionally cover 16-MiB media and odd sector counts.
`--sfd-helper` without `--fat-bits` remains the FAT32 regression. No ISO, installer,
Windows customization, host mount/device or real polkit prompt is involved.
Fixtures are automatically removed; keep logs/manifests/results and restore
`RUFUS_BUILD_VM_TEST=OFF`. FAT16 Qt exposure is the next checkpoint (CP16t).


### SFD/NTFS complete helper job (CP16u)

With `RUFUS_BUILD_VM_TEST=ON`, build the existing helper/handoff targets and run:

```sh
python3 linux/tests/vm/mbr.py --build /tmp/rufus-linux-qt6-build --kernel 6.18.52-1-cachyos-lts --accel tcg --sfd-ntfs-helper /tmp/rufus-sfd-ntfs-helper
```

Repeat with the sanitizer build and a fresh output directory. The 64-MiB,
512-byte-sector simulated USB uses the configured native mkntfs, private UDisks
fixtures and a guest-only authorization stand-in. No host device, real polkit
prompt, mounted filesystem or Windows customization is involved. NTFS-3G/kernel
mount-driver selection is not exercised by non-bootable formatting.

Eleven non-bootable jobs cover missing formatter, invalid label/cluster, stale
device sequence, formatter failure/cancellation, matching injected partition
entries in both NTFS boot copies, final-sync failure/cancellation, interrupted
sync and success. A bootable GPT/NTFS regression verifies the UEFI:NTFS payload
still works. Adapter preflight also refuses uncleared GPT children, shifted or
shortened SFD extents and a boot payload without changing the disk. The test
checks one launcher, exact full-device result, finalization/exit waiting and
claim/descriptor/child cleanup. Every native formatter child verifies the retained
exclusive disk claim; SFD explicitly requires `-F`, partition jobs omit it.

Independent host readback checks zero hidden sectors, geometry, equal boot
copies, no partition entries, the generated serial, Unicode label, NTFS version
and clean flags using ntfsinfo/ntfslabel/ntfsck. `--mbr-ntfs-helper` checks the
shared partition route; `--sfd` checks the shared FAT32 whole-disk entry validation.
The recipe removes disks, copied volumes and guest trees, retaining only compact
logs/manifests/results. Restore `RUFUS_BUILD_VM_TEST=OFF` after qualification.
Qt availability follows as CP16v; bootable SFD is not enabled by this checkpoint.


### SFD/exFAT complete helper job (CP16w)

With `RUFUS_BUILD_VM_TEST=ON`, build the existing helper/handoff targets and run:

```sh
python3 linux/tests/vm/mbr.py --build /tmp/rufus-linux-qt6-build --kernel 6.18.52-1-cachyos-lts --accel tcg --sfd-exfat-helper /tmp/rufus-sfd-exfat-helper
```

Repeat with the sanitizer build and a fresh output directory. This uses the
configured native exfatprogs formatter on a disposable 64-MiB, 512-byte-sector
USB, private UDisks fixtures and a guest-only authorization stand-in. It does
not mount media or involve host devices, real polkit or Windows customization.

Thirteen jobs cover missing/old formatter, invalid label, insufficient metadata
space, stale device sequence, formatter failure/cancellation, injected partition
entries, competing claim during reacquisition, failed/cancelled/interrupted final
sync and success. Adapter preflight additionally refuses bootable SFD, uncleared
kernel partitions and shifted/shortened extents. Calling the native formatter
while retaining the exclusive disk claim must fail without changing media.
The partition-entry fault updates both boot regions and their checksums, and
asserts that ordinary exFAT volume validation still passes; SFD must reject the
injected table separately. One launcher, phase order, waiting for helper exit,
whole-device result and descriptor/claim/child cleanup are checked.

Host readback verifies zero partition offset, full capacity, identical boot
regions/checksums, empty partition entries, FAT/heap/cluster geometry and Unicode
label, followed by fsck.exfat and dump.exfat. Use `--mbr-exfat-helper` for the
shared partition-helper regression and `--exfat` for direct GPT exFAT descriptor,
geometry, handoff and 4096-byte-sector refusal coverage. Recipes remove disks,
copied volumes and guest trees; keep compact logs/manifests/results and restore
`RUFUS_BUILD_VM_TEST=OFF`. Qt exposure follows in CP16x.


## Native target unmounts

CP17b2–CP17b4 qualify `target_mounts` on a disposable 64-MiB USB disk with two FAT16
partitions. Build with `RUFUS_BUILD_VM_TEST=ON`, then run:

```sh
cmake --build /tmp/rufus-linux-qt6-build --target test_target_mounts_guest
python3 linux/tests/vm/target_mounts.py --build /tmp/rufus-linux-qt6-build \
  --kernel 6.18.52-1-cachyos-lts --accel tcg /tmp/rufus-native-mounts
```

Use a new output directory. The recipe needs the existing minimal-guest tools
plus `sfdisk`, `mkfs.fat`, `fsck.fat`, `mmd`, `dmsetup` and `mkswap`; the matching
guest kernel needs vfat and device-mapper support. It runs without host root,
mounts, polkit or USB access. The guest executable rejects non-fixture machines
before opening its disk. The recipe removes disks, modules and initramfs after
success, retaining serial/host logs, QEMU command, checksums and case results.

The 45 core cases cover unmounted/invalid descriptors, partition-vs-disk rejection,
unknown/malformed mountinfo, unsupported filesystem/source layouts, cancellation,
source-on-target and actual busy files, protected or aliased/nested mounts,
untrusted mountpoint parents, stale identity, syscall failure, active swap and
native device-mapper holders/slaves. Successful one- and two-volume unmounts
must allow a new O_EXCL whole-disk claim. An independent tmpfs source remains
readable. Host fsck independently checks both resulting volumes.

A private-namespace child retains a copy of a mount while its parent unmounts;
the parent's empty inventory still cannot acquire O_EXCL. Only after the child's
copy is unmounted and the child exits is the disk available. Cancellation after
the first of two unmounts reports partial completion and leaves the other volume
mounted. No force/lazy flags or recursive cleanup are used. Injected failures
are test-only linker wrappers, not production environment switches.

The worker-namespace cases run a real QThread and check that its private mounts
appear in `/proc/thread-self/mountinfo`, stay absent from the main thread's view,
and are inherited by both QProcess and posix_spawn children. The guest starts
with shared propagation so a missing recursive-private step is observable.
Unshare failure, propagation failure and cancellation before isolation leave the
main namespace unchanged. The successful worker unmounts before exiting; its
mount holds the disk busy until cleanup.

The caller-home cases use real libc account lookup against a fixture-only passwd
file and `passwd: files` NSS configuration. They reject root/invalid/absent UIDs,
relative/missing/non-directory homes, cancellation, invalid descriptors, and a
home on the target or unresolved rootfs. Refusal must clear the result and avoid
any unmount syscall. A symlinked home on independent tmpfs ignores misleading
HOME and keeps its filesystem identity after the symlink is retargeted; reopening
that symlink correctly sees the target and refuses it. An account record larger
than the initial lookup buffer checks ERANGE growth. The independent retained
home and source coexist with successful two-volume unmount and are closed before
tmpfs cleanup. The host `rufus_udisks_mounts` suite separately qualifies exact
candidate/native matching, including stale, duplicated and byte-encoded paths.

The complete helper recipes also include `namespace-create` and
`namespace-private` failures. They must return before writes with an unchanged
whole-disk digest and release descriptors/descendants before completion. CP17b3
ran the SFD FAT32 recipe (`mbr.py --sfd-helper`, 14 jobs) and Syslinux recipe
(`mbr.py --helper`, five jobs plus independent readback and MBR boot), normally
and under ASan/UBSan. Other formatter variants inherit the failure cases but
were not rerun for this checkpoint.

The same recipe passes against the ASan/UBSan build (leak detection enabled).
This qualifies the current-namespace native operation, not GUI integration or a
real authorization prompt. Creation still refuses mounted targets until fresh
UDisks/native matching, caller-path protection and unmount-before-acquisition
are connected and qualified. The syscall is synchronous: cancellation between
volumes is bounded, but an active kernel unmount must return before completion.


## Mounted-target preparation helper

CP17b5 adds `mbr.py --mounted-helper` (FAT32 only), using the same disposable
64-MiB SFD guest and existing pipe-protocol client. Build with VM tests enabled:

```sh
python3 linux/tests/vm/mbr.py --build /tmp/rufus-linux-qt6-build \
  --kernel 6.18.52-1-cachyos-lts --accel tcg --mounted-helper \
  /tmp/rufus-mounted-helper
```

This runs the existing 14 SFD formatting jobs followed by 12 mounted-target jobs.
It tests missing/mismatched mount metadata, caller home or retained source on the
USB, actual EBUSY, stale disk sequence/service owner, cancellation after unmount,
metadata that never settles, service disappearance, a competing remount despite
an empty service snapshot, and complete creation from a writable desktop mount.
The test reports each native unmount and whether preparation started. Refusal
fixtures mount read-only so byte-for-byte before/after comparison can distinguish
preparation writes from filesystem dirty-bit changes caused by normal unmount.

The fixture delays mount notifications for 250 ms. Each job must launch the
authorized helper once, join helper/launcher exit, and release all descriptors
and descendants; no UDisks OpenDevice or Unmount call is allowed. Host readback
checks the final whole-device FAT32 filesystem independently. A files-only NSS
fixture provides a home on independent tmpfs. No host root, mounts, USB or real
polkit prompts are involved. `mounted-results.json` and the serial log record
the additional cases; the existing `results.json` records formatting/readback.
Successful recipes remove their disk and guest trees. Restore VM tests OFF.

This is helper qualification for an initially mounted SFD target. CP17b6 below
adds partitioned/source-backed success; mounted DD remains separate work. Existing source-on-target refusal uses a retained fixture
file; it does not claim ISO parsing or boot success for that refused source.

## Mounted partition with ISO source

CP17b6 adds `mbr.py --mounted-iso-helper` (FAT32 only). With VM tests enabled:

```sh
python3 linux/tests/vm/mbr.py --build /tmp/rufus-linux-qt6-build \
  --kernel 6.18.52-1-cachyos-lts --accel tcg --mounted-iso-helper \
  --upstream /home/marius/proj/lufus/.rufus-test-artifacts/deps/syslinux-6.04-pre1 \
  /tmp/rufus-mounted-iso-helper
```

The existing five Syslinux helper cases create an initial MBR/FAT32 disk. Three
additional jobs mount its first partition and use an ISO plus authenticated
caller home on independent tmpfs: changed-source refusal without unmount/writes,
cancellation after unmount without writes, then successful recreation/copying and
native Syslinux installation. Failure fixtures mount read-only for whole-disk
hash comparison; success starts with a writable desktop mount. Each checks one
helper launch, held-launcher exit ordering, descriptor/child cleanup and no
UDisks OpenDevice/Unmount authorization. Final independent readback, FAT fsck
and BIOS boot validate the newly created disk.

A guest-only pkexec stand-in and private D-Bus fixture exercise the production
helper with root privileges inside the disposable guest. No host devices,
mounts or real polkit prompts are involved. `mounted-results.json` records the
three additional results; ordinary helper results stay in `results.json`.
Sanitizer runs may pass `--installer` pointing to the qualified normal native
installer. Successful recipes delete large fixtures. Restore VM tests OFF after
qualification. Qt protocol tests separately cover confirmation and completion;
this recipe does not claim end-to-end desktop authorization qualification.

## Native mounted DD operation

CP17b7a adds `mbr.py --mounted-raw` (512-byte target sectors, FAT32 existing
partition). Build VM targets, then run:

```sh
python3 linux/tests/vm/mbr.py --build /tmp/rufus-linux-qt6-build \
  --kernel 6.18.52-1-cachyos-lts --accel tcg --mounted-raw \
  --upstream /home/marius/proj/lufus/.rufus-test-artifacts/deps/syslinux-6.04-pre1 \
  /tmp/rufus-mounted-raw
```

The five existing Syslinux helper jobs produce the initial bootable disk. The
guest copies this 64-MiB disk into a caller-owned ordinary file on independent
tmpfs and runs 14 `WriteRawDevice` cases against its mounted FAT32 partition.
These test changed source/owner, bad marker/WIM/sector/size, stale disk sequence,
real EBUSY, cancellation before work/after unmount/during copy, injected fsync
failure, injected readback corruption and success. A test wrapper publishes
matching UDisks metadata after successful native unmount; production code has
no fixture bypass. Root execution is entirely inside the guest. This invokes
the native operation directly, not a DD helper or real authorization dialog.

Refusals use read-only mounts and whole-disk before/after hashes. Raw progress
callbacks check the exclusive claim; copy cancellation must still call fsync.
Each case releases its descriptors and permits a new exclusive acquisition.
Final whole-disk equality plus independent file/filesystem readback and BIOS boot
check the successful output. `raw-results.json` records the new cases, while
`results.json` retains existing helper and readback results. The sanitizer recipe
uses the sanitizer build and a qualified normal `--installer`. Generated disks,
ISO trees and guest files are deleted by the recipe, leaving compact evidence.
Restore VM build flags to OFF after qualification.

## Mounted DD through the authorized helper

CP17b7b adds `mbr.py --mounted-raw-helper`:

```sh
python3 linux/tests/vm/mbr.py --build /tmp/rufus-linux-qt6-build \
  --kernel 6.18.52-1-cachyos-lts --accel tcg --mounted-raw-helper \
  --upstream /home/marius/proj/lufus/.rufus-test-artifacts/deps/syslinux-6.04-pre1 \
  /tmp/rufus-mounted-raw-helper
```

This uses the same initial five Syslinux helper jobs and independent caller-owned
64-MiB raw source as the native DD recipe. Five additional jobs run through
`PrepareImageProcess`, a guest-only pkexec stand-in and the production helper:
changed source, actual EBUSY, EOF cancellation after unmount before writing, then
successful mounted and unmounted DD creation. Each counts one launcher, delays UDisks mount
notifications, holds launcher exit after the helper result, and checks cleanup.
Success requires native raw progress and complete written/verified counters. The
final disk must equal its source and pass independent FAT/file readback and BIOS
boot. `raw-helper-results.json` records the five cases. This tests root execution
inside the guest, not host polkit prompts or desktop automount behavior.

`rufus_raw_helper_process` separately uses an ordinary protocol child to test
request schema, raw phase/counter validation, permission denial/dismissal, EOF
cancellation/destruction, uncertain write state and completion after child exit.
Its launch redirection exists only in the test executable. No production helper
path override is provided. Restore VM tests OFF after qualification; the recipe
removes disposable disks and trees while retaining compact evidence.

The 2026-09-27 consolidation removes direct GUI UDisks writing. The full GUI
guest recipe now copies the matching helper and includes its hivex dependency;
older results for that recipe do not establish qualification of the revised route.
`mbr.py --mounted-raw-helper` adds an unmounted production-helper case, while
`mbr.py --sfd` also checks that a rejected 4K formatting target remains eligible
for raw helper acquisition. That check alone does not qualify a complete 4K DD
write or real desktop authorization behavior.


### CP17e1: NTFS-3G native unmount qualification

The same small mount guest optionally runs both `ntfs-3g` and `lowntfs-3g`, using
packages unpacked only inside the guest. No host installation is needed:

```sh
python3 linux/tests/vm/target_mounts.py --build /tmp/rufus-linux-qt6-build \
  --kernel 6.18.52-1-cachyos-lts --accel tcg \
  --ntfs-3g-package ../.rufus-test-artifacts/deps/ntfs-3g.pkg.tar.zst \
  --ntfs-library-package ../.rufus-test-artifacts/deps/libntfs-3g.pkg.tar.zst \
  /tmp/rufus-ntfs-mounts
```

This needs host `mkntfs`, read-only `ntfsck -n`, libfuse2, and the guest kernel's
FUSE module. The driver/library packages must match. Eighteen NTFS cases extend
the 45 native cases: each driver retains real block identity; source/home/system,
busy files, alias/nesting, stale plans and cancellation refuse; normal unmount
releases the disk for immediate O_EXCL before waiting for the daemon, which then
exits cleanly. Remount readback preserves a written marker. The final first
partition is checked by host `ntfsck -n`; the second remains FAT16 and is checked
by `fsck.fat -n`. Package/library/tool checksums accompany the result. The recipe
removes its generated disk and guest on success; retain the compact evidence and
restore `RUFUS_BUILD_VM_TEST=OFF` afterward. CP17e2 will qualify full helper
recreation from a pre-existing NTFS-3G mount; this target exercises the native
checks/unmounts, not desktop polkit or a per-customization Windows installation.

### CP17e2: complete helper jobs from NTFS-3G mounts

The existing MBR/helper recipe also qualifies both NTFS-3G interfaces through
`PrepareImageProcess` and the production helper:

```sh
python3 linux/tests/vm/mbr.py --build /tmp/rufus-linux-qt6-build \
  --kernel 6.18.52-1-cachyos-lts --accel tcg --ntfs-mounted-helper \
  --upstream ../.rufus-test-artifacts/deps/syslinux-6.04-pre1 \
  --ntfs-3g-package ../.rufus-test-artifacts/deps/ntfs-3g.pkg.tar.zst \
  --ntfs-library-package ../.rufus-test-artifacts/deps/libntfs-3g.pkg.tar.zst \
  /tmp/rufus-ntfs-mounted-helper
```

It retains the five Syslinux seed jobs and five FAT32 mounted/unmounted DD jobs.
Fourteen additional jobs start from real `ntfs-3g` or `lowntfs-3g` block mounts:
changed source, busy mount, caller home on target, mismatched UDisks mount path,
cancellation after native unmount, non-bootable FAT32 creation and successful DD.
The last job restores the complete bootable seed image. Refusal/cancellation
requires whole-disk equality; success checks written/verified counters, final
sync/cleanup, released claims and no leaked descriptors or descendants. The
existing delayed UDisks update, single-launch count and held launcher exit test
that acquisition and completion wait for the required events. Test-owned desktop
daemons exit normally after unmount; the helper does not adopt or kill them.

`raw-helper-results.json` records all 19 mounted/unmounted jobs. Independent FAT
and file readback and BIOS boot validate the final disk. Driver/library packages
are unpacked only inside the guest, and their hashes are retained with the input
manifest. The sanitizer recipe uses the sanitizer build with a qualified normal
`--installer`; this longer recipe has a 300-second outer VM deadline, with the
same per-job checks. Restore VM build flags OFF afterward. Generated media, ISO trees
and guest files are removed even on failure. This uses guest-only authorization
and a controlled UDisks service, not desktop polkit or physical automount races.

### CP18a: standalone GPT/NTFS preparation

The existing NTFS formatter/helper recipe can retain a final source-free
UEFI:NTFS disk for independent readback:

```sh
python3 linux/tests/vm/mbr.py --build /tmp/rufus-linux-qt6-build \
  --kernel 6.18.52-1-cachyos-lts --accel tcg --uefi-ntfs-helper \
  /tmp/rufus-standalone-uefi-ntfs
```

Temporarily enable `RUFUS_BUILD_VM_TEST=ON` and build the guest/helper targets.
This needs host mkntfs/ntfsinfo/ntfslabel/ntfsck and the unchanged bundled
`res/uefi/uefi-ntfs.img`; no Syslinux source or external ISO is needed.
The 64-MiB generated USB first runs 12 existing non-bootable NTFS cases, then
four standalone jobs: final-sync failure, cancellation at final sync,
interrupted-sync retry and ordinary success. Faults are confined to the existing
test probe and occur after actual boot-payload copy/flush/readback. The final
success uses the production helper. Each job audits one launcher and completion
after helper/launcher exit, released claims, sync/error propagation and cleanup.

The host independently checks both GPT copies/entries, the exact UEFI:NTFS
payload and native NTFS metadata, label, cluster size and read-only consistency.
`results.json` records the standalone extent and payload hash. This prepares a
blank data volume plus loader, not an installed operating system; no fresh
firmware boot, Windows customization or physical-device test is included.
Repeat with the sanitizer build for ASan/UBSan/leak checks. The recipe removes
the guest/media even on failure; restore both VM build flags OFF afterward.


### CP18b: standalone GPT/exFAT preparation

Use the same helper guest with an explicit source-free exFAT boot choice:

```sh
python3 linux/tests/vm/mbr.py --build /tmp/rufus-linux-qt6-build \
  --kernel 6.18.52-1-cachyos-lts --accel tcg --uefi-exfat-helper \
  /tmp/rufus-standalone-uefi-exfat
```

Build the same VM targets listed for CP18a with `RUFUS_BUILD_VM_TEST=ON`.
This variant needs native `mkfs.exfat`, `fsck.exfat`, `dump.exfat` and libblkid,
plus the unchanged embedded payload; no ISO or Syslinux dependency is added.
It runs 13 ordinary exFAT helper jobs followed by the same four standalone
sync/cancellation/success cases. The ordinary jobs still create one data
partition. Standalone jobs use 128-KiB clusters, the Unicode volume label and
both GPT partitions. The host checks GPT copies/CRCs/attributes/extents, both
exFAT boot regions and checksums, label/cluster/offset metadata, read-only fsck
and exact payload equality. Repeat `--uefi-ntfs-helper` after changes to shared
payload handling. Both modes support the sanitizer build and remove their
64-MiB media and guest trees, retaining compact manifests/results/logs only.
Restore `RUFUS_BUILD_VM_TEST=OFF` after qualification. This is helper preparation;
Qt selection and fresh firmware boot are separate checkpoints.

### CP18d: standalone MBR NTFS/exFAT helper jobs

Add `--uefi-mbr` to either standalone helper recipe above:

```sh
python3 linux/tests/vm/mbr.py --build /tmp/rufus-linux-qt6-build \
  --kernel 6.18.52-1-cachyos-lts --accel tcg --uefi-ntfs-helper --uefi-mbr \
  /tmp/rufus-standalone-mbr-ntfs
python3 linux/tests/vm/mbr.py --build /tmp/rufus-linux-qt6-build \
  --kernel 6.18.52-1-cachyos-lts --accel tcg --uefi-exfat-helper --uefi-mbr \
  /tmp/rufus-standalone-mbr-exfat
```

The modifier requires one of those two standalone modes. NTFS runs 13 ordinary
MBR format regressions and four bootable jobs; exFAT runs 15 ordinary MBR format
regressions and four bootable jobs. The extra ordinary cases cover table changes
and, for exFAT, failure to reacquire the whole-disk claim. All standalone jobs
check the full boot payload, single launcher, final-sync outcomes and completion
after helper/launcher exit. Host readback checks the active `0x07` main and
inactive `0xEF` tail, zero boot code/signature, exact extents, NTFS/exFAT metadata
and consistency, and equality with the embedded payload. No external ISO is used.

Build `rufus-prepare-helper`, `prepare_exfat_probe`, `test_mbr_handoff_guest` and
`udisks_fixture` with VM targets ON. Repeat both commands with the sanitizer
build for ASan/UBSan/leak checks. The recipe removes its 64-MiB USB, extracted
volume and guest tree even on failure; restore VM targets OFF afterward. These
are isolated block-device/helper jobs with a test authorization launcher and
controlled UDisks metadata, not desktop polkit or firmware-boot qualification.

### CP19d2b: complete FreeDOS helper jobs

With `RUFUS_BUILD_VM_TEST=ON`, build `rufus-prepare-helper`,
`prepare_extract_probe`, `test_mbr_handoff_guest`, `udisks_fixture`,
`partition_image_probe` and `dos_locale_reference`. Run:

```sh
python3 linux/tests/vm/mbr.py --build /tmp/rufus-linux-qt6-build \
  --kernel 6.18.52-1-cachyos-lts --accel tcg --freedos-helper --fat-bits 32 \
  /tmp/rufus-freedos-fat32
```

Repeat with `--fat-bits 16`, and add `--freedos-win7` for the fallback MBR.
The default tests explicit RO/852 startup; the fallback variant uses explicit
US/437, without CONFIG.SYS. Six jobs check unchanged-media preflight refusal,
mount failure, EOF cancellation during file copying, flush/unmount failures and
success. The real client verifies discrete progress, one launcher per job and
waits through a deliberately held launcher exit. The guest checks device-claim
release, temporary mountpoints and descriptor/child cleanup.

Native FAT, mkfs.fat and the production helper are used; the guest's private bus
has fixture UDisks metadata and a test authorization launcher. No real polkit
prompt or host block device is involved. The host independently checks original
MBR/PBR bytes, all 27 resources, hidden/system attributes, startup oracle bytes,
file counts and read-only fsck. Only the original NO NAME BPB/directory-label
warning is accepted. This is media creation, not a BIOS boot test. Restore VM
targets OFF afterward. Disks, extracted volumes and guest trees are removed;
compact logs, hashes and results remain.

### CP19d2c: BIOS boot of the helper-created FreeDOS USB

Add `--freedos-boot` to the preceding FreeDOS helper recipe:

```sh
python3 linux/tests/vm/mbr.py --build /tmp/rufus-linux-qt6-build \
  --kernel 6.18.52-1-cachyos-lts --accel tcg --freedos-helper --freedos-boot \
  --fat-bits 32 /tmp/rufus-freedos-bios
```

The modifier requires `--freedos-helper`. After creation and independent media
readback, a second QEMU VM starts from BIOS with only the created USB. No direct
kernel/initrd, additional DOS files or changed startup configuration are used.
The 64-MiB stick boots with original Rufus MBR/FAT32 records and the explicit
RO/852 choice. Repeat with `--fat-bits 16 --freedos-win7` for original Windows 7
MBR/FAT16 records and explicit US/437 without CONFIG.SYS.

The BIOS observer reuses the existing QMP keyboard and VGA-text helpers. It
requires the DOS prompt, the expected startup selection, VER followed by its
output and a new prompt, and an ECHO result on a separate output line. It saves
screenshots, text observations, exact commands and the firmware/QEMU identities.
Both boot paths use snapshot mode and verify an unchanged backing-image hash
before the disk is deleted. Failures retain compact evidence and terminate the
VM; the outer recipe also removes creation guest trees and disk images.

This qualifies representative SeaBIOS USB boots, not every physical firmware,
keyboard/code-page combination or advanced BIOS option. Qt integration remains
a separate checkpoint.

## Standalone Syslinux 4 and 6 helpers

CP20c extends the existing MBR guest with `--syslinux6-helper --fat-bits 16|32`.
CP20g reuses it for `--syslinux4-helper`, with `--upstream` pointing to 4.07;
v6 continues to require 6.04-pre1. The mode selects the matching executable and
fixed helper path. It creates no source ISO. Each mode runs eleven complete client/helper jobs:
stale sequence and missing-installer refusal before writes; EOF cancellation
inside an mtools child; failed/no-op installer; installer flush, readback, boot
corruption, temporary-directory cleanup and final disk flush failures; success.
The fixture checks one authorization launch, the retained exclusive claim in
mcopy children, two discrete bootloader copy events, no extraction counters,
complete phase order, child/descriptor cleanup and wait-for-exit behavior.
The failed-cleanup case deliberately leaves one empty directory, removed by
the guest after it verifies failure. No production fault controls are added.

```sh
cmake -S linux -B /tmp/rufus-linux-qt6-build -DRUFUS_BUILD_VM_TEST=ON
cmake --build /tmp/rufus-linux-qt6-build --target rufus-prepare-helper \
  prepare_syslinux_probe test_mbr_handoff_guest udisks_fixture partition_image_probe
python3 linux/tests/vm/mbr.py --build /tmp/rufus-linux-qt6-build \
  --kernel 6.18.52-1-cachyos-lts --accel tcg --syslinux6-helper --fat-bits 32 \
  --upstream /home/marius/proj/lufus/.rufus-test-artifacts/deps/syslinux-6.04-pre1 \
  /tmp/rufus-syslinux6-fat32
# Repeat with --fat-bits 16 and a new output directory.
cmake -S linux -B /tmp/rufus-linux-qt6-build -DRUFUS_BUILD_VM_TEST=OFF
```

The existing independent FAT/MBR checks validate the final filesystem and
companion. The v6 inventory contains only ldlinux.sys/ldlinux.c32; v4 has only ldlinux.sys.
Loader size/attributes and the alignment gap are checked. FAT32 fsck normalizes
only the expected primary/backup boot-code difference on a throwaway copy.
Each completed disk then boots as a snapshot USB through SeaBIOS to the bare
Syslinux prompt. No OS handoff or physical firmware coverage is claimed.
Logs, manifests and screenshots remain; disks, guest trees and copied firmware
are removed, including after failures. The guest authorization stand-in does
not test a desktop password dialog or change host privileges.


## Standalone Syslinux on NTFS

CP20l extends the same MBR helper fixture with `--syslinux6-helper
--syslinux-ntfs ntfs|lowntfs-3g`. Enable `RUFUS_BUILD_VM_TEST=ON` before building;
old binaries left from a previously enabled build are not current test targets.
Build `rufus-prepare-helper`, `prepare_syslinux_probe`, `test_mbr_handoff_guest`,
`udisks_fixture`, `partition_image_probe` and `rufus-extlinux` in the main tree.
Use the verified 6.04-pre1 source as `RUFUS_SYSLINUX_UPSTREAM`.

```sh
cmake -S linux -B /tmp/rufus-linux-qt6-build -DRUFUS_BUILD_VM_TEST=ON
cmake --build /tmp/rufus-linux-qt6-build
python3 linux/tests/vm/mbr.py --build /tmp/rufus-linux-qt6-build \
  --kernel 7.2.7-arch1-1 --accel tcg --syslinux6-helper --syslinux-ntfs ntfs \
  --upstream /path/to/syslinux-6.04-pre1 \
  --ntfs-3g-package /path/to/ntfs-3g.pkg.tar.zst \
  --ntfs-library-package /path/to/libntfs-3g.pkg.tar.zst /tmp/rufus-syslinux-ntfs-helper
```

Repeat with `--syslinux-ntfs lowntfs-3g` and a new output directory. The fallback
guest omits the kernel NTFS module, exercising real absence-based selection.
Packages are unpacked only inside the VM. The kernel run needs `ntfs`,
`nls_utf8` and `fuse` modules. Host tools include ntfsprogs-plus and 7-Zip, as in
the earlier adapter recipe. No host mount, installation or physical USB is used.
For sanitizer builds, enable/build the same VM targets and pass the normal
`--installer /tmp/rufus-linux-qt6-build/syslinux/rufus-extlinux`. Native upstream
executables remain unsanitized; enable leak detection for our worker/client.

Each driver runs the complete client/helper flow through the private D-Bus and
authorization fixtures. Cases cover stale identity, missing installer/formatter,
running cancellation, failed/no-op provider, damaged boot code, raw read/flush,
Windows attributes, filesystem sync, unmount, mountpoint removal, final disk
flush and success. Kernel NTFS also checks a mount failure with the fallback
available. Every job uses exactly one authorization launch, emits the expected
two copy steps only after verification, and waits for cleanup and helper exit.
A guest-only provider wrapper checks competing whole-disk exclusive opens and
pauses the child for cancellation; it delegates to the unchanged provider.

Independent readback checks the original MBR, random disk signature, partition,
BPB/backup, zero gaps, both file hashes and Windows attributes. The actual USB
then boots to the bare Syslinux prompt. `ntfsinfo`'s known jump restriction is
recorded; only its disposable metadata-check copy is normalized. Production
GUI NTFS enablement follows separately. The recipe removes disks and guest
images in `finally`, retaining logs, hashes, manifests and boot evidence.


## Syslinux ISO extraction on NTFS

CP20n adds `syslinux_ntfs.py --iso` to the existing NTFS component guest, using
the production extractor followed by upstream `rufus-extlinux`. CP20o combines
`--adapter --iso` to run the checked adapter on that extracted tree. Omit
`--iso` to run the standalone empty-root regression matrix. Neither component
mode enables an image-mode helper/Qt operation.

```sh
cmake --build /tmp/rufus-linux-qt6-build --target iso_extract_probe \
  syslinux_scan_probe syslinux_ntfs_probe mbr_image_probe ntfs_format_probe rufus-extlinux
python3 linux/tests/vm/syslinux_ntfs.py --adapter --iso \
  --build /tmp/rufus-linux-qt6-build --probe-build /tmp/rufus-extlinux-probe \
  --upstream /home/marius/proj/lufus/.rufus-test-artifacts/deps/syslinux-6.04-pre1 \
  --kernel 7.2.7-arch1-1 \
  --ntfs-3g-package /home/marius/proj/lufus/.rufus-test-artifacts/deps/ntfs-3g.pkg.tar.zst \
  --ntfs-library-package /home/marius/proj/lufus/.rufus-test-artifacts/deps/libntfs-3g.pkg.tar.zst \
  --mkisofs /home/marius/proj/lufus/.rufus-test-artifacts/deps/usr/bin/mkisofs \
  /tmp/rufus-syslinux-ntfs-iso
```

Use the existing observer build described by the NTFS probe recipe. For the
sanitizer run, point `--build` at the sanitized main build and `--installer` at
the normal build's `syslinux/rufus-extlinux`. Leak detection stays enabled; the
upstream installer and mapping observer are not sanitizer-instrumented. QEMU
uses TCG, two disposable 64-MiB USB disks and no network. The guest mounts with
the same case-folding, fixed-permission and recovery policy used by the helper.

The fixture exercises the original root-loader skip, matching root/nested
companions, selected-directory label patch, `syslinux.org` backup and generated
root redirect. Repeating extraction must reject the populated target without
writes. Guest logs report whether the backup uses native no-overwrite rename
or its NTFS-3G hard-link fallback, and check the mount's device claim and mapped
loader sectors. Host readback compares all eight copied/generated files and
BPB/backup/disk gaps, then boots the unchanged disk through its copied config
to `RUFUS_RAW_BOOT_OK`. All source and backing-image hashes stay unchanged by
the respective readback/boot checks.

The recipe removes its media, fixture ISO/tree and guest trees on success.
After a failure, retain logs/manifests before removing those disposable inputs.
This qualifies one source layout and 512-byte sectors/4-KiB clusters, not all
image versions, the full source-backed helper lifecycle or physical firmware.
With `--adapter --iso`, each driver runs 83 checked-adapter cases, verifies
Windows attributes before/after remount and preserves the copied tree. Timestamp
faults change source metadata but must never change its content hash. Standalone
`--adapter` still runs 59 cases per driver and boots the bare Syslinux prompt.
No Windows customization qualification is involved.


## Syslinux NTFS image helper

CP20p extends the existing `mbr.py` standalone NTFS helper guest with
`--syslinux-image`. It uses the real client and production helper for successful,
preflight and installer-cancellation jobs. A separately linked fault helper
injects copy-boundary cancellation and write/cleanup errors. The guest-only
authorization launcher audits one launch
and holds exit to check completion ordering. No host authorization, mount or
physical device is involved.

```sh
cmake --build /tmp/rufus-linux-qt6-build --target rufus-prepare-helper \
  prepare_syslinux_probe test_mbr_handoff_guest udisks_fixture \
  iso_write_check_probe syslinux_scan_probe rufus-extlinux
python3 linux/tests/vm/mbr.py --build /tmp/rufus-linux-qt6-build \
  --kernel 7.2.7-arch1-1 --accel tcg --syslinux6-helper \
  --syslinux-ntfs ntfs --syslinux-image \
  --upstream /path/to/syslinux-6.04-pre1 \
  --ntfs-3g-package /path/to/ntfs-3g.pkg.tar.zst \
  --ntfs-library-package /path/to/libntfs-3g.pkg.tar.zst \
  --mkisofs /path/to/mkisofs /tmp/rufus-syslinux-ntfs-image-helper
```

Repeat with `--syslinux-ntfs lowntfs-3g` and a fresh output directory for real
absence-based fallback. For sanitizers use the sanitized main build and pass
`--installer /tmp/rufus-linux-qt6-build/syslinux/rufus-extlinux`. Leave leak checks
enabled. Omit `--syslinux-image` and `--mkisofs` for the standalone regression.

The five small ISO fixtures are checked with the production preflight before
starting a VM. The 25 kernel/24 fallback job matrix covers preflight refusal,
copy/installer cancellation, source changes, payload/attribute/read/sync errors,
cleanup failures and known/compatible-unknown success. Every request verifies
one authorization, progress, completion after exit, source preservation and
released resources. Independent NTFS readback and BIOS boot use CP20o's eight-file
oracle and original configured handoff. EFI images requiring an extra UEFI:NTFS
partition are rejected before writes until that helper layout is integrated.
The recipe removes generated media and guest trees in `finally`, including failed
runs; logs, manifests, preflight results and boot/readback evidence remain.

### Syslinux images with UEFI:NTFS

CP20q adds `--syslinux-uefi` to the preceding `mbr.py --syslinux-image` recipe.
Build the additional `uefi_handoff_fixture` CMake target first. This optional
VM-only target uses Clang and lld-link to produce a tiny x64 EFI application;
neither tool nor the fixture is needed by the production executable.

The generated source includes that application at `/EFI/BOOT/BOOTX64.EFI`.
The production helper must reserve and write the unchanged embedded UEFI:NTFS
payload while preserving the original Syslinux MBR and its random signature.
The fixture checks missing and unnecessary boot-partition requests, malformed
EFI headers, payload write/flush failures and cancellation before ISO copying.
Its usual source/payload/cleanup fault matrix then runs on the smaller NTFS
partition: 30 kernel-driver jobs or 29 NTFS-3G fallback jobs.

Independent readback checks the exact embedded payload, both MBR entries, file
hashes/attributes, NTFS boot records and preserved unused space. The same USB
boots through its copied Syslinux config under BIOS, then through the original
UEFI:NTFS loader to the copied EFI application under OVMF. Serial output must
contain both the UEFI:NTFS banner and the fixture marker, with QEMU's expected
exit status. This proves firmware handoff, not an OS installation or Secure Boot.
Both boots use snapshots and preserve the backing-disk hash. Firmware copies
are also removed after use.

The Syslinux guest launcher waits for an explicit test acknowledgement before
exiting, avoiding scheduling-dependent completion assertions under TCG and
sanitizers. The real helper/client completion rules remain unchanged. Omit
`--syslinux-uefi` to retain the BIOS-only image regression; Qt enablement remains
a separate checkpoint.


## Standalone GRUB2 helper (CP21c)

Build with `RUFUS_BUILD_VM_TEST=ON`, then run each FAT width in a new directory:

```sh
python3 linux/tests/vm/mbr.py --build /tmp/rufus-linux-qt6-build \
    --kernel 6.18.52-1-cachyos-lts --accel tcg --grub2-helper --fat-bits 16 \
    /tmp/rufus-grub2-fat16
python3 linux/tests/vm/mbr.py --build /tmp/rufus-linux-qt6-build \
    --kernel 6.18.52-1-cachyos-lts --accel tcg --grub2-helper --fat-bits 32 \
    /tmp/rufus-grub2-fat32
```

Use an installed matching kernel. This reuses the format-helper guest, not the
full desktop qualification recipe above. It exercises 22 complete jobs through
the real client/helper with fixture authorization and UDisks. Guest-only linker
wrappers inject core/PBR write, sync, readback and partial-write cancellation
faults; ordinary success uses the production helper. Preflight, formatter
handoff/reacquisition, final sync/interruption/cancellation, one authorization,
no file-copy progress, descriptor/child cleanup and helper-exit ordering are
checked. The boot-record stages assert the exclusive whole-disk claim.

The host independently checks original MBR/core/standard PBR bytes, FAT32 backup,
geometry/labels, empty root directory and the formatter's full reported extent.
Only the known original `NO NAME` BPB/directory-label fsck diagnostic is accepted.
A snapshot BIOS boot must enter the bare GRUB rescue prompt with the expected
prefix/root and recognize the FAT filesystem, preserving the backing hash.
No modules, config or OS is installed. It does not qualify NTFS/ext, a GRUB ISO
job, physical firmware or GUI selection. Sanitized builds use the same recipe;
ASan/UBSan/LSan remain enabled in the guest. Large fixtures and snapshots are
removed on success/failure; logs, manifests, screen text and hashes remain.

## Standalone GRUB2 ext2/ext3 helper (CP21k2)

Build `rufus-prepare-helper`, `prepare_exfat_probe`, `test_mbr_handoff_guest`,
`udisks_fixture` and `partition_image_probe` with `RUFUS_BUILD_VM_TEST=ON`.
Run both formats in separate new output directories:

```sh
python3 linux/tests/vm/mbr.py --build /tmp/rufus-linux-qt6-build \
    --kernel 6.18.52-1-cachyos-lts --accel tcg --grub2-ext-helper --ext-bits 2 \
    /tmp/rufus-grub2-ext2
python3 linux/tests/vm/mbr.py --build /tmp/rufus-linux-qt6-build \
    --kernel 6.18.52-1-cachyos-lts --accel tcg --grub2-ext-helper --ext-bits 3 \
    /tmp/rufus-grub2-ext3
```

Use an installed matching kernel. The guest uses a disposable 320-MiB USB disk
and one fixture authorization per job. Ext2 runs a complete job; ext3 also
checks preflight refusal, formatter failure/cancellation, altered MBR,
competing reacquisition, final-sync failure/cancellation and core write faults.
The host verifies the original MBR/core bytes, native `dumpe2fs` and read-only
`e2fsck` results, then boots each disk to the bare GRUB rescue prompt. The
advanced Qt filesystem choice and physical USB are outside this recipe.


## Standalone GRUB2 ext Qt creation (CP21l)

Also build `test_grub2_widgets_guest` and run:

```sh
python3 linux/tests/vm/mbr.py --build /tmp/rufus-linux-qt6-build \
    --kernel 6.18.52-1-cachyos-lts --accel tcg --grub2-ext-gui --ext-bits 3 \
    /tmp/rufus-grub2-ext-gui
```

This mode runs one complete job through the production MainWindow and
SelectionController with offscreen Qt Widgets, a real disposable USB block
node, fixture UDisks service and the production helper. It activates advanced
GRUB2/format controls, chooses ext3, confirms START and audits one fixture
authorization. The launcher is held after helper completion to check that the
GUI remains Finalizing until process exit, then shows full READY. It verifies
claim release and completed READY after removal. Native filesystem readback,
original MBR/core comparison and bare BIOS boot use the same host checks as
CP21k2. The minimal initramfs supplies the partition link normally made by udev;
no physical USB, desktop polkit agent or OS installation is exercised.

## GRUB ISO helper and compatible BIOS boot (CP21w)

Build `rufus-prepare-helper`, `prepare_exfat_probe`, `test_mbr_handoff_guest`,
`udisks_fixture` and `partition_image_probe` with `RUFUS_BUILD_VM_TEST=ON`.
Extract the pinned Ubuntu package below as test data, without installing it:

- URL: https://archive.ubuntu.com/ubuntu/pool/main/g/grub2/grub-pc-bin_2.12-1ubuntu7_amd64.deb
- Package SHA-256: `5d5ee8a68cd9b75c859eb0c45ff3da5721fc6d4d8bb3673b98f6ce358db32014`.
- Extract its `data.tar.*` archive, then supply `usr/lib/grub/i386-pc`.

Run with an installed kernel and its matching modules:

```sh
python3 linux/tests/vm/mbr.py --build /tmp/rufus-linux-qt6-build \
    --kernel 6.18.52-1-cachyos-lts --accel tcg --grub2-image \
    --grub-modules /tmp/grub-fixture/usr/lib/grub/i386-pc \
    --grub-core linux/tests/fixtures/download/grub-2.12/core.img \
    --mkisofs /path/to/mkisofs /tmp/rufus-grub-iso
```

Use a new output directory. `grub2_iso.py` verifies the complete module-set hash,
generates a small ISO/configuration/kernel/initramfs fixture, and reuses the
existing native guest. The production client/helper creates MBR/FAT32 media by default on
a 512-MiB virtual USB. Add `--fat-bits 16` for the CP21y FAT16 recipe; its
8-KiB clusters keep the filesystem within the FAT16 cluster-count limit. Independent readback checks all 308 files, original boot
records, the GRUB volume-label patch and only the established fsck label diagnostic.
A separate snapshot BIOS USB boot must load normal.mod/configuration and reach
Linux userspace, which reads a fixture marker from the USB filesystem. No kernel
or initrd is supplied directly to this second QEMU invocation.

The guest-only authorization wrapper models caller-owned private staging for its
root test driver. This qualifies one simulated launch and complete native helper
cleanup/exit, not real polkit or the unprivileged GUI. The driver also checks shared
copy counters, disk claim release, descriptors and descendant cleanup. Sanitized
builds use the same recipe with ASan/UBSan/LSan enabled in the guest. The driver
refuses host execution. No host mount, real USB or installed GRUB is needed.

The runner removes disposable guest/media/ISO trees on success or failure and
retains logs/manifests/hashes. Remove extracted package data and test builds after
exporting evidence. These exact GRUB 2.12 BIOS/FAT32 and BIOS/FAT16 combinations are qualified;
other images/versions/filesystems/firmware and physical hardware remain separate
work. CP21x enables the FAT32 GUI route; FAT16 GUI enablement is the next bounded
checkpoint. See [CP21w](../../../docs/testing-and-checkpoints.md#cp21w--compatible-grub-iso-native-creation-and-boot)
and [CP21y](../../../docs/testing-and-checkpoints.md#cp21y--native-fat16-grub-iso-creation-and-boot).


## GRUB ISO on NTFS (CP21aa)

Use the CP21w/y recipe above with the same pinned GRUB core/modules,
`--kernel 7.2.7-arch1-1`, and add:

```sh
--grub-ntfs ntfs \
--ntfs-3g-package /path/to/ntfs-3g.pkg.tar.zst \
--ntfs-library-package /path/to/libntfs-3g.pkg.tar.zst
```

Repeat with `--grub-ntfs lowntfs-3g` and a new output directory. Both packages
are unpacked only into the creation guest. The preferred-driver guest contains
the new kernel NTFS module; the fallback creation guest deliberately omits it.
The production helper must log the selected provider and complete with one
simulated authorization. The boot fixture uses the kernel NTFS driver read-only
to verify the copied marker after GRUB has loaded normal.mod/configuration
and booted the copied Linux kernel/initramfs. BIOS boot uses no direct kernel
or initrd arguments and preserves the snapshot backing-media hash.

Independent host readback checks the original Rufus MBR/core/NTFS PBR, preserved
BPB and backup boot sector, partition extent, sector/cluster/hidden geometry,
label and clean NTFS metadata. A 7-Zip inventory excludes only its virtual
`[SYSTEM]/` NTFS metadata records; ordinary files must match the complete
308-file source manifest. All extracted hashes must match, accounting only
for the selected-label GRUB configuration patch. No host mount is used.

Normal and sanitized recipes use the same arguments and their respective
CMake builds. Host sanitized seeding probes need LeakSanitizer process inspection;
run them outside a tracing sandbox, keeping leak checks enabled in the guest.
QEMU's snapshot temporary file is explicitly placed in the checkpoint directory.
The runner removes generated media/ISO/initramfs trees on success and failure;
retain compact evidence and remove builds/package extraction afterward. This
qualifies plain BIOS/MBR/NTFS; GUI enablement and EFI/UEFI:NTFS combinations
remain separate checkpoints.


## GRUB ISO NTFS with UEFI:NTFS (CP21ad)

Build the CP21aa native targets plus `uefi_handoff_fixture`. Use the same pinned
GRUB package/core and kernel, then add `--grub-uefi` to the NTFS recipe:

```sh
python3 linux/tests/vm/mbr.py --build /tmp/rufus-linux-qt6-build \
    --kernel 7.2.7-arch1-1 --accel tcg --grub2-image \
    --grub-ntfs ntfs --grub-uefi \
    --grub-modules /tmp/grub-fixture/usr/lib/grub/i386-pc \
    --grub-core linux/tests/fixtures/download/grub-2.12/core.img \
    --mkisofs /path/to/mkisofs \
    --ntfs-3g-package /path/to/ntfs-3g.pkg.tar.zst \
    --ntfs-library-package /path/to/libntfs-3g.pkg.tar.zst \
    /tmp/rufus-grub-iso-uefi
```

Repeat in new output directories with `--grub-ntfs lowntfs-3g`, and both providers
with the sanitizer build. Keep host LSan outside tracing restrictions; guest
ASan/UBSan/LSan checks remain enabled. `--grub-uefi` requires the NTFS GRUB image
recipe and the built x64 EFI fixture; incomplete invocations are rejected before
creating output.

The fixture adds the existing `uefi-handoff.efi` as `/EFI/BOOT/BOOTX64.EFI` to
CP21aa's compatible ISO. There are 309 source files. This application retains its
historical `RUFUS_SYSLINUX_UEFI_OK` marker; it contains no Syslinux installer or
runtime and is reused unchanged to prove firmware handoff from NTFS.

The production helper creates the original GRUB MBR/core/NTFS PBR, the formatted
NTFS main partition and the complete bundled UEFI:NTFS trailing partition with
one simulated authorization. Native readback verifies the confirmed extent,
complete format byte count, retained core/payload, cleanup and child exit. The
host independently checks both MBR entries, original records, NTFS BPB/backup,
label/clean metadata, complete ordinary-file inventory and every file hash,
accounting only for Rufus's volume-label configuration patch.

BIOS must load the copied GRUB modules/configuration and boot the copied Linux
kernel/initramfs into userspace reading the USB marker. A separate OVMF x64 boot
must display the bundled UEFI:NTFS banner and launch the copied EFI application
from NTFS. Both boots use snapshot media, take no direct kernel/initrd arguments
and must preserve the backing-disk hash. OVMF firmware copies and all disposable
media/ISO/initramfs/readback trees are removed on success or failure; compact
serial logs, firmware hashes, commands, manifests and results remain.

This qualifies the fixture's combined BIOS/x64 UEFI route on 512-byte sectors
and 4-KiB NTFS clusters. It does not qualify Secure Boot, physical firmware,
arbitrary distribution EFI loaders/module sets, other EFI architectures, real
polkit or Qt integration. The EFI-capable GRUB/NTFS Qt gate is lifted only in a
subsequent GUI checkpoint. No Windows customization qualification is added.


## Windows BIOS/dual NTFS creation (CP22d2a)

Build `test_mbr_handoff_guest`, `udisks_fixture`, `rufus-prepare-helper`,
`partition_image_probe`, `wim_fixture` and `uefi_handoff_fixture` with the existing
CMake VM-test configuration. Use the upstream WIM input and existing mkntfs
configuration. The creation fixture requires no real Windows ISO:

```sh
python3 linux/tests/vm/mbr.py --build /tmp/rufus-linux-qt6-build \
    --kernel 7.2.8-arch1-1 --accel tcg \
    --windows-bios rufus --windows-ntfs ntfs --windows-dual \
    --windows-version 11 --mkisofs /path/to/mkisofs \
    --ntfs-3g-package /path/to/ntfs-3g.pkg.tar.zst \
    --ntfs-library-package /path/to/libntfs-3g.pkg.tar.zst \
    /tmp/rufus-windows-bios-dual
```

Repeat in fresh directories for Windows 7 MBR/kernel/plain BIOS (`--windows-bios
win7`, omit `--windows-dual`, add `--windows-version 10 --windows-udf`), Rufus
MBR/fallback/plain BIOS (`--windows-ntfs lowntfs-3g`, omit dual), and Windows 7
MBR/fallback/dual with Windows 10 UDF bridge. Repeat all four with the sanitizer
build. Host LSan needs execution outside tracing restrictions; keep guest
ASan/UBSan/LSan enabled. The guest uses 1 GiB RAM for sanitizer initramfs capacity.

The small six-file ISO/UDF fixture has generated Windows metadata, an opaque
BOOTMGR marker and the existing x64 EFI application. The production helper uses
one simulated authorization to partition/format/mount/copy/sync/unmount. The
same guest checks provider selection, progress, held-result/exit ordering,
source preservation and complete resource cleanup. Independent readback checks
original MBR/PBR, extent/signature/active fields, exact optional UEFI:NTFS, NTFS
BPB/backup/label/clean metadata, complete inventory and file hashes. All large
media/source/guest/readback artifacts are removed on success or failure.

This recipe qualifies native creation/readback only. It neither boots BOOTMGR
nor the EFI application; direct kernel/initrd arguments start the creation guest.
It does not qualify Windows Setup, real polkit, physical firmware, Secure Boot,
other EFI architectures or Qt enablement. CP22d2b adds executable BIOS/EFI
handoff tests before CP22e enables the Windows BIOS/dual Qt route. No additional
Windows customization qualification is added.


## Windows BIOS/dual firmware handoff (CP22d2b)

Build the CP22d2a targets plus `windows_bios_handoff_fixture` (requires NASM).
Add `--windows-boot` to each of CP22d2a's four normal/sanitized recipes. Select an
installed kernel with its matching modules; CP22d2b used 7.2.8-arch1-2.

The generated 72-KiB BOOTMGR verifies the original NTFS PBR's 2000:0000 entry,
BIOS drive and loaded tail beyond 64 KiB. SeaBIOS boots the written USB alone
through either original MBR. Rufus MBR jobs additionally test the actual VGA
second-disk prompt with Space selecting USB and timeout selecting the generated
second-disk MBR. Dual jobs reuse the original UEFI:NTFS/OVMF/x64 application boot
check. Firmware VMs receive no kernel/initrd arguments, use snapshot disks and
verify backing hashes. No real Windows ISO or installation test is needed.

All prior creation/readback/single-authorization/sync/cleanup/exit checks remain.
The optional flag is rejected without a Windows fixture; missing built BIOS
fixtures fail before guest/output creation. Large media, source/readback trees,
second disks, VGA memory dumps and firmware copies are removed even on failure.
Compact manifests, prompt text, commands and serial/firmware records remain.

Ensure host formatter/readback dependencies load correctly before running. The
CP22d2b host had an ntfsprogs-plus/hwinfo ABI mismatch; a signature-verified Arch
hwinfo 25.5 library was supplied only through a private LD_LIBRARY_PATH and the
existing dependency copier. This did not repair or modify installed packages.

Generated handoff does not qualify Windows Setup/BCD, physical firmware, Secure
Boot, other EFI architectures, actual polkit or Qt dispatch. No additional
Windows customization qualification is added; Qt enablement is CP22e.


## Windows FAT32 BIOS/dual creation and handoff (CP22i)

Build the CP22d2b targets, then select `--windows-fat32` instead of
`--windows-ntfs`. These options are mutually exclusive. Keep `--windows-boot`
for generated BIOS/EFI handoff; `--windows-dual` puts the EFI application on the
main FAT volume and adds no UEFI:NTFS payload. A BIOS-only FAT source omits EFI
files. Example (use an installed matching kernel):

```sh
python3 linux/tests/vm/mbr.py --build /tmp/rufus-cp22i-linux \
    --kernel 7.2.8-arch1-2 --accel tcg --windows-bios rufus \
    --windows-fat32 --windows-dual --windows-boot \
    --mkisofs /home/marius/proj/lufus/.rufus-test-artifacts/deps/usr/bin/mkisofs \
    /tmp/rufus-windows-fat32-dual
```

Repeat in fresh directories for both MBR choices and plain/dual inputs;
`--windows-bios win7 --windows-version 10 --windows-udf` covers the UDF bridge.
Repeat with sanitized helper/client binaries and leak detection/halt-on-error.
These 64-MiB fixtures use 512-byte sectors and clusters. All existing native
source/device, authorization-launch, phase/progress, held-exit and cleanup checks
remain. Original primary/backup/tail executable bytes match after copying;
live FSInfo allocation hints may change. Only the precise original NO NAME
boot-sector versus directory-label fsck warning is accepted. Every file is
independently read back and hashed. Native FAT EFI boot must omit the UEFI:NTFS
banner. No Windows installation/customization matrix is added.

The Windows QMP runner uses unbuffered pipes so select() sees queued responses
without Python read-ahead hiding them during the original MBR's timed prompt.
Completed and failed recipes remove disposable disks/ISO/initramfs/readback trees;
export compact evidence and remove temporary builds after the checkpoint.


## Older BOOTMGR plain BIOS/NTFS (CP22l2)

Use the CP22d2b NTFS build targets and add `--windows-version 7`, without
`--windows-dual` or `--windows-fat32`. The existing backend guest keeps the GUI
gate intact. Generated known Windows 7 metadata, executable BOOTMGR and root
bootmgr.efi model an older source with no fallback EFI file; plain BIOS copies
it unchanged instead of performing the UEFI/Alt+E-only repair.

```sh
python3 linux/tests/vm/mbr.py --build /tmp/rufus-cp22l2-linux \
    --kernel 7.2.8-arch1-2 --accel tcg \
    --windows-bios rufus --windows-version 7 --windows-ntfs ntfs \
    --windows-boot --mkisofs /path/to/mkisofs \
    --ntfs-3g-package /path/to/ntfs-3g.pkg.tar.zst \
    --ntfs-library-package /path/to/libntfs-3g.pkg.tar.zst \
    /tmp/rufus-older-windows-rufus
```

In a fresh output directory, use `--windows-bios win7 --windows-ntfs lowntfs-3g
--windows-udf` for the original Windows 7 MBR/fallback mount/UDF bridge. Repeat
these two cases with the sanitizer build. All four pass native NTFS record/
inventory readback, one simulated authorization, source/progress/helper-exit/
cleanup assertions and generated BIOS handoff. Rufus MBR key/timeout choices
also pass. This reuses the existing provider/MBR qualification and CP22l1's
7/8/8.1 component checks; it is not a per-release Windows installation matrix.
Qt enablement remains CP22l3; actual Windows Setup/BCD, older EFI/FAT and legacy
WinPE remain separate. Export compact evidence, then remove temporary builds
and guest-output duplicates; never retain completed disposable media.

## Legacy WinPE record handoff (CP22n4)

Build `mbr_image_probe`, `fat_format_probe`, `freedos_boot_probe` and
`winpe_prepare_probe` with the Linux CMake build. Run on the host without root:

```sh
python3 linux/tests/vm/winpe_bios.py --build /tmp/rufus-cp22n4-linux \
    --output /tmp/rufus-winpe-handoff-evidence
```

Requires NASM, QEMU/SeaBIOS, mcopy/mtype, mkfs.fat, fsck.fat and sfdisk. Output
must be fresh; `--case` selects a named case for diagnosis. Six cases cover
original NT-record disk/filename failures, a fixture-only drive-ID correction
and NTLDR alias, BOOTMGR-record handoff, and /minint without masquerading.
The shared generated loader checks CS, DL and a marker beyond 64 KiB. Its
existing default/second-disk binaries remain byte-identical.

Every disk is a disposable sparse nine-GiB regular file occupying about 19 MiB.
The size avoids a small-USB SeaBIOS CHS translation mismatch; it is not a new
minimum supported USB size. QEMU uses TCG, USB storage and snapshots, with no
kernel, privileged guest or Windows payload. Exact source/output/file readback,
original record bytes and filesystem checks precede boot. Snapshot immutability
uses a SHA-256 fingerprint of logical size, allocated extent positions and data,
not a SHA-256 of all logical zero bytes.

Diagnostic BPB changes/aliases are explicitly marked in the evidence. Modern PE
records overwrite five bytes of FSInfo allocation hints; this mount-free fixture
restores known formatter hints for mcopy. That control does not qualify the real
mount/remount metadata lifecycle. No production provider or route is changed.
Repeat with sanitized probes and leak detection enabled. Temporary disks, roots
and readback volumes are removed on success and failure; only compact evidence
remains. Actual Windows Setup and full helper/Qt integration stay unqualified.

## Mount-free legacy NTFS BIOS handoff (CP22n5)

Set `RUFUS_NTFS_FIXTURE_ROOT` to the retained NTFS-3G prefix containing matching
`include/ntfs-3g` and `lib/libntfs-3g.so`; it is only used under BUILD_TESTING.
Build `ntfs_file_fixture`, `winpe_prepare_probe`, `mbr_image_probe`,
`ntfs_format_probe` and `ntfs_boot_probe` with CMake. No installed production
executable depends on the test library/tool. Run unprivileged, without mounts:

```sh
python3 linux/tests/vm/winpe_ntfs_bios.py --build /tmp/rufus-cp22n5-linux \
    --output /tmp/rufus-winpe-ntfs-evidence
```

The five fresh-output cases model full i386/amd64 and /minint root preparation,
NTLDR-only fallback and no loader. Native mkntfs uses the retained partition
offset; original NTFS code and BPB/secondary record are independently checked
before/after file population. The library tool accepts only bounded caller-owned
regular files/fixed names and refuses root. It uses the retained parent when
closing the child; reopening the same parent at child close loses a cached inode.
Readback uses a fresh read-only handle, ntfsck/ntfsinfo check metadata and QEMU
USB/SeaBIOS checks entry/drive/tail or the exact missing-loader diagnostic.
Neither NTFS BPB patching nor a production NTLDR alias is introduced.

Repeat with sanitized native probes and leak detection. The library itself is
the retained package binary; sanitizer interceptors still catch fixture leaks
and allocator misuse, but do not establish full upstream-library instrumentation.
All 65-MiB disks, volume copies and extracted roots are removed after each case
on success/failure. Export compact evidence before removing temporary builds.
This is generated-loader component evidence, not a full helper mount workflow
or actual Windows Setup. Preserve the legacy Qt gate until that workflow passes.


### Retained legacy root detector through the Qt/helper job

Use the existing generated i386/FAT32 GUI recipe to verify a differing mixed-case
root NTDETECT.COM alongside a matching root BOOTMGR and a distinct root SIF:

```sh
python3 linux/tests/vm/mbr.py --build /tmp/rufus-cp22n21-linux \
    --kernel 7.2.8-arch1-2 --accel tcg --windows-bios win7 \
    --windows-winpe i386 --windows-winpe-gui --windows-fat32 \
    --windows-winpe-root-detector --windows-winpe-root-detector-content different \
    --windows-winpe-root-loader --windows-winpe-root-sif \
    --mkisofs /home/marius/proj/lufus/.rufus-test-artifacts/deps/usr/bin/mkisofs \
    /tmp/rufus-winpe-retained-detector
```

The original bare `--windows-winpe-root-detector` option still generates identical
content; the new optional content choice defaults to `identical`. `different`
requires the root-detector option and supplies 131,110 binary bytes, spanning
multiple read buffers, while the directory detector stays at 28 bytes. Source,
target and final readback manifests explicitly record the content choice and
both hashes. Exact seven-file readback checks unchanged root/directory detectors,
unchanged directory SIF/loader and the expected root SIF/loader edits. Existing
record/FSInfo checks, the precisely allowed original boot-label fsck warning,
held helper exit before READY, one simulated authorization, cleanup/unplug and
generated BIOS drive/tail handoff are reused. This does not execute NTDETECT.COM
or qualify actual Windows Setup or host polkit. Run as the ordinary user; QEMU
uses only disposable media/private sockets. The recipe removes its guest trees,
ISO, disk and readback copies even on failure. Export compact evidence before
removing the remaining logs/build.


### Prepatched legacy root loader through the Qt/helper job

Reuse the preceding i386/FAT32 recipe with
`--windows-winpe-root-loader --windows-winpe-root-loader-content prepatched`,
the differing retained detector and distinct root SIF. The new content choice
requires the root-loader flag and an `i386` or `amd64` fixture. Keep-loader
`minint` or `--windows-winpe-partial-minint` cannot use this choice. Omitting it
retains the original `identical` default, including minint retention. These are fixture options, not production
request fields or an additional patcher.

The fixture's existing independent expected bytes also supply the prepatched
root input. Source/job/readback manifests record its content choice accurately.
Exact readback verifies the prepatched root hash is preserved, the original
directory loader remains different and unchanged, and the SIF/detector/source
contracts above remain. Reuse the original record/FSInfo checks and the narrowly
allowed original boot-label fsck notice, one simulated authorization, held exit
before READY, cleanup/unplug and generated BIOS handoff. CP22n23 qualifies this
complete generated job; it does not execute NTDETECT or Windows Setup. Export
compact evidence and remove the temporary build after the recipe cleans media.


### Partial minint beside a complete setup through Qt/helper

Use the existing retained-detector i386/FAT32 recipe with
`--windows-winpe-partial-minint` and an identical root loader:

```sh
python3 linux/tests/vm/mbr.py --build /tmp/rufus-cp22n25-linux \
    --kernel 7.2.8-arch1-2 --accel tcg --windows-bios win7 \
    --windows-winpe i386 --windows-winpe-gui --windows-winpe-partial-minint \
    --windows-winpe-root-sif --windows-winpe-root-detector \
    --windows-winpe-root-detector-content different --windows-winpe-root-loader \
    --windows-fat32 \
    --mkisofs /home/marius/proj/lufus/.rufus-test-artifacts/deps/usr/bin/mkisofs \
    /tmp/rufus-cp22n25-gui
```

The opt-in flag requires i386 or AMD64. It adds only `minint/TxTsEtUp.SiF`
and puts `/minint` in the selected complete family's options. Family directory
selection remains separate from the keep-loader decision. The expected loader
stays raw, the expected root SIF stays untouched and BIOS expects drive 0x80.
A prepatched root fixture is refused because keep-loader requires raw equality.
Omitting the new flag preserves every existing fixture default. No new request
field or production code is needed.

Exact eight-file readback checks all source and ancillary bytes, including the
differing retained detector. The selected Win7 MBR and original PE PBR must be
present without masquerading. Reuse the existing record/FSInfo checks, narrowly
allowed original boot-label fsck notice, one simulated authorization, held
launcher exit before READY, cleanup/unplug and generated BIOS handoff. This
does not execute NTDETECT or Windows Setup or qualify host polkit. The recipe
removes large media/guest trees; export compact evidence and remove the build.


### Mixed WinPE/Syslinux FAT32/NTFS backend helper job

`--windows-winpe-syslinux create|keep|overwrite` adds independently expected
Syslinux configuration data to the generated legacy fixture. It requires
`--windows-bios`, `--windows-winpe` and exactly one of `--windows-fat32` or
`--windows-ntfs`; root-write
failure mode is refused before build/output access. Add `--windows-winpe-gui`
to run the admitted route through Widgets. Omit it to preserve all
existing source/target file defaults. Qt admission requires the saved Alt+E choice;
the generated Widgets driver enables it before loading and chooses the requested
filesystem explicitly.

```sh
python3 linux/tests/vm/mbr.py --build /tmp/rufus-cp22n29-linux \
    --kernel 7.2.8-arch1-2 --accel tcg --windows-bios win7 \
    --windows-winpe i386 --windows-fat32 --windows-winpe-syslinux overwrite \
    --windows-winpe-root-loader --windows-winpe-root-loader-content prepatched \
    --windows-winpe-root-sif --windows-winpe-root-detector \
    --windows-winpe-root-detector-content different \
    --mkisofs /home/marius/proj/lufus/.rufus-test-artifacts/deps/usr/bin/mkisofs \
    /tmp/rufus-cp22n29-guest
```

CP22n29 qualifies this overwrite fixture: ten exact target files, label edits,
generated root config and patched backup, prepared WinPE SIF, preserved loader/
detector/source bytes, original MBR/PE/backup/FSInfo, zero SBR gap and generated
BIOS drive 0x81 with intact tail. Both native Syslinux installer paths must be
absent in the guest and no installation may be logged. Retain the precisely
allowed original boot-label fsck notice without repair. One simulated authorization,
sync/private unmount/released claim and helper/launcher exit precede client
completion. This does not exercise Widgets green READY/unplug behavior, actual
Windows Setup/detector execution, host polkit or another customization. The recipe
cleans large guest/media/readback copies; export compact evidence and remove the
remaining build/log directories. Creation/retention modes have fixture controls
and CP22n28 source/native functional coverage, not a full USB qualification here.

CP22n31 qualifies the same overwrite fixture on the preferred kernel NTFS driver:

```sh
python3 linux/tests/vm/mbr.py --build /tmp/rufus-cp22n31-linux \
    --kernel 7.2.8-arch1-2 --accel tcg --windows-bios win7 \
    --windows-winpe i386 --windows-ntfs ntfs --windows-winpe-syslinux overwrite \
    --windows-winpe-root-loader --windows-winpe-root-loader-content prepatched \
    --windows-winpe-root-sif --windows-winpe-root-detector \
    --windows-winpe-root-detector-content different \
    --mkisofs /home/marius/proj/lufus/.rufus-test-artifacts/deps/usr/bin/mkisofs \
    --ntfs-3g-package /home/marius/proj/lufus/.rufus-test-artifacts/deps/ntfs-3g.pkg.tar.zst \
    --ntfs-library-package /home/marius/proj/lufus/.rufus-test-artifacts/deps/libntfs-3g.pkg.tar.zst \
    /tmp/rufus-cp22n31-guest
```

The mixed token freezes Alt+E without UEFI:NTFS; other fixture defaults remain
unchanged. The guest confirms kernel ntfs selection and absence/nonuse of the
native NTFS Extlinux provider as well as both FAT Syslinux installers. Reuse
the exact ten-file/config/WinPE readback, original NTFS record/backup/BPB and
clean volume-flag checks, zero SBR gap, final drive 0x81/tail/snapshot and backend
completion checks above. Included fallback packages are not exercised by this
qualification. No new full Windows Setup/detector/host-polkit or Widgets READY/
unplug claim. The recipe cleans large copies; export compact evidence and remove
the remaining build/output directories.


CP22n32 adds `--windows-winpe-gui` to the CP22n31 NTFS command above, using
`/tmp/rufus-cp22n32-linux` and `/tmp/rufus-cp22n32-gui-final`. This qualifies the
shared FAT32 default followed by explicit NTFS choice, BIOS-only confirmation,
unknown fresh Syslinux version and absence/nonuse of all three native providers.
Retain the same independent ten-file/record/clean-volume/BIOS readback. The actual
Qt window stays Finalizing below full progress while helper/launcher exit is
held, then displays full green READY after release and keeps it after unplug.
One simulated authorization covers the job and cleanup. This adds no actual
Windows Setup/detector, host-polkit or customization qualification. Export compact
logs/manifests/hashes before deleting the remaining build/output directories.


CP22n33 adds `--windows-winpe-grub-data grub2|grub4dos|both` to that mixed Widgets
recipe. It requires `--windows-winpe-syslinux` and `--windows-winpe-gui`; other
modes fail before build/output access. With `both`, use build
`/tmp/rufus-cp22n33-linux` and output `/tmp/rufus-cp22n33-gui-final`. The driver
answers No to the original differing-GRUB-version lookup before confirmation,
checks fresh GRUB flags/no SBR/no core request, and retains the same durable
READY/unplug checks. Independent readback now covers 14 target files from 13
inputs: patched grub.cfg and preserved normal.mod/grldr/menu.lst in addition to
the existing WinPE/Syslinux files. The entire SBR gap must remain zero; no GRUB
installation may be logged. This does not qualify a GRUB-only SBR route or actual
Windows Setup. The recipe cleans large media even on failure; export compact
logs/manifests/hashes before deleting remaining build/output directories.


CP22n45 qualifies the original standalone GRUB4DOS FAT32 helper media through BIOS:

```sh
python3 -B linux/tests/vm/mbr.py --build /tmp/rufus-cp22n45-linux \
    --kernel 7.2.8-arch1-2 --accel tcg --grub4dos-helper /tmp/rufus-cp22n45-native
```

Build rufus-prepare-helper, test_mbr_handoff_guest, prepare_exfat_probe,
udisks_fixture and partition_image_probe with RUFUS_BUILD_VM_TEST=ON first. The
recipe retains the complete native job, one simulated authorization, durable
completion/cleanup and exact readback, then boots the unchanged media via its
original MBR/SBR and root grldr. No direct boot kernel/initrd or added menu.lst.
The original preset menu reaches its command prompt. Require original version,
root (hd0,0)/FAT32/type0x0C, grldr listing, its own complete loader CRC and a distinct
ECHO output. A private copy with grldr removed must fail with the original error.
Snapshot boots must preserve backing hashes and join both VMs on exit/failure.
Firmware/QEMU/commands/VGA text/screenshots/results stay compact; recipe cleanup
removes native guest/media and control disks. This qualifies the fixed standalone
loader, not other filesystems, image versions, OS chainloads or Windows Setup.
Export evidence before removing the remaining build/output directories.


CP22n47 combines the Qt advanced choice with that unchanged native helper/BIOS route:

```sh
python3 -B linux/tests/vm/mbr.py --build /tmp/rufus-cp22n47-linux \
    --kernel 7.2.8-arch1-2 --accel tcg --grub4dos-gui /tmp/rufus-cp22n47-qualified
```

Also build test_grub2_widgets_guest with RUFUS_BUILD_GUI=ON and RUFUS_BUILD_VM_TEST=ON.
The existing driver chooses Grub4DOS/FAT32/MBR/BIOS, checks frozen original cache
bytes, replaces the cache, confirms and runs one real native helper. A guest-only
pkexec/getuid fixture models UID 1000; host root/polkit/devices are not used. Hold
launcher exit to inspect Finalizing/progress/no notification and private staging,
then require cleanup, reaped descendants, released claim and READY exactly once.
Unplug must preserve completion. Independently verify chosen serial/label, original
records and bytes; reuse the unchanged BIOS/CRC/ECHO and missing-loader qualifier.
The old --grub4dos-helper route keeps its fixed serial/label expectations. Capture
compact evidence, then remove completed native images/builds. No additional Windows
customization, host desktop session, other filesystems or OS chainload qualification.


CP22n48 uses the same helper recipe for FAT16 and retains the FAT32 regression:

```sh
python3 -B linux/tests/vm/mbr.py --build /tmp/rufus-cp22n48-linux \
    --kernel 7.2.8-arch1-2 --accel tcg --grub4dos-helper --fat-bits 16 \
    /tmp/rufus-cp22n48-qualified-fat16
python3 -B linux/tests/vm/mbr.py --build /tmp/rufus-cp22n48-linux \
    --kernel 7.2.8-arch1-2 --accel tcg --grub4dos-helper --fat-bits 32 \
    /tmp/rufus-cp22n48-qualified-fat32
```

The existing FAT16 marker selects the 2-KiB cluster geometry. Require exact original
standard FAT boot records, original GRUB4DOS MBR/SBR, exact retained root grldr,
precise standard-label fsck diagnostic, one launcher, sync/cleanup/held exit and
released descriptors/descendants/claim. Bare BIOS execution uses a filesystem-specific
root response (FAT16/0x0E or FAT32/0x0C), full-loader CRC and ECHO. Missing-loader
controls must fail the same qualifier. The GUI recipe still rejects FAT16 until
CP22n49. Preserve compact evidence and remove disposable media/builds afterwards.


CP22n49 combines FAT16/FAT32 Qt selection with the real native helper and BIOS boot:

```sh
python3 -B linux/tests/vm/mbr.py --build <build-directory> \
    --kernel 7.2.8-arch1-2 --accel tcg --grub4dos-gui --fat-bits 16 <fat16-output>
python3 -B linux/tests/vm/mbr.py --build <build-directory> \
    --kernel 7.2.8-arch1-2 --accel tcg --grub4dos-gui --fat-bits 32 <fat32-output>
```

The driver checks both FAT choices and the FAT16 default, then explicitly chooses
its requested filesystem. Freeze geometry/confirmation/original loader before any
launch; replace the cache and check retained bytes. Hold launcher exit at original
60853/65535 discrete progress with no notification, then require all sync/unmount/
descendant/staging/claim cleanup before full READY/one notification and stable unplug.
Readback uses Qt's frozen random serial and RUFUS_TEST root label on either FAT.
Existing helper-only calls retain their fixed serial and RUFUS TEST expectations.
Reuse actual filesystem-specific BIOS root/CRC/ECHO and missing-loader controls.
If test files are moved to a long filesystem path, provide a short TMPDIR alias for
private Unix sockets; preserve large files off /tmp. This checkpoint used
/home/marius/proj/lufus/.rufus-test-artifacts/cp22n49-work with /tmp/rufus-cp22n49-t
pointing to its tmp directory. Export verified compact evidence then remove both.


CP22n50 qualifies standalone NTFS before exposing it in Qt:

```sh
python3 -B linux/tests/vm/mbr.py --build <build-directory> \
    --kernel 7.2.8-arch1-2 --accel tcg --grub4dos-helper --grub4dos-ntfs \
    --ntfs-3g-package <ntfs-3g-package> \
    --ntfs-library-package <matching-library-package> <ntfs-output>
```

Build the test-only ntfs_file_fixture with RUFUS_NTFS_FIXTURE_ROOT pointing at the
retained matching NTFS-3G development prefix. Its library is for independent host
regular-file readback and negative-control deletion; production never links it.
The VM recipe loads new kernel ntfs and asserts its selection, uses host mkntfs in
the guest, and keeps existing fallback packages available without exercising them.
Check original records/backup BPB, clean NTFS 3.1/4096-byte clusters, only grldr as
user content, independent 7-Zip/library bytes, single helper, held exit and complete
mount/claim/descendant cleanup. Bare BIOS must report NTFS/0x07, read full grldr CRC
and execute ECHO. A private disk copy with only grldr removed must fail the same
boot check. No host mounts/root/physical devices or Windows customization tests.
The fallback route is the next checkpoint; Qt still admits FAT16/FAT32 only.


CP22n51 selects the absent-kernel NTFS-3G qualification route without changing the
production provider policy or adding any field to the authorized job request:

```sh
python3 -B linux/tests/vm/mbr.py --build <build-directory> \
    --kernel 7.2.8-arch1-2 --accel tcg --grub4dos-helper --grub4dos-ntfs \
    --grub4dos-ntfs-driver lowntfs-3g \
    --ntfs-3g-package <ntfs-3g-package> \
    --ntfs-library-package <matching-library-package> <fallback-output>
```

The fallback initramfs omits kernel NTFS and asserts ENODEV, loads FUSE and uses
the packaged real foreground lowntfs-3g. A guest-only wrapper holds its exit after
clean unmount: the client stays active, the loader snapshot is retained, native
child is reaped and the helper's private mount is gone. Release it, then require
all owned processes and the exclusive claim to be released before completion.
Use the existing NTFS readback and BIOS positive/missing-loader controls; repeat
with an ASan/UBSan build and leak detection enabled. The old command form, or
--grub4dos-ntfs-driver ntfs, qualifies the preferred kernel route. Output creation
must be refused for a driver option without --grub4dos-ntfs. Qt remains FAT-only.


CP22n53 extends that same recipe to native Qt NTFS creation after CP22n52 admission:

```sh
python3 -B linux/tests/vm/mbr.py --build <build-directory> \
    --kernel 7.2.8-arch1-2 --accel tcg --grub4dos-gui --grub4dos-ntfs \
    --ntfs-3g-package <ntfs-3g-package> \
    --ntfs-library-package <matching-library-package> <kernel-output>
```

Repeat with --grub4dos-ntfs-driver lowntfs-3g and a fresh output directory. Build
RUFUS_BUILD_GUI=ON/RUFUS_BUILD_VM_TEST=ON and ntfs_file_fixture with the retained
matching development prefix. Repeat both providers with ASan/UBSan and leak
detection enabled. The native Qt driver chooses NTFS from the original three
standalone filesystem choices, freezes 4096-byte clusters, formatter-owned serial,
normalized case-sensitive Unicode label and original grldr, replaces the cache,
confirms and starts exactly one guest authorization/native helper. NTFS still means
BIOS/MBR here; no UEFI:NTFS is selected. Old FAT16/FAT32 forms remain supported.

The fallback guest proves kernel NTFS is absent, delegates to the real foreground
lowntfs-3g and holds its wrapper after native unmount/exit. Qt must stay active with
no READY/notification; the real child is reaped, the helper's private mount is gone
and the loader snapshot stays retained. Release FUSE, then separately hold launcher
exit at original 62414/65535 finalization before allowing full green READY once.
After unmount, the existing mount handoff joins FUSE before reacquiring/rechecking
and syncing the whole disk; it does not promise an exclusive whole-disk claim in
that intervening unmounted interval. Require all descendants, staging and mounts
gone and the claim available before READY. Unplug keeps READY without another beep.

Readback checks original MBR/SBR/NTFS PBR, backup BPB, nonzero formatter serial,
clean NTFS 3.1/4096-byte clusters, exact Rufus ș😀_test label and only original grldr.
Independent 7-Zip and the test-only library must agree. Reuse actual NTFS root/CRC/
ECHO BIOS execution and the missing-loader control. Qt guests use 2 GiB RAM because
Qt/debug/sanitizer cpio and its unpacked root coexist during early boot; 1 GiB
could fail before any application ran. Save failed tooling assertions/boot logs
alongside final evidence, then remove completed images/builds and socket alias.
No host root, real polkit/physical USB, OS chainload or new Windows customization
qualification is claimed. Production code is unchanged in this checkpoint.


CP22n55 qualifies the plain BIOS/FAT32 ISO backend with its image-owned GRUB4DOS
loader and menu, using the existing ISO guest driver and standalone BIOS oracle:

```sh
python3 -B linux/tests/vm/mbr.py --build <build-directory> \
    --kernel 7.2.8-arch1-2 --accel tcg --grub4dos-image \
    --mkisofs <mkisofs-path> <output-directory>
```

Build test_mbr_handoff_guest, udisks_fixture, rufus-prepare-helper and
prepare_exfat_probe with RUFUS_BUILD_VM_TEST=ON. Repeat with ASan/UBSan builds;
keep leak detection enabled. The recipe constructs its own small ISO containing
unchanged original grldr, menu.lst, image-marker and an 8193-byte payload. No
selected-loader/core or download cache is supplied to the guest. The production
client/helper must complete seven ISO phases, monotonic full copy progress, one
simulated authorization, native sync/unmount, held launcher exit and cleanup.
Independent readback checks the original MBR/SBR/gap and standard FAT32 records,
volume geometry/serial/label and all four exact files. BIOS checks root, loader
and menu CRCs, the variable set by the booted menu and the marker file. Removing
only grldr must fail with the original diagnostic; snapshot backing hashes stay
unchanged. The menu variable uses eight characters (RUFUSISO), matching GRUB4DOS.
The shared QEMU keyboard helper types dot, percent and minus for these commands.

CP22n59 also qualifies --fat-bits 16 through the native image recipe;
--mkisofs is required. This is
native simulated USB/helper and representative original-loader/menu execution,
not arbitrary-loader compatibility, OS chainloading, host polkit or physical-media
durability. Ordinary Qt admission is CP22n56; the optional GUI recipe below
combines that path with native helper/readback/BIOS qualification in CP22n57.


### Ordinary Qt GRUB4DOS ISO creation (CP22n57)

Add --grub4dos-image-gui to the preceding --grub4dos-image command and build
test_grub2_widgets_guest as well. Keep RUFUS_BUILD_GUI=ON and RUFUS_BUILD_VM_TEST=ON.
The same guest now loads the ISO through the normal Qt image selector, checks
MBR/BIOS/FAT32 defaults and uses the image's own loader/menu without a standalone
cache or staged payload. Require one simulated authorization, monotonic native
ISO percentages, original MBR/SBR caption and 64879/65535 at first finalization.
Hold launcher exit after native cleanup: no full READY or notification until it
joins. Successful full READY, one beep/alert, released claim, closed source,
restored descriptors, reaped children and stable unplug are checked before the
same independent readback and BIOS/missing-loader oracles. Repeat under ASan/
UBSan/leak detection and keep the existing --grub4dos-gui FAT32 route as a control.
This companion flag requires --grub4dos-image. CP22n60 also qualifies
--fat-bits 16 through ordinary Qt selection and the same native readback/BIOS
checks. The 64-MiB GUI defaults to FAT16; choose FAT32 explicitly for its control. The guest-only UID/launcher model does not
qualify host polkit.


### Native GRUB4DOS FAT16 ISO creation (CP22n59)

Use the existing --grub4dos-image command with --fat-bits 16, without the GUI flag.
The 64-MiB guest retains 2048-byte FAT16 clusters; the 8192-byte override belongs
to the separate 512-MiB GRUB2 fixture. Reuse the same generated ISO, native
client/helper, original FAT16/MBR/SBR readback and BIOS menu/missing-loader checks.
Require all four files and 331262 copied bytes, original 0x0E partition type,
label/serial and clean joined completion after native sync/unmount/launcher exit.
Run normally and with ASan/UBSan/leak detection; retain the FAT32 image route as
a regression. CP22n60 also admits FAT16 with --grub4dos-image-gui.
No production change or additional Windows customization qualification is needed.


CP22n60 repeats ordinary Qt FAT16 creation/readback/BIOS under sanitizers and
keeps Qt FAT32 as a regression control. Initial archive packing under concurrent
load reached the generic 20-second tool deadline before the guest started.
The existing recipe now gives only cpio packaging a named 60-second deadline
through subprocess.run; the generic wrapper already owns its default timeout.
Keep failed packaging/retry logs alongside final evidence. Native operation,
helper and BIOS deadlines remain unchanged.


## CP22n62: source-backed GRUB4DOS NTFS ISO

Reuse the generated --grub4dos-image fixture with --grub4dos-ntfs. Build the
existing native targets and ntfs_file_fixture using RUFUS_NTFS_FIXTURE_ROOT
pointing to the retained matching NTFS-3G prefix. The kernel guest needs a kernel
with the new ntfs module (retained 7.2.8-arch1-2 is qualified; 6.18.55 is not).

```sh
python3 -B linux/tests/vm/mbr.py --build <build> \
    --kernel 7.2.8-arch1-2 --accel tcg --grub4dos-image --grub4dos-ntfs \
    --mkisofs <mkisofs> --ntfs-3g-package <ntfs-3g-package> \
    --ntfs-library-package <matching-library-package> <kernel-output>
```

Repeat with --grub4dos-ntfs-driver lowntfs-3g and a fresh output directory;
6.18.55-1-cachyos-lts qualifies actual kernel-support absence for that route.
Repeat both with sanitizer builds. Observe native ISO byte progress and one
simulated authorization, held real FUSE wrapper after unmount, held launcher,
released claims/descriptors/children/source identity and complete joined cleanup.
The image has no selected-loader or frozen-core snapshot. Read all four source
files with both 7z and the independent NTFS-3G reader. Verify original MBR/SBR/gap/
standard NTFS records, geometry, formatter-owned serial and clean filesystem.
Actual BIOS root/loader/menu CRC/variable/image-marker checks and missing-loader
controls remain the existing oracle. Keep FAT32 ISO and standalone NTFS fallback
as regressions. No host polkit or physical-device write is performed.

CP22n62 passes those six native jobs. Its initial wrong-kernel and filename-list
fixture failures are saved alongside final evidence. Three extra reader names
are read-only, and new write/remove/traversal attempts remain refused. The
--grub4dos-image-gui --grub4dos-ntfs combination was explicitly refused before
output creation at CP22n62. CP22n63 admits and qualifies that ordinary Qt route.


## CP22n63: ordinary Qt GRUB4DOS NTFS ISO

Add --grub4dos-image-gui to the CP22n62 NTFS image command. Build the existing
Qt/native targets and ntfs_file_fixture with GUI/VM tests enabled. The ordinary
image flow now requires FAT16 (Default), FAT32 and NTFS at 64 MiB, then selects
NTFS/4096-byte clusters. Confirmation uses the ISO caption, no cached standalone
loader or core, and zero caller serial. Observe original continuous NTFS ISO
progress and finalizing 64224/65535, not standalone discrete progress. Root PID1
models UID1000 through the existing test-only getuid wrapper and authorization
launcher; filesystem/device I/O remains the actual native code. This does not
qualify real host polkit, an unprivileged host session or physical media.

Run normally and with ASan/UBSan/leak detection for kernel ntfs and explicit
lowntfs-3g. While its real child has unmounted/exited, hold the owned FUSE wrapper:
no READY/beep/alert/private mount or source-free loader snapshot. Hold launcher
exit separately, then require joined full READY/one notification, released claim,
restored descriptors, reaped children, unchanged source and stable unplug. Reuse
CP22n62 two-reader/all-file/record checks and BIOS menu/missing-loader oracles.
Ordinary Qt FAT32 ISO and standalone Qt NTFS fallback remain native controls.

CP22n63 passes all six jobs. One sanitizer kernel negative-control deletion hit
its 15-second deadline under concurrent VM load after successful GUI creation,
readback and positive BIOS boot. A full isolated rerun passes without changing
code or deadlines; retain both logs. Component tests also expose and fix a real
completion-validator bug: a short NTFS format report used to reach Complete.


## CP22n65: native GRUB4DOS combined FAT32 BIOS/UEFI image

Build the existing native guest/helper targets and uefi_handoff_fixture. Add
--grub4dos-image-uefi to the ordinary --grub4dos-image FAT32 recipe:

```sh
python3 -B linux/tests/vm/mbr.py --build <build> \
    --kernel 6.18.55-1-cachyos-lts --accel tcg --grub4dos-image \
    --grub4dos-image-uefi --mkisofs <mkisofs> <combined-output>
```

Repeat with a sanitizer build and keep the plain BIOS recipe as a regression.
The generated source now has five files, including the existing 2048-byte x64
EFI application at EFI/BOOT/BOOTX64.EFI. Recursive source manifests and mdir/mtype
checks verify all leaves/bytes, including the 333310-byte copy total, alongside
original MBR/SBR/gap/standard FAT records and source preservation. Retain native
phase/progress/held launcher/joined cleanup checks and the BIOS menu/missing-loader
oracle. Boot the same verified FAT32 image using the existing OVMF/application
oracle with native_fat=True: expect the shared RUFUS_SYSLINUX_UEFI_OK marker,
emulator status 33, no UEFI:NTFS banner and unchanged snapshot backing media.
This marker belongs to the reused test application; no Syslinux is installed.

At CP22n65 the flag rejects missing image mode, GUI admission, FAT16/NTFS and a missing built
EFI fixture before output creation; CP22n66 admits the qualified GUI companion. CP22n65 qualifies native normal/sanitizer
combined jobs and plain BIOS regression. Combined MBR Qt admission is CP22n66;
this does not qualify an OS chainload, Secure Boot, other architectures, real
host polkit or physical media. No additional Windows customization is tested.


## CP22n66: ordinary Qt combined GRUB4DOS FAT32

Add --grub4dos-image-gui to the CP22n65 --grub4dos-image --grub4dos-image-uefi
command and build the existing Qt guest targets. Load the source normally,
require its shared GPT default and MBR option, then choose MBR: one qualified
FAT32 choice, BIOS or UEFI target, original ISO confirmation and retained x64
fallback/source facts. The physical Default marker is calculated before image
filtering; the 64-MiB target retains FAT16 as its physical default, so combined
FAT32 has no Default suffix. No extra payload/core/cache/partition is staged.

Run normally and with ASan/UBSan/leak detection. Observe native ISO progress and
held launcher after sync/unmount, no notification/full READY until joins, then
one completion beep/alert, released claims/descriptors/children/source identity
and stable unplug. Reuse CP22n65 five-file/original-record/missing-loader/BIOS/
UEFI checks, with the plain BIOS Qt recipe as a regression. The existing root
PID1/getuid wrapper/authorization stand-in models the caller only; native I/O is
real in the guest. Real host polkit and physical hardware remain unqualified.
CP22n66 admits and passes this GUI companion flag; combined FAT16/NTFS remain
gated until their contracts/native/GUI proofs are complete. No extra Windows
customization qualification is performed.


## CP22n67: combined FAT16 source/native image

Use the existing --grub4dos-image --grub4dos-image-uefi command with --fat-bits 16.
Build existing source/request and native/EFI fixtures; run normally and with
ASan/UBSan/leak detection, plus FAT32 regression. The same five-file fixture,
recursive mdir/mtype/record checks and BIOS/OVMF oracles suffice. Expect FAT16/
2048-byte clusters, table type 0x0E, 333310 copied bytes and direct native FAT
EFI application execution with no UEFI:NTFS. Retain one simulated authorization,
source preservation, exact phase/progress, held launcher after unmount and joined
cleanup. Firmware snapshots must not change backing disks. CP22n67 passes these
native jobs and source/GUI-gate suites, without a production helper change.

--grub4dos-image-gui with this combined FAT16 command remains refused until
CP22n68. Ordinary Qt still hides combined FAT16, while direct backend FAT16 is
now valid; its obsolete direct-refusal test row is removed and NTFS refusal
remains checked. OS chainload/Secure Boot, other architectures, real host polkit
and physical hardware remain unqualified. No extra customization test.


## CP22n68: ordinary Qt combined FAT16/FAT32

--grub4dos-image --grub4dos-image-uefi --fat-bits 16 now accepts the
--grub4dos-image-gui companion. After ordinary image loading, choose MBR and
require FAT16 (Default) plus FAT32, BIOS or UEFI, shared FAT16/2048-byte small-
device default and frozen source-owned x64 fallback. Select/freeze either FAT
geometry and keep no extra payload/core/cache. Run normal and sanitizer FAT16
jobs plus FAT32 regression, observing held launcher/sync/unmount, original
64879/65535 finalization, joined full READY/one notification, released resources,
unchanged source and stable unplug. Reuse existing all-file/record/BIOS/OVMF/
missing-loader checks; snapshot backing media must stay unchanged.

Preserve initial failed logs: old FAT32-only expectations were corrected to
shared FAT16 behavior; concurrent emulator QMP greetings timed out after correct
creation/readback; one isolated retry dropped letters in injected menu-variable
input. GRUB4DOS now uses 40-ms key holds and 80-ms spacing via optional shared
helper parameters. Other test callers keep default timing. All three final
native jobs are rerun individually with unchanged firmware/command deadlines.
This is test orchestration, not a production change. Combined NTFS pairing,
other architectures/OS/Secure Boot and real host/hardware qualification remain.


## CP22n70: paired GRUB4DOS NTFS native image

Use --grub4dos-image --grub4dos-image-uefi --grub4dos-ntfs with the retained
NTFS-3G/library packages and mkisofs, using a kernel with the new ntfs module:

```sh
python3 -B linux/tests/vm/mbr.py --build <build> --kernel 7.2.8-arch1-2 \
    --accel tcg --grub4dos-image --grub4dos-image-uefi --grub4dos-ntfs \
    --mkisofs <mkisofs> --ntfs-3g-package <package> \
    --ntfs-library-package <matching-library-package> <kernel-output>
```

Repeat with --grub4dos-ntfs-driver lowntfs-3g and 6.18.55-1-cachyos-lts to prove
actual kernel absence; run both with sanitizer builds, plus direct FAT32 control.
Run emulator jobs individually. Reuse existing foreground FUSE/launcher holds,
one authorization, source/resource ownership and complete joined cleanup. Read
all five files with 7z and the bounded reader, including its exact read-only EFI
path, and verify original records/main extent plus exact bundled trailing
UEFI:NTFS payload/table. Missing BIOS loader control changes only the main extent.

Actual BIOS menu/loader CRC and missing-grldr checks remain the existing oracle.
OVMF must emit the original UEFI:NTFS banner and then shared source application
marker/status33; snapshot backing disks stay unchanged. CP22n70 passes all five
native jobs without production/deadline changes. Combined NTFS GUI companion
still rejects before output until CP22n71. Other architecture/OS/Secure Boot,
real host polkit and physical media remain unqualified. No extra customization.


## CP22n71: ordinary Qt paired NTFS

Add --grub4dos-image-gui to the preceding paired NTFS command and build the
existing test_grub2_widgets_guest target. Qualified host kernels are
7.2.10-arch1-1 for kernel ntfs and 6.18.55-2-cachyos-lts for explicit lowntfs-3g
fallback. Run the two providers normally and with sanitizers, then the direct
FAT32 GUI control, with one emulator job at a time.

The GUI guest supplies both ordinary /dev/block partition links for paired NTFS;
its minimal devtmpfs environment has no udev. Ordinary selection retains GPT,
then offers MBR with FAT16 (Default), FAT32 and NTFS. Freeze NTFS, its required
bundled payload, source fallback and native geometry. The launcher records helper
output while preserving live progress, EOF cancellation and held exit. Check
one authorization, original ISO finalization, source/provider ownership, released
resources, one notification after full READY and stable completed unplug.
Existing five-file/two-reader/record/payload/BIOS/missing-loader/UEFI oracles apply.

Use a fresh RAM-backed scratch directory for host-side negative-control copies
if the output filesystem has long writeback waits. CP22n71 captures F2FS
rq_qos_wait inside fsync during ntfs_umount; this is outside the guest writer.
The final ordinary sanitizer recipes also run from /tmp without a debugger.
Do not change their deadlines, skip sync or remove the missing-loader control.
The test-only deletion API consumes both inode handles, so neither may be
closed again. Three create/delete/absence cycles per build and the existing
path/operation guard checks cover that cleanup correction. Native proof remains
virtual USB with representative x64 boot files; physical media/host polkit,
other architectures/Secure Boot and arbitrary OS chainload remain separate.


## CP24d — guest-kernel gzip helper and bootable readback (2026-10-18)

`mbr.py --compressed-raw-helper` extends the mounted raw recipe with 15 gzip jobs.
Run from the repository's parent workspace, with retained upstream sources and a
main build configured with `RUFUS_BUILD_VM_TEST=ON`. Build the same qualified
native installer without installing it on the host:

```sh
cmake -S rufus/linux/probes/syslinux -B .rufus-test-artifacts/cp24d/syslinux-build -G Ninja -DSYSLINUX_SOURCE_DIR=/home/marius/proj/lufus/.rufus-test-artifacts/deps/syslinux-6.04-pre1 -DBUILD_TESTING=OFF
cmake --build .rufus-test-artifacts/cp24d/syslinux-build --target rufus-syslinux
cmake --build BUILD --target test_mbr_handoff_guest compressed_helper_guest_probe rufus-prepare-helper udisks_fixture
ulimit -c 0
python rufus/linux/tests/vm/mbr.py --build BUILD --kernel 6.18.55-2-cachyos-lts --accel tcg --compressed-raw-helper --upstream .rufus-test-artifacts/deps/syslinux-6.04-pre1 --installer .rufus-test-artifacts/cp24d/syslinux-build/syslinux/rufus-syslinux NEW_OUTPUT
```

Use a new owned output directory for each run. This recipe attaches only a newly
created regular backing file, disables network, and gates the native driver with
root/PID1-parent/DMI/command-line fixture facts. No host block passthrough. Never
run the guest executables on host devices or add them to host CTest. The second
helper binary is compiled from production helper/worker/owner/writer code with
only guest-guarded link wrappers at its compressed boundary and positioned I/O,
fsync and close. Its observer is thread-local. It does not replace UID, sysfs,
ioctl, native target/source/home/mount checks or the real O_EXCL claim. It is not
installed. Guest UDisks properties and the counted/held pkexec launcher remain
fixture substitutes, so this does not qualify host polkit.

Each run first passes five Syslinux seed and five legacy raw jobs. Gzip cases:

- Digest/decoded-size mismatch, real sector mismatch and bad CRC: unchanged disk.
- Busy directory, caller home on target, cancellation after real unmount, and
  mounted success: one launcher and joined exit each.
- 17 actual bytes then EIO; 65536 actual bytes then cancellation; actual fsync
  followed by EIO; altered actual readback buffer returning EBADMSG; actual close
  followed by EIO; and observed success. Dirty bytes flush once. The probe checks
  EBUSY from another whole-disk O_EXCL open through I/O and just before close.
- Each observed/fault job starts from distinct poisoned target bytes. Independent
  reads compare decoded bytes and explicit padding (1 MiB + 37 -> 1 MiB + 512),
  and hash every byte beyond the actual writes. Check result/progress byte totals,
  retained sync errors, FAILED close outcome, released claim and joined children.
- Complete 64-MiB gzip restore using the unchanged production helper; full guest
  SHA256 then independent host hash, MBR/FAT/file readback and BIOS -> Syslinux/
  menu -> boot-sector marker, with unchanged backing media.

Guest timeout is 600 seconds for full-stream preflight and independent full-disk
hashes on TCG. A compressed mounted job has a bounded 60-second fixture deadline
and logs phase/counters if it reaches the previous 15-second boundary. Product
and legacy deadlines are unchanged. Initial parallel sanitizer timing failure is
retained; final sequential sanitizer mounted success takes 4175 ms. The complete
normal and sanitizer recipes pass with no ASan/UBSan/LeakSanitizer diagnostics.

CP24d qualifies a real 512-byte-sector kernel disk and a bootable fixture image;
it does not qualify physical USB, other real sector sizes, power-loss persistence,
a Linux-kernel/installed-OS boot, or ordinary compressed Qt selection. Other
sector sizes retain previous simulated/file evidence and the supported range.
The recipe removes its generated disk, copied ISO trees, kernel/initramfs/root
and readback copies after exporting serial/QEMU commands, input hashes, results,
claim observations and independent hashes. Evidence: `.rufus-test-artifacts/cp24d`.
Preserve the user's source images and Mac recovery USB. No further Windows
customization installation matrix is introduced.


## CP24e — ordinary gzip selection and confirmed Qt DD creation (2026-10-18)

Use --compressed-qt-helper with the CP24d recipe/qualified native installer to add
one ordinary SELECT/START/controller job after its 25 direct/helper regressions:

```sh
cmake --build BUILD --target test_compressed_qt_guest test_mbr_handoff_guest compressed_helper_guest_probe rufus-prepare-helper
ulimit -c 0
python rufus/linux/tests/vm/mbr.py --build BUILD --kernel 6.18.55-2-cachyos-lts --accel tcg --compressed-qt-helper --upstream .rufus-test-artifacts/deps/syslinux-6.04-pre1 --installer .rufus-test-artifacts/cp24d/syslinux-build/syslinux/rufus-syslinux NEW_OUTPUT
```

The new guarded child requires its MBR-driver parent/DMI/fixture marker, shares
the live private bus/source/home, and models UID1000 only in its front-end thread.
It creates no host/physical target and is never installed or host CTest. Its
production helper still runs as real root with native source/home/mount/claim/
ioctl/I/O checks. Private UDisks and the counted/held pkexec remain fixtures.
The VM stages the extra Qt libraries/binary and uses 2 GiB; timeout remains 600s.

Before launching the child, the fixture poisons 4096 target bytes. The ordinary
analysis worker must recover the accepted decoded size/hash; START must freeze
actual 512-byte geometry and exact physical totals. No authorization during scan
or before confirmation; exactly one helper, no completion while launcher is held.
Restore all 64 MiB, then compare Qt/guest/host hashes, independently inspect
MBR/FAT/files and execute the BIOS Syslinux/menu/boot-sector marker. Both normal
and ASan/UBSan/leak runs pass all 26 jobs. This combines host full-green Widgets
checks with a real guest-kernel controller/helper path; it is not a desktop UI,
host-polkit, physical-device or installed-OS/kernel-boot qualification. Other
sector sizes have focused component evidence, not new actual-device coverage.

Logs/input hashes/results survive; recipe disks, copied trees, root and archives
are removed even on failure. CP24e's evidence contains the focused Qt commands,
initial stale-fixture failures and the broad 600-second sanitizer timeout. The
current 45-case focused Widgets runs and complete analysis/native-facts suites
pass in both variants. Preserve user sources/Mac USB; no Windows install matrix.


## CP25c — XZ helper, actual kernel claim and bootable readback (2026-10-18)

```sh
cmake --build BUILD --target test_mbr_handoff_guest compressed_helper_guest_probe rufus-prepare-helper
ulimit -c 0
python rufus/linux/tests/vm/mbr.py --build BUILD --kernel 6.18.55-2-cachyos-lts --accel tcg --xz-raw-helper --upstream .rufus-test-artifacts/deps/syslinux-6.04-pre1 --installer .rufus-test-artifacts/cp24d/syslinux-build/syslinux/rufus-syslinux NEW_OUTPUT
```

This stages /usr/bin/xz and its libraries only as a guest fixture encoder and runs
45 jobs: the existing five Syslinux/five raw/15 gzip cases, then 20 XZ cases.
Preserve real UID/kernel/sysfs/ioctl/mount/home/source/block-claim/I/O/flush/close
checks. Only UDisks properties and counted/held pkexec are fixtures. Both normal
and ASan/UBSan/leak builds pass. XZ's 16 direct and four mounted jobs include
CRC64/hash/size/sector refusal, busy/home protection, post-unmount cancellation,
partial actual writes, sync/readback/close failure, exclusive-claim observations,
CRC32/SHA256 refusal/success pairs, no-data-check success and full 64-MiB restore.

After each write, compare actual bytes/padding and the complete unwritten suffix.
After final restore, require guest/host raw/gzip/XZ hashes, MBR/FAT/files and BIOS
Syslinux/menu/boot-sector execution. Full 512-byte actual-device coverage here
is a disposable guest, not physical media or installed-OS/kernel boot. Ordinary
XZ Qt selection is still gated. No new Windows customization installation tests.

The new full XZ job has a 300-second fixture bound and 15-second progress samples;
the 45-job recipe has 1200 seconds for Debug/sanitizer TCG costs. Existing gzip
and legacy bounds remain unchanged. Initial legacy/full-XZ timeouts, diagnostics,
input/provider hashes and final evidence are under cp25c. The sanitizer run uses a private /tmp tmpfs backing file after the workspace
flush deadline exposed host storage variability; binaries, cases and limits stay
identical. The normal run retains workspace-file proof. The recipe removes its
large files/trees on success or failure; preserve user sources and the Mac USB.


## CP25d — ordinary gzip and XZ Qt controller creation (2026-10-18)

```sh
cmake --build BUILD --target test_compressed_qt_guest test_mbr_handoff_guest compressed_helper_guest_probe rufus-prepare-helper
ulimit -c 0
python rufus/linux/tests/vm/mbr.py --build BUILD --kernel 6.18.55-2-cachyos-lts --accel tcg --xz-qt-helper --upstream .rufus-test-artifacts/deps/syslinux-6.04-pre1 --installer .rufus-test-artifacts/cp24d/syslinux-build/syslinux/rufus-syslinux NEW_OUTPUT
```

This stages both marker sets and adds two actual SelectionController jobs to all
45 direct/helper regressions. The gzip and XZ jobs each poison 4096 prefix bytes
and the final sector, freeze exact decoded identity/type/report facts through the
worker, check native geometry before confirmation, assert no early authorization,
launch one production helper and wait through its held exit. Restore all 64 MiB,
then compare controller/guest/host hashes, filesystem records/files and BIOS
Syslinux/menu/boot-sector execution. Both normal and ASan/UBSan/leak runs pass all
47 jobs. Use a new output path under a private /tmp tmpfs directory for predictable
host flush behavior; the cp25d/run-guest.py artifact records exact orchestration.

The new XZ worker/helper/child bounds are 180/300/600 seconds and the new complete
recipe bound is 1800; existing gzip/direct-helper bounds remain. This models caller
UID1000 only in the guest front-end thread, with a real root helper/kernel and
fixture UDisks/pkexec. No desktop-polkit, physical media, other actual sectors or
installed-OS/kernel boot qualification. Large generated files are removed before
exporting the small result directory, then its private tmpfs parent is removed.
Source/private-root hashes, diagnostics, controller markers and boot output remain.


## CP26c — BZIP2 native guest helper with retained gzip/XZ regressions (2026-10-18)

```sh
cmake --build BUILD --target test_mbr_handoff_guest test_compressed_qt_guest compressed_helper_guest_probe rufus-prepare-helper
ulimit -c 0
python rufus/linux/tests/vm/mbr.py --build BUILD --kernel 6.18.55-2-cachyos-lts --accel tcg --bzip2-raw-helper --upstream .rufus-test-artifacts/deps/syslinux-6.04-pre1 --installer .rufus-test-artifacts/cp24d/syslinux-build/syslinux/rufus-syslinux NEW_OUTPUT
```

Use a private /tmp tmpfs output parent; the cp26c/run-guest.py artifact records
exact normal/sanitizer invocations, UID, backing type, input hashes and cleanup.
This mode implies --xz-qt-helper, adding 22 BZIP2 jobs to all 47 old jobs. The
actual block/root helper tests metadata/check refusal, corrupt empty members in
four placements, busy/home/mount/cancel, retained claims through real dirty
write/sync/readback/close faults and concat/tail success. Poison all 64 MiB before
BZIP2 full restore, then require guest/host raw/gzip/XZ/BZIP2 hashes, independent
MBR/FAT/files and BIOS Syslinux/menu/boot-sector execution. Both variants pass 69.

New BZIP2 encoding/full-helper bounds are 120/300 seconds and the new whole
recipe permits 2400. All prior gzip/XZ/legacy bounds and assertions remain. A
first retained gzip Qt run timed out at its existing 90-second completion bound;
archive it and add 15-second type/state/phase/counter/held diagnostics before
retrying, without changing the deadline. Actual source/decoder/writer/helper/
protocol/controller production and fault-probe bodies remain unchanged.

UDisks/pkexec and the old Qt UID are fixtures; the kernel/block/native helper are
real guest root. Actual sectors are 512 bytes, backing is tmpfs, and BIOS boots
a marker sector rather than an installed OS/kernel. Physical media, host polkit,
other actual sectors and power-loss persistence remain unqualified. The ordinary
BZIP2 front-end is still gated for its later controller/READY checkpoint. Export
small evidence, then remove all roots/images/archives/trees/private parents.


## CP26d — full-size ordinary BZIP2 Qt creation (2026-10-18)

```sh
cmake --build BUILD --target test_mbr_handoff_guest test_compressed_qt_guest rufus-prepare-helper
ulimit -c 0
python rufus/linux/tests/vm/mbr.py --build BUILD --kernel 6.18.55-2-cachyos-lts --accel tcg --bzip2-qt-helper --upstream .rufus-test-artifacts/deps/syslinux-6.04-pre1 --installer .rufus-test-artifacts/cp24d/syslinux-build/syslinux/rufus-syslinux NEW_OUTPUT
```

Use a private /tmp tmpfs output parent; cp26d/run-guest.py records invocations,
UID, backing and cleanup. This focused mode retains five bootable seed jobs and
five raw setup jobs, then runs ordinary BZIP2 worker SELECT/START/confirmation/
production helper/joined completion (11 total). Poison the first 4096 bytes and
final sector and restore all 64 MiB. Check frozen identity/report/type/hash,
actual sector/padded total before confirmation, no early authorization, one
helper and held exit, complete counts/guest/host hashes, independent MBR/FAT/
files and BIOS Syslinux/menu/boot-sector execution. Both variants pass.

The same per-Qt poison/identity-pipe/child/join assertions are extracted for the
new and old modes. BZIP2 analysis/helper/child bounds are 180/300/600 seconds;
the new 11-job recipe permits 900. Old gzip/XZ and all prior modes/bounds/checks
remain unchanged. Reuse identical native backend/helper binaries and CP26c's
both 69-job qualifications rather than repeat unchanged fault matrices. Complete
80/81 host suites cover all three formats and READY/error/cancel behavior.

The front-end models UID1000 only in its main thread, with guest-root kernel/
helper and fixture UDisks/pkexec. Real logical sectors are 512, host backing is
tmpfs, and BIOS boots a marker rather than an installed OS/kernel. Physical
media, desktop polkit, other actual sectors and power-loss persistence remain
unqualified. Export small evidence then remove owned roots/images/archives/trees
and private parents. No user source or physical recovery USB is touched.


## CP27c — focused LZMA source/helper/kernel qualification (2026-10-18)

```sh
cmake --build BUILD --target test_mbr_handoff_guest compressed_helper_guest_probe rufus-prepare-helper
ulimit -c 0
python rufus/linux/tests/vm/mbr.py --build BUILD --kernel 6.18.55-2-cachyos-lts --accel tcg --lzma-raw-helper --upstream .rufus-test-artifacts/deps/syslinux-6.04-pre1 --installer .rufus-test-artifacts/cp24d/syslinux-build/syslinux/rufus-syslinux NEW_OUTPUT
```

Use a private /tmp tmpfs parent; cp27c/run-guest.py records UID, backing, command,
exit and cleanup. This mode retains the necessary five bootable seed and five
raw jobs, then four mounted and 24 direct LZMA jobs (38 total). It does not imply
older compression/UI modes. Keep those modes and reuse their unchanged backend
qualifications. Production helpers are byte-identical to CP27b.

Check actual source errors/EOF after 65536 committed bytes, six retained fault/
claim gates, metadata/size/history/dictionary/EOS refusal and known-prefix/tail
semantics. Eight observation cases hold the actual block claim through I/O/sync/
checked close and allow reacquisition afterward. Poison all 64 MiB before the
production full restore. Compare original raw/frozen LZMA/guest/host hashes,
independent MBR/FAT/files and BIOS Syslinux/menu/boot-sector execution. Both builds
pass. New LZMA encoder/small-helper/full-helper/whole-guest bounds are 120/90/600/
1500 seconds; old modes and bounds remain unchanged.

Host is unprivileged; guest driver/helper use real root with actual 512-sector
USB block/kernel claims. UDisks/pkexec are fixtures and host backing is tmpfs,
with no network or host device passthrough. No ordinary LZMA GUI, desktop polkit,
physical media, other actual sectors, installed OS/kernel boot or persistence
claim. Export small evidence and remove owned images/roots/archives/trees and
private parents; retain builds for the next ordinary GUI qualification.


## CP27d — full-size ordinary LZMA Qt creation (2026-10-18)

```sh
cmake --build BUILD --target test_mbr_handoff_guest test_compressed_qt_guest rufus-prepare-helper
ulimit -c 0
python rufus/linux/tests/vm/mbr.py --build BUILD --kernel 6.18.55-2-cachyos-lts --accel tcg --lzma-qt-helper --upstream .rufus-test-artifacts/deps/syslinux-6.04-pre1 --installer .rufus-test-artifacts/cp24d/syslinux-build/syslinux/rufus-syslinux NEW_OUTPUT
```

Use a private /tmp tmpfs parent; cp27d/run-guest.py records command, UID, backing,
exit and cleanup. New mode retains five seed/five raw setup jobs and one complete
64-MiB LZMA worker/START/confirmation/controller/helper/joined-exit job (11).
Require frozen source/report/hash facts, actual sector/padded total before
confirmation, no early authorization, one helper, held completion, complete
counters/guest/host hashes, independent files/MBR/FAT and BIOS markers. Poison
the prefix/final sector first; the full target poison is separately CP27c.

Both normal/sanitizer runs pass. Reuse unchanged production helpers and both
CP27c 38-job native qualifications; keep older modes/assertions/deadlines. New
LZMA analysis/helper/child/whole-recipe bounds are 180/600/960/1200 seconds. The
front-end models UID1000 only in its main thread; actual guest helper/kernel
are root. Metadata/pkexec are fixtures, actual sectors 512, backing tmpfs and
boot a Syslinux marker rather than an installed OS/kernel. Physical media, host
polkit, other actual sectors and persistence remain unqualified. Export small
evidence then remove owned roots/disks/archives/trees and private parents.


## CP28c — focused LZW source/helper/claim/fault qualification (2026-10-18)

Run as an ordinary user with a new output directory on private tmpfs:

```sh
python rufus/linux/tests/vm/mbr.py --build BUILD --kernel 6.18.55-2-cachyos-lts --accel tcg --lzw-raw-helper --lzw-encoder .rufus-test-artifacts/cp28a/ncompress-current --upstream .rufus-test-artifacts/deps/syslinux-6.04-pre1 --installer .rufus-test-artifacts/cp24d/syslinux-build/syslinux/rufus-syslinux NEW_OUTPUT
```

The pinned fixed ncompress binary is an independent test encoder/decoder. The
host stages 20 deterministic fixtures and verifies their complete output/error;
the guest also checks stored/decoded identities. This mode keeps five seed and
five raw setup jobs, then four mounted and 36 direct LZW jobs (50). Eleven real
claim observations cover write/cancel/sync/verify/close, late source EIO/EOF,
late invalid code and valid in-memory source-data changes during copy/verify.
Require exact byte boundaries, dirty sync, checked close, released claims and
preserved unwritten suffixes. EOF/incomplete-bit behavior has no fictitious CRC.

Poison all 64 MiB before the final four-pass production-helper restore; compare
raw/frozen LZW/guest/host whole hashes, filesystem/files and BIOS markers. Both
normal and sanitizer runs pass. Actual sectors are 512, guest root/kernel/I/O
are real, metadata/pkexec are fixtures and host backing is tmpfs without network
or passthrough. Small/full/encoder/whole bounds are 90/600/120/1800 seconds;
older modes and their deadlines remain unchanged. Ordinary LZW GUI/fallback,
physical media/polkit/other actual sectors/persistence/installed OS boot await
their qualifications. Export small logs/manifests and remove owned large inputs.


## CP28d — ordinary LZW and gzip fallback controller recipe

Use mutually exclusive `--lzw-qt-helper` with pinned `--lzw-encoder <ncompress>`.
It retains five seed and five raw setup jobs, then requires ordinary `.Z` and
LZW-data `.gz` full 64-MiB controller/helper restores and five real fallback
claim/source/data faults (17 jobs). Host gates require both exact numeric frames,
including HELD99=1/PERCENT=100/LAUNCHES=1, full raw/decoded/guest/host hashes,
independent filesystem/files and BIOS boot with unchanged backing SHA256.
The frontend main thread models UID1000; actual guest root helper/kernel/device
I/O and unprivileged host process remain real, UDisks/pkexec remain fixtures.
LZW-only analysis/helper/child/whole-guest bounds are 180/600/960/2400 seconds.
Pinned encoder is test-only; keep binaries and source inputs frozen during runs.
Both normal/sanitizer complete corrected-parser runs pass. Preserve the initial
normal host parser failure and controlled same-parser sanitizer stop separately.
Export small evidence and delete owned disks/archives/roots/tmpfs parents.


## CP29d — real guest ZIP helper recipe

Build `test_mbr_handoff_guest`, `compressed_helper_guest_probe`,
`rufus-prepare-helper` and `udisks_fixture` with Ninja's default parallelism.
Run the mutually exclusive recipe on a new output directory:

```sh
python rufus/linux/tests/vm/mbr.py --build BUILD --kernel 6.18.55-2-cachyos-lts --accel tcg --zip-raw-helper --upstream .rufus-test-artifacts/deps/syslinux-6.04-pre1 --installer .rufus-test-artifacts/cp24d/syslinux-build/syslinux/rufus-syslinux NEW_OUTPUT
```

The guest builds five ZIP entry methods using zlib and staged bzip2/xz fixture
encoders; production needs no ZIP executable. Require five seed/five raw jobs,
four mounted ZIP jobs, 23 direct ZIP cases and eight actual claim observations.
The final deflate archive restores a poisoned 64-MiB disk, then independent
guest/host hashes, FAT file reads and BIOS boot must agree. Normal and sanitizer
recipes pass as an unprivileged host user. Media and initramfs are tmpfs-backed
and removed after exporting small logs. The guest tests 512-byte sectors;
ordinary GUI ZIP creation remains a separate gate.


## CP29e — ordinary ZIP Qt controller recipe

Build `test_mbr_handoff_guest`, `test_compressed_qt_guest`,
`rufus-prepare-helper` and `udisks_fixture` with Ninja's default parallelism.
Run `mbr.py --zip-qt-helper` with the same pinned kernel, Syslinux source and
installer options as CP29d, using a new output directory. This mutually
exclusive recipe keeps five seed and five raw helper jobs, then makes a
deflate ZIP inside the guest and selects it through the ordinary Qt controller.
Require frozen decoded identity, target geometry before confirmation, no
authorization before confirmation, one helper launch, 99% while launcher exit
is held, joined 100%, full 64-MiB write/verify, independent host disk/FAT-file
readback and BIOS boot. Both normal and sanitizer variants pass. Export small
logs/results and remove disposable tmpfs disks and initramfs afterward.
## Full FFU batch

`mbr.py --ffu-qt-helper` runs one seed, a small plain-XPRESS FFU Qt job with
tail preservation and one full 64-MiB uncompressed FFU Qt restore. It checks
one helper per job, held-99/joined READY, independent disk/FAT-file readback
and BIOS boot. Run once with the normal build and once with ASan/UBSan;
reuse the shared helper fault evidence. All media is disposable. CP34 records
the external V1/V2 encoder and codec comparisons and cleanup.

## CP35 combined device-saving qualification

`mbr.py --save-qt-helper` creates one bootable seed and captures VHD, VHDX and
FFU together through the ordinary selection/save controller. The root fixture
passes an exclusive read-only descriptor over private D-Bus to a child running
with actual uid/gid 1000; the child cannot open the raw device itself. Pending
authorization cancellation and denial preserve an existing backup. A separate
disposable ext4 virtio disk retains the outputs for independent host readers.
The route restores the saved VHDX through the production helper, then checks
whole-disk/FAT contents and BIOS boot. Existing helper faults are reused.
Run once for normal and once for sanitizer binaries. Export logs, checksums and
reader results, then remove the export disk along with all guest media. Never
point this fixture at a host block device. See CP35 for the compact runner and
its QEMU/Img2Ffu readback evidence.


## CP36 combined source unmount and ISO export qualification

Add `--save-export` to `mbr.py --save-qt-helper`. This uses one bootable seed,
then six source-helper jobs through the real uid/gid 1000 Qt client: mounted
capture cancellation, mounted VHDX capture, busy-source refusal, injected
private-unmount failure, FAT ISO and NTFS-3G ISO. The guarded setuid launcher
exists only in the guest and records one authorization per job. The helper
holds its exit so the client can verify READY waits for joined cleanup.

A separate disposable virtio disk stores captures. The guest mounts each ISO
read-only with the kernel UDF driver and compares all paths and file hashes,
including hidden/Unicode entries and `ldlinux.sys`. The distinct NTFS-3G daemon
must exit. One saved VHDX is restored through the existing helper and booted,
reusing the shared writer fault evidence. Both normal and sanitizer variants
run this combined route. No host block device is exposed to the guest.
The CP36 runner retains compact logs, input manifests, kernel file inventories,
QEMU VHDX decoding and ISO recognition checks, then removes disposable media.


## CP37 read-only discovery and capture batch

Use `mbr.py --save-qt-helper --save-export --save-read-only`. One seed is followed
by VHD, mounted VHDX, ISO and GPT FFU capture through the real uid/gid 1000
client and production discovery. Only the guarded fixture sets/clears kernel
BLKROSET on its simulated USB disk and partition. Every capture must retain
BLKROGET=1 and the complete source SHA256. Unmounted VHD/FFU each use one
UDisks read acquisition; mounted VHDX/ISO each authorize one source helper,
including cleanup and held process exit. Kernel UDF readback compares the ISO
file tree; QEMU independently decodes VHD/VHDX on the host. Native capture
verification covers every output, reusing CP35's independent FFU qualification.

Default write snapshots refuse read-only metadata. A native target open also
refuses when only the guest's metadata is deliberately changed to writable,
proving kernel write protection remains effective. After all captures, the
fixture clears protection and restores one saved VHDX through the existing
writer, with full disk/file readback and BIOS boot. Do not expose host media.
The CP37 runner retains compact logs, manifests and hashes and removes images.

### Runtime UEFI media validation batch

`mbr.py --runtime-validation` creates three small ISO fixtures and drives the
ordinary Qt SELECT/ISO-copy/confirmation path for GPT FAT16, FAT32 and NTFS on
an owned 320-MiB virtual USB. Each job checks one authorization, held launcher
exit, full joined READY, independent filesystem MD5/config/original-EFI/resource
readback and claim release. A conflicting original-loader backup is refused by
the real helper with the whole disk unchanged. Existing MD5SUMS and generated
runtime lists have separate expectations: untouched incorrect original checksums
remain incorrect in the existing source list.

The final NTFS media boots the unchanged bundled validator and a bare native EFI
marker through UEFI:NTFS. Changing one payload byte in a disposable copy must
report one validation failure and wait at the unanswered continue prompt. No
Windows application or installation runs. No network or host block device is used.

```sh
cmake --build BUILD --target test_mbr_handoff_guest test_native_format_qt_guest rufus-prepare-helper uefi_handoff_fixture partition_image_probe udisks_fixture
python3 linux/tests/vm/mbr.py --build BUILD --kernel KERNEL --accel tcg \
  --runtime-validation --mkisofs /absolute/path/to/mkisofs NEW_OUTPUT
```

Repeat once with the sanitizer build; do not build either directory while its
tests are running. Export JSON/logs, then remove owned guest disks and stages.

## Native application language batch

`localization.py` runs the original 38-language catalog/parser and representative
live French/Arabic/Romanian state, saved-choice and RTL checks as UID1000 in a
networkless, diskless guest. Build `test_localization`, then run once per normal
and sanitizer build. It reuses one parsed catalog for all language assertions;
it adds no USB writer or authorization matrix.

```sh
python3 linux/tests/vm/localization.py --build BUILD --kernel KERNEL --accel tcg NEW_OUTPUT
```

Do not rebuild that directory during the guest run. Preserve compact JSON/logs,
then remove the owned initramfs/staging directories.


## Native application update batch

`application_updates.py` uses the same diskless, networkless UID1000 fixture
shape. Its private loopback server qualifies available/current/forced/channel
release selection, bounded metadata and dates, cancellation, settings consent
and deferred notifications. It does not download a package or execute an
updater. Build `test_application_updates`; run once per normal/sanitizer build,
then export compact logs/JSON and remove owned staging.

```sh
python3 linux/tests/vm/application_updates.py --build BUILD --kernel KERNEL --accel tcg NEW_OUTPUT
```


## ISO controls batch

`mbr.py --iso-options --mkisofs <mkisofs> --msdos-resource <pinned-diskcopy.dll>`
uses the existing guarded root/PID1/DMI disposable guest and ordinary Qt client.
It groups MS-DOS/FAT16 with explicit 64 KiB clusters, FAT32 ISO9660/Rock Ridge and
NTFS UDF timestamp-preserving jobs, independent kernel bytes/time/BPB readback,
one authorization and joined READY per job, claim release and one original
UEFI:NTFS/native EFI fixture boot. The Microsoft DLL is read only as data and is
never executed or packaged. Run normal/sanitizer builds as one qualification
batch. Existing writer faults/polkit/customization evidence is reused.


## NTFS options batch

`mbr.py --ntfs-options --mkisofs <mkisofs> --ntfs-3g-package <package> --ntfs-library-package <matching-library>`
uses the existing guarded disposable guest, ordinary Qt client and one helper
per job. Three jobs cover compressed NTFS, the 8192-byte uncompressed fallback
and a Windows-metadata/native-EFI ISO with actual compressed payload allocation
and uncompressed boot-loader files. It independently reads flags/hashes through
the kernel, checks one authorization/joined READY, refuses a missing required
provider before writes and boots one EFI fixture. Packages are staged only in
the guest. No Windows app/installation or host block device is involved.
