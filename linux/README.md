# Rufus Linux components

The Linux port uses **Qt6 Widgets** and follows Rufus's original Win32 dialog
layout, labels, control order and default visibility. The form uses standard
widgets and layouts; the desktop palette is the default, with an explicit saved
light/dark override. Keep device/media logic in jobs and controllers.
[Qt Widgets documentation](https://doc.qt.io/qt-6/qtwidgets-index.html).

Start with the [supported media and limits](../docs/linux-support.md) for current
GUI choices, tested boot examples, runtime requirements and unavailable features.
The [build instructions](#build-and-test) and [desktop packaging guide](packaging/README.md)
describe how to run a matching GUI/helper pair. The component notes below retain
historical checkpoints; their early test counts are not the current suite total.

Preview49 builds require Qt 6.4 or newer and treat compiler warnings
as errors by default. CI targets Ubuntu 24.04. Native code uses nondeprecated
Qt APIs; bundled dependency fixes are applied as tracked patches to private
build copies. Original upstream inputs and boot payloads remain unchanged.
The [warning qualification](../docs/linux-compiler-warnings.md) records zero
compiler warnings, 266 passing local suites and 34 affected sanitizer suites.
The release reuses earlier media/desktop evidence with its existing limits.

CP78 / Preview47 prepares the useful x86-64 release, with original Rufus
translations and disclosed Linux-specific English fallback, Debian/Fedora native
packages, an Arch AUR source recipe and GitHub tag/release instructions. The
[release evidence index](../docs/linux-release-evidence.md) states qualification
and remaining hardware/architecture limits. Existing UDF support remains;
UDF2.50/2.60, ReFS formatting and Dev Drive detection are user-deferred. The
actual Windows UDF indexing attribute is unavailable.

CP76 / Preview45 delivered 512/512e/4Kn formatting geometry, 64 KiB FAT32 clusters
and UDF provider bounds; see the [geometry guide](../docs/linux-sector-geometry.md).

CP75 / Preview44 delivered B4 legacy Windows, ReactOS and Kolibri ISO routes.

CP74 / Preview43 delivered B3 ISO extraction, EFI and boot resources. Source-selected
EFI architectures, embedded EFI/repair, NTFS/FAT/UDF link behavior, Syslinux version
resources/old modules, Slax/mastering recovery, portable accessors and local cached
issuer metadata are integrated. See the [ISO/EFI guide](../docs/linux-iso-efi-resources.md).
The current completion register records the remaining work.

CP73 / Preview42 delivers optical ISO capture, USB reset/ESP toggling,
exclusion settings, file-backed virtual disks, shared source/home backing
resolution, session locking override and explicit VHD/VHDX parent relocation.
See [device/source operations](../docs/linux-device-source-operations.md).
The current [register](../docs/linux-parity-register.json) tracks the remaining batches.

CP72 / Preview 41 groups desktop shortcuts, worker priority, cache cleanup,
USB logging, explicit dark/light appearance and progress views. See
[desktop controls](../docs/linux-desktop-controls.md). Full native translation
coverage remains an explicit final-completion item.

CP70 / Preview 39 groups NTFS compression, file indexing and uncompressed
boot-loader creation. See the [NTFS options guide](../docs/linux-ntfs-options.md).

CP69 / Preview 38 groups ISO support, Joliet/Rock Ridge, timestamp preservation
and size checks with the explicit MS-DOS 64 KiB override. See the
[ISO controls guide](../docs/linux-iso-options.md).

CP32 delivers the first versioned Release GUI/helper package for Arch/CachyOS
and a generic CPack install archive, with About/license/credits and headless
help/version. The [delivery finish lines](../docs/linux-release-plan.md) track
this usable preview milestone and the remaining feature-completion batches.

CP59 / Preview 28 groups portable INI/storage, startup preferences/instance wait,
saved advanced panels and automatic/persistent session logs. See the
[desktop guide](../docs/linux-desktop-settings.md) for shortcuts and storage paths.

CP60 groups UDF data formatting and non-bootable ext2/ext3 MBR/GPT/SFD layouts.
The [formatting guide](../docs/linux-filesystem-formatting.md) records provider
requirements, supported revisions and the remaining UDF/ReFS work.

CP61 / Preview 30 groups runtime UEFI media validation and checksum maintenance
for config changes, Windows file replacements and generated split-WIM parts.
See the [runtime validation guide](../docs/linux-runtime-validation.md) for
eligibility, original-loader preservation and supplementary ZIP ordering.

CP62 / Preview 31 adds bundled-policy Secure Boot issuer/revocation checks to
image analysis and the original START-time revoked-loader warning. The
[Secure Boot guide](../docs/linux-secure-boot.md) records its metadata/trust
boundary and remaining firmware work.

CP63 / Preview 32 adds opt-in remote policy refresh, scheduled DBX checks,
confirmed atomic caches and cancellation in Settings. Existing selections keep
their analysis snapshot; reselect an image to use new policy. See the
[policy update guide](../docs/linux-policy-updates.md).

CP64 / Preview 33 adds normal MS-DOS and advanced ReactOS boot records on
MBR/FAT16/FAT32, with explicit MS-DOS locale choices and a confirmed fixed-hash
Microsoft resource cache. The [legacy boot guide](../docs/linux-legacy-boot.md)
records behavior, native metadata differences and bounded qualification.

See the [Linux port implementation plan](../docs/linux-port-plan.md) for the
agreed architecture, reuse strategy and implementation phases.
The [testing and checkpoint plan](../docs/testing-and-checkpoints.md) defines
small delivery steps and how to test without physical USB drives.

The standalone [CP12a Syslinux probe](probes/syslinux/README.md) builds the
unchanged upstream native installer using CMake and qualifies Rufus's matching
v6 payload on small FAT32 images, including two BIOS volume boots. CP12g shares
its native build with the main Linux project; CP12l qualifies a real Alpine BIOS
boot and CP12m enables the narrow MBR/FAT32/Syslinux route in Qt.
CP12b adds a read-only native-pair check to the Linux component build and extends
that probe with 23 selection cases and four extracted-ISO boots. These preserve
shared root-config/backup rules and unknown detection. The native root companion
file, remaining search-path limits and reproduction commands are documented in
the probe; these component checkpoints did not enable BIOS writes.
CP12c/d add process-group supervision and source-layout checks. CP12e connects
them through `InstallNativeSyslinux` (originally `InstallNativeSyslinuxImage`), a dedicated file-only worker operation
with retained source/target checks, final sync, private-directory cleanup and
independent loader/VBR readback using Rufus's FAT reader and patcher. Its contract
requires a newly formatted, privately populated FAT32 image and the qualified
native executable/tools. 32 adapter cases, 24 packed-field checks and eight
extracted-ISO boots pass. Later CP12f–k add MBR/device/helper integration.
CP12f adds file-only Syslinux MBR creation/readback to the existing partition
component: one active FAT32 LBA partition, shared Rufus placement and its original
MBR payload. Two whole-disk BIOS boots and 57 normal/sanitized MBR calls pass.
CP12g adds optional native-installer packaging through that same CMake recipe.
The staged private executable passes two whole-disk boots; changed payloads fail
configuration. Qt integration follows in CP12m. See the probe notes for scope and
reproduction.
CP12h qualifies MBR preparation on simulated USB block devices: bounded clearing
of old disk/new-partition metadata, verified sync, conversion from GPT and kernel
partition refresh while keeping exclusive access. The existing table-only API
still refuses uncleared GPT. 26 new preparation/readback and 57 MBR checks pass
with sanitizers; guest tests reject unsupported 4K media unchanged.
CP12i adds explicit MBR metadata handling to the existing device formatter/mount
backend, keeping GPT as the default. Thirteen simulated-USB cases pass normally
and with sanitizers, including complete readback after filling FAT32 through the
disk's final sector. The backend reuses the same CLI handoff, sync/unmount and
claim reacquisition.
CP12j extends the same Syslinux adapter to retained whole-device descriptors,
checking the exact MBR layout, device identity/capacity and disk sequence before
and after installation. The disk claim stays held through upstream Syslinux and
its mtools children. Ten normal/sanitized guest cases and independent readback
pass; the resulting disk boots through MBR/Syslinux/menu to the fixture payload.
CP12k connects the complete MBR/FAT32/Syslinux job to the existing privileged
helper with the explicit `prepare-syslinux-device-v1` operation. Fresh preflight
checks policy, source/config/module compatibility and fixed installed tool paths
before disk writes. It reuses formatting, shared extraction, progress, private
mount cleanup and cancellation, then installs/verifies/syncs Syslinux before
helper success/exit. There is no second authorization. Five actual-helper guest
cases pass normally and with sanitizers, including installer cancellation and
unknown-version compatibility; independent readback and whole-disk boots pass.
CP12l qualifies the unchanged Alpine 3.22.5 ISO through that job, independent
readback and a full BIOS USB boot. It also corrects source eligibility to follow
Win32's known-version/suffix selection for preserved distribution modules.
Unknown images and any overwritten root companion still need the exact pinned
pair; installed-payload readback remains exact.
CP12m connects Qt partition selection, the unchanged ISO/DD dialog and one-helper
creation. Eligible hybrids default to MBR/FAT32, with GPT/UEFI still selectable.
Source/payload and installed-tool preflight runs before authorization; missing
tools never silently switch modes. Unknown versions still need the exact pair.
CP12p qualifies the same MBR/FAT32 media under SeaBIOS and OVMF. Images with
regular EFI boot files now show **BIOS or UEFI** in the target and confirmation;
BIOS-only images retain **BIOS (or UEFI-CSM)**. This qualifies the existing copied
EFI path, with no additional bootloader implementation. Secure Boot enforcement,
other distributions/architectures and physical firmware remain separate.
Widgets tests cover
progress, cancellation and successful READY after helper exit. The normal idle
status bar now matches Win32's `Using image: filename.iso`; details stay in the
log and tooltip. See the checkpoint record for qualification limits.
After DD or ISO creation is cancelled, the lower bar uses Win32's short
**Operation cancelled by the user** wording; the full diagnostic stays in its
tooltip and the log, leaving room for the elapsed-time field.

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
CP10i qualifies the GPT adapter on retained exclusive USB descriptors in a VM.
Stock mkfs.fat's second exclusive open conflicts with that ownership; its device
formatting handoff is qualified in CP10j: release, run the CLI, reacquire and
verify. CP10m integrates GPT/FAT32 preparation into one authorized helper,
with at most one privilege prompt per creation.
CP10v connects ISO START to preflight, confirmation and that same helper for the
initial GPT/FAT32 x64 UEFI workflow; see the supported settings below.
CP10w shares Rufus's label normalization and FAT32 cluster choices with Windows.
CP11a adds file-based GPT layout for UEFI:NTFS; CP11b adds native NTFS formatting
on partition-sized regular files, sharing Rufus's NTFS cluster rules and FAT's
child-process supervision. CP11c writes the bundled UEFI:NTFS payload through the
existing raw writer with checked extent bounds and GPT verification. An optional
offline VM qualifies NTFS EFI/Linux boot; see [its recipe](tests/vm/README.md#uefintfs-firmware-handoff).
CP11d adds two-partition discovery and native NTFS preparation to the authorized
helper, retaining the whole-disk claim during formatting. CP11e writes/verifies
the embedded UEFI:NTFS payload on the retained device, preserving its padding
and the main filesystem. CP11f adds NTFS ISO extraction through the common
extractor and cleanup path, preferring the new kernel `ntfs` driver with an
NTFS-3G fallback. CP11g adds ISO9660/UDF files beyond 4 GiB; creation and independent
readback pass with both providers, including sanitizer-built NTFS-3G jobs.
CP11h shares Rufus's filesystem eligibility/preference rules and reuses FAT32
compatibility in Linux preflight. CP11i also shares the extra-partition decision.
CP11j connects FAT32/NTFS selection and Windows ISO preflight to this path in
Widgets. Generated metadata images and simulated-device tests cover the GUI
connection. CP11k also qualifies actual Windows 11 media creation under one
native authorization. Independent readback matches every file in the supplied
25H2 ISO, then Windows 11 Pro installs onto a separate VM disk and boots to its
first-use screen with the USB detached. Account setup, activation and physical
USB qualification are outside this baseline test. See the
[real Windows VM recipe](tests/vm/README.md#real-windows-11-creation-and-observed-installation).
Secure Boot analysis
remains a separate increment; see the
[Windows-image boundary](../docs/linux-windows-image-metadata.md).
Runtime validation of the original Win32 Rufus application remains deferred,
as requested; the Windows OS installation above qualifies Linux-created media.
CP11l shares Rufus's Windows answer-file generation with explicit platform
inputs and component tests. CP11m adds native OEM answer-file application for
account, privacy, BitLocker and QoL choices in the same authorized helper,
with shared image eligibility and ISO/UDF functional tests. CP11n connects these
five choices through a Widgets dialog before confirmation and authorization,
including remembered selections, username validation and cancellation. All Windows
ISO customization options remain in scope except copying the current Windows
user's regional settings, which stays visible but disabled on Linux. Later
checkpoints below enable the remaining eligible customization paths; the
[current support table](../docs/linux-support.md#windows-customization) summarizes them.
CP11o adds a tested upstream-wimlib adapter for updating the extracted `boot.wim`.
CP11p adds native SYSTEM-hive editing through Hivex, reusing Rufus's shared value
names and preserving the input. CP11q connects those adapters and enables the
hardware-requirements checkbox, preserving Rufus's answer-file fallback,
appraiser placeholder and original x64/ARM64 setup-wrapper resources. The same
authorized helper performs all stages and cleanup. Component tests use small
WIM/hive fixtures and remove their outputs automatically. The privacy-only
media creation test remains the single customization qualification in scope.
CP11q's 26 Setup checks and affected OEM/preflight/Widgets/helper suites pass
normally and with ASan/UBSan/leak detection (10 suites total). This verifies the
native application path with small fixtures; Windows execution of the selected
customization remains deferred.

CP11r enables the original CA 2023 bootloader checkbox for eligible images.
It copies the needed Setup-image fonts and EFI loaders through native wimlib,
using Rufus's shared architecture order and the same authorized helper. Each
destination uses atomic Qt replacement; incomplete selected changes fail the job.
CA-only selection leaves `boot.wim` unchanged. Combined hardware-requirements
selection preserves the registry/answer-file fallback paths. Small generated
fixtures cover copying, cancellation, failures and GUI/helper communication;
this adds no Windows customization installation test.
All 10 affected suites pass normally and with ASan/UBSan/leak detection: 248 Qt
checks (including 27 new Setup checks) plus the disposable-file helper tests.

CP11s enables SkuSiPolicy through Rufus's shared first-logon command, using the
installed Windows system's policy file. Policy-only creation leaves `boot.wim`
unchanged. A small shared mask correction ensures SkuSiPolicy and QoL generate
their first-logon commands when selected independently, without requiring
another checkbox. The original command payloads and one-helper lifecycle remain
unchanged; Windows execution of these customizations remains deferred.
All 12 affected suites pass normally and with ASan/UBSan/leak detection.

CP11t adds a read-only edition/Setup-language component using the existing ISO
reader and upstream wimlib metadata API. It preserves edition indexes, Rufus's
label fallbacks and its `en-US` language fallback, while retaining full language
tags. Generated ISO9660/UDF WIM/ESD fixtures and focused regressions pass normally
and with sanitizers. CP11w connects this component to the existing preflight
worker: its result carries owned editions, Setup language and the first install
archive's path to the GUI. Optional metadata failures leave ordinary creation
available; cancellation or a changed ISO still aborts preflight. Silent
installation was kept disabled until the complete creation-flow integration
in CP11aa below.

CP11x puts the edition dropdown beside the silent-install checkbox, as in
Win32. It receives the preflight labels and original WIM indexes without choosing
an edition. Both controls stayed disabled at that checkpoint. A tested Widgets regional
editor collects explicit language, keyboard and time-zone settings without host
defaults, using Qt's locale suggestions and Windows time-zone mapping. This
editor is now opened by CP11aa's creation flow. Its input checks validate syntax,
not language-pack availability in an edition. The Windows-host regional-copy
option remains visibly disabled.

CP11y adds the native silent-install answer application.
It accepts Rufus's existing options structure and uses the shared XML writer.
When hardware bypass also edits the Setup registry, it retains the silent WinPE
section and commits the registry and answer together. It preserves the original
answer-file fallback if registry editing fails. Explicit input checks run before
file changes. CP11z carries explicit settings and the acknowledgement through the
existing helper request. Before any partition writes, the helper re-reads source
metadata, checks the selected archive/index and obtains the Setup language. It
then uses the same backend for application. Ordinary requests retain their wire
format; older helpers reject the additional silent fields. The 4 KiB request
limit is unchanged.

Silent requests currently require a sole conventional `sources/install.wim` or
`install.esd` archive. The shared writer selects by image index, without naming
the archive; ambiguous or nonstandard layouts are therefore refused for silent
installation while ordinary creation remains available. This is an interim
restriction pending explicit archive handling, not removal from the port's scope.
CP11aa connects the GUI flow for supported images. Choose a local account and
privacy skipping, then use **Choose regional settings…** beside the permanently
disabled Windows-host copy option. After accepting explicit regional values, the
silent-install row appears, as Win32 reveals it after its prerequisites. Check
silent installation and select an edition; no edition is chosen automatically.
OK then shows Rufus's original warning and three unchecked acknowledgements.
All three are required before continuing to the usual USB-erasure confirmation
and single authorization. Cancelling the warning returns to the options dialog.

Changing a choice invalidates an open warning, and device removal closes both
parent and child dialogs. Silent mode, edition, regional inputs and consent are
never remembered for another creation job. The controller merges only Windows
choices into its retained request and revalidates the source before authorization;
the helper independently revalidates before writes. Windows-host regional copying
stays disabled, and Linux host defaults are never substituted. This path has
component/simulated-device coverage; no extra Windows installation qualification
was run. The installed host helper has not been updated by these checkpoints;
use matching rebuilt GUI/helper versions for a future physical trial.

CP11ab adds Rufus's **Ctrl+Alt+E** expert-mode toggle and its S Mode choice.
Expert mode is saved, while S Mode itself is remembered only for this session
and starts unchecked after relaunch, following the original selection mask.
The checkbox retains Rufus's incompatibility warning and tooltip. As on Win32,
selecting it does not silently clear online-account bypass. The native backend
uses the existing shared `offlineServicing` XML and the original OEM/Setup-WIM
placement rules; it adds no Linux-specific S Mode implementation. This setting's
effect inside installed Windows remains deferred.

CP11ac qualifies upstream `wimlib_split` directly on the borrowed ISO/UDF reader.
The tests retain both editions and
all file contents, check split progress and fsync, and exercise failure/cleanup
boundaries with small disposable fixtures. Solid archives are refused by the
upstream splitter, and an individual resource can produce an oversized part.
CP11ad adds an explicit extraction option with read-only resource/name checks,
same-filesystem staging, actual FAT32 size checks and no-replace publication.
Cancellation/errors remove this operation's parts and preserve existing files;
progress retains Rufus's compressed-resource arithmetic. CP11ae connects the
helper's freshly checked decision for enhanced installation (Windows Alt+E),
while ordinary GUI requests keep that preference false by default. Read-only upstream split
planning supplies a conservative output reservation; partition checks include it,
and extraction checks mounted free space before writing. Actual split output is
bounded by that reservation. This does not model every filesystem/customization
allocation. The split suite now has 51 checks. CP11af qualifies the real Windows
11 ISO through the production helper on a disposable FAT32 USB: one authorization,
two FAT32-sized SWMs, all 11 images verified by system wimlib, and all 975 ordinary
files unchanged. Sync, cleanup, GPT/FAT32 and read-only disk checks pass. The source
ISO stays on a read-only share and disposable disks are removed after verification.
CP11ag exposes **Alt+E** in Qt as a persistent enhanced-installation preference,
separate from Ctrl+Alt+E expert mode. For a supported large-WIM Windows ISO, enable
it and choose **FAT32**; **NTFS remains the default**. The shortcut preserves an
allowed filesystem selection and resets cluster choices as Win32 does. Turning
it off removes incompatible FAT32 choices. It is ignored during scanning,
preflight, confirmation and creation, so it cannot change an active request.
Linux's status says **UEFI only**, because BIOS installation is not implemented;
the target remains GPT/UEFI. The preference is saved under
`EnableWindowsDualUefiBiosMode` in the existing Rufus/Linux QSettings file.
CP11ah boots independently verified FAT32/SWM media into Windows 11 Setup under
UEFI, with no installation disk or customizations. WinPE sees both SWM files;
DISM reads their metadata and Setup displays all 11 editions. Applying the full
split archive during installation remains untested. The runner's final return
was interrupted (status 143); its backing USB hash was verified separately after
QEMU/TPM exited. Disposable disks were removed after saving evidence. No extra
customization trial ran.
CP11ai adds VM supervisor status records and interrupt cleanup, tested with tiny
processes and diskless QEMU/TPM; it does not repeat the Windows media workload.
Multiple/nonstandard-archive silent installation remains deferred: the archive
picker alone does not bind the index-only answer to a specific Windows Setup path.

CP11u shares Rufus's original progress allocation and percentage arithmetic
between Win32 and Qt. ISO extraction reports 2,048-byte blocks through the
existing C reporting adapter and the same authorized helper. The copy status
shows one decimal place, while the bar uses the original weighted operation
ranges. A completed copy does not show READY until flush, unmount and helper
cleanup succeed. Native formatting and customization currently advance at
phase boundaries; their finer progress callbacks remain future work. The
installed helper and GUI must be updated together to enable these new reports.

After a successful creation the bar is full, green and says **READY**, and Qt
requests one completion beep/desktop alert. Idle READY has an empty bar. During
sync and cleanup the ISO path shows Rufus's “Finalizing, please wait...” text.
Success waits for filesystem sync, private-mount cleanup, NTFS provider shutdown
where used, and helper exit; errors cannot produce completed READY. The raw path
also waits for device sync, verification and worker/descriptor release. Removing
the USB after success leaves the completed result intact. The application does
not eject or power off the device. Linux only removes its temporary private
write mount; Windows Rufus normally leaves the volume accessible by drive letter.

## Build and test

The Linux port is strictly CMake-based, including dependencies compiled as part
of the port. Ninja is the preferred generated backend. The Windows application
continues to use Rufus's existing Visual Studio/MSBuild and MinGW/Autotools
builds; `linux/CMakeLists.txt` is the Linux entry point.

Requires CMake 3.21+, a C compiler, a C++17 compiler, Qt6 Core/Widgets/DBus/Network 6.2+,
OpenSSL 3.0+ Crypto development files (`libssl-dev` on Debian/Ubuntu, `openssl`
on Arch), zlib development files for the compressed-source foundation,
pkg-config, libfdisk/libblkid/libmount 2.35+, e2fsprogs headers and Hivex development files
(`libfdisk-dev`, `libblkid-dev`, `libmount-dev`, `libext2fs-dev` and
`libhivex-dev` on Debian/Ubuntu; `util-linux-libs`, `e2fsprogs` and `hivex` on Arch),
`patch`, and the [upstream wimlib 1.14.5 source release](../docs/linux-wimlib-prototype.md#reproduction).
The application uses the extended upstream library for ordinary and embedded WIMs.
Native Windows image deployment additionally links the NTFS library and headers
from ntfsprogs-plus, or libntfs-3g when that provider is unavailable. On Debian/
Ubuntu install `ntfs-3g-dev`; ntfsprogs-plus includes the headers on CachyOS.
This offline library dependency does not change the preferred kernel mount driver.
The registry adapter links the distribution's Hivex C library through pkg-config;
there is no secondary dependency build or configure-time download.
System wimlib development files are needed only for optional comparison tests
(`libwim-dev` on Debian/Ubuntu, `wimlib` on Arch). Native preparation requires
`mkfs.fat` from dosfstools and `mkntfs` from ntfs-3g or ntfsprogs-plus; CMake
records their absolute paths for the helper.
Tests additionally require Qt6 Test, `dbus-daemon`, Python 3 and libarchive's
`bsdtar`, plus `genisoimage` or cdrtools `mkisofs` for UDF fixtures. FAT fixtures
require `mkfs.fat` from dosfstools and `mcopy`/`mmd` from mtools. NTFS fixtures
require `mkntfs`, `ntfsinfo` and `ntfslabel` (Debian/Ubuntu's `ntfs-3g` package,
or ntfsprogs-plus). CMake overrides are `RUFUS_MKNTFS`, `RUFUS_NTFSINFO` and
`RUFUS_NTFSLABEL`. An available `ntfsck` is used read-only; it is optional and
can be selected with `RUFUS_NTFSCK`. These run on disposable regular files.
`mkntfs` is the native NTFS adapter's formatter; the other tools independently
check the result. Prefer ntfsprogs-plus as their provider. The optional
`RUFUS_NTFS_3G` CMake path selects the NTFS-3G package's `lowntfs-3g` executable
(normally `/usr/bin/lowntfs-3g`), whose case-insensitive mode matches Windows
lookup. The helper checks its availability before disk writes when the new
kernel driver is unavailable. See [the provider decision](../docs/linux-windows-installer.md).

The signature test uses the `openssl` command line as an independent reference;
its small signed GRUB fixture is checked in and the test runs offline.

The ext2/ext3/ext4 formatter requires `mke2fs` and libext2fs from e2fsprogs; its file test also
uses `e2fsck` and `dumpe2fs`. It uses the native superblock header at build
time and the existing child supervisor for formatting. The single privileged
helper now handles source-free GRUB2/ext2 and GRUB2/ext3 jobs. It temporarily
releases its whole-disk claim for `mke2fs` and verifies the disk again before
writing the original GRUB2 core; no second authorization occurs. The standalone Grub 2.14
choice offers ext2/ext3 when advanced format options are expanded and the
target is at least 256 MiB. Its cluster choice is **Default**. Standalone ext4
remains unavailable; CP42 adds ext2/ext3/ext4 persistence volumes to compatible
ISO creation (see the support guide). Source-backed GRUB GUI creation was added
after the original formatter checkpoint; CP21v adds the FAT/NTFS
helper extraction path, with compatible-image boot qualification still pending.

The exFAT adapter additionally requires runtime libblkid 2.39+ for
boot-checksum validation and exfatprogs supporting `-C` (metadata readback), `-K`
(no discard) and `-P none` (no partition table). CP16a qualifies exfatprogs 1.4.3
with libblkid 2.42.4. Earlier libblkid versions remain build-compatible but the
adapter returns unsupported before writing; unsupported formatter flags fail
without retry. The optional `rufus_exfat_format` test uses `mkfs.exfat`,
`fsck.exfat -n` and `dump.exfat`; CMake overrides are `RUFUS_MKFS_EXFAT`,
`RUFUS_FSCK_EXFAT` and `RUFUS_DUMP_EXFAT`. It skips when providers lack the
required capabilities. That CTest uses disposable partition-sized regular files;
CP16e exposes non-bootable GPT/exFAT in Qt. CP16b shares Rufus's
original exFAT cluster/default calculation with Win32. Linux preflight filters
choices by volume geometry, preserving the whole-disk default where it fits.
Formatting passes explicit 1-MiB alignment and checks space for native metadata
before writes; range/cluster-count checks do not allocate a volume or run a tool.
CP16c also qualifies GPT partition formatting and the existing formatter claim
handoff in offline QEMU guests. It checks expected device/capacity/read-only state
and sysfs partition start, then verifies the native boot offset. Only 512-byte
sectors and starts aligned to 1 MiB are supported. exFAT mount/copy paths remain
disabled. Reproduce with `linux/tests/vm/mbr.py --exfat` and a guest-enabled build,
as documented in the [checkpoint record](../docs/testing-and-checkpoints.md#cp16c-exfat-block-formatting-and-claim-handoff-2026-09-24).
CP16f extends complete non-bootable GPT jobs to FAT16 and FAT32 through
`format-fat16-device-v1` and `format-fat32-device-v1`. These source-free operations
reuse the native formatter/claim handoff and require final whole-disk sync and
helper exit. Sources and boot/customization settings are rejected. The prior
preparation and ISO/Syslinux operations retain their existing contract. CP16g
exposes these FAT choices in Qt through the existing non-bootable workflow;
use `mbr.py --fat-helper`
with `--fat-bits 16` or `--fat-bits 32` for the isolated backend qualification.

CP16h adds `format-ntfs-device-v1` for non-bootable GPT/NTFS, using the configured
native formatter and existing partition metadata checks. It rejects sources,
boot/customization options and caller-specified serials; no UEFI:NTFS partition,
payload or mount is involved. The whole-disk claim stays held through formatting
and final sync. The bootable `prepare-ntfs-device-v1` operation keeps its prior
contract. CP16i exposes NTFS in Qt's Non bootable selection alongside FAT16,
FAT32 and exFAT. It uses payload-free cluster geometry, NTFS label normalization,
formatter checks before confirmation/authorization and the original non-bootable
progress weighting. Full READY requires whole-volume completion and helper exit.
Reproduce backend qualification with `mbr.py --ntfs-helper` using the VM guide;
Qt selection/lifecycle coverage is in the format-options, window and write-Widgets
tests. Non-bootable MBR/SFD remain later checkpoints.

CP16d adds a complete **non-bootable GPT/exFAT** helper operation through the same
single authorization. It checks formatter help/capabilities (three-second deadline,
16-KiB help limit) and libblkid before any partition writes. `mkfs.exfat` is an
optional CMake-discovered provider; a missing provider leaves existing FAT/NTFS
builds usable and causes an exFAT job to fail unchanged. The client cannot supply
a tool path, ISO source, boot options or serial number. Formatting is followed by
a final whole-disk sync, target recheck and joined helper exit. No mount, copied
files or bootloader is involved. Use `mbr.py --exfat-helper` for the complete
client/helper qualification. In Qt, choose **Non bootable**, then the target,
label and cluster size. GPT/exFAT is currently the only qualified non-bootable
route. The selected ISO/DD image is retained for switching back; it is not part
of the format request. START checks providers/settings/device before the
destructive confirmation and repeats checks before the single authorization.


Set
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

To check another Qt installation, configure a **new** build directory with
`-DCMAKE_PREFIX_PATH=/path/to/Qt/6.2.4/gcc_64` (or its distribution equivalent).
CMake prints the selected Qt version. Keep its headers, libraries, build tools
and plugins together; do not reuse a build directory configured with another Qt.
For an unpacked SDK, its plugins may also need
`QT_PLUGIN_PATH=/path/to/Qt/6.2.4/gcc_64/plugins` in the test environment.
These are per-build settings, not changes to the desktop's Qt installation.
CP13a verified the complete build and hardware-free suite with Qt **6.2.4**,
and the Widgets/preflight checks with the host's **6.11.2**. See the
[qualification record](../docs/testing-and-checkpoints.md#cp13a-minimum-qt-qualification-2026-09-23)
for the test-setup correction and platform limits; the Ubuntu CI jobs remain
separate from this local SDK check.

To also build/package the qualified native Syslinux installer, set
`-DRUFUS_SYSLINUX_UPSTREAM=/path/to/syslinux-6.04-pre1` at configuration time.
Use the [verified release and package instructions](syslinux/README.md).
The optional package test requires Python 3.11+, Perl, NASM and QEMU in addition
to the FAT/image test tools. It stages only `SyslinuxRuntime` in a temporary
directory and boots generated disks; it does not install on the host.
CP20k includes `rufus-extlinux` in the same package, using eight unchanged
upstream source files and the same pinned v6 payloads. The NTFS adapter and
Windows file attributes are qualified with the staged provider on kernel NTFS
and NTFS-3G simulated USBs. CP20l connects standalone v6/MBR/NTFS to the existing
single-authorized helper, with final unmount, disk sync and helper-exit checks.
CP20m exposes NTFS for **Syslinux 6.04** under advanced drive properties,
retaining MBR/BIOS and the shared Windows defaults and cluster choices.
**Syslinux 4.07** remains FAT-only. The GUI checks the selected installer and
formatter before authorization; NTFS does not require mtools or the FAT
installer. Source-backed NTFS Syslinux compatibility remains separate work.
The packaged installer is required by the MBR/FAT32/Syslinux GUI route introduced
in CP12m. The existing CI configuration does not yet select this dependency.

CMake also installs the `rufus-linux` GUI into `CMAKE_INSTALL_BINDIR`, alongside
the existing helper, policy and optional Syslinux runtime installation. Configure
the distribution prefix (normally `/usr`) before building so the GUI, helper and
polkit policy agree on their absolute paths. Use `DESTDIR` to stage a package;
changing `cmake --install --prefix` afterwards does not update compiled paths.
CP13 adds the **Rufus (Linux preview)** menu entry and unchanged upstream icons,
with a matching Qt desktop identity. See the
[desktop packaging guide](packaging/README.md) for staged install commands,
runtime tools/services, configured paths and packaging limits.
The [installed-runtime guest recipe](tests/vm/README.md#installed-runtime-and-widgets)
exercises this layout without installing anything on the host.

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

`UDisksTarget` performs read-only preflight and a fresh snapshot after
confirmation. All GUI DD jobs then use `PrepareImageProcess` and the existing
single authorized helper. The helper revalidates the source and device, unmounts
eligible targets, acquires exclusive native access, and calls
`WriteRawImageToDevice` for copy, flush and full readback. The desktop's polkit
agent owns the password prompt. The GUI never receives a writable descriptor.

Tests use a private D-Bus, disposable files and test-only simulated kernel facts.
The optional [USB guest fixture](tests/vm/README.md) covers native block devices
and separate boot checks. `rufus_qt_write` retains confirmation, cancellation,
percentage reporting and selection invalidation. `RawWriteJob` and the direct
UDisks `OpenDevice` route were removed by the
[production consolidation](../docs/linux-consolidation.md). Only complete
verification, successful sync and helper exit produce 100%/READY. Cancellation
still flushes written bytes; logs retain partial counts and flush failures.

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

## ISO creation from the Widgets form

`IsoWriteController` connects SELECT's captured source, START, the existing
ISOHybrid choice dialog, read-only preflight, destruction confirmation and one
authorized preparation helper. ISO remains recommended and selected by default.
Explicit DD uses the existing raw copy/flush/verify writer with that exact source,
even when the ISO formatting settings are invalid. ISO → ESP is available for eligible small regular-EFI images on GPT/FAT16 or
GPT/FAT32, including compatible persistence. Non-hybrid images receive the same
ESP question; ISO remains recommended. See CP52 and the support guide.

The UEFI ISO path uses GPT, FAT16, FAT32 or NTFS, and a regular x64 UEFI fallback
in a complete supported image tree. The compatible Syslinux path additionally
offers MBR with FAT16 or FAT32 and BIOS, with copied EFI files supporting UEFI when present; its native
installer/source checks run before authorization. NTFS adds the bundled
UEFI:NTFS boot partition to GPT media. Filesystem
choices and the preferred selection use shared Rufus policy, filtered by native
formatting capabilities. The volume label and cluster size are editable. Shared
Rufus code normalizes FAT labels before preflight: uppercase, forbidden-character
removal, Unicode/dot/tab replacement, an 11-character limit and the original
size-based fallback for mostly underscores. NTFS preserves case and Unicode
with Rufus's 32-UTF-16-unit limit. Confirmation shows the effective
label; that same value goes to formatting and boot-config updates. Edge spaces
and other control characters remain unsupported by the native formatter.

Cluster choices use Rufus's size-dependent allowed/default policy. Linux filters
out sizes that cannot format the planned partition or exceed the native
formatter's supported range. It retains the shared default when supported, otherwise
selects the smallest remaining allowed size. Geometry-only calculation runs
without opening the device or requesting privileges.
Preflight rejects unsupported image trees and invalid cluster geometry before
authorization. Windows ISO preflight rereads install metadata from the retained
source using the existing upstream-wimlib reader extension. It accepts known
Windows 10/11 UEFI installers; older, unknown or unreadable metadata remain
unsupported. NTFS permits files larger than FAT32's per-file limit. Real Windows
Setup installation through the NTFS route was qualified in CP11k; FAT32/SWM
media reached Setup and exposed all 11 editions in CP11ah, but a complete
installation from those parts remains untested. Secure Boot analysis stays pending.
Windows BIOS installation, persistence, Windows To Go, non-bootable modes other than GPT/exFAT and
extended label/icon creation remain unavailable.

The application uses the helper path fixed at CMake configuration:
`${CMAKE_INSTALL_FULL_LIBEXECDIR}/rufus-prepare-helper`. ISO START requires that
executable and a connected system bus. Package the matching generated polkit
policy as described below; running the GUI from a build directory alone does not
install either. The GUI remains unprivileged, and cancellation keeps the same
helper until cleanup finishes. Progress shows phases; READY/100% requires successful
helper exit, cleanup, the expected source-byte count and the confirmed main
partition extent. This is creation completion,
not an independent readback or a claim that arbitrary ISOs will boot.

Filesystem-content notifications preserve device selection during formatting.
Live identity changes and removal still invalidate the job. The optional
[Widgets USB guest](tests/vm/README.md#widgets-start-through-uefi-boot) exercises
the actual form with live discovery, uncached authorization, independent readback
and a separate USB-only boot.

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
later replaced. The extractor now applies the shared root `syslinux.cfg` rules:
keep an existing file unless the scanner selected an override, back up an override
as `syslinux.org`, and generate Rufus's CONFIG/APPEND redirect. Generated bytes are
reported separately from copied source bytes. A backup failure stops the Linux
operation and preserves both existing files; Windows currently ignores that error.
Remaining filesystem restrictions, timestamps, other config workarounds,
payload replacement and bootloader installation remain separate work.

An explicit `split_install_wim` option replaces the conventional
`sources/install.wim` with `install.swm`, `install2.swm`, etc. The source byte/file
counts remain logical processed-input totals; `split_bytes` and `split_parts`
report the actual SWM outputs. Preflight requires one conventional archive,
reserves the numbered SWM names against source collisions, checks native WIM
resources and enforces FAT32 limits on other files. It does not predict WIM output
overhead exactly. `required_bytes` adds the conservative split reservation to
ordinary source bytes; the split path checks actual mounted free space before
creating output. The adapter checks each actual part and syncs it before publication;
failed cleanup or final directory sync cannot report success. Other completed
source entries remain on failure. The destination and mount must remain private
through cleanup. Fresh helper preflight can now select this option for enhanced
installation. CP11ag connects the GUI's Alt+E preference; it defaults to disabled.

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
label and sector geometry are explicit. CP76 supports 512 through 4096-byte
logical sectors and clusters through 65536 bytes, with sufficient space for a standards-sized FAT32.
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

## Native GPT preparation

`rufus_partition_image` supplies `CreateGptImagePartition` for an existing,
exclusively borrowed regular file or whole block device. A block request supplies
the accepted plan's device number; zero requires a regular file. Native ioctls
check capacity, frozen logical/physical sector geometry and read-only state. Caller-owned
exclusive access is required; the preparation helper opens the device with `O_EXCL`.
Rufus's `GetPartitionStartOffset` and
`GetMainPartitionSize` now live in shared `src/partition_layout.c`; the existing
Windows `CreatePartition` calls them too. Linux delegates GPT serialization,
CRCs and identifiers to libfdisk. The initial layout keeps Rufus's 1 MiB start,
track-then-cluster rounding, backup-GPT reservation, Microsoft Basic Data type
and `Main Data Partition` name. It does not automatically choose an ESP type.

CP11a adds an optional `uefi_ntfs_size` (payload bytes) for regular-file requests.
Shared trailing-partition calculations reserve and align a second `UEFI:NTFS`
partition. It is Microsoft Basic Data with the no-drive-letter attribute,
matching the Windows layout. The returned `uefi_ntfs_offset` and `uefi_ntfs_size`
describe its allocated extent. This writes only GPT metadata; NTFS formatting
and copying the bundled boot image remain separate. CP11d accepts retained
block-device requests and adds `prepare-ntfs-device-v1` to the same authorized
helper, with checks of both kernel extents and native formatting of the main
partition. CP11e adds the embedded payload write and verification in that job,
using the existing bounded copy loop. The GUI continues to use the existing
single-partition FAT32 path.
See the [Windows installer implementation notes](../docs/linux-windows-installer.md).

The adapter builds the table in memory, rechecks the expected image size before
writing, flushes even after an attempted write fails or is cancelled, and reads
the table back through a fresh library context. Recovery from a damaged GPT copy
is treated as failed write verification. Descriptor ownership and seek position
remain with the caller. Authorization, kernel partition refresh and mount are
the caller's responsibility; the adapter never opens the device by pathname.

Functional tests independently decode primary/backup headers, entries and CRCs,
inspect the protective MBR and use `sfdisk` for an external table check. They
format the actual reported partition extent, verify both GPT copies are
unchanged, then inspect FAT32 with fsck and mtools. Cancellation, failed writes,
failed flushes, changed size, damaged table copies and descriptor cleanup are
covered. Tests require `sfdisk` from util-linux (`fdisk` package on Debian/Ubuntu).
The optional [GPT USB guest](tests/vm/README.md#retained-gpt-block-descriptors)
checks native device ioctls, writes and retained exclusive ownership. It also
confirms that dosfstools 4.2 cannot reopen that exclusive descriptor: stock
mkfs.fat uses another `O_EXCL` open and receives EBUSY. CP10m's production FAT32
adapter now also accepts a validated native partition descriptor after the helper
releases its whole-disk claim.
CP10j's guest now demonstrates a direct CLI handoff after releasing our claim;
CP10k's [authorization guest](tests/vm/README.md#one-authorization-for-the-helper)
runs those operations through one real KDE/polkit request from an unprivileged
Qt caller. Approval caching is disabled, and dismissal/missing-agent requests
stop before helper entry. CP10l adds the process connection for ordinary-file
jobs below; CP10m integrates native device preparation and CP10v connects ISO START.

```sh
ctest --test-dir /tmp/rufus-linux-qt6-build -R 'rufus_partition_layout|rufus_partition_image|rufus_fat_format' --output-on-failure
```

See [CP10e](../docs/testing-and-checkpoints.md#cp10e-shared-placement-and-native-gpt-images-2026-09-15).

## GPT/FAT32 preparation helper

`rufus-prepare-helper` and `PrepareImageProcess` execute one concrete preparation
job across a process boundary. The ordinary-file operation accepts an existing regular
image file owned by the ordinary caller. The request includes its absolute path,
expected filesystem device/inode/size, cluster size, volume ID and label. The
helper opens without following the final symlink, checks the retained file and
pathname, and holds an advisory file lock until completion. This operation rejects
elevated execution; native devices use a distinct request and launch mode.

`prepare-device-v1` additionally carries the accepted UDisks object, service owner,
identity and kernel disk sequence. `PrepareImageProcess` launches the fixed helper
with `pkexec --disable-internal-agent` and `--device` once. The helper uses the real
system bus to repeat eligibility, identity and device-use checks; it verifies its
exclusive descriptor's device number, disk sequence, size, sector size and writable
state. Mounted or layered targets are refused before writing.

After GPT creation, the helper refreshes the kernel partition table and checks
partition 1's actual extent and disk sequence. It retains a non-exclusive partition
descriptor, releases its whole-disk exclusive claim, runs the fixed CLI, then
reacquires the disk and verifies identity and both GPT copies. `FormatFat32Partition`
shares the file formatter's options, cancellation, flush and Rufus FAT-reader checks;
its descriptor starts at partition offset zero while the FAT hidden-sector field
retains the disk offset. A busy or changed disk fails without bypass or retry.

`PlanGptImagePartition` and `CheckFat32ImageRequest` expose the existing adapters'
preflight checks without writing. Both steps must pass before GPT creation starts.
The worker then calls the existing GPT and formatter implementations, retaining
their geometry, child cancellation/reaping, flush and readback checks. The mkfs
path is selected at CMake configuration; no request can replace it.

The Qt controller waits for the helper's ready message, sends one JSON line and
receives checking/partitioning/formatting phases, bounded logs and a result. Frames
are at most 4096 bytes; 64-bit fields use decimal strings. **Stdin stays open until
completion: closing it cancels the job.** This is not a shell filter for `cat` or
`echo` pipelines. Caller disconnect uses the same interruption path, and the
helper stops/reaps its child and flushes before exiting. The controller reports
completion only after process exit. A missing or malformed result reports failure
and possible partial changes instead of success. It never retries a launch.

Device requests can now include a selected ISO source and Rock Ridge/Joliet choices.
`CapturePrepareSource` records its identity and modification/change times; the
helper retains and revalidates the same inode throughout the job. The initial
native extraction path accepts caller-owned, owner-readable regular files with
no final-component symlink. It refuses a source on the target. `CheckIsoTree`
shares the extractor's preflight traversal without creating output; unsupported
entries, incomplete trees and files above FAT32's size limit fail before GPT writes.
An obvious total-size overflow is also refused, though filesystem allocation and
naming can still fail during copying.

The native helper creates a private mount namespace before starting Qt threads,
then mounts the freshly formatted partition as FAT with `nodev,nosuid,noexec`,
UTF-8 names and a root-only temporary mountpoint. The existing extractor retains
Rufus's filename substitutions, collision behavior and root `ldlinux.sys` exclusion.
Additional phases are mounting, extracting and finalizing. Cleanup calls `syncfs`,
closes file descriptors, unmounts, removes only the empty mountpoint, reacquires
exclusive disk access and checks both GPT copies. It does not recursively remove
the mountpoint or lazily detach a failed mount. A failed unmount is reported and
can leave an empty `/run/rufus-*` directory after the helper's namespace exits;
the job is failed, including when cancellation also occurred.

```sh
cmake --build /tmp/rufus-linux-qt6-build
ctest --test-dir /tmp/rufus-linux-qt6-build -R 'rufus_prepare_(process|helper)' --output-on-failure
```

The tests use ordinary disposable files and independent GPT/fsck/mtools inspection.
A separately linked probe supplies formatter/flush faults; the production helper
has no fault controls. The optional [native preparation guest](tests/vm/README.md#production-device-preparation)
qualifies real authentication, active cancellation and caller death. CMake generates
the `org.rufus.PrepareDevice` policy and installs it alongside the helper using
GNUInstallDirs. The policy names the configured absolute helper path and `--device`,
uses `auth_admin` without caching, and must be packaged into polkit's system actions
directory (configure the distribution prefix, normally `/usr`). No policy or helper
is installed on the development host by these tests. `IsoWriteController` connects
this component to application START for the initial supported ISO workflow.
See [CP10l](../docs/testing-and-checkpoints.md#cp10l-one-request-preparation-process-2026-09-15)
and [CP10m](../docs/testing-and-checkpoints.md#cp10m-authorized-native-gptfat32-preparation-2026-09-15).
The [extraction guest](tests/vm/README.md#authorized-device-extraction) covers the
complete preparation/mount/copy/cleanup operation.
The [UEFI ISO guest](tests/vm/README.md#uefi-boot-after-authorized-extraction)
boots a generated self-contained EFI tree from the helper's GPT/FAT32 USB output,
with one real authorization and independent payload checks. This qualifies the
mechanism on x86-64 OVMF; it does not establish Arch/CachyOS or other distribution
ISO compatibility. See [CP10o](../docs/testing-and-checkpoints.md#cp10o-uefi-boot-after-authorized-iso-extraction-2026-09-15).
The [real Alpine ISO guest](tests/vm/README.md#real-alpine-iso-extraction-and-uefi-boot)
also boots the unchanged Alpine standard 3.22.5 EFI payload after the full helper
job. It verifies the release, mounted USB, boot-file hashes and squashfs modloop.
The readback also checks the additional root `syslinux.cfg` against Rufus's shared
generation rule; this does not install a BIOS bootloader.
Alpine's EFI loader emits an original-label search warning before successfully
booting on OVMF. This result does not cover other distributions, BIOS or installation.

## Shared configuration text

`src/config_text.c` contains Rufus's token replacement matcher, extracted from
the Windows parser. It also contains the original character expansion helper and
the ordered ISO label/Red Hat patch list and FreeNAS volume-reference formatting
used by both platforms. It matches a token at the start after spaces/tabs, requires
a separator after it, and replaces at most four case-sensitive source substrings
per input chunk. Windows continues to use its existing file/encoding operations.

`platform/config_patch.cpp` supplies `ReplaceConfigTokenAt` for Linux. It edits one
file in a retained private directory, using Qt Core for UTF-8/UTF-16LE conversion
and the common matcher. It preserves Rufus's 1023 UTF-16-unit chunk boundaries,
including surrogate pairs that cross a boundary. Unmatched files retain their
original bytes and inode. Changed files preserve BOM/encoding, use LF, and are
written/flushed/closed before a checked atomic rename. The original survives
errors before rename; `modified` remains true if the final directory flush fails.
Owner/group and ordinary permission bits are retained. No GUI/application object
or privilege request is needed.

Malformed text, NUL/EOF markers and unsupported BOMs fail explicitly. Whole-file
input/output are limited to 8 MiB to bound memory. The UTF-16 validation uses
Qt's existing API; no Unicode codec is implemented here. The adapter is tested
independently and on the native guest FAT driver. The extractor now applies the
shared label and Red Hat passes to files selected by Rufus's classifier, followed
by FreeNAS's `set` reference change in GRUB configs. FreeNAS uses literal label
spaces and still changes the volume path when the source and USB labels are equal. It uses the
formatted USB label and original full image filename (including the case-sensitive
`netinst` exception). Creation jobs supply both even when reading a retained FD.

`CheckConfigText` runs the same ordered transformations without writing, including
encoding, expansion and resulting-size checks. The helper calls this through tree
preflight before GPT creation. `ReplaceConfigTokensAt` preserves the successive
chunk boundaries and encoding conversions, then commits once per changed file.
A failed later pass preserves the original, rather than leaving a partially patched
config. Source copy counts exclude rewrite bytes; `patched_files` counts changes.
The extractor also uses the shared EFI Syslinux rule to copy a patched
`/EFI/BOOT/isolinux.cfg` when no EFI `syslinux.cfg` was found. It keeps an existing
regular destination and rejects other copy errors, cleaning an incomplete new
copy while preserving the source. The copy uses the retained private directory
and the existing transfer buffer. These generated bytes, like the root Syslinux
config, remain separate from source copy totals. Persistence remains unavailable;
no native Syslinux installer is selected by these config fixes.

```sh
ctest --test-dir /tmp/rufus-linux-qt6-build -R '^rufus_config_(text|patch)$' --output-on-failure
```

See [CP10r](../docs/testing-and-checkpoints.md#cp10r-shared-token-matching-and-linux-config-files-2026-09-15),
[CP10s](../docs/testing-and-checkpoints.md#cp10s-iso-label-and-red-hat-config-integration-2026-09-15)
and [CP10t](../docs/testing-and-checkpoints.md#cp10t-freenas-and-efi-syslinux-config-fixes-2026-09-15).

## Read-only ISO preparation preflight

`rufus_iso_write_check` supplies `CheckIsoWriteImage` for GPT/FAT32 or NTFS UEFI
and the compatible MBR/FAT32/Syslinux path. The caller first captures the image
with `CapturePrepareSource` and supplies the proposed target capacity, cluster
size and normalized volume label.
The check retains that exact caller-owned image, uses geometry-only
partition planning and the selected filesystem's option validator, applies shared
Rufus image policy, and checks either the regular x64 EFI fallback or the native
Syslinux source/config/module compatibility. Extraction/config preflight enforces
FAT32's per-file limit and any selected WIM split; NTFS supports larger files.
It does not open the target or request authorization.

The EFI check reuses Rufus's PE architecture recognition and Qt's endian readers,
with at most 1 MiB of header storage. It requires an x64 PE32+ EFI application;
it is not a full executable, Secure Boot or payload dependency validator. The
result retains pending analysis facts and all shared mode choices, including
ISO → ESP. Successful source-byte capacity checking is not complete FAT allocation
accounting. The Widgets controller uses this check before confirmation and repeats
source/device validation before launching the helper. Syslinux selection uses
the qualified native pair; it does not turn unknown detection into a known version.

The test-only probe accepts an image path, proposed target bytes, cluster bytes
and label; it emits a diagnostic JSON result without creating media:

```sh
/tmp/rufus-linux-qt6-build/iso_write_check_probe image.iso 536870912 512 'RUFUS TEST'
ctest --test-dir /tmp/rufus-linux-qt6-build -R '^rufus_iso_write_check$' --output-on-failure
```

See [CP10u](../docs/testing-and-checkpoints.md#cp10u-read-only-iso-preparation-preflight-2026-09-15).

## Shared ISO tree scan

`rufus_iso_scan` builds `src/iso_scan.c`, `src/iso_read.c`, `src/image_file.c`
and `src/strarray.c`, plus shared Syslinux version/selection, GRUB inspection and
EFI banner reporting code, plus `src/iso_analyze.c`, `src/iso_bootloaders.c` and
`src/embedded_efi.c`, `src/iso_file.c` and `src/winpe_scan.c`. The main Linux suite has
51 tests, or 52 with the optional system-wimlib comparator.
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
Analysis alone does not authorize writing. The controller enables the implemented
creation routes separately and START performs fresh preflight. Device discovery
is independent of read-only scanning.

Qt resources contain Rufus's existing v4/v6 Syslinux payloads as data. The shared
version parser obtains their fallback versions; no version numbers are hardcoded
or taken from the host's installer. An unknown image version stays unknown when
a fallback is inferred, and the log states that native compatibility is unchecked.
The job checks the selected file's type, size and modification time before and
after analysis. This is not a pinned image snapshot or write-time validation;
changes after completion require a rescan. The controller also handles the narrow
raw-image route and passes shared ISOHybrid policy choices to the creation flow.

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
Embedded inspection alone does not establish an implemented creation route.
The current GPT ISO path still requires a regular fallback EFI file; Secure Boot
analysis remains pending.

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
uses the existing status/log/label controls. Legacy WinPE-only ISO creation remains
outside the known Windows 10/11 installation route.

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
extraction using the same provider. Later Windows boot/installation evidence is
listed in the [support table](../docs/linux-support.md); Secure Boot enforcement
remains unqualified. See the [extraction boundary](../docs/linux-windows-image-metadata.md#cp3g4a-bounded-single-file-extraction-and-shared-pe-recognition).

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
of the shared scan selection below; the separate native installer/source checks
now gate the MBR/FAT32 creation route.

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
by name in Boot selection, with its full path and size in the tooltip. SELECT
also starts analysis or validates the supported raw-image input. Invalid
selections clear the previous image and show an explanation in the status bar.

The application starts discovery asynchronously and displays eligible devices
with name, path and capacity, ordered from smallest to largest as in Win32.
The first eligible list selects its smallest device, including when discovery
initially finds no eligible media. Selection alone never authorizes or writes.
`SelectionController` preserves the selected identity across reorderings and name
changes, and clears it after removal, replacement, changed size/path, ambiguous
identity or discovery failure. After that invalidation, the user must select a
target again, even if discovery recovers. Unlike Win32's fallback to the first
drive, this does not silently rebind a lost UDisks identity or reset a completed
READY state. Synthetic records live only in tests.

Filesystem labels use “FAT32” at every supported size: Linux uses the same native
formatter throughout and needs no “Large FAT32” distinction. “(Default)” marks
the first format supported by the native target/layout before image filtering,
independently of the selected format. Linux considers its implemented FAT16/FAT32/NTFS
routes, including FAT16 on eligible small GPT/UEFI and MBR/Syslinux media.
Rufus's shared image policy still prefers FAT32 when available for those images.
An image can exclude the target's default, leaving no marked
entry. Cluster labels retain the original units and default-size marker.
With a target selected but no image, filesystem/cluster defaults are visible
and disabled, as on Windows. Rejected scans restore those device choices while
discarding the failed image and retaining a valid manual filesystem preference.
START remains disabled until a usable image and target pass the existing gates.

Both advanced sections expand/collapse, while unsupported operations remain
disabled. Image-specific options stay hidden until real analysis can determine
their applicability. The toolbar opens a reusable log window with Clear, Save
and Close; closing the main window also closes its dialogs. DD and ISO creation
use the connected controllers described above.

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
flash devices may be omitted. CP17d3 enables **List USB Hard Drives** and **Alt+F**
to include eligible fixed USB storage explicitly. It defaults off each session;
all other exclusions remain in force. The policy is frozen during analysis and
creation, including confirmation. Disabling it clears a selected fixed target
without choosing a replacement. Opted-in jobs use the existing helper for native
system/source/home/holder protection, including DD on unmounted disks, with one
authorization. See the [support limits](../docs/linux-support.md) and the UDisks
[Block](https://storaged.org/doc/udisks2-api/latest/gdbus-org.freedesktop.UDisks2.Block.html)
and [Drive](https://storaged.org/doc/udisks2-api/latest/gdbus-org.freedesktop.UDisks2.Drive.html)
contracts. Mounted-device/system-use checks remain required before any write.

Selection identities belong to this client session. They combine observed
object lifetime with path, size, device/drive identifiers and detection times.
Removal invalidates the identity immediately, even if identical metadata
reappears at the same path before the next snapshot. Relevant property changes
also invalidate selection while refreshing. These IDs are not persistent device
identifiers and do not replace authoritative revalidation before writing.

Discovery opens no device nodes and requests no authorization. The write paths
request UDisks2/polkit access only after confirmation. The opt-in host smoke probe
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
discrete steps, and the `OP_INIT` info payload is not a byte count. Controllers
must not treat every percentage as whole-job completion or every info payload
as bytes. The ISO creation controller now uses shared `progress_policy.c` for
Win32's weighting and one-decimal copy percentage. The reporting adapter only
transports/coalesces these reports; it does not calculate weighting, ETA or
localization.

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
service calls beyond read-only discovery. Later simulated-USB, authorization,
media-write and boot checks are recorded in the [support table](../docs/linux-support.md).
Physical USB qualification remains separate.

[Linux CI](../.github/workflows/linux-qt6.yml) builds and runs `no-hardware`
tests on Ubuntu 22.04 and 24.04. The workflow is prepared locally; it has not
yet run on GitHub.


CP16j generalizes the Linux MBR table API to `PlanMbrImageLayout`,
`CreateMbrImage`, `CheckMbrImage` and `PrepareMbrTarget`, with `MBR_IMAGE_OPTIONS`
and an explicit `syslinux` choice. Existing Syslinux callers keep active FAT
partitions and the bundled boot code. Non-bootable FAT16/FAT32/NTFS/exFAT use the
original Win32 zero-MBR payload (446 zero bytes, including disk signature), an
inactive partition and the corresponding Rufus partition type. Clearing, flush,
cancellation, descriptor checks and readback remain shared. The formatter must
separately validate filesystem geometry; this adds table support only.

Run `ctest --test-dir <build> -R 'rufus_nonboot_.*_mbr_' --output-on-failure`
for the four-filesystem regular-file table/clearing matrix. Existing Syslinux
regressions are `rufus_syslinux_mbr_*` and `rufus_fat16_mbr_*`. Disposable images
are removed by each test, including failures. The following checkpoints add
non-bootable MBR device jobs; Qt exposure remains a separate step.


CP16k adds `format-fat16-mbr-device-v1` and `format-fat32-mbr-device-v1` to the
same authorized helper. Set `nonBootable` and `mbr` in its request; this plain
MBR selector is separate from existing Syslinux operations. These source-free
device jobs reuse MBR clearing/creation, native FAT formatting/claim handoff,
final whole-disk sync and target checks. No installer, mount, copied image or
second authorization is needed. Qt non-bootable creation still offers GPT only;
CP16l/m add NTFS and exFAT below.
Use `linux/tests/vm/mbr.py --mbr-fat-helper --fat-bits 16` or `--fat-bits 32`
with the VM guide's prerequisites for bounded native helper qualification.


CP16l adds `format-ntfs-mbr-device-v1` using the same `nonBootable`/`mbr`
request selection. It shares native NTFS formatting with GPT jobs, retaining
the exclusive disk claim throughout mkntfs and readback. The plain MBR contains
one inactive NTFS partition and zeroed boot code/signature. No boot payload,
image source or mount is used. Sources, customizations and caller-selected
volume serials are rejected. The existing GPT/UEFI:NTFS path is unchanged.
Use `linux/tests/vm/mbr.py --mbr-ntfs-helper` with the VM guide's prerequisites
for bounded native helper qualification and independent metadata checks.


CP16m adds `format-exfat-mbr-device-v1`, completing the four non-bootable MBR
filesystem backends. This explicit `nonBootable`/`mbr` request uses the existing
exfatprogs formatter, retained partition descriptor and whole-disk claim
handoff/reacquisition. The helper verifies table and volume metadata before
final whole-disk sync. An unsuccessful claim reacquisition or changed MBR fails
the job even if mkfs succeeded. No mount or extra privilege request is needed.
Use `linux/tests/vm/mbr.py --mbr-exfat-helper` with the VM guide's prerequisites.
Non-bootable MBR Qt selection and defaults remain the next checkpoint.


CP16n exposes the complete non-bootable MBR filesystem set in Qt. MBR is the
default regardless of the existing disk table, and explicit MBR/GPT choices
survive device and boot-mode changes when supported. Above 2 TiB the remembered
choice switches to GPT; MBR is omitted because the native 512-byte-sector layout
cannot represent the full disk. Both layouts display BIOS or UEFI. Filesystem
cluster filtering and preflight now use the chosen table, and confirmation names
it. The existing controller sends the qualified source-free helper operation
and preserves one authorization and READY only after final sync and helper exit.
SFD remains a later backend checkpoint.


**Historical CP17a behavior, superseded by the 2026-09-27 consolidation below:**
CP17a separated ISO/DD mode availability from preparation-helper availability.
An eligible raw or ISOHybrid DD write uses the existing UDisks path and works
without the ISO helper. Hybrid choices and the recommended ISO default remain
unchanged; choosing ISO without its helper reports an actionable error before
any writes. ISO-only and non-bootable jobs remain unavailable until their helper
is installed. Source identity, target checks, authorization, sync and independent
readback verification on the DD path are unchanged. The initial SFD deferral
favored improvements to the main creation workflow; the full-parity decision
below supersedes it.


The 2026-09-25 scope decision targets the complete Windows Rufus feature set,
including SFD and advanced/legacy features, without a fixed implementation order.
See the [feature parity backlog](../docs/linux-feature-parity.md). Features still
missing from the GUI remain implementation work, not permanent exclusions.


CP16o starts SFD support with whole-device layout/preparation, reusing the MBR
metadata-clearing, sync and readback implementation. Generated file tests replace
stale GPT metadata and format FAT32 at byte zero with native mkfs.fat, fsck.fat
and mtools validation. The GUI and device-helper route remain unchanged until
SFD block-device handoff and kernel partition refresh are qualified (CP16p).


CP16p qualifies the whole-device FAT32 handoff through PrepareDeviceTarget::Table::Sfd.
After clearing old metadata, the backend removes stale kernel partitions, lends
a retained non-exclusive whole-disk descriptor to native mkfs.fat, reacquires
exclusive access and verifies FAT32 geometry/identity and the absence of nested
MBR entries. The existing MBR/GPT table snapshots remain unchanged. SFD is gated
to non-bootable FAT32, with mounting and other filesystems unavailable in this
step. Normal/sanitizer VM tests use generated 64-MiB USBs; the complete helper
job and GUI integration follow as CP16q and a subsequent Qt checkpoint.


CP16q connects SFD/FAT32 to the complete source-free helper operation
`format-fat32-sfd-device-v1`. Unprivileged preflight checks whole-device geometry
and formatter availability before authorization. The helper clears stale metadata,
formats with native mkfs.fat, reacquires its claim, verifies the volume and syncs
the device. The client requires the full device extent, finalization and helper
exit before success. Twelve native helper cases pass normally and under sanitizers
using a disposable USB VM and a guest-only authorization stand-in. No host polkit
prompt is exercised. SFD Qt selection follows in CP16r; FAT16/NTFS/exFAT SFD remain
separate checkpoints. See [the VM recipe](tests/vm/README.md#sfdfat32-complete-helper-job-cp16q).


CP16r exposes **Super Floppy Disk** under **Non bootable** for FAT32-capable
media. It follows Win32's option order, BIOS or UEFI target, MBR initial default
and remembered explicit choice. The filesystem list is FAT32-only for SFD until
other native routes are qualified; ordinary MBR/GPT retain FAT16/FAT32/NTFS/exFAT.
Cluster sizes are validated against the whole device, and confirmation identifies
the SFD layout. Progress and READY retain the existing final-sync/helper-exit
contract. CP16s will qualify FAT16 SFD before enabling that filesystem in Qt.


CP16s qualifies the FAT16 SFD backend through `format-fat16-sfd-device-v1`.
The existing formatter, source-free helper lifecycle and disk-claim handoff are
reused, with FAT16-specific validation before and after formatting. Normal and
sanitizer native/file/protocol tests cover failures, cancellation, final sync,
whole-volume readback and waiting for helper exit. Qt remains SFD/FAT32-only in
this checkpoint; FAT16 selection and geometry are CP16t. The reproduction steps
are in [the VM guide](tests/vm/README.md#sfdfat16-native-handoff-and-complete-helper-cp16s).


CP16t enables FAT16 alongside FAT32 under **Non bootable → Super Floppy Disk**.
Filesystem order/defaults follow Rufus, with valid manual choices remembered.
Cluster choices use the full device: small FAT16-only media can select SFD, while
FAT16 disappears near 4 GiB when that extent exceeds the native cluster limit.
Confirmation, progress, one authorization and final-sync/helper-exit completion
reuse the existing FAT workflow. NTFS SFD qualification is CP16u; exFAT SFD remains
planned. No new large fixtures or additional Windows customization tests are needed.


CP16u adds the NTFS SFD backend through `format-ntfs-sfd-device-v1`. It reuses
native mkntfs, SFD metadata clearing and the existing source-free helper. The
whole-disk claim stays exclusive throughout; mkntfs receives `-F` only for this
explicitly validated whole-device route (partition jobs still omit it). Readback
requires matching boot copies, correct full-volume metadata and empty partition
entries before final sync. No mount or bootloader payload is needed. Qt remains
FAT16/FAT32 for SFD until CP16v. See [the VM guide](tests/vm/README.md#sfdntfs-complete-helper-job-cp16u).


CP16v enables **NTFS** under **Non bootable → Super Floppy Disk**, using native
whole-device cluster filtering and Rufus's filesystem order/defaults. Manual
filesystem preferences survive fallback on smaller media. NTFS Unicode labels,
confirmation, cancellation, progress and final-sync/helper-exit READY use the
existing backend. Device refresh defaults to GPT above 2 TiB, as on Windows;
explicitly choosing SFD then works if native geometry permits. ExFAT SFD remains
unavailable pending CP16w native/helper qualification.


CP16w qualifies the exFAT SFD backend through `format-exfat-sfd-device-v1`.
It reuses the native formatter with `-C -K -P none`, explicit geometry and zero
partition offset. The existing handoff lets mkfs obtain exclusive access; the
helper reacquires the whole disk, verifies exFAT metadata and empty partition
entries, and performs final sync before completion. No force option or mount is
added. Qt remains FAT16/FAT32/NTFS for SFD until CP16x. Reproduction is in
[the VM guide](tests/vm/README.md#sfdexfat-complete-helper-job-cp16w).


CP16x enables **exFAT** under **Non bootable → Super Floppy Disk**. All four
qualified filesystems now share that choice, filtered by native whole-device
geometry. ExFAT uses Rufus's default cluster thresholds and label normalization;
manual filesystem choices survive temporary fallback on smaller targets.
Confirmation, progress, cancellation, one authorization and READY after final
sync/helper exit reuse the existing backend. Next is the planned CP17b work on
unmounting eligible target volumes inside the same authorized job.


CP17b2 adds an isolated native unmount backend and simulated-USB qualification;
automatic unmounting is **not yet enabled in creation**. It uses libmount for
kernel mount/swap parsing, checks device sequence, partition holders and known
system/source filesystems, and performs ordinary unmounts with cancellation
between volumes. It refuses busy, nested/aliased or unsupported layouts. See
[the native unmount fixture](tests/vm/README.md#native-target-unmounts) for the
exact tested scope and the remaining authorization/namespace integration.

## Production consolidation (2026-09-27)

All DD jobs now use the matching preparation helper, whether mounted or
unmounted. This supersedes earlier helper-independent DD checkpoints in this
history. It removes a second authorization/writer lifecycle while preserving
ISO/DD choices, raw percentages, source/device checks, cancellation flush and
helper-exit-before-READY. DD retains its native sector range; CP76 extends
formatting to 512/512e/4Kn while retaining explicit boot-provider limits.

Read-only selection checks no longer pull `PrepareDeviceTarget` into the GUI.
Layout planning is shared by cluster filtering, preflight and helper preparation;
actual partition creation stays in the helper. Native formatter and partition
adapters share writable-descriptor inspection, retaining their filesystem,
extent, geometry and readback rules. See the [consolidation record](../docs/linux-consolidation.md)
for scope, measurements and qualification.


## FreeDOS creation (CP19e)

Choose **FreeDOS** in Boot selection, select the USB and FAT16/FAT32 settings,
then press START. MBR and BIOS (or UEFI-CSM) are selected as on Windows. The
keyboard/code-page dialog starts at **US/437** and uses Rufus's existing lists;
choose explicitly, without Linux host detection. Cancel aborts before writing.
After confirmation, the existing single authorized helper creates the stick
using native FAT formatting, original Rufus boot records, bundled DOS files and
shared startup-file content. Copy progress and sync-before-READY reuse the
original policy and existing controller lifecycle.

CP19d2b/c qualified complete native helper creation/readback and representative
FAT16/FAT32 SeaBIOS boots; CP19e connects and tests the normal GUI workflow.
Other geometries and advanced BIOS fixes remain unqualified. See the
[support guide](../docs/linux-support.md#freedos-creation) for current limits and
[testing record](../docs/testing-and-checkpoints.md#cp19e--normal-qt-freedos-workflow-2026-09-29)
for exact evidence.


## CP21p — signed bootloader fetching

The unprivileged Qt Network component fetches one bounded payload and its
original URL's `.sig`, then uses the shared Rufus key and native verifier before
returning owned bytes. It uses the Qt event loop, without a blocking worker,
cache writes or privilege requests. Cancellation and failures publish no bytes;
payload progress reaching 100% still requires signature verification.
The [download audit](../docs/linux-download-signatures.md#cp21p--bounded-qt-network-fetcher)
records its Win32 contract, the deliberate refusal of HTTPS downgrades and the
63-case local-server qualification. This component is not yet connected to
image-backed GRUB creation; cache installation, warning/selection and frozen
helper transfer remain subsequent checkpoints.


## CP21q — atomic bootloader cache writes

The existing unprivileged GRUB cache component now installs verified bytes with
QSaveFile, sharing the reader's full-version/path/gap checks. Failed/canceled
writes preserve previous contents and leave no partial final core. The storage
suite has 45 passing Qt cases normally and under sanitizers. This remains a
component checkpoint: the Win32 warning/candidate selection and helper transfer
are not wired into image-backed creation yet. The
[cache audit](../docs/linux-download-signatures.md#cp21q--atomic-full-version-cache-installation)
records the signature precondition and commit boundary.


## CP21r — connected GRUB core selection

The unprivileged selection component in the existing Qt analysis library now
connects full-name cache lookup, the Win32 Yes/No/Cancel contract, shared server
candidate order, signed downloads and atomic installation. Source identity and
installed readback checks stop changed/substituted inputs. Its 53-case local
suite and focused regressions pass normally and under sanitizers. The
[selection audit](../docs/linux-grub2-reuse.md#cp21r--connected-unprivileged-core-selection-2026-09-30)
records its fallback and unsigned-cache contracts. Main-window warning, frozen
helper transfer and compatible-image boot integration remain pending; this does
not enable image-backed GRUB creation yet.


## CP21s — frozen GRUB core transfer

The existing process/helper optionally retains selected core bytes before target
access using private staging and bounded path/size/hash metadata. Eight focused
suites pass normally and under sanitizers; GUI/helper builds pass. The
[handoff audit](../docs/linux-grub2-reuse.md#cp21s--frozen-core-handoff-and-helper-preflight-2026-09-30)
records the tests and limits. Main-window warning, source-backed GRUB job and
compatible-image boot integration remain pending; creation is still disabled.


CP21t is complete: GRUB download warning/START integration and all 18 new GUI
scenarios pass normally and under sanitizers. The full Widgets suite also passes
(647 cases, two optional real-ISO skips). Pure image-backed GRUB creation remains
disabled; CP21u extends the request/helper route before compatible-image boot
qualification. See `../docs/linux-port-plan.md`.


CP21u adds GRUB ISO request/read-only preflight using the existing source and
boot-policy code. Nine focused suites pass normally and under sanitizers. The
launcher/helper write route stays gated; image extraction/helper integration
and compatible-image boot qualification remain before GUI enablement. NTFS
images needing UEFI:NTFS and ext ISO jobs are still pending parity work.


CP21v joins GRUB ISO jobs to the existing FAT16/FAT32/NTFS helper extraction and
boot-record paths; it adds no second production writer or job layer. Normal and
sanitized helper/launcher regressions pass. GUI creation remains disabled until
CP21w proves one compatible image/core/module/configuration boot. Test details
and the simulated-mount qualification limits are in
[the CP21v record](../docs/testing-and-checkpoints.md#cp21v--grub-iso-helper-integration).


CP21w qualifies one compatible GRUB 2.12 BIOS/MBR/FAT32 ISO through production
helper creation, independent complete readback and Linux userspace boot, normally
and under sanitizers. It also fixes suppressed GRUB ISO copy progress. GUI
creation remains disabled until CP21x connects and qualifies START/READY; other
filesystem/firmware combinations remain pending. See [the test record](../docs/testing-and-checkpoints.md#cp21w--compatible-grub-iso-native-creation-and-boot).


CP21x enables the existing GRUB2 BIOS/MBR/FAT32 ISO route in Qt Widgets. It
preserves shared Syslinux precedence, core warning/cache/download/fallback choices,
ISO-recommended/optional-DD selection and unknown image versions. Retained core
bytes reach the existing helper handoff; copy progress uses Rufus's ISO weighting,
and READY waits for sync/cleanup and helper exit. Other GRUB ISO filesystems and
new firmware/module combinations remain pending. See the
[test record](../docs/testing-and-checkpoints.md#cp21x--grub-iso-qt-start-progress-and-ready).


CP21y qualifies the same compatible GRUB 2.12 ISO/core/module fixture on native
BIOS/MBR/FAT16 media without changing production code. Normal and sanitized
helper creation, all 308 file hashes/original boot-record readback and Linux
userspace USB boots pass; the FAT32 regression passes too. GUI ISO creation
still offers FAT32 until CP21z enables/test-checks the FAT16 selection. Other
GRUB ISO filesystem/firmware combinations remain pending. See the
[test record](../docs/testing-and-checkpoints.md#cp21y--native-fat16-grub-iso-creation-and-boot).


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
See [the port plan](../docs/linux-port-plan.md#cp21ac--grub-iso-ntfs-with-uefintfs-backend-integration).


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
See [the test record](../docs/testing-and-checkpoints.md#cp21ad--native-grub-ntfs-with-bios-and-uefintfs-handoffs).


CP21ae enables EFI-capable GRUB ISO MBR/NTFS in Qt using the existing combined
helper qualified by CP21ad. Rufus's shared extra-partition rule determines the
UEFI:NTFS reservation in cluster selection and the frozen request. Plain BIOS,
MBR/FAT, GPT/UEFI and hybrid ISO/DD behavior is retained. Windows image mode
supports FAT16/FAT32/NTFS; ext choices belong to standalone GRUB. Earlier notes
listing ext ISO creation as parity work are superseded by this source audit.
See [the current support guide](../docs/linux-support.md) and
[the checkpoint](../docs/linux-port-plan.md#cp21ae--efi-capable-grub-iso-mbrntfs-in-qt).


CP22e enables the qualified Windows 10/11 BIOS and dual NTFS routes in Qt.
EFI-capable images default to GPT/UEFI; MBR selects BIOS (or UEFI-CSM), while
Alt+E selects MBR and BIOS or UEFI when the fallback EFI loader is present.
Alt+A persists the original Rufus/Windows 7 MBR choice. Shared image/filesystem/
extra-partition policy, native Windows geometry and the existing single helper
supply creation. FAT BIOS/dual and legacy Windows paths remain work; GPT FAT32
splitting remains available by explicitly selecting GPT after Alt+E. See
[the support guide](../docs/linux-support.md#windows-bios-and-dual-ntfsfat32-creation).


CP22j also enables qualified Windows 10/11 MBR/FAT32 through the same Qt/helper
flow. Alt+E permits FAT32 and direct EFI copying; it needs no UEFI:NTFS partition.
Small-WIM images retain a valid FAT32 choice, while large-WIM images retain NTFS
until FAT32 is selected explicitly, using existing install.wim splitting. GPT
remains selectable for UEFI-only media. Alt+A, native geometry, confirmation
freezing and READY after helper cleanup/exit are retained. Seven normal and
sanitized suites pass; native creation/handoff evidence comes from CP22i.
Earlier FAT32 BIOS/dual gate notes are superseded; legacy Windows/FAT16 remain
work. See [the current support guide](../docs/linux-support.md) and
[the checkpoint](../docs/linux-port-plan.md#cp22j--windows-fat32-biosdual-in-qt).


CP22n71 enables source-owned GRUB4DOS MBR/NTFS BIOS/UEFI image creation in the
ordinary Qt flow. Shared policy adds the required trailing UEFI:NTFS payload;
FAT16/FAT32 remain available, GPT retains its default and manual format choices
survive rescans. The source keeps its own loader and unknown version. Normal
and sanitized component checks cover selection, paired geometry, progress,
failure/cancellation and joined READY. Native Qt creation/readback and BIOS/x64
UEFI checks cover both NTFS providers plus direct FAT32 regression. See the
[support guide](../docs/linux-support.md#grub4dos-standalone-and-iso-creation)
and [checkpoint](../docs/linux-port-plan.md#cp22n71--ordinary-qt-paired-grub4dos-ntfs-2026-10-18)
for the representative-image, sector/cluster and authorization limits.


### CP23a: image checksum foundation

The original Windows digest core is shared through `src/hash_core.inc` and
`hash_core.h`; the native shim retains its algorithms and CPU acceleration.
`platform/image_hash.c` reads one retained ordinary source with 64-KiB pread I/O,
validates identity before/after reading, and returns MD5/SHA-1/SHA-256 plus optional
SHA-512. Errors/cancellation clear results; descriptor ownership and offsets stay
with the caller. There is no target, privileged helper or USB access. Windows'
parallel reader/algorithm pipeline remains a performance-comparison item.
At CP23a the GUI button remained disabled; CP23b below enables the qualified workflow.

Build `test_image_hash`, `image_hash_probe` and `test_image_source`, then run
`ctest --test-dir BUILD --output-on-failure -R '^rufus_image_(hash|hash_vectors|source)$'`.
The reference-vector driver also accepts qemu-x86_64 as a second argument:
`python3 linux/tests/test_image_hash.py BUILD/image_hash_probe /usr/bin/qemu-x86_64`.
This explicitly requires emulated SHA-1/SHA-256 acceleration. Normal and
ASan/UBSan/leak checks, including simultaneous first-use readers, pass. On this
host leak checks require process inspection outside the ptrace-restricted sandbox.
See the checkpoint record and `.rufus-test-artifacts/cp23a` for the Windows behavior
and reuse audit, source/core comparisons, logs and limits. No physical USB is used.


### CP23b: Qt checksum workflow

Select an accepted raw/ISO image and use the checksum icon. The worker freezes
source identity and MD5/SHA1/SHA256 plus optional SHA512; **Alt+H** toggles the saved
EnableExtraHashes preference, default false. No USB/helper/authorization is needed.
CANCEL stays asynchronous; controls, full READY and results wait for joined
cleanup. The GUI-thread dialog offers original read-only monospaced fields,
full-value copying, optional-hash hint and OK/Escape. Source changes and I/O errors
publish no digests. See the support and checkpoint guides for behavior and limits.

`test_image_hash_widgets` covers worker/UI lifecycle, queued-result cancellation,
source freezing, retry/teardown, unprivileged/no-target operation, ISO analysis
eligibility, settings, deferred device refresh and real app-font scaling. Build it
and run `ctest --test-dir BUILD --output-on-failure -R '^rufus_image_hash_widgets'`.
Normal and sanitizer UI, analysis/window/scaled and focused raw-write regressions
pass. Evidence and inspected screenshots are in `.rufus-test-artifacts/cp23b`.
Parallel-pipeline throughput comparison is the next checksum checkpoint.


### CP23c: native parallel checksum pipeline

The checksum reader now uses three 64-KiB buffers, one pread reader and a worker
for each requested algorithm. Source offsets/identity and the Qt interface are
unchanged. Reporting/cancellation stay on the caller; startup errors/cancellation
join all created workers before freeing buffers or publishing results. The
monotonic idle wait polls cancellation every 50 ms. The existing GUI gate still
waits for its source close and Qt worker exit.

Build `test_image_hash_pipeline`, `test_image_hash`, `image_hash_probe`,
`test_image_source` and `test_image_hash_widgets`, then run:
`ctest --test-dir BUILD --output-on-failure -R '^rufus_image_(source|hash|hash_pipeline|hash_vectors|hash_widgets|hash_widgets_scaled)$'`.
Normal and ASan/UBSan/leak tests pass. The native pipeline's 27 setup/backpressure/
cancellation cases also pass ThreadSanitizer and a single-CPU run. The optional
qemu-x86_64 vector command above passes 100 cases with SHA instructions enabled.

Optimized warm-input probes show about 1.96x default and 2.63x optional-SHA512
throughput at 256 MiB relative to the retained serial source on this host. This is
not a Windows executable or complete desktop/storage benchmark. Reproduction,
source/core hashes, test logs and exact limits are in `.rufus-test-artifacts/cp23c`
and the checkpoint record. All generated benchmark/source files are removed.


### CP24a: shared compressed policy and read-only gzip source

`src/image_compression.h` shares Windows Rufus's original extension associations,
numeric compression/container identifiers and strict 512-byte boot-marker rule.
The native `ReadCompressedImageSource` currently implements gzip only; the other
original associations and Unix-compress fallback inside .gz return ENOTSUP pending
their providers. It uses a retained regular descriptor,
64-KiB input/output buffers and a 256-KiB per-operation provider allocation budget.
The caller supplies a maximum decoded byte count. Actual size is counted in 64
bits; every contiguous member's CRC/ISIZE is validated. Original gzip ignored-tail
behavior is preserved and the tail length is reported. A consumer receives ordered
provisional chunks; no result is published after a later error, cancellation or
source change. The source descriptor and offset remain owned by the caller.

CMake requires `ZLIB::ZLIB`; this host qualifies the distribution's zlib-compatible
zlib-ng provider. No package was installed. Build `test_compression_policy`,
`test_compressed_source`, `compressed_source_probe` and `test_image_source`, then:
`ctest --test-dir BUILD --output-on-failure -R '^rufus_(compression_policy|compressed_source|compressed_source_vectors|image_source)$'`.
Normal and ASan/UBSan/leak checks pass, including a real stream expanding to
2^32 + 512 bytes without an expanded temporary image. Native concurrent-source
and failure/cleanup checks also pass ThreadSanitizer. See CP24a's checkpoint and
`.rufus-test-artifacts/cp24a` for the provider/reuse audit and Windows extraction
proof. This is a source foundation; compressed USB creation remains unavailable.


### CP24b: exact decoded plans and sector-aware gzip file writing

`PrepareCompressedWrite` freezes source identity, complete gzip stream facts,
decoded SHA256 and exact output rounded to the explicitly supplied sector size.
It uses the shared boot-marker/explicit-force rule. `CompressedImageWriteSize`
checks 512..65536-byte power-of-two sectors and signed 64-bit offset overflow.

`WriteCompressedImageToFile` accepts an existing buffered O_RDWR regular file.
It recomputes the complete plan before writing, aggregates even tiny members into
64-KiB batches, explicitly zero-pads/checks the final sector, fsyncs written bytes
even after failure/cancellation, then decodes and compares all padded output.
It preserves descriptor offsets, target length and the remaining target tail.
RAW_WRITE_RESULT counts physical bytes including padding and preserves separate
copy/cancellation and sync errors. Source/target changes prevent DONE. The caller
still owns descriptors, helper/worker join and GUI READY. No block-device access,
new authorization path or ordinary Qt compressed admission is enabled yet.

Native digest consumers call `InitializeNativeHashes` from `hash_native.h` before
using the shared original core. The existing file checksum pipeline and compressed
planner now share one pthread_once initialization, including concurrent first use.
Original Windows startup/algorithms and the native hash pipeline are unchanged.

Build `compressed_write_probe` and run:
`ctest --test-dir BUILD --output-on-failure -R '^rufus_compressed_write$'`.
230 independent geometry/plan/write/flush/readback cases pass normally, under
ASan/UBSan/leak checking and ThreadSanitizer. Affected raw/hash/source regressions
also pass; SHA-capable QEMU still passes the original 100 digest vectors. The tests
write only disposable regular files with synthetic boot-marker data. No compressed
OS boot or device creation is claimed. See `.rufus-test-artifacts/cp24b` and the
checkpoint record for reproduction, exact limits, reuse audit and cleanup.


### CP24c: compressed block boundary and authorized helper protocol

`WriteCompressedImageToDevice` shares the existing bounded writer engine and
target inspector. Borrow an already authorized exclusive block descriptor; require
the exact dev_t, capacity and frozen 512..65536-byte logical sector size before
writing and after sync/readback. The owner retains mount/holder/ancestry protections.
Pure `CompressedImageWriteSize` lives in a separate object so protocol-only users
need no decoder/reporting implementation or source I/O.

The new `compressed-write-device-v1` request reuses the same raw helper operation,
single pkexec launch and worker lifecycle. Stored source identity remains distinct
from decoded size, sector, SHA256 and explicit ignore-marker choice. Reject mixed,
stray, noncanonical or unsupported metadata. Raw progress/result counters use padded
physical bytes; the existing checking phase covers full preflight. Ordinary raw
packets keep their old shape. Native source ownership/no-follow checks precede
complete decoded plan reconstruction, which precedes target acquisition. The job
keeps its claim through decode/write/sync/readback and explicitly checks target,
caller-home and source descriptor closure before publishing. Cancellation cannot
overwrite a real close/write failure. Fsync failures remain distinct too.

Build `compressed_device_helper_probe`, `test_raw_helper_process`,
`compressed_write_probe`, `test_prepare_source`, `test_prepare_image_process`,
`test_udisks_target` and `raw_write_probe`. Normal and ASan/UBSan/leak runs pass:
`ctest --test-dir BUILD --output-on-failure -R '^rufus_(compressed_write|compressed_device_helper|raw_helper_process|raw_write|prepare_source|prepare_process|udisks_target)$'`.
The D-Bus target fixture needs a permitted private Unix socket; LeakSanitizer also
needs process inspection outside this host's sandbox restriction.

49 real-helper/native-job cases use link-only authorization/kernel/selection facts
around private regular files, with a real exclusive file lease checked throughout
I/O. 59 raw/compressed Qt protocol/process results cover one authorization launch,
invalid totals, cancellation/cleanup failure, joined exit and exact metadata. Old
raw/source/preparation/target regressions pass. Production contains no fixture or
environment override. Both apps/helpers build. GUI compressed admission remains
gated pending real kernel-device/boot qualification. Evidence/reuse/limits/cleanup
are in `.rufus-test-artifacts/cp24c` and the checkpoint record.


## CP24d — guest-kernel gzip helper and bootable readback (2026-10-18)

The gzip helper now has real guest-kernel coverage on a disposable 512-sector USB
block disk. Both normal and ASan/UBSan/leak runs pass 15 gzip jobs plus ten existing
Syslinux/raw regressions. They exercise source/decoded/CRC/sector refusal, real
mounted-target protection/unmount, physical padding/progress, partial dirty writes,
cancellation, sync/readback/close failure, exclusive claim lifetime and joined exit.
The unchanged production helper restores a complete 64-MiB bootable gzip image;
independent host/guest hashes, MBR/FAT/files and BIOS Syslinux/menu/boot-sector
execution agree. UDisks metadata and pkexec are guest fixtures; native kernel,
mount/source/home/claim operations remain real. No physical-device, host-polkit,
other-real-sector-size or installed-OS/Linux-kernel boot claim is made.

See `tests/vm/README.md` for `--compressed-raw-helper` and the checkpoint record
for exact bounds, failed attempts and retained evidence. The production runtime
and original algorithms remain unchanged in this checkpoint. Ordinary gzip
selection remains gated for frozen Qt analysis/plan/geometry, padded progress and
joined READY integration. All remaining compression/container/image-saving and
advanced/legacy Windows parity work stays in scope.


## CP24e — ordinary gzip selection and confirmed Qt DD creation (2026-10-18)

SELECT now accepts *.gz through the cancellable analysis worker and freezes exact
decoded size/SHA256 and source identity. START reads native target geometry, checks
sector padding/capacity and revalidates the confirmed plan, then uses the existing
single-authorized helper. Progress/READY use physical padded bytes; completion
still waits for successful sync/readback/cleanup and helper exit. Confirmation
shows written and compressed sizes. Strict 55 AA marker behavior remains.

Normal and sanitizer checks pass 55 analysis, 77 native-facts and 45 focused
Widgets results. Two real-kernel disposable guests each pass 26 jobs including
ordinary Qt gzip selection/creation, full independent hashes/readback and BIOS
Syslinux/boot-sector execution. UDisks/pkexec and front-end UID are guest fixtures;
physical media, host polkit and installed-OS/kernel boot are not qualified.
The full SELECT scan adds one decode pass to the existing four-pass helper; it
keeps the GUI responsive and stores no expanded production image.

See the checkpoint record and tests/vm/README.md for commands, original/native
differences, initial fixture failures, the broad sanitizer timeout and cleanup.
Current focused suites pass; do not interpret initial broad logs as all-green.
Other compression/container/force-marker/saving and advanced/legacy parity remain.


## CP25a — verified XZ source provider (2026-10-18)

The retained-source API now reads XZ using the original Rufus XZ Embedded LZMA2
and host BCJ decoders, with the original 64-MiB dictionary/first-stream behavior.
Native CRC64/SHA256 verification strengthens the original skipped checks; reserved
checks are refused. Opt-in shared hooks preprocess identically when disabled.
Production XZ selection/device creation remains gated while its write path is
qualified. Ordinary gzip creation remains available.

Build `compressed_source_probe`, `compressed_xz_legacy_probe`,
`test_compressed_source`, `compressed_write_probe` and `test_image_analysis`.
Run `ctest --test-dir BUILD -V -R '^rufus_(compressed_(source|source_vectors|xz|write)|image_analysis_widgets)$'`
with `QT_QPA_PLATFORM=offscreen`; sanitizer builds use ASan/UBSan/leak checks.
The tests need Python's lzma and the xz CLI for independent fixtures; production
needs neither external decoder. Both builds pass five suites, including 221 XZ
cases, retained-source/failure/concurrent checks, actual >4-GiB decoded counting,
gzip writer regressions and 55 complete Qt analysis results. No expanded image
or XZ guest/physical qualification. See the checkpoint record and cp25a evidence.


## CP25b — qualified XZ writer and helper protocol (2026-10-18)

XZ now uses the existing compressed plan/write/sync/readback engine and strict
compressed helper request. Retain original first-stream/dictionary behavior and
CP25a's stronger native integrity checks. Ordinary XZ SELECT remains explicitly
gated pending real guest qualification. The native helper/source/claim/cleanup
and process/controller bodies remain unchanged; gzip creation remains available.

Normal and ASan/UBSan/leak checks pass XZ's 139 file and 55 native-helper cases,
gzip's 232 file and 49 helper regressions, 59 raw/gzip plus 33 XZ process results,
56 complete Qt analysis results, and the source contract/corpus suites. Targets
are private regular files with link-only kernel/authorization facts; no XZ guest,
physical media or boot qualification is claimed. Both app/helper builds finish.

Build `compressed_write_probe`, `compressed_device_helper_probe`,
`test_raw_helper_process`, `test_image_analysis`, `compressed_source_probe`,
`compressed_xz_legacy_probe` and `test_compressed_source`. Run with
`QT_QPA_PLATFORM=offscreen`:

```sh
ctest --test-dir BUILD -V -R '^rufus_(compressed_(source|source_vectors|xz|write|xz_write|device_helper|xz_device_helper)|(raw|xz)_helper_process|image_analysis_widgets)$'
```

The raw/gzip and XZ process rows run separately within unchanged 30-second bounds;
no product deadline is extended. See the checkpoint record and cp25b evidence for
initial attempts, exact timings, reused sources, failure coverage and cleanup.


## CP25c — real guest-kernel XZ helper and bootable restore (2026-10-18)

Both normal and ASan/UBSan/leak guests pass 20 XZ jobs plus 25 existing seed/raw/
gzip jobs. Actual guest block I/O, source/home protection, mount/unmount, exclusive
claim lifetime, dirty cancellation, flush/readback/close failures and joined exit
are qualified. CRC32/CRC64/SHA256 refusals preserve the entire disk; successful
checks and no-data-check input write exact padded bytes. The unchanged production
helper restores a complete 64-MiB CRC64 XZ image. Guest/host hashes, MBR/FAT/files
and BIOS Syslinux/menu/boot-sector execution agree. All 721 production files are
unchanged. Ordinary XZ SELECT remains gated for its controller qualification.

See tests/vm/README.md for --xz-raw-helper. The encoder is an independent guest
fixture tool; production adds no xz/liblzma dependency. UDisks/pkexec are fixtures,
and no physical media/host polkit/other actual sector/installed-OS or kernel boot
claim is made. Failed attempts, exact resource bounds and cleanup are retained in
the checkpoint record and cp25c evidence. All large generated fixtures are removed.


## CP25d — ordinary gzip/XZ SELECT and confirmed DD creation (2026-10-18)

SELECT offers *.gz and *.xz. The cancellable worker validates full decoded size,
SHA256/source identity and strict boot marker, then the existing confirmed native
plan/controller uses observed sector padding and one helper through cleanup/exit.
Normal and sanitizer runs pass 64 complete analysis and 63 focused Widgets results,
including cross-format replacement, invalidation, errors/cancel, held 99%, joined
full green READY and one completion alert. Run:

```sh
ctest --test-dir BUILD -V -R '^rufus_(image_analysis_widgets|compressed_write_widgets)$'
```

Both 47-job guests add ordinary gzip and XZ controller restores to the previous
45 helper jobs, poison both ends of the disk, then require full independent hashes,
MBR/FAT/file readback and BIOS Syslinux/menu/boot-sector execution. See the VM
README for --xz-qt-helper and the checkpoint record for bounds, UID/UDisks/pkexec/
tmpfs fixture limits, first compile correction and cleanup. No expanded production
image or additional decoder runtime dependency; other original formats remain.


## CP26a — BZIP2 retained-source foundation (2026-10-18)

The internal BZIP2 reader reuses the original Huffman/BWT/RLE decoder, keeps all
state per operation and uses retained pread with bounded buffers, complete block/
combined CRCs, concatenated members and actual 64-bit output accounting. It
strictly refuses corrupt empty combined CRCs that the original wrapper accepts.
Normal and sanitizer ten-target regression runs pass, including 180 independent/
differential cases, single/two-member source faults and two actual >4-GiB decode
passes. Run the provider tests with:

```sh
ctest --test-dir BUILD -V -R '^rufus_compressed_bzip2(_large)?$'
```

No external bzip2/libbz2 production dependency. This is an internal source
checkpoint; BZIP2 write/helper/guest/ordinary Qt creation still needs integration.
See CP26a in the port plan for reuse, allocation, checksum divergence, tests,
preprocessor proof, limits and fixture cleanup.


## CP26b — BZIP2 native write/helper backend (2026-10-18)

BZIP2 uses the existing frozen source/hash/geometry plan, four helper decode
passes, dirty fsync, full readback and checked cleanup/child-exit gates. Normal
and sanitizer runs pass 138 file-writer cases, 56 simulated-native helper jobs,
33 strict protocol/process results and all 65 analysis results, with gzip/XZ
regressions retained. Run the new backend targets with:

```sh
ctest --test-dir BUILD -V -R '^rufus_(compressed_bzip2_write|compressed_bzip2_device_helper|bzip2_helper_process)$'
```

The backend accepts BZIP2; ordinary SELECT remains gated for real guest and
controller qualification. The image-analysis test proves both sides using a
valid fixture from the independent bzip2 CLI. No new production runtime provider
or authorization process. See CP26b for the BWT/member fault fixtures, exact
partial-work/readback/cleanup assertions, reuse proof and qualification limits.


## CP26c — real BZIP2 guest helper (2026-10-18)

The new VM --bzip2-raw-helper recipe retains all 47 gzip/XZ jobs and adds 22
BZIP2 jobs: metadata/block/combined/empty-CRC refusal, mounted protections/cancel,
real claimed block writes/sync/readback/close faults, concatenated/tail success
and a fully poisoned 64-MiB restore. Normal and sanitizer runs pass all 69, full
guest/host hashes, independent filesystem/file checks and BIOS boot. Every
production file and the native fault probe are unchanged. Ordinary BZIP2 SELECT
remains gated for controller/READY qualification. See the VM README and CP26c
record for commands, preserved failed gzip attempt, diagnostics, time bounds,
UID/UDisks/pkexec/tmpfs/sector/boot limits and complete owned-fixture cleanup.


## CP26d — ordinary gzip/XZ/BZIP2 selection and creation (2026-10-18)

SELECT offers *.gz, *.xz and *.bz2. All use complete worker validation, frozen
source/hash, actual target geometry, one confirmation/helper and joined READY.
Both normal/sanitizer runs pass 80 complete analysis and 81 focused Widgets
results, including every directed format replacement and corrupt empty BZIP2
refusal. Run:

```sh
ctest --test-dir BUILD -V -R '^rufus_(image_analysis_widgets|compressed_write_widgets)$'
```

The new --bzip2-qt-helper guest performs full-size ordinary creation/readback/
BIOS boot with five seed and five raw setup jobs (11 total). Production helpers
and all backend algorithms are unchanged from CP26c's 69-job qualification.
Both variants pass with private tmpfs backing; source/target/user files are
preserved as documented and every owned large fixture is removed. See CP26d
for the exact reuse proof, bounds, UID/UDisks/pkexec/sector/boot limits and tests.


## CP27a — internal original LZMA-Alone source (2026-10-18)

The retained-source foundation now includes LZMA-Alone. Ordinary SELECT/creation
continues to offer gzip/XZ/BZIP2 until LZMA writer/helper/controller qualification.
The native adapter shares the original range/model/dictionary core, preserves
its full property/dictionary range and exact first-stream consumption, and
rejects proven known-size/history defects. It has no container checksum.

```sh
cmake --build BUILD --target compressed_source_probe compressed_lzma_legacy_probe test_compressed_source
ctest --test-dir BUILD -V -R '^rufus_compressed_lzma(_large)?$'
```

Both normal/sanitizer runs pass 360 vectors, 30892 source checks and actual
4294967808-byte output plus a separate known-size 4-GiB limit refusal. Existing
gzip/XZ/BZIP2 sources/writers and complete/focused Qt regressions also pass
(12 selected targets per build). See CP27a for original differences, exact
consumption, independent properties, memory bounds, test correction and limits.


## CP27b — LZMA writer and strict helper foundation (2026-10-18)

LZMA-Alone now uses the existing frozen plan/native writer/helper protocol with
original dictionaries/properties and known-size/first-stream behavior. Both
normal/sanitizer runs pass all 13 selected backend targets: 145 LZMA writer
cases, 60 helper jobs, 33 protocol results, 81 complete analysis results and
gzip/XZ/BZIP2 regressions. Valid altered streams with matching metadata prove
SHA256/readback protection without a LZMA container CRC.

```sh
cmake --build BUILD --target compressed_write_probe compressed_device_helper_probe test_raw_helper_process test_image_analysis rufus-linux rufus-prepare-helper
ctest --test-dir BUILD -V -R '^rufus_(compressed_lzma_(write|device_helper)|lzma_helper_process|image_analysis_widgets)$'
```

This is private-file/simulated-kernel qualification. LZMA remains absent from the
ordinary SELECT filter/worker until actual guest/helper/controller and joined
READY checks pass. Enabled GUI compression remains gzip/XZ/BZIP2. See CP27b for
exact source/hash/geometry/claim/close/cancel contracts and qualification limits.


## CP27c — real LZMA guest helper qualification (2026-10-18)

--lzma-raw-helper runs 38 focused jobs: five seed/five raw setup, four mounted
LZMA and 24 direct source/format/geometry/claim/fault/full-restore cases. Both
normal/sanitizer runs pass. All production hashes and both helper binaries are
unchanged from CP27b. Real source error/EOF after a 64-KiB commit, dirty sync,
readback and checked close retain an actual guest-kernel block claim. Full
64-MiB poisoned restore, raw/LZMA/guest/host hashes, files and BIOS boot agree.

Ordinary LZMA selection/controller/READY remains gated for the next checkpoint.
The recipe uses private tmpfs host files, a real root guest kernel/helper and
fixture metadata/launcher; no physical device or host desktop authorization is
qualified. See CP27c and the VM README for commands, exact bounds and limits.


## CP27d — ordinary gzip/XZ/BZIP2/LZMA creation (2026-10-18)

SELECT now offers *.lzma alongside *.gz/*.xz/*.bz2. All use complete retained
worker validation, actual target geometry, one confirmation/helper and joined
READY. Both normal/sanitizer runs pass 96 complete analysis and 99 focused
Widgets results, all twelve directed replacements and LZMA size/history/EOS
refusal. The focused --lzma-qt-helper guest passes full-size ordinary creation/
readback/BIOS with five seed and five raw setup jobs (11 total).

```sh
ctest --test-dir BUILD -V -R '^rufus_(image_analysis_widgets|compressed_write_widgets)$'
```

Production helpers and all backend algorithms remain unchanged from CP27c's
38-job qualification. See CP27d for original LZMA semantics, exact test/ownership/
sector/boot/persistence limits and evidence. Other original providers and full
Windows parity remain active work.


## CP28a — internal Unix-compress source foundation

The read-only `.Z` provider now shares Rufus's original LZW dictionary/bitstream
code, with bounded per-operation ownership and retained-file I/O. Native final
consumer failures propagate; nine-bit resets avoid the original's silent output
loss, matching fixed current ncompress. Disabled Windows preprocessing is exact.
The format has no size/checksum/end marker: EOF terminates it, incomplete code
bits are ignored, and truncation can appear as a valid shorter stream.
Normal passes 387 corpus cases, 37197 source checks and an actual 4294967808-byte
count/content qualification without storing an expanded image.
The final ASan/UBSan source/corpus/large qualification also passes. Independent encoders remain test-only; no production
runtime dependency is added. Run `rufus_compressed_lzw` and
`rufus_compressed_lzw_large` with CTest for focused checks.
Ordinary creation remains gzip/XZ/BZIP2/LZMA. `.Z` writer/helper/controller and
Unix-compress fallback inside `.gz` await their next qualifications. See CP28a
in the checkpoint log for exact differences, independent providers and limits.


## CP28b — internal LZW writer and helper qualification

The retained `.Z` source can now prepare frozen decoded plans and use the native
writer/strict helper. One helper owns all four decodes, claims, padded writes,
sync, full readback and checked cleanup. The existing writer/job algorithms and
all Windows/shared cores remain intact; there is no new runtime dependency.
Native late invalid-code diagnostics now return EBADMSG instead of accepting an
old positive write count. LZW has no size/checksum/end marker, so actual decoded
size, SHA256 and complete readback are frozen and checked on every pass.
Normal qualification passes 390 source cases, 188 writer cases, 57 simulated
helper jobs, 33 LZW protocol and 97 analysis results, plus all admitted-format
regressions. The complete ASan/UBSan 17-target qualification also passes. These tests use owned files and
simulated kernel/UDisks facts; actual LZW guest/device/controller checks are next.
Ordinary creation remains gzip/XZ/BZIP2/LZMA. LZW SELECT and `.gz` magic fallback
remain gated pending qualification. See CP28b for error semantics and test limits.


## CP28c — LZW real guest helper qualification

Both normal/sanitizer 50-job guests qualify the unchanged LZW production helper
with real kernel block claims, source/home protection, dirty faults and checked
cleanup. Twenty independently decoded fixtures and eleven claim observations
include late invalid codes and valid source-data changes without a format CRC.
A fully poisoned 64-MiB restore passes raw/LZW/guest/host hashes, independent
filesystem/files and BIOS boot. All 730 production files and both helpers are
unchanged from CP28b. Actual sectors are 512; metadata/pkexec are fixtures, host
backing is private tmpfs and no physical media/polkit/persistence claim is made.
Ordinary creation remains gzip/XZ/BZIP2/LZMA. LZW GUI creation and `.gz` magic
fallback are next. See CP28c for exact coverage, limits and cleanup.


## CP28d — ordinary LZW creation and original gzip fallback

Ordinary SELECT and confirmed DD now include `*.Z`/`*.z`, plus original initial
Unix-compress magic dispatch inside `*.gz`. Filename association stays gzip for
that fallback, with LZW's actual no-checksum/EOF semantics. Genuine gzip retains
its member CRC/ISIZE and ignored-tail rules. No runtime ncompress dependency.
Both normal/sanitizer 23-target host suites, 130 analysis/135 focused Widgets
results and complete 17-job guests pass. Each guest includes two full 64-MiB
ordinary controller restores, five actual fallback claim/source/data faults,
held99 then joined READY/100, independent readback/files and BIOS boot. Real
guest sectors are 512 bytes; physical media, desktop polkit, persistence and
installed OS boot remain unqualified. See CP28d for exact evidence and limits.
At CP28d, ordinary compression was gzip/XZ/BZIP2/LZMA/LZW; ZIP and other original
providers, containers, force-marker UI, saving and remaining parity stay active.


## CP29a — internal ZIP source reader

The Linux build embeds a bounded retained-source ZIP reader for first-entry
stored and raw-deflate streams, including ZIP64. It uses the existing zlib
dependency and no external unzip executable. Original Windows ZIP preprocessing
remains unchanged. Both normal and ASan/UBSan/leak 28-target host suites pass,
including a complete actual >4-GiB ZIP64 count/content/CRC check; focused ZIP
rechecks pass 140 cases. Run `rufus_compressed_zip` and
`rufus_compressed_zip_large` with CTest for source checks. ZIP is not yet an
ordinary GUI selection or USB creation format: methods 12/14/95, writer/helper,
real guest and confirmed READY are pending. At CP29a, GUI compression remained
gzip/XZ/BZIP2/LZMA/LZW. See CP29a for evidence and limits.


## CP29b — complete original ZIP source methods

The internal ZIP source reader now handles all five methods named by the
original Windows dispatcher: stored (0), deflate (8), BZIP2 (12), LZMA (14)
and XZ (95). These use embedded decoders; no external unzip/bzip2/lzma/xz
runtime program is added. Bounded first-entry slices, ZIP CRC, ZIP64 and the
ZIP-LZMA EOS flag are checked. Both normal and sanitizer 28-target host suites
and final 256-case focused ZIP rechecks pass. Run `rufus_compressed_zip` and
`rufus_compressed_zip_large` with CTest for the source checks. ZIP ordinary
SELECT/USB creation remains gated pending writer/helper/guest/Qt qualification.
At CP29b, GUI compression remained gzip/XZ/BZIP2/LZMA/LZW. See CP29b for the
evidence and remaining work.


## CP29c — ZIP frozen plans and strict helper writing

`rufus_compressed_zip_write`, `rufus_zip_helper_process` and
`rufus_compressed_zip_device_helper` exercise the new ZIP writer/protocol path.
Stored, deflate, BZIP2, LZMA and XZ first entries use the embedded decoders.
The plan freezes decoded size and SHA256; the writer revalidates the retained
archive before writing, checks padding, sync and full readback, and the helper
retains claim/cleanup. These tests use owned regular files and simulated
kernel facts. ZIP is still excluded from ordinary Qt SELECT until real guest
block-device and complete controller/READY qualification. At CP29c, GUI
compression remained gzip/XZ/BZIP2/LZMA/LZW. See CP29c for evidence.

The sanitizer host run passes 31/31 targets. The normal run passes 30 targets
inside the sandbox and the remaining private-D-Bus Widgets target passes
135/135 cases outside it as an ordinary user.


## CP29d — real guest ZIP helper qualification

`mbr.py --zip-raw-helper` adds real guest block-device ZIP jobs. The guarded
fixture constructs methods 0, 8, 12, 14 and 95 in the guest, then uses the
production helper for writes and CRC refusals. Both normal and sanitizer runs
pass 23 ZIP direct jobs, eight actual claim observations, the required seed/raw
jobs, fully poisoned 64-MiB deflate restore, independent filesystem/readback
and BIOS boot. The 512-byte-sector QEMU disk is disposable and no physical USB
device is used. Ordinary ZIP SELECT and joined READY remain gated. See CP29d.


## CP29e — ordinary ZIP picker, selection and joined READY

The file picker offers `.zip` and `.ZIP`, and the analysis worker admits the
bounded first-entry ZIP source to ordinary SELECT. Frozen decoded size/SHA256,
marker and native target geometry pass through the existing confirmed DD
controller and one-helper route. Normal and sanitizer Qt analysis/Widgets
tests cover picker selection, malformed ZIP refusal, identity/capacity and
error/READY behavior. `mbr.py --zip-qt-helper` passes a complete 64-MiB
deflate controller restore in both guests, including HELD99, one launch,
joined 100%, independent disk/FAT-file readback and BIOS boot. The earlier
CP29d method matrix covers helper writes for stored, deflate, BZIP2, LZMA
and XZ. No external ZIP runtime executable is required. See CP29e evidence.

## CP30a/b — Zstd retained source, writer and strict helper

The Linux build now embeds the original bundled Zstd decoder for `.zst`.
Retained-source reading validates complete checked, concatenated and skippable
frames, rejects non-Zstd tails/truncation, counts actual decoded bytes in 64
bits and enforces the original 128-MiB window bound. Frozen decoded SHA256
plans and the padded writer/strict helper packet are admitted. The independent
test encoder is the `zstd` CLI; production does not launch it. Run CTest targets
`rufus_compressed_zstd`, `rufus_compressed_zstd_write`,
`rufus_zstd_helper_process` and `rufus_compressed_zstd_device_helper`.

At CP30a/b, ordinary Qt selection remained gated pending a real disposable
guest block device and the full SELECT/confirmation/READY route. No physical
USB was written. See CP30a/b evidence and the CP30c/d result below.

## CP30c/d — ordinary Zstd selection and creation

Real disposable guest block-device tests now qualify Zstd helper writes and
full bootable readback in normal and sanitizer builds. The picker offers
`.zst`/`.ZST`; the worker freezes the decoded SHA256/size before START. The
confirmed controller route uses one helper, holds 99% until exit and reports
READY only after full verify, sync and cleanup. Both Qt test suites and both
64-MiB ordinary guest restores pass independent disk/FAT-file/BIOS checks.
The system `zstd` executable is used only to generate test fixtures. See the
CP30 evidence and parity backlog for remaining formats and features.

## CP31a — VTSI retained sparse source

The read-only `.vtsi` provider validates the Ventoy footer, segment table and
sparse layout, then exposes the declared logical disk image with zero-filled
gaps. It preserves the retained file descriptor offset and uses bounded,
batched reads. Run `rufus_compressed_vtsi` for independent malformed-layout,
fault, cancellation and 4-GiB-plus vectors. Native VTSI writes and ordinary
Qt selection remain gated pending separate qualification. See CP31a evidence.

## CP31b — VTSI native sparse writer and helper

The VTSI plan and strict helper packet freeze both logical image and extent-map
SHA256. The native writer preserves target gaps, syncs and verifies listed
extents, and counts actual segment I/O. Run `rufus_compressed_vtsi_write`,
`rufus_vtsi_helper_process` and `rufus_compressed_vtsi_device_helper` for
regular-file, strict packet and simulated device coverage. Real guest and Qt
creation remain gated. See CP31b evidence.

## CP31c — VTSI real guest block write

`--vtsi-raw-helper` in `tests/vm/mbr.py` qualifies a wrong-map refusal, two
separated sparse extents with preserved poisoned gaps, real exclusive block
claims and independent readback. It then restores the full 64-MiB bootable
image through the production helper, with host disk/FAT hashes and BIOS boot.
The raw client now checks VTSI logical progress separately from physical
segment counters; the normal and sanitizer process/guest suites pass. The
guest image is disposable and no host block device is opened. See CP31c
evidence. Ordinary Qt `.vtsi` selection remains gated.

## CP31d — ordinary VTSI selection and creation

The picker now offers `.vtsi`/`.VTSI`; its worker freezes both the logical
image and sparse extent-map hashes before START. The confirmed Qt controller
uses one helper and reports READY after logical progress, physical extent
verification, sync and joined exit. `--vtsi-qt-helper` qualifies a sparse
two-extent Qt job with untouched gaps and a full bootable Qt restore on a
disposable QEMU USB disk. Normal and sanitizer Widgets and guest checks pass
independent host disk/FAT readback and BIOS boot. See CP31d evidence.

## CP31e — fixed/dynamic VHD native foundation

`rufus_compressed_vhd` covers fixed and dynamic VHD logical reads, malformed
metadata and BAT overlaps, faults, QEMU-produced examples and native
regular-file copy/sync/verify. Both normal and sanitizer builds pass. The
source descriptor stays retained and the write plan freezes decoded SHA256.
Differencing parent chains, device-helper/guest and ordinary Qt admission are
still gated; `.vhd` is not in the picker. See CP31e evidence.

## CP31f/g — fixed/dynamic VHD guest and ordinary Qt creation

The strict device helper and normal/sanitizer disposable QEMU guests qualify
changed-hash refusal, dynamic VHD claim/readback and full bootable fixed VHD
restore. The ordinary picker now offers `.vhd`/`.VHD` and freezes the decoded
SHA256 before confirmation. Normal full Widgets and focused sanitizer Widgets
pass; both Qt guests complete dynamic and fixed VHD through one helper per job,
joined READY, host disk/FAT-file readback and BIOS boot. No host block device
was opened. See CP31f/g evidence. VHDX, FFU, differencing parent chains and
device-image saving remain to be implemented.

## CP33 — ordinary fixed/dynamic VHDX creation

`.vhdx`/`.VHDX` now use the ordinary picker, asynchronous logical-image analysis,
frozen SHA256, confirmation and single-helper DD route. The embedded reader
checks redundant CRC-32C headers/regions, metadata and nonoverlapping BAT
extents. Clean fixed and dynamic images reconstruct zero gaps; pending logs
and parent chains return ENOTSUP without modifying the source. The 64-MiB BAT
validation budget can reject very large tables within the 64-TiB format limit.

Run `rufus_compressed_vhdx` for the focused 40-case source/native-write corpus.
`tests/vm/mbr.py --vhdx-qt-helper` combines a small dynamic write and a full
bootable fixed restore, with one seed job and the existing helper fault evidence
reused. Normal and sanitizer guests check independent readback, one helper per
Qt job, held-99/joined READY and BIOS boot. The native decoder adds no QEMU
runtime dependency. Preview 2 packages the complete route; see CP33 evidence
and the support guide for limits. FFU, log replay, parent chains and saving
remain in the parity backlog.

## CP34 — full FFU selection and creation

`.ffu`/`.FFU` now feed ordinary analysis, confirmation and the single-helper DD
writer. The native reader supports full single-store V1/V2 and V1-compressed
none/plain-XPRESS, checks every SHA256 chunk before output, resolves repeated
destinations in write order and reconstructs the captured disk with zero gaps.
Windows uses DISM; the Linux route currently keeps captured size/partition
bytes. Portable resizing, other FFU codecs/stores/partial updates and catalog
trust remain in scope. See the support guide for allocation/alignment limits.

`rufus_compressed_ffu` is the focused source/native-write corpus.
`tests/vm/mbr.py --ffu-qt-helper` combines a small XPRESS Qt job and one full
bootable restore, using one seed and existing helper fault evidence. Img2Ffu
and the independent XPRESS decoder are test-only providers; production adds no
DISM, Wine, .NET or Python dependency. Preview 3 includes the complete route;
see CP34 for evidence and remaining boundaries.

## CP35 — device saving and preview 4

The save icon beside Device captures an unmounted selected device as fixed VHD,
dynamic VHDX or full uncompressed FFU, without selecting an input image.
The GUI uses one read-only UDisks acquisition, checks source consistency and
decodes the temporary output before atomic publication. Cancellation preserves
an existing backup; full READY waits for sync, descriptor closure and worker exit.
No new privileged destination writer or external encoder runtime is required.
The included catalog template retains the Img2Ffu MIT notice in
`share/licenses/rufus-linux-preview/LICENSE.img2ffu`. See the support guide for
format/geometry limits. ISO filesystem export and remaining capture layouts
remain in the parity backlog.

## CP36 — ISO export, automatic source unmount and preview 5

The save dialog now also exports the first filesystem as a UDF-only `.iso`.
It retains hidden files, Unicode names, empty directories and files above 4 GiB;
it does not synthesize a boot catalog. Install optional cdrtools for `mkisofs`.
The exporter verifies every output path and payload and rereads the source
before atomically publishing the image.

Mounted device captures and all ISO exports use one authorized source helper
for safe unmount, read-only acquisition, private mount and cleanup. The GUI,
encoder and destination stay unprivileged. Busy sources fail without forced
unmount; READY waits for source/helper cleanup and durable output. Sources
remain unmounted after completion. Supported source filesystems and explicit
limits are in the [support guide](../docs/linux-support.md). Read-only device
discovery and the remaining container/image operations stay in the parity backlog.

## CP37 — read-only device discovery and saving, preview 6

Eligible USB media now appear even when read-only. The Device list marks them
`[Read-only]`; the save action remains available for VHD, VHDX, FFU and ISO,
while START and format controls stay disabled. Capture keeps existing write
protection and uses the same read acquisition, verification, atomic publication
and joined cleanup as writable sources. Known access changes preserve source
selection and stop a creation job when the device becomes read-only. Unknown
access state remains excluded. See the support guide for source/format bounds.

## CP38 — parent images and VHDX log replay, preview 7

The native source/writer now handles VHD and VHDX differencing parent chains,
including inherited sparse sectors and VHDX partial block bitmaps. Parent
linkage, geometry, VHD timestamp, cycles, ownership and file identity are checked;
the root helper retains all descriptors until cleanup and protects every backing
from being selected as the target. The existing frozen decoded hash and verified
writer are reused. Dirty VHDX logs replay into a private interval overlay before
metadata/BAT reads, without writing the original files. See the support guide
for relative-path behavior, immutable-source differences, chain/log bounds and
remaining Windows-specific parent relocation work.

One normal/sanitizer component corpus covers valid chains, malformed locators,
stale sources and circular/overlapping/zero/extended log replay. The batch guest
uses VHD/VHDX child readback and one full parent-plus-log restore with BIOS boot;
shared writer/helper faults are reused. Evidence is in
`.rufus-test-artifacts/cp38/checks.json`. Windows parity remains in development.

## CP39 — FFU codecs, descriptor runs and capture names, preview 8

Full single-store FFU creation now reads all standard compression identifiers:
uncompressed/default, LZNT1, plain XPRESS and Huffman-XPRESS. Native bounded
MS-XCA readers support chunk boundaries, match history, optional markers and
exact decoded sizes. Grouped compressed payloads and multi-block end-relative
runs reuse the frozen hash and verified writer. Saving FFU accepts unnamed
and Unicode GPT names; the exact GPT payload is retained and optional ASCII
manifest names are omitted when necessary. See the support guide for limits.

The batch uses independent codec/capture comparisons, normal/sanitizer grouped
vectors, one combined guest per build, one full LZNT1 restore/BIOS boot and
one Qt gate. Shared writer/helper/acquisition/transaction evidence is reused.
Compact evidence: `.rufus-test-artifacts/cp39/checks.json`. Target-dependent FFU
deployment features and the other Windows parity groups remain in progress.

## CP40 — full FFU deployment on larger disks, preview 9

Full single-store FFU jobs now apply final sparse extents at the selected
capacity, preserving unlisted bytes and resolving end-relative runs and partial
beginning/end overlaps. On larger GPT disks, both captured GPT copies are
validated before writing and the backup metadata moves to the target end.
Partition entries and filesystem sizes stay unchanged. SELECT and the helper
freeze an FFU layout digest as well as decoded content; sync, verified readback,
cleanup and helper exit still precede successful READY.

One grouped normal/sanitizer corpus and one combined guest per build cover
codecs, sparse placement, GPT enlargement and one full bootable restore. See
[the support guide](../docs/linux-support.md) for bounds and remaining FFU work;
full Windows parity remains active.

## CP41 — exact-sector FFU capture and adaptive sizing, preview 10

FFU capture no longer requires the device size to be a 128-KiB multiple or less
than 128 GiB. It uses a full end-relative tail block, unique source hashing and
progress, and a shared bounded planner that chooses larger chunks when needed.
Logical analysis reconstructs exact capacity and partial overlaps. The existing
native sparse writer, one authorized job, verified readback, cleanup and joined
READY remain in use. The support guide records dense/table/manifest limits and
qualification boundaries; full Windows parity remains active.


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


## CP50 — extended FFU capture layouts, preview 19 (2026-10-20)

FFU saving now supports manifests spanning multiple chunks, empty GPT disks,
and GPT entry tables up to 4 MiB without separate entry-count/width caps.
Capture and larger-target relocation share the table budget. Independent
manifest/store alignment retains every metadata hash; pre-acquisition space
planning covers every accepted manifest without staging the source twice.
A grouped normal/sanitizer corpus checks independent reconstruction and equal/
larger dirty-target restores. Combined guests capture a read-only 8192-entry GPT
source, restore its multi-chunk manifest through ordinary Qt and perform one
representative full restore/readback/BIOS boot. One Qt gate and inspected packages
complete the batch. Optimized resizing, multi-store deployment, signer trust and
the complete Windows parity backlog remain active. See the support/testing records
for bounds and interoperability limits.


## CP51 — old-BIOS compatibility, preview 20 (2026-10-20)

The advanced drive option **Add fixes for old BIOSes** now works for eligible
MBR/BIOS jobs. It starts unchecked and retains the user's session choice while
unsupported modes clear/disable its effective value. The frozen job carries the
choice through one authorization, native formatting, bootloader writes and
joined READY. Shared Rufus placement doubles the track/cluster-aligned prefix.
An inactive final one-track 0xEA partition supplies compatibility unless the
required UEFI:NTFS partition takes priority; persistence precedes either.
The planner, GUI filesystem/cluster choices, boot records, payload checks and
readback use the selected geometry. Oversized GRUB cores refuse before writes.
A grouped native/sanitizer corpus, combined guests, one Qt gate and inspected
packages qualify delivery. Full Windows parity remains active; see the support
and testing records for exact coverage and limits.


## CP52 — ISO → ESP, preview 21 (2026-10-20)

Eligible small regular-EFI images can now use **ISO → ESP** on GPT/FAT16 or
GPT/FAT32. ISO remains the recommended default. The shared policy includes the
separate ESP question for non-hybrid images and persistence, as Windows does.
The helper revalidates source eligibility, writes the main ESP type/name and
retains optional persistence. Partition layout uses the selected allocation
unit; ESP formatting uses the original default-unit rules. FAT32 ESP retains
Windows' quick large-FAT32 behavior. Confirmation shows the selected mode and
actual formatter default. One authorization and READY after sync/cleanup/exit
remain required. One grouped component corpus, combined normal/sanitizer guests,
one Qt gate and inspected packages qualify delivery; full Windows parity remains
active. See the support/testing records for exact limits.


## CP53 — explicit FFU stores, preview 22 (2026-10-20)

V2 FFUs with multiple stores now offer an explicit store chooser. The list shows
one-based store numbers, capacities and source UEFI device paths. START stays
disabled until the chosen store passes complete analysis. The retained source
identity is checked again after choosing; cancellation or a changed source stops
selection. Confirmation identifies the store. The strict optional `store` field
freezes it through native preflight and the single authorized helper. Legacy
requests may omit it for a single-store image; multiple stores never default to
one. The native layout digest binds the chosen store even when two stores have
identical decoded content.

All store headers/descriptors are bounded and parsed in metadata order; payloads
follow all metadata. Metadata hashes authenticate the chooser, and every store's
payload hashes are checked before any logical output or target writes. Only the
selected store contributes writes, end-relative placement, GPT relocation and
partial-update base comparisons. Other stores' ranges are never applied to the
selected device. Existing sector/chunk/map/table bounds, source protection,
claims, sync/readback, cleanup and READY after helper/launcher exit remain.

Windows Rufus delegates FFU deployment to DISM with the selected physical drive.
This Linux route makes the store explicit for its one selected target. Source
UEFI device paths are displayed for identification; they are not resolved into
host devices or used to write additional disks. Automatic multi-target/UEFI-LUN
mapping, optimized partition/filesystem resizing and catalog signer trust remain
unqualified and pending. Full Windows parity remains active.


## CP54 — FFU catalog signature policy, preview 23 (2026-10-20)

FFU input now validates a present catalog as a bounded PKCS#7 Microsoft CTL.
Unsigned catalogs remain accepted. Linux also retains its existing catalog-free
FFU input compatibility: these sources have no signer identity and receive the
same chunk-integrity checks as unsigned CTLs. Windows normally parses a CTL
even for unsigned input; keeping already-supported catalog-free images usable
is an explicit Linux compatibility difference. For a signed
catalog, the first cryptographically valid signer is selected; invalid signatures,
changed signed content and missing signer certificates refuse before logical
output, store listing or target writes. As Windows desktop DISM does, the reader
accepts test/self-signed and expired certificates and rejects its two known
production-chain SHA-1 thumbprints. It does not consult the host trust store,
fetch certificates, apply host clock/revocation policy or claim publisher trust.
A fixed public Microsoft Root Certificate Authority 2010 certificate lets an
included production intermediate reach the same root without host PKI state.

The policy was checked against the static FFU provider shipped in the user's
Windows 11 25H2 ISO. No Windows application was executed. Catalog signatures
cover the CTL contents; payload and metadata chunk hashes are still checked
separately. This preserves the observed desktop policy and does not add a new
requirement to match a CTL member to the FFU hash table. The entire catalog's
SHA-256 is additionally bound into the frozen Linux layout identity, so changing
to another valid catalog cannot retain the selected job's identity. Catalogs
are limited to 16 MiB. FFU saving continues to produce unsigned catalogs.

Qualification groups native normal/sanitizer signature and file-writer cases,
independent osslsigncode signing/verification, a genuine Windows production CAT
refusal, combined normal/sanitizer Qt guests, one Qt gate and inspected native/
generic packages. Each guest applies selected stores from signed FFUs, verifies
readback and source retention, then performs one full restore and BIOS boot.
Existing shared helper faults are reused. Evidence is in
`../.rufus-test-artifacts/cp54/`; disposable disks and package stages are removed.

Optimized partition/filesystem resizing and automatic multi-target/UEFI-LUN
mapping remain pending. Static format findings for the next resizing batch:
`[FullFlash] Optimized=True` identifies a single non-first `UseAllSpace=True`
partition. Windows shifts following begin-relative descriptors in 128-KiB units,
aligns the following partition to 1 MiB, updates GPT geometry and extends the
selected filesystem, with recovery metadata updated afterward. Implementation
must handle the filesystem and recovery consequences together; descriptor
relocation alone does not complete this behavior. Full Windows parity remains
active.

## CP55 — optimized FFU deployment, preview 24

Optimized FFUs now support smaller/larger targets, NTFS growth and moved recovery
locations in one authorized job. See the [support bounds](../docs/linux-support.md#optimized-ffu-deployment-cp55)
and [batch record](../docs/linux-port-plan.md#cp55--optimized-ffu-deployment-preview-24-2026-10-20).
Automatic target mapping remains pending; the full parity goal is active.

## CP56 — FFU closeout, preview 25

FFU feature implementation is closed. Saving now uses desktop-compatible plain
XPRESS, with independent decoding and the shared verified restore path. The
[audit](../docs/linux-ffu-parity.md) corrects the earlier automatic multi-drive
mapping requirement against Windows Rufus's actual desktop provider. The full
Windows parity goal continues with the other backlog groups.

## CP57 — supplementary ZIP archives, preview 26

Ctrl+SELECT now attaches a ZIP overlay for file-based creation/formatting.
The same authorized job validates sources and CRCs, copies the files last
and waits for sync/cleanup/helper exit before READY. DD/ext jobs omit it.
See the [ZIP guide](../docs/linux-supplementary-zip.md) and CP57 batch evidence.

## CP58 — Windows To Go and standalone WIM/ESD, preview 27

Preview 27 closes Windows To Go and standalone WIM/ESD as one batch: ordinary
archive/edition selection, native NTFS metadata including EA/EFS, GPT ESP/MSR or
MBR BIOS/UEFI, BCD/boot files, installed SAN/S-mode and one-helper joined READY.
Normal/sanitizer components cover 284 cases per build. Combined guests cover all
four source forms and one native EFI handoff per build; one full Qt gate passes.
Windows execution/installation and decryption remain deferred. See the
[To Go guide](../docs/linux-windows-to-go.md) for precise coverage and evidence. Full
Windows parity remains active outside this completed feature group.


## CP65 / Preview 34 — device discovery

Running-application disks are excluded and existing mounted extended labels
are shown with Multiple Volumes captions. Discovery is unprivileged and does
not mount media. See the [device-discovery guide](../docs/linux-device-discovery.md)
for native backings, original behavior, metadata safeguards and qualification.


## CP66 / Preview 35 — native image downloads

SELECT/DOWNLOAD now offers the signed Fido Windows and UEFI Shell choices,
browser handoff or streamed ISO saving and automatic selection. No device
authorization is requested for downloading. See [native downloads](../docs/linux-image-downloads.md)
for trust, cancellation, native filesystem publication/recovery and evidence.


## CP67 — application language selection and catalog reuse (Preview 36)

The [language batch](../docs/linux-localization.md) reuses the unchanged original
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

The [application-update batch](../docs/linux-application-updates.md) groups native release
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
