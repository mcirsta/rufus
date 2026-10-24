# Windows installer path on Linux

Source trace and implementation checkpoints CP11a–CP11p (2026-09-17).
This describes the checked-out Rufus source; Windows runtime validation remains
deferred. Ordinary Windows installation media passes the simulator milestone;
five Windows User Experience options now reach the native helper from Widgets.
Windows To Go remains deferred.

## Rufus's existing decisions

| Decision | Existing source and behavior |
| --- | --- |
| Filesystem eligibility | `SetAllowedFileSystems` in `src/rufus.c` calls shared `GetAllowedFileSystems`/`IsFat32Compatible` in `src/format_policy.c`. These use Windows/WinToGo markers, target firmware and the dual BIOS/UEFI option. A large Windows file normally excludes FAT32. |
| Preferred filesystem | `SetFSFromISO` calls shared `GetPreferredFileSystem`, which prefers FAT for an EFI image targeting UEFI without large files, then NTFS for Windows boot-manager/WinPE images. Explicit valid selections and other bootloaders have their own precedence; do not replace this with a rule based only on the filename or Windows version. |
| Large files | `src/image_file.c` records files at least 4 GiB. The special report value `0x11` identifies the single oversized `install.wim` case. With the dual BIOS/UEFI option, Rufus permits FAT32 and `src/iso.c` splits that WIM into SWM files. This is a separate path from the normal NTFS choice. |
| Extra boot partition | `FormatThread` in `src/format.c` calls shared `GetExtraPartitions` in `src/format_policy.c`. It requests UEFI:NTFS for an EFI-capable image on NTFS/exFAT, omitting it for BIOS-only Windows installation unless dual boot is enabled. Windows To Go on GPT/UEFI instead requests ESP/MSR partitions. |
| GPT layout | `CreatePartition` in `src/drive.c` places the ordinary main partition first and UEFI:NTFS near the end, reserving backup GPT metadata and preserving track/cluster alignment. Both are Microsoft Basic Data. The small partition has the no-drive-letter attribute, not the ESP type or a read-only attribute. The original comment explains that an extra ESP can interfere with Windows Setup. |
| Boot payload | Windows writes `res/uefi/uefi-ntfs.img` into the small partition. These are firmware executables and a FAT filesystem image, so the same bytes can be used by Linux without a Windows API bridge. |
| Installation files | The normal installer path extracts the ISO tree. Windows 7 fallback-loader extraction, BIOS WinPE fixes, NTFS finalization and WUE customization are separate operations in `src/format.c`; copying the tree alone does not qualify every Windows image. |

The bundled UEFI:NTFS image is currently 1,048,576 bytes with SHA-256
`72683fa1250eeea772d3399277b434d4e55ba8dd0dc926e52d817e701fc2eb9e`.
Keep its licensing/provenance in `res/uefi/readme.txt`. The presence of signed
executables does not establish that a particular firmware's current Secure Boot
policy accepts them; that needs separate boot qualification.

## What is shared and what remains native

CP11a moves the two trailing-partition calculations into the existing
`src/partition_layout.c`. Windows calls those functions in its existing layout
builder; Linux uses them before asking libfdisk to serialize the table. The
original arithmetic and ordering remain intact, with zero returns for invalid
or overflowing geometry. No Windows disk structs, GUI handles or conditional
compilation enter the shared calculations.

The Linux writer accepts an optional payload byte size and reports both extents.
It can create the two-partition table on regular files and verifies names, types,
attributes, extents and both GPT copies. The table operation writes **only
partition metadata**; CP11c adds a separate checked file payload write. CP11d
allows the same table on retained block descriptors and discovers/checks both
kernel partition extents in the helper. The existing FAT32 request remains
single-partition; an explicit NTFS preparation request uses the new layout.

Continue using the existing shared ISO/UDF analysis, extraction and image policy.
CP11h shares filesystem selection; CP11i shares the extra-partition decision.
Callers supply explicit image facts/settings and retain their UI responsibilities.
Avoid extracting an unused general format-planning framework in advance.

## Native NTFS formatting (CP11b)

`src/format_policy.c` now shares the original NTFS allowed/default cluster rules
with Windows and Linux. The Linux adapter invokes native `mkntfs` on an existing,
partition-sized regular file: 512-byte sectors, explicit 4–64 KiB cluster size,
geometry and UTF-8 label. Its `partitionOffset` sets BPB metadata only; writes
start at byte zero. A whole-disk file must not be passed as a partition extent.
The formatter generates the volume serial number, which the adapter returns;
we do not rewrite NTFS metadata to invent a caller-selected ID.

FAT and NTFS reuse the same child supervision, retained descriptor, bounded
diagnostics, timeout, cancellation, reaping and flush logic. Only the child gets
`LC_ALL=C.UTF-8`; the caller's environment remains unchanged. Readback reuses
Rufus's bundled NTFS boot-sector definition and checks geometry, primary/backup
boot-sector equality and the initial MFT/MFTMirr headers. This is deliberately
not a full NTFS validator. Independent native tools check labels, version,
cluster size and clean flags in tests.

The offline USB VM also mounts a formatted 6 GiB volume with kernel NTFS3,
creates a 5 GiB + 512-byte file, remounts and verifies beginning/end data and
zero-filled regions. Normal and sanitizer-built formatter runs pass, including
post-shutdown metadata checks. This tests filesystem storage; it does not yet
qualify ISO extraction of a large WIM or firmware boot.

The local formatter is ntfsprogs-plus 1.0.0; Ubuntu CI is configured to exercise
the ntfs-3g provider when run. CP11d adds block-device preparation; mounting in
the authorized helper and GUI filesystem selection remain subsequent checkpoints.

## Bundled boot payload and firmware handoff (CP11c)

`WriteUefiNtfsImage` verifies the stored GPT against the expected Rufus layout,
then writes the retained bundled image at the reported boot-partition offset.
It reuses the raw writer's copy/flush/cancellation/byte-verification loop through
a bounded regular-file extent. GPT is checked again afterward. The original
Windows operation also copies only the payload bytes; unused track padding stays
untouched. The main filesystem, metadata and other gaps are preserved. Partial
writes are flushed and reported as failure without promising rollback.

The offline x64 OVMF fixture uses a 256 MiB disk with native NTFS on the main
partition and the unchanged bundled FAT image on the small partition. A guarded
Linux guest populates a generated EFI/Linux file tree. A separate VM then boots
only that USB through OVMF → UEFI:NTFS 2.8 → bundled NTFS driver 1.9 → the main
partition's EFI loader → Linux. The booted guest mounts NTFS read-only and checks
every fixture file's SHA-256. A missing required payload is correctly rejected.
Secure Boot is disabled in this qualification.

This establishes firmware handoff, not Windows Setup installation. The fixture
uses guest `cp` to populate NTFS; NTFS ISO extraction through the production
helper still needs qualification. CP11e adds resource packaging and device payload
writing below; GUI exposure remains pending. Windows source and bundled firmware
bytes are unchanged by CP11c.

## Native preparation in one authorized helper (CP11d)

The new `prepare-ntfs-device-v1` request creates the two-partition GPT, validates
both kernel partitions by number, offset, size, device identity and disk sequence,
and formats the main partition with native mkntfs. The requested cluster size
and Unicode label are explicit; the volume serial is generated by the formatter.
The helper derives the reserved boot size from the bundled image at CMake time.
CP11d reserved the boot partition; CP11e writes the payload below. CP11f extends
the request with the same retained ISO source used by FAT extraction.

NTFS keeps the exclusive whole-disk descriptor throughout formatting, cancellation
and verification. Its non-exclusive partition descriptor is passed through the
existing child runner. This differs from FAT's necessary release/reacquire:
mkntfs opens block devices without requesting a second exclusive claim. See
[the upstream device open](https://github.com/ntfsprogs-plus/ntfsprogs-plus/blob/main/libntfs/unix_io.c)
and [mkntfs's caller](https://github.com/ntfsprogs-plus/ntfsprogs-plus/blob/main/src/mkntfs.c).
No force flag is used for block devices. A formatter provider that cannot work
with the retained claim fails; the helper does not retry with weaker access.

Existing UDisks use checks and root-device identity checks remain. After the
child, the adapter verifies the NTFS boot/backup/MFT headers; the helper checks
the retained disk and saved GPT bytes. The same authorization covers partitioning,
formatting and cancellation. This preparation request is not exposed in Widgets
and does not claim a bootable Windows stick.

The device fixture starts with data sentinels. Before GUI exposure, also qualify
reformatting previously used media: mkntfs providers can reject existing foreign
filesystem signatures without force. Resolve that through native signature
handling under the retained claim, with preservation tests, rather than silently
retrying with force after a formatting failure.

## Embedded payload in the device helper (CP11e)

CMake's Qt resource compilation embeds the original `res/uefi/uefi-ntfs.img`
directly in the preparation helper. The request cannot supply a replacement
path or bytes. Before modifying GPT, the helper uses Qt's
[QTemporaryFile::createNativeFile](https://doc.qt.io/qt-6/qtemporaryfile.html#createNativeFile)
to provide the existing descriptor-based writer with a private native source.
A scoped Qt owner handles cleanup on every return, including cancellation.
The resource size must match the CMake plan. No custom resource reader or new
copy loop was added, and the bundled bytes/licensing are unchanged.

After NTFS formatting, a distinct `bootloader` phase writes, flushes and reads
back the payload. The Qt controller requires that phase before accepting a
successful NTFS preparation result. `WriteUefiNtfsImage` now accepts the already
authorized block descriptor, verifies the GPT before and after, and delegates
to the existing raw writer through `WriteRawImageToDeviceExtent`. That entry
checks device identity/capacity/read-only state, sector alignment and extent
bounds. Only payload bytes are copied; track padding remains untouched, matching
the original Windows operation. The whole-disk claim and authorization stay held.

CP11e leaves the main NTFS filesystem empty. That stage installs the firmware bridge;
it does not yet create Windows installation media. The existing file-based
firmware fixture qualifies the unchanged bridge with a generated EFI/Linux tree;
CP11f adds ordinary NTFS extraction below; a real Windows Setup boot remains a
separate checkpoint.

## Native extraction and provider choice (CP11f)

The requested provider policy is **ntfsprogs-plus plus the new kernel `ntfs`
driver**, with NTFS-3G available when kernel support is absent. This is distinct
from `ntfs3`, which was used for the earlier qualification. See the
[kernel NTFS documentation](https://docs.kernel.org/filesystems/ntfs.html) and
[NTFS-3G options](https://github.com/tuxera/ntfs-3g/wiki/Manual).
The new driver uses the old limited driver's `ntfs` filesystem name. Automatic
selection therefore requires kernel 7.1 or later and successful filesystem
context creation, before partitioning. Earlier kernels use the FUSE fallback;
backported new drivers on earlier version numbers are not automatically selected.
The fallback uses the same package's `lowntfs-3g` executable for `ignore_case`,
matching Windows filename lookup, and stays in the foreground under the helper.
Neither driver is forced onto a damaged or busy volume. File copying, flushing,
unmounting and cancellation continue to use the common extraction path.

The helper retains and validates the source before any writes, then uses the
existing GPT, formatter, boot-payload writer and ISO extractor. NTFS preflight
uses its own capacity without FAT32's per-file limit. Mounting releases the
whole-disk claim so the filesystem driver can claim the validated partition,
as in the existing FAT path. The temporary mount remains in the helper's private
namespace with `nodev,nosuid,noexec`; cleanup flushes files, unmounts, reaps any
FUSE process and reacquires/revalidates the whole disk and GPT. The Qt controller
requires every stage through `finalizing` before reporting success. No extra
authorization, custom NTFS writer or second extraction loop was introduced.

Performance follow-up from CP11g: the fallback currently does not request
`big_writes`. The packaged manual says this avoids FUSE splitting writes into
4 KiB requests. Upstream `lowntfs-3g` requires that option and a volume of at
least 4 GiB before requesting `FUSE_CAP_BIG_WRITES`; see
[its initialization](https://github.com/tuxera/ntfs-3g/blob/edge/src/lowntfs-3g.c#L581-L600).
Qualify the option with the selected package and compare the same generated
large-file workload, including the small-volume control. Do not infer native
hardware throughput from these software-emulated guests.

## Shared filesystem selection (CP11h)

`src/format_policy.c` now owns Rufus's eligibility, FAT32 compatibility and image
preference rules. The Win32 wrappers collect settings, handle no-device/no-image
selection, display the original forced-FAT warning and update the combobox.
Filesystem/boot/target enums move unchanged into the existing shared header.
At CP11h the Linux FAT32 preflight uses the shared compatibility check with dual
mode disabled and still rejects Windows media before any writes. CP11j extends
this to the ordinary Windows installer path below.

Eligibility and preference remain distinct: the caller intersects eligibility
with actual drive/formatter capabilities, supplies the preference mask and
keeps its existing/default selection when no applicable preference is offered.
No GUI handles, logging callback, Windows API or new planning framework enters
the shared functions. The normal large-WIM Windows image selects NTFS; the
dual-mode single-large-WIM compatibility exception remains shared policy, not
an implementation of WIM splitting on Linux.

For behavioral parity, Win32 retains its original `fs_mask` seed expression.
That expression uses enum ordinals rather than shifted filesystem bits and can
produce a FAT preference without a corresponding dropdown row. The wrapper
still selects only an offered row. The shared function adds no seed bits, and
tests characterize both inputs. Any correction to the Windows seed is a
separate behavior change requiring review and real Windows validation.

## Shared extra-partition decision (CP11i)

`GetExtraPartitions` receives the image report and explicit format settings;
the Windows caller still resolves the selected Windows To Go option and reads
the old-BIOS checkbox. The original `XP_*` flag values move from `drive.h` to
the existing shared `format_policy.h`. Native layout and locking remain with
their existing callers. No new backend abstraction or conditional compilation
is needed.

The distinction between ordinary installation media and Windows To Go remains
exact. The BIOS-only installer exception checks `HAS_BOOTMGR_BIOS`, not every
Windows marker. Persistence retains its image/bootloader and DD restrictions,
and compatibility remains an additive flag. The original code can still
request UEFI:NTFS when `write_as_image` is true; its caller subsequently follows
the whole-image path without creating those partitions. Preserve that calling
contract when wiring Linux selection to the common rules.

Portable tests and comparison with the original decision pass; these flags do
not expose unimplemented Windows To Go, BIOS or persistence paths in Linux.
CP11j applies these rules to Windows ISO preflight and Widgets below.

## Windows ISO preflight and Widgets integration (CP11j)

The form now offers FAT32 and NTFS according to shared image eligibility,
intersected with native formatter support for the actual planned partition.
It uses Rufus's filesystem preference and allowed/default cluster sizes.
NTFS includes the trailing UEFI:NTFS payload in that calculation. Shared label
normalization preserves NTFS case and Unicode with the original 32-UTF-16-unit
limit; FAT32 retains its original normalization and 11-character limit. The
native NTFS formatter chooses the volume serial number. Confirmation shows the
actual filesystem, normalized label and cluster size.

`CheckIsoWriteImage` rereads Windows install metadata through the existing
upstream-wimlib reader extension on the retained selected ISO/UDF view. It
requires known Windows 10/11 installer metadata and an EFI boot manager, then
checks the regular x64 fallback application and supported extraction tree.
Unknown, unreadable and older Windows metadata remain unsupported. Eligibility
and extra-partition flags come from the shared Rufus functions; this backend
supports no extra partition on FAT32 and UEFI:NTFS on NTFS. NTFS removes the
FAT32 per-file limit while retaining the main partition's capacity check.

ISO remains recommended/default when offered, with explicit DD available under
the existing shared mode rules. Choosing NTFS removes ISO → ESP as Rufus's
policy specifies. Filesystem changes reset cluster defaults and invalidate an
old write plan; controls stay disabled during creation. The existing helper
does partitioning, formatting, boot payload, extraction and cleanup under one
authorization. The controller requires the reported copied-byte count and main
partition extent to match the accepted preflight before reporting success.

Generated Windows metadata WIMs in ISO9660/UDF fixtures cover both filesystems,
metadata refusal and cancellation. Widgets tests cover native NTFS settings,
the large-WIM classification, ISO/DD choices, one helper launch and completion
validation without opening a real device. These WIMs contain no Windows system
files, and their EFI headers are structural fixtures. CP11g provides actual
large-file/provider readback coverage; a real Windows Setup installation remains
the next qualification, not a result claimed by these tests.

The user-supplied `/home/marius/Downloads/Win11_25H2_English_x64_v2.iso`
(8,471,603,200 bytes) also passes read-only preflight in both normal and sanitizer
builds. A 16 GiB target with 4 KiB NTFS clusters plans UEFI:NTFS and checks
976 files totaling 8,465,957,864 bytes in its UDF tree, including the existing
x64 fallback loader. The two builds return identical results. A valid FAT32
request is refused by the shared compatibility check. No target was opened or
written. Use this same ISO for the next creation and installation checkpoint.

The user subsequently installed the missing VM tools. Host KVM, QEMU x64,
`qemu-img`, `swtpm` and x64 OVMF are available for CP11k. Use a private virtual
TPM and Secure Boot-capable firmware for the ordinary installer, with enforcement
disabled for this baseline. Secure Boot qualification remains separate.

## Real Windows 11 creation and installed-system boot (CP11k)

The supplied 25H2 ISO now passes the actual Widgets creation path under one
uncached native authorization, using ntfsprogs-plus and the new kernel `ntfs`
driver. Independent host 7-Zip UDF extraction and a separate NTFS3 readback match
all 976 files and 94 directories, including the 7,578,075,168-byte install WIM.
The GPT layout, clean NTFS metadata, unchanged bundled UEFI:NTFS payload and its
padding pass independent checks too. No production changes were needed in this
qualification checkpoint.

The verified simulated USB boots Setup through the original UEFI:NTFS loader.
Windows 11 Pro installs onto an empty 80 GiB VM disk, then boots through its own
UEFI boot manager with the USB detached and reaches the first-use region screen.
The installed OS reports `C:\WINDOWS`, build **26200.8037** and **25H2**. This is
an installation/boot qualification; account setup, activation and desktop use
are not claimed. Windows User Experience options remain a separate next step.

Use KVM, a private TPM 2.0 and Secure Boot-capable OVMF, with enforcement disabled
for this baseline. Use a writable overlay over the verified USB backing image:
Windows PE refuses the hardware-write-protected fixture despite a clean CHKDSK.
The ordinary OVMF CODE build also lacks the capability Windows 11 requires; use
the `.secboot` build with SMM. Neither correction changes the installer files or
bypasses requirements. See the CP11k testing record and
[VM recipe](../linux/tests/vm/README.md#real-windows-11-creation-and-observed-installation)
for exact inputs, layout, hashes and retained evidence. Secure Boot enforcement
and physical USB qualification remain separate.

## Shared Windows answer-file generation (CP11l)

`src/unattend.c` contains Rufus's original XML generation, using explicit inputs
from `RUFUS_UNATTEND_OPTIONS` in `src/unattend.h`. The shared writer borrows a
stream and the caller's strings, preserves username normalization and returns
the removable WinPE section offsets. It retains the original flags, section
conditions, command ordering and XML text. It reuses `StrArray` and the existing
logging bridge; no Qt types, Windows API emulation or XML framework are needed.

Windows's `CreateUnattendXml` remains the caller in `src/wue.c`. It creates the
temporary file, gathers Windows locale/timezone and image-language inputs,
copies static-buffer API results, invokes the writer and checks close failure.
The existing Windows application of the answer file and offline registry/WIM
work remain in place. Both existing Windows build manifests include the shared
source; Linux builds it through CMake. Failed allocation, stream positioning,
writing or closing must not leave an answer file reported as successful.

The Linux component tests pass normally and with ASan/UBSan/leak detection:
176 checks plus 28 allocation and two offset failure cases. A separate comparison
using the pre-extraction caller and simulated host APIs matches XML bytes,
removal offsets and username normalization in 65,536 cases. This establishes
generation parity; it is not native Windows application runtime validation or
an installed-Windows customization test. Native application and GUI integration
follow in CP11m/CP11n below.

**Scope decision (2026-09-17):** keep the option to copy the current Windows
user's regional settings visible but disabled on Linux, explaining that it
requires settings from a Windows host. Do not substitute Linux locale/timezone
values. All other Windows ISO customizations remain in scope, including local
accounts, hardware/online-account requirements, privacy/QoL, BitLocker, silent
installation, S Mode and bootloader/policy choices. Native WIM and offline
registry adapters should supply the host operations these need. Preserve
Rufus's image/version eligibility and option dependencies; parity does not mean
offering an option where the original image cannot use it.

## Native OEM answer-file application (CP11m)

The first Linux application path covers online-account requirement removal,
local-account creation, privacy, automatic BitLocker encryption and QoL choices.
These use Rufus's existing non-WindowsPE route: copy the shared answer file to
`sources/$OEM$/$$/Panther/unattend.xml`, which Windows Setup carries into the
installed system. Do not replace this with a root `Autounattend.xml`: Rufus's
source explicitly distinguishes how later setup passes find these files.

`GetWindowsUnattendFlags` now shares the original installation-dialog eligibility
with Windows and Linux, including Windows version/build, existing Panther files
and expert choices. The Windows caller retains its UI/order/default decisions.
The current Linux application mask is narrower than the shared eligibility;
unimplemented choices are rejected rather than silently ignored. All other
Windows ISO options remain planned, with regional-settings copying the only
deliberate permanent exception.

The helper protocol accepts option flags and a bounded UTF-8 username, never
XML text or an output path. Before partitioning, the helper repeats the image
preflight using the retained source identity and the same shared eligibility.
After extraction it creates the OEM answer file in its private case-insensitive
FAT/NTFS mount, rejects existing files and symlink components, and attempts to
remove partial output on write/close failure or cancellation. Existing OEM content
is preserved. Final flush/unmount remains in the same authorized job, including
on failure; no further authorization or host command is introduced.

Functional tests cover generated ISO9660/UDF extraction followed by answer-file
application on ordinary temporary directories, XML validity, preservation,
refusals and injected failures. Protocol tests check one simulated helper launch,
customization progress and cleanup after extraction/customization failure;
success with an omitted customization phase is rejected. The seven affected
CTest suites pass normally and with ASan/UBSan/leak detection. This checkpoint
does not yet connect the Qt choices or qualify actual Windows Setup behavior.

## Widgets Windows User Experience dialog (CP11n)

After ISO preflight, eligible Windows images open the Qt Widgets dialog before
destructive confirmation and authorization. It uses the shared version/build and
existing-Panther rules. The five OEM options are selectable; other eligible
options are visible but disabled with a pending-support explanation. Regional
copying has a separate tooltip explaining that it requires a Windows host.
Non-expert mode does not expose S Mode, matching the original default UI.

Option order, English wording and applicable tooltips follow `src/rufus.c` and
`res/loc/rufus.loc`. The username field starts with the current local username,
remains editable when unchecked, and uses Rufus's trimming/replacement rules.
Invalid Unicode, control characters and names beyond the helper's UTF-8 bound
disable OK. The original writer still handles reserved/empty account names.
The accepted selection mask is saved through QSettings, preserving upstream's
default-mask encoding and defaults for choices not offered yet. Cancelling the
dialog does not save its edits or start authorization.

At CP11n, the disabled silent-install row was temporarily visible rather than
hidden behind Win32's username/region/privacy prerequisites. CP11aa below restores
that prerequisite-driven visibility using explicit regional inputs, while keeping
the Windows-host regional-copy checkbox disabled. This earlier checkpoint did
not accept silent-install requests.

The controller validates selections, passes flags/username to the existing helper
request and keeps the original source/device checks. Device removal closes the
dialog. The helper retains its independent validation and one-authorization
lifecycle. Dialog, controller and layout tests cover the new path. The VM runner's
`--windows-privacy` option selects only the privacy choice and independently
checks the added OEM answer file as well as every original Windows file.

The first VM run caught a Linux preflight bug: the repeated image check used
`getuid()` inside the root helper, rejecting the ordinary user's ISO with EACCES.
`CheckIsoWriteImage` now requires an explicit caller UID. GUI/probe callers pass
their own UID; the authorized helper passes its validated `PKEXEC_UID`. Image
ownership/readability and retained-identity checks are preserved. A regression
test verifies rejection for a different caller and acceptance for the owner.

The real Windows 11 25H2 ISO now passes the Widgets/helper USB simulator with
only **Skip privacy questions** selected. Creation uses ntfsprogs-plus and the
new kernel `ntfs` driver, with one uncached authorization. Independent NTFS3
readback matches all 976 original files and the expected 551-byte OEM answer
file. GPT, NTFS metadata, UEFI:NTFS payload and the unchanged whole-disk checksum
also pass. The completed test disks were removed after exporting the evidence.

This is the single customization qualification requested by the user. The
privacy setting's effect inside installed Windows remains unverified and
deferred; no further customization VM/installation tests are planned unless
requested. The detailed attempts and cleanup are recorded under CP11n in
`testing-and-checkpoints.md`.

## Native Setup WIM updates (CP11o)

`linux/platform/windows_wim.c` supplies two small C entry points using the
already selected upstream wimlib 1.14.5 build. `OpenWindowsSetupWim` opens the
extracted file with write access and selects image 2, falling back to image 1
as `ApplyWindowsCustomization` does. It rejects non-regular files, symlinks and
hardlinks; the caller must keep the helper's private target path stable.
`UpdateWindowsSetupWim` accepts the native `wimlib_update_command` array and
calls the existing update/overwrite APIs. No new command representation, WIM
format code, mount helper, library patch or Windows-source change is needed.

The write policy keeps Rufus's `RECOMPRESS` and automatic compression thread
selection. Linux also requests `FSYNC` so a flush failure is reported before
this stage succeeds. An empty command array avoids an unnecessary rewrite.
Upstream chooses append or temporary-file rebuild and owns its error recovery;
the adapter does not promise rollback after storage failure or power loss.
The existing job cancellation query is connected to the library's progress
callback. A rename notification is already past commit, so cancellation there
does not falsely report that the committed operation was aborted. The caller
must free the handle after the update, including on failure, and retain its
normal final flush/unmount lifecycle.

The helper now initializes wimlib with `ASSUME_UTF8` and
`DEFAULT_CASE_INSENSITIVE` before any other library call (CP11q). This matches
Windows path lookup without changing the existing read-only ISO reader extension.

The component suite passes 15 Qt checks normally and with ASan/UBSan/leak
detection. Generated WIMs contain ordinary text files only. Tests cover image
selection, additions/replacement with case-insensitive lookup, preservation of
the other image and boot index, cancellation during capture/write/metadata,
post-commit cancellation, missing input, flush failure, invalid/read-only WIMs,
unsafe target types and no-op preservation. Readback and wimlib's full data
verification pass. Fixtures are automatically removed. There is no new Windows
customization or installation qualification, and no newly enabled GUI option.

## Native offline SYSTEM hive editing (CP11p)

`PrepareWindowsSetupHive` in `linux/platform/windows_registry.c` uses the
[Hivex C API](https://libguestfs.org/hivex.3.html) on the extracted SYSTEM hive.
It retains Rufus's shared `bypass_name` list and writes the same three
little-endian DWORD values under `Setup/LabConfig`. The existing Setup key is
required, as with Windows's `RegOpenKeyEx`; LabConfig is created only when absent.
Hivex handles case-insensitive names and single-value replacement, preserving
other values and child keys. No Windows registry API emulation or custom hive
format writer is introduced.

The input is opened as a regular file through a retained descriptor and loaded
into Hivex's editable memory copy. A separate, exclusively created output is
committed through its descriptor, flushed and closed. This avoids truncating the
original on a failed commit and gives the caller an unchanged hive for Rufus's
existing answer-file fallback. The paths must stay in the helper's private
workspace. An existing destination is never replaced. Cancellation or an error
removes this operation's output, logging any cleanup failure. Hivex commit cannot
be interrupted internally; cancellation is checked around it, before any future
WIM update. No mounting or additional authorization is involved.

CMake finds the distribution Hivex development library via pkg-config; the
Linux CI dependency list includes `libhivex-dev`. Local validation uses the
unmodified Arch Hivex 1.3.24 library, extracted into the development dependency
directory, with package SHA-256 verified against repository metadata. No system
package was installed and no Autotools build was added.

The component suite passes 17 checks normally and with ASan/UBSan/leak detection.
It builds synthetic keys in copies of Hivex's 8 KiB minimal fixture, with source
provenance and license retained under `linux/tests/fixtures/hivex`. The separate
upstream wimlib registry reader validates the output and checks its values.
Tests cover preservation, name matching, output refusal, malformed input,
partial commits, flush/close failures, cancellation and descriptor cleanup.
Temporary hives are deleted automatically. The distribution Hivex binary itself
is not sanitizer-instrumented; our adapter/test and wimlib reader are.
CP11q connects this adapter to the helper as described below. No Windows ISO/VM
or additional customization qualification was run for CP11p.

## Hardware-requirements option in the helper (CP11q)

**Implemented and verified locally on 2026-09-18.** A host kernel crash interrupted
the initial build; fresh builds and focused checks passed after the host restart.

The Qt dialog now enables the original hardware-requirements checkbox, including
its default selection and remembered state. `ApplyWindowsCustomization` uses
the helper's independently analyzed architecture/build, after extraction and
inside the existing authorization and final flush/unmount lifecycle.

The native implementation follows `src/wue.c:ApplyWindowsCustomization`:

- Back up `sources/appraiserres.dll` as `appraiserres.bak` and create the empty
  placeholder, even when the original is absent. An existing ordinary appraiser
  backup is replaced, matching Rufus.
- For builds 26000 and later, inspect `setup.exe` with Rufus's shared PE reader,
  rename it to `setup.dll` without overwriting an existing backup, and use the
  unchanged x64 or ARM64 binary from `res/setup`. CMake embeds those same files
  as Qt resources. An unsupported architecture retains Rufus's warning/skip
  behavior; Linux does not execute these binaries.
- Open `sources/boot.wim`, select image 2 (otherwise 1), and extract SYSTEM
  using upstream wimlib. Case-insensitive lookup retains the actual filename.
  Use Hivex to prepare the three original Setup/LabConfig DWORDs in a separate
  private file, then replace SYSTEM in the selected WIM image.
- On registry success, use the existing shared answer-file writer and token
  replacement to disable its WindowsPE pass, leaving the original commands
  inside that disabled pass, and place the answer in the OEM Panther directory.
  If extraction or registry editing fails, retain the original WindowsPE commands
  and add the answer at `Autounattend.xml` inside the selected WIM image instead.
  The other selected account/privacy/BitLocker/QoL settings follow that answer.

Cancellation never turns into fallback success. WIM-open/update failures stop
the job. The extracted installer remains a private, stable tree; native file
operations reject symlinks/non-regular files, and WIM replacement retains its
checked parent directory through commit. Existing OEM answers remain untouched.
Temporary hives and answers are removed on return; the enclosing helper always
flushes/unmounts, including after errors.

**Documented divergence:** Win32 logs and continues after some appraiser or
setup-wrapper I/O failures. Linux fails the job instead, because reporting such
media as ready could silently omit the selected in-place-upgrade customization.
A failed wrapper write restores its original executable when possible and logs
any restoration error. This is not a transaction across the complete media:
earlier backups/placeholders may remain, and a storage failure during WIM commit
does not promise rollback. A failed creation must be restarted from the normal
format/extract workflow. No second authorization is introduced.

**Validation:** all 26 Setup component checks pass normally and with
ASan/UBSan/leak detection. Synthetic SYSTEM hives and tiny WIMs exercise the
native registry and answer-file fallback paths, both Setup-image selections,
case-insensitive lookup, append/rebuild commits, unchanged wrapper payloads,
backup rules, write failures and cancellation. Independent hive readback and
upstream WIM verification pass. The affected OEM, preflight, dialog and helper
regressions also pass: 10 suites, 218 Qt checks plus the disposable-file helper
tests in each build. Temporary fixtures are removed. The distribution Hivex
binary itself is not sanitizer-instrumented. No additional Windows installation
test was run; actual execution of this customization remains deferred.

## CA 2023 bootloader option in the helper (CP11r)

**Implemented and verified locally on 2026-09-18.** All 10 affected suites pass
normally and with ASan/UBSan/leak detection: 248 Qt checks, including 27 added
Setup checks, plus the disposable-file helper tests.

The Linux Windows User Experience dialog enables Rufus's original CA 2023
checkbox for eligible Windows 11 builds (26200 and later), retaining its text,
default-off state, compatibility tooltip and remembered selection. The helper
applies it within the existing authorization after ISO extraction. No Windows
source or build change is needed for this checkpoint.

`windows_bootloaders.cpp` borrows the selected Setup WIM image, using the same
image-2/otherwise-image-1 rule as the hardware option. Native wimlib reads only
the needed payloads with case-insensitive lookup and their actual filenames:

- Copy the immediate files from `Windows/Boot/Fonts_EX` into the existing
  `efi/microsoft/boot/fonts` directory, removing uppercase `_EX` from each
  destination basename as Rufus does. Subdirectories are not copied. An empty
  font set needs no destination directory.
- Replace the first existing `efi/boot/boot<arch>.efi` in the original shared
  `efi_archname` order with `Windows/Boot/EFI_EX/bootmgfw_EX.efi`. Do not create
  a fallback loader if none exists; that unused source file is then unnecessary.
- Copy `Windows/Boot/EFI_EX/bootmgr_EX.efi` to root `bootmgr.efi`, replacing or
  creating it. The same shared answer-file writer handles accompanying choices.

CA-only selection leaves `boot.wim` byte-for-byte unchanged and does not modify
the SYSTEM hive or in-place-upgrade wrappers. Combined with hardware bypass,
copy the CA files before preparing/committing the hive or answer-file fallback.
This finishes all payload reads before the native WIM commit consumes the handle
and avoids committing a WIM change after a failed CA copy. Both hardware paths
remain supported. No second helper or authorization is introduced.

**Documented divergence:** Win32 can log extraction/copy errors and continue
after omitting some or all of the selected CA option. Linux fails creation on
those errors, so media with incomplete selected bootloader changes is not
reported as ready. Destination directories are retained by descriptor and
checked without following symlinks; non-regular files and destination hardlinks
are refused. WIM reparse/encrypted/device entries and unsafe mapped basenames
are also refused. Qt `QSaveFile` owns each temporary output and atomic rename,
with direct-write fallback disabled, bounded copying and checked `fsync`.
Write/cancellation/synchronization failures preserve the current destination.
Earlier successful replacements may remain if a later step fails: the complete
operation is not transactional. The existing helper still flushes/unmounts on
every exit, and failed creation must restart through normal format/extract.

The payloads are taken from the user's ISO; Linux does not download substitutes
or claim to validate their signatures or the target firmware's certificate
store. The original compatibility warning remains necessary. Focused tests use
small synthetic WIM payloads, not executable EFI firmware or another Windows
customization qualification.

Coverage includes both Setup-image selections, mixed-case source names,
nonrecursive font copying and `_EX` removal, first-existing architecture order,
an absent fallback loader, creating root `bootmgr.efi`, empty/missing font
directories, malformed/missing/link payloads, unsafe destinations, short/partial
writes, cancellation and synchronization failure. CA-only WIM bytes and upgrade
files remain unchanged; combined registry and answer-file fallback changes are
read back independently. GUI tests use an explicit build-26200 metadata fixture
for CA choices and retain the older build-26100 fixture for existing cases.
Temporary files are removed automatically; no further Windows/VM qualification
was run. Distribution Qt and Hivex binaries are not sanitizer-instrumented.

## SkuSiPolicy option and independent first-logon choices (CP11s)

**Implemented and verified locally on 2026-09-18.** All 12 affected suites pass
normally and with ASan/UBSan/leak detection: 255 Qt checks, 220 shared answer-file
assertions, answer-writer failure injection and the disposable-file helper tests.

The Linux dialog enables Rufus's original SkuSiPolicy checkbox for eligible
Windows 11 builds (26200 and later), retaining its default-off state, text,
compatibility warning and remembered selection. The implementation reuses the
shared answer-file writer and existing helper; it adds no native policy installer.

The answer contains Rufus's original first-logon command: mount the installed
Windows system's EFI partition at `S:`, copy
`%WINDIR%\system32\SecureBootUpdates\SkuSiPolicy.p7b` into
`S:\EFI\Microsoft\Boot`, then remove that mount point. The command runs later
on Windows. Linux neither executes it nor substitutes a policy from the host,
a download or another Windows release. Policy-only creation writes the OEM
answer file and leaves `boot.wim` untouched. Hardware-bypass combinations keep
the policy command in either the OEM answer or the Setup-image answer fallback;
CA 2023 file replacement remains an independent choice.

**Shared bug correction:** the original `UNATTEND_OOBE_SHELL_SETUP_MASK` omitted
SkuSiPolicy and QoL, although their first-logon commands were inside that section.
Selecting either alone therefore omitted those commands; unrelated account,
privacy, regional or silent-install selections happened to enable the section.
CP11s adds those two flags to the shared mask. This is an explicit correctness
fix for both platforms, justified by honoring the choices actually selected.
It does not enable privacy/account/locale choices on the user's behalf, change
the command payloads or introduce Linux conditionals. The prior extraction test
that deliberately preserved the original QoL omission is replaced by regressions
for independent policy, independent QoL and the two together. The policy-only
regression was observed failing before the correction.

Focused tests parse the policy-only XML, check the exact decoded command and
single first-logon section, exercise ISO9660/UDF extraction, verify combined
WIM/registry paths, and check GUI/helper communication and persistence. They do
not execute the command on Windows or validate a resulting firmware policy;
the single agreed customization qualification remains CP11n.

All temporary fixtures are removed. The shared generator has been exercised on
Linux; validation of the Win32 application and execution of these commands on
Windows remain deferred. Distribution Qt and Hivex binaries are not themselves
sanitizer-instrumented.

## Silent-installation inputs (CP11t)

**Implemented and verified locally on 2026-09-18.**
`linux/platform/windows_install_options.cpp` reads edition and Setup-language
metadata with upstream wimlib's public property API. It reuses the existing
ISO-file reader extension and adds no alternate XML parser, WIM copy or Windows
compatibility layer. The caller supplies the selected install archive and the
already selected ISO9660 or UDF view. The view remains borrowed and serialized;
returned Qt strings own their data after the handles close.

The adapter preserves these rules from `GetEditions` and `GetUILanguage` in
`src/wue.c`:

- Keep native one-based image indexes, independent of duplicate display labels.
  Use `DISPLAYNAME`, then `DESCRIPTION`, then `Unknown Windows Version`;
  `NAME` is not a display-label fallback. Report nonstandard names.
- Read the first `LANGUAGE` in image 2 of `/sources/boot.wim`, regardless of the
  WIM's boot index or `DEFAULT` language. Missing/unreadable metadata retains
  `en-US`, with fallback provenance and a separate library error when available.
  This includes a differently cased boot path: the original bundled reader in
  `src/wimlib/wim.c` uses the same case-sensitive ISO/UDF lookup.
- Install-archive failure does not select a later candidate. Failure or
  cancellation clears partial results. No edition is automatically selected;
  Linux has no Windows host edition to match. No host locale is consulted.

**Documented divergence:** retain complete language tags. Win32's eight-byte
buffer truncates tags longer than seven bytes; for example, `zh-Hans-CN` would
be damaged. The native result keeps the full value so later validation can use
the actual image metadata. This reader does not itself generate XML or accept
an installation request.

All six focused suites pass normally and with ASan/UBSan/leak detection. The
new suite has 28 checks including setup/cleanup, using small generated ISO9660
and UDF images with WIM/ESD metadata. It covers the rules above, cancellation,
read failures, source ownership, unchanged ISO hashes and invalid inputs.
Fixtures are removed automatically; no Windows customization installation test
was added. Distribution Qt binaries are not sanitizer-instrumented.

**Integration status:** CP11w connects the metadata reader to preflight, CP11x
adds the edition display and regional editor, CP11y preserves the silent WinPE
pass alongside native registry changes, and CP11z connects helper validation and
application. CP11aa below enables the complete selection/confirmation flow for
supported single-archive images. Windows-host regional copying stays disabled.
Explicit binding for other archive layouts and S Mode remain later checkpoints.

## Silent-installation metadata in preflight (CP11w)

**Implemented and verified locally on 2026-09-18.** `CheckIsoWriteImage` now
reads optional edition/Setup-language metadata for images where Rufus's shared
policy offers silent installation. It uses the retained ISO/UDF view and first
install archive already validated by the Windows version scan. The result owns
the archive path and metadata; the existing serialized worker delivers them to
the GUI after its source handles close. No additional worker, callback layer,
source ISO copy or helper protocol field is needed.

This supplies choices, not a selected edition. Multiple-archive selection remains
future UI work; this checkpoint does not substitute a different archive when
the first read fails. An optional edition-read error is retained separately as
a wimlib error and logged. It cannot block ordinary media creation or existing
customizations such as privacy settings. Cancellation still aborts preflight,
and the retained source identity is checked after these reads so a changed ISO
cannot proceed. Missing Setup language keeps CP11t's documented fallback.

Focused ISO9660/UDF tests cover successful results, metadata errors, cancellation
and source changes during the edition read. The Widgets test verifies that owned
metadata reaches the controller through the worker. Silent installation remains
disabled pending its edition/regional UI, three acknowledgements and helper
validation/application. No Windows customization installation test was added.

## Edition display and explicit regional editor (CP11x)

**Implemented and verified locally on 2026-09-21.** The Windows User Experience
dialog now receives CP11w's edition metadata from the main window. Its dropdown
sits beside the silent-install checkbox, matching `src/stdlg.c`. Each item owns
its display label and native one-based WIM index; duplicate labels are allowed.
Nothing is selected automatically because Linux has no Windows host edition to
match. Missing metadata clears the list without disabling ordinary options.
At this checkpoint the dropdown and silent-install checkbox remained disabled;
CP11aa below enables them once the prerequisites and full creation path are ready.

`WindowsRegionDialog` is a separate, tested input editor for that future flow.
CP11aa now opens it from the creation flow. It collects display language,
optional fallback language, regional format, non-Unicode program language,
keyboard/input languages and time zone. Initial values are empty unless the
caller explicitly supplies them. Qt locale data supplies editable suggestions;
typed script/region tags are preserved rather than normalized or truncated.
Qt's Windows/IANA mapping supplies one choice per Windows time-zone ID, labelled
with its representative city. The full label is available as a tooltip. Storing
the Windows ID avoids passing an IANA name to the shared answer writer; removing
aliases also prevents reopening the editor with an apparently different zone.
See the [Qt time-zone mapping API](https://doc.qt.io/qt-6/qtimezone.html#ianaIdToWindowsId).

`CheckWindowsRegionOptions` is a small Qt Core input validator that can also be
used by the future helper path. It checks bounded language-tag/keyboard syntax,
rejects XML metacharacters and accepts only a known Windows time-zone ID. Input
languages accept tags for default keyboards or `LLLL:KKKKKKKK` layout IDs, with
semicolon-separated entries retained in their original order. Those are the
documented [Windows InputLocale forms](https://learn.microsoft.com/en-us/windows-hardware/customize/desktop/unattend/microsoft-windows-international-core-inputlocale).
An empty [fallback language](https://learn.microsoft.com/en-us/windows-hardware/customize/desktop/unattend/microsoft-windows-international-core-uilanguagefallback)
is permitted; the other fields require explicit values. The supported tag
syntax covers language-region and script/region tags, not arbitrary extensions.
Validation establishes syntax, not whether a particular language pack or layout
is installed in the selected Windows edition. The editor states that languages
must be available in that edition; Qt's suggestion catalogue is not a Windows
compatibility list. Missing time-zone data leaves acceptance disabled.

**Necessary Linux difference:** Win32 exposes silent installation only when
local-account, regional-copy and privacy choices are selected. Linux cannot
copy a Windows host's regional settings. Keep that original checkbox visibly
disabled, and use the explicit editor to meet the regional-input prerequisite
when the complete silent-install flow is connected. Do not infer these values
from Linux locale/time-zone settings or assume that the Setup UI language is
the installed edition's display language. The existing account/privacy
prerequisites and all three original acknowledgements still apply.

All seven focused suites pass normally and with sanitizers; the new editor is
tested at large font size and 2x scaling. Tests cover metadata ownership/indexes,
real worker-to-dialog delivery, explicit values, locale independence, invalid
inputs, keyboard layouts, full language tags, zone mapping/reopening and
cancellation. No helper protocol field, customization flag or answer-file
behavior changes in this checkpoint. Archive selection beyond the preflight's
first archive, original confirmations, helper revalidation and correct WinPE
application remain pending. No additional customization qualification was run.

## Native silent-install answer application (CP11y)

**Implemented and verified locally on 2026-09-21; application integration is
still pending.** The backend now accepts a fully prepared
`RUFUS_UNATTEND_OPTIONS`, the same structure as the original shared answer writer.
There is no second XML template or new Linux equivalent of that structure.
The caller retains the UTF-8 strings through the call and supplies a mutable
username buffer, as the writer requires. At this checkpoint, the GUI/helper
adapter still accepted only its prior choices and rejected silent-install or
regional-copy flags; CP11z below extends the checked helper path.

Before changing files, the new entry checks the positive edition index, required
account/privacy/regional choices, normalized nonempty account name, explicit
regional fields and Setup-language syntax. Silent installation cannot rely on
Setup asking the user when an account is missing: it therefore rejects names
the original writer would skip. `IsUnattendUserNameReserved` extracts the original
list and comparison into a shared C predicate; the original writer still skips
those same names and behaves as before. Native regional fields use CP11x's
validator. The internal `DUPLICATE_LOCALE` flag makes the shared writer emit
these explicit values; Linux does not read or copy host regional settings.

**Original behavior retained:** `src/wue.c` disables the WinPE pass only when
hardware bypass was its sole purpose. If silent-install work remains after a
successful registry edit, it removes the bypass-command byte range and injects
the remaining answer as `Autounattend.xml` in the Setup WIM. The native backend
now does the same. It generates the remaining answer with the bypass flag
cleared, rather than adding another byte-editing routine, and batches the edited
SYSTEM hive and answer in one upstream-wimlib commit. A focused test compares
that result byte for byte with the original generate-then-remove method.

Silent-only creation leaves the registry and upgrade files alone. Successful
combined registry work retains the partitioning, edition selection and regional
sections; it does not place an inactive answer in the OEM directory. Registry
failure retains Rufus's full answer-file fallback. Cancellation and answer/WIM
write failures fail the operation. Hardware-only behavior, CA 2023 copying,
policy choices and existing OEM placement remain covered by the regression
suite. WIM commit errors do not promise rollback after an actual partial disk
write; the enclosing creation helper still owns final sync and cleanup.

The new backend entry checks values, **not source identity, image eligibility,
whether the selected edition exists, language-pack availability or user
confirmation**. Those are caller prerequisites and must be enforced when the
GUI/helper flow is connected. The helper must re-read archive/edition metadata
from the retained source before writing and supply its own Setup-language result.
It must also honor archive identity: the original answer writer currently emits
an image index without an `InstallFrom/Path`. Multi-archive handling remains an
explicit open integration point, not permission to silently use a different WIM.

All ten affected suites pass normally and with ASan/UBSan/leak detection.
Generated WIMs exercise silent-only, combined native registry, fallback,
single-image and rebuild paths, plus cancellation and injected failures. Reopened
WIMs are verified and their answer/hive contents read independently. Invalid
inputs fail before file creation. These are component tests using small fixtures;
no Windows installation or additional customization qualification was run.

## Silent-install request and helper validation (CP11z)

**Implemented locally on 2026-09-21; CP11aa below enables the GUI flow.** The existing
`PrepareImageRequest` now owns the selected image-internal archive path, edition
index, explicit regional fields and a boolean recording acceptance of all three
original warnings. The bounded JSON protocol carries these in a `silent` object
under `windows`; it never accepts XML, destination filenames, host-regional
instructions, architecture, Windows build or a client-supplied Setup language.
All six regional strings must be present, including an explicitly empty optional
fallback. The existing 4 KiB frame limit still applies. Ordinary requests keep
their previous shape; an older helper rejects the extra silent field rather than
silently ignoring settings. Use matching app/helper builds when testing this path.

The helper checks values before opening the target. Silent installation needs
account/privacy/regional flags, a usable normalized account, positive edition,
explicit valid regional fields and an acknowledgement. The flag that the shared
writer calls `DUPLICATE_LOCALE` means only "emit these supplied fields" on this
path; no Windows- or Linux-host settings are read. Without the silent flag, stray
edition/regional/confirmation data is rejected. The controller clears these
values when preparing a new job so a previous selection cannot supply consent.

Before partition writes, `CheckIsoWriteImage` re-reads the retained source,
checks image eligibility and requires the selected archive path and native index
to match fresh metadata. A read error now fails a silent request, while remaining
optional for ordinary creation. Source changes and cancellation abort either
path. Setup language comes from the helper's own metadata read, preserving Rufus's
existing `en-US` fallback when unavailable. Malformed language syntax is rejected;
this still does not verify language-pack availability in the installed edition.
The UTF-8 adapter owns its buffers through the existing native backend call and
preserves the caller's account text while the shared writer sanitizes its copy.
One authorized helper still owns the full job through sync, cleanup and exit.

**Interim archive restriction:** `GetEditions` in Win32 can show multiple install
archives, but the shared answer writer emits `/IMAGE/INDEX` without an explicit
`InstallFrom/Path`. Selecting an index from another archive is not sufficient to
bind Setup to that archive. Silent requests therefore accept only a sole
conventional `/sources/install.wim` or `/sources/install.esd` (case-insensitive
conventional-path recognition, exact selected-path identity). Multiple and
nonstandard layouts remain available for ordinary creation. This restriction is
specific to the unfinished silent path; support for explicitly binding other
archive layouts remains planned. Split-WIM support is a later checkpoint.

At CP11z, the ordinary GUI flag mask was unchanged and silent installation stayed
disabled. CP11aa below connects the editor, edition selection and original three
checkbox acknowledgements. Windows-host regional copy remains disabled. The
helper can reject an absent acknowledgement; it cannot establish that a human
actually viewed a dialog and does not replace those UI steps.

All thirteen selected suites pass normally and with ASan/UBSan/leak detection.
Focused component tests exercise typed request round trips and rejection,
ISO9660/UDF source and selection checks, and native WIM answer readback. The
request adapter's output matches the shared writer's original post-processing,
including combined registry bypass; ordinary OEM answers remain identical.
No physical USB or additional Windows customization installation trial is used.

## Complete silent-install Qt flow (CP11aa)

**Implemented and verified locally on 2026-09-21.** For images accepted by the
existing single-archive backend, Windows User Experience now opens the explicit
regional editor from a button beside the disabled Windows-host copy choice.
The button explains why silent installation is unavailable when metadata or
archive layout is unsupported. Account and privacy choices plus accepted regional
values reveal the original silent-install checkbox and edition row, matching
Win32's prerequisite-driven visibility. The edition is never auto-selected;
items retain their native WIM indexes even when labels repeat. Empty/reserved
accounts and unusable surrounding Unicode whitespace prevent silent acceptance.
Ordinary account sanitization keeps Rufus's existing behavior.

The disabled copy checkbox remains unchecked and cannot be used to copy settings
from Linux. Accepting explicit regional values is the necessary platform difference.
Cancelling the editor preserves the preceding explicit values; it does not invent
host defaults. Regional values are included in the request only with silent mode.
The shared writer's `DUPLICATE_LOCALE` bit then means "emit these supplied fields".

Pressing OK with silent mode selected opens a dedicated Widgets warning using
exact English MSG_356 and MSG_372–374 from `res/loc/rufus.loc`. All three checkboxes
start unchecked; OK and programmatic acceptance are gated until all are checked.
Cancelling or closing this warning returns to Windows User Experience. Changing
an option, username or edition invalidates an open warning, and the next attempt
starts with three unchecked acknowledgements. Removing the selected device closes
the parent and either child dialog without starting a helper. Dialogs use Qt's
normal asynchronous `open()`/finished signals; no nested event loop is added.

Once accepted, the controller copies only Windows choices into its existing
source/device request. It validates the inputs, eligibility and selected metadata
before the ordinary USB-erasure confirmation, then repeats source preflight
before authorization. The helper still performs its own fresh validation before
partition writes. Exactly one helper/authorization covers the complete operation.
The completion/sync/cleanup/READY behavior is unchanged. A new creation job has
no edition, regional inputs or acknowledgement; silent mode is never persisted.
Existing ordinary choice persistence retains its prior mask.

Source eligibility and selection checks are shared by the dialog, controller and
helper. The single conventional install.wim/esd restriction from CP11z remains;
nonstandard or multiple archives are still available for ordinary creation. No
Win32 source, XML template or Windows build-file change was needed here. There
is no claim that region syntax checks establish language-pack availability or
that the Windows silent installation has been exercised end to end.

All thirteen selected suites pass normally and with ASan/UBSan/leak detection.
New coverage includes all eight acknowledgement combinations, cancellation,
changed choices, missing/invalid editions/accounts/metadata, native-index identity,
explicit regional values, device removal with either child open, and changed-source
rejection before authorization. The protocol-only child independently checks the
complete GUI-selected request; one launch reaches success, while the next job
requires fresh inputs. Existing generated-WIM application/readback tests remain
in the same focused run. Offscreen normal and large-font/2x screenshots were
inspected. No USB, Windows VM or additional customization installation trial ran.
Use matching rebuilt app/helper versions before a later physical test: the host
installation was not updated by this checkpoint.

## Expert-mode S Mode choice (CP11ab)

Ctrl+Alt+E toggles Expert Mode with Rufus's 3.5-second status message and saves
the preference. The next Windows User Experience dialog includes **Restrict
Windows to S-Mode (INCOMPATIBLE with online account bypass)** after the ordinary
choices, with the original MSG_367 tooltip. This uses the shared expert-mode
eligibility for Windows 10/11 and still suppresses customizations for an existing
Panther answer. The default dialog remains unchanged. The checkbox starts off
in a new application session; accepting it remembers it only for that session.
Turning expert mode off hides the choice without clearing its saved session
value. Cancel does not save edits. The controller rejects choices outside the
current offered set before authorization.

The backend accepts the existing `UNATTEND_FORCE_S_MODE` flag and independently
checks image/architecture eligibility; expert mode is a GUI preference rather
than a helper capability or new request field. The unchanged shared writer
emits `Microsoft-Windows-CodeIntegrity/SkuPolicyRequired=1` in `offlineServicing`.
The native adapter follows Rufus's existing placement rules: S Mode alone writes
the OEM Panther answer without opening `boot.wim`; remaining WinPE work uses
the Setup WIM answer route. No S Mode registry implementation, extra library,
helper launch, Windows build change or XML-template fork is introduced.

Rufus warns that S Mode and online-account bypass are incompatible, but its
dialog permits both to remain checked. Linux keeps the same warning and behavior
instead of changing the selected options implicitly. Output/component coverage
does not establish that every target Windows edition supports S Mode, or qualify
its effects after installation. Those Windows installation tests remain deferred
under the agreed single-customization qualification scope.

Ten focused suites pass normally and with ASan/UBSan/leak detection. Tests cover
the actual keyboard shortcut, normal/expert visibility, image refusal, session
versus restart preferences, original XML contents and OEM/Setup-WIM placement,
including the silent/native-registry combination. The protocol-only child checks
the S Mode request independently and observes one launch. Large-font/2x layout
checks and an inspected screenshot cover the extra row. The first keyboard test
failed because the offscreen file-picker teardown had not returned Qt's shortcut
context to the main window; it now waits for that context before sending keys.
No production shortcut workaround was required.

## Archive binding investigation (CP11ac)

Source inspection confirms that Win32's `GetEditions` (`src/wue.c`) offers the
archive picker and reads the selected archive. The shared `WriteUnattendXml`
then emits only `/IMAGE/INDEX`, without an `InstallFrom/Path`. Consequently, adding
the same picker to Qt alone would not establish which archive Setup applies.
The Linux request's `windowsInstallPath` currently verifies source identity; it
does not become a Windows runtime path.

Microsoft documents both local/UNC and relative
[OSImage InstallFrom paths](https://learn.microsoft.com/en-us/windows-hardware/customize/desktop/unattend/microsoft-windows-setup-imageinstall-osimage-installfrom-path).
The related [DataImage path examples](https://learn.microsoft.com/en-us/windows-hardware/customize/desktop/unattend/microsoft-windows-setup-imageinstall-dataimage-installfrom-path)
make the working-directory dependency explicit and show `%configsetroot%` with
`UseConfigurationSet`. That is a separate
[configuration-set workflow](https://learn.microsoft.com/en-us/windows-hardware/customize/desktop/unattend/microsoft-windows-setup-useconfigurationset),
not evidence that an ISO-relative path automatically resolves from our current
answer inside `boot.wim`.

**Decision:** keep multiple/nonstandard archives unavailable for silent creation
until their selected edition can be tied to a defined Setup source. Ordinary
creation remains available. The documentation establishes an explicit-path
mechanism; it does not by itself establish the correct path for this boot flow.
Do not guess a USB drive letter, rename installation archives, add a WinPE launcher
or change the shared XML just to make the picker appear functional. This remains
a parity gap, not a claim that arbitrary layouts cannot be supported. Return to
it when a concrete source/launch contract is ready; additional Windows
customization installation trials remain outside the current agreed scope.

## Native WIM split qualification (CP11ac)

The selected upstream wimlib 1.14.5 can split a non-solid WIM directly through our
existing borrowed ISO9660/UDF reader. No additional reader patch, temporary
whole-WIM copy, split algorithm or production wrapper is required for this
operation. Component fixtures preserve two editions, shared resources, UTF-8
edition metadata and every file byte across split parts. They cover uncompressed,
LZX and pipable WIMs; the complete input chain is released before output readback.
This is a library qualification, not an enabled FAT32 creation feature.

The intended call matches `WimSplitFile` in `src/vhd.c`: 4094 MiB and
`WIMLIB_WRITE_FLAG_FSYNC`. Small fixtures use smaller part targets to exercise
multiple parts without allocating a multi-gigabyte image. The same aggregate
resource counter used by Rufus's progress callback is checked for monotonicity;
each part is synced, and read/sync/cancellation errors remain failures.
The 4094 MiB argument is exercised with a small input, not a greater-than-4-GiB
boundary image. Large-input FAT32 qualification still belongs to integration.

The public upstream API and these tests confirm three integration requirements:

- A part can exceed the requested size because a resource cannot span parts.
  Enforce the real FAT32 file-size limit; the 4094 MiB request alone is insufficient.
- Solid archives cannot be split directly. Keep their NTFS route; any later
  conversion needs its own deliberate scope and space/progress accounting.
- Cancellation can leave complete or partial output parts. The one authorized
  helper must own output collision checks and cleanup through final sync/unmount.

The tests inject read failure, read cancellation, cancellation after part one,
and per-part fsync failure. They verify unchanged source-ISO hashes and remove
all generated files. No Windows installer was run and no user ISO was copied.
All six selected splitter/reader regression suites pass normally and with
ASan/UBSan/leak detection; the new split suite has 15 checks including Qt setup
and teardown. Upstream wimlib and the reader extension remain unchanged.

## Checked extraction splitting (CP11ad)

`ExtractIsoTree` and its read-only `CheckIsoTree` preflight now accept an explicit
split option. It requires exactly one conventional `/sources/install.wim`, matched
case-insensitively. A source `install.swm` or numbered `installN.swm` entry in that
directory is rejected before writes, including entries encountered after the WIM.
This prevents a later ordinary copy from replacing a generated part. Existing
creation callers explicitly leave the option false; the GUI FAT32 restriction
has not been relaxed.

The small Linux adapter opens the unchanged ISO/UDF reader through upstream
wimlib, rejects multipart/solid resources, and checks individually oversized
resources. These read-only checks do not reproduce the split algorithm or predict
all metadata/header overhead. It calls upstream `wimlib_split` with Rufus's
4094 MiB target and FSYNC flag, then checks every actual part against
`UINT32_MAX`. Oversized output fails; it never silently changes filesystem or
recompresses a solid archive.

Parts are staged in a newly created private directory on the destination
filesystem and published as `install.swm`, `install2.swm`, etc., using
`RENAME_NOREPLACE`. This requires no second copy or whole-WIM staging on the host.
Existing target files, links and directories are preserved. Failure removes the
adapter's staged and already published parts, even after cancellation; removal
or sync failures are reported and the whole target remains incomplete. The
caller must keep the destination private through cleanup. Prior ordinary
extracted files are not rolled back.

The original compressed-resource progress formula is retained. Failed splitting
also returns the last reported block count, preventing the extractor's forced
final update from moving backwards. Logical source totals remain separate from
actual SWM bytes/parts. Source identity is rechecked during splitting, publication
and completion. The existing one-job helper still owns eventual filesystem/device
sync and unmount; this component adds no privilege prompt.

All eight selected suites pass normally and with ASan/UBSan/leak detection. The
split suite has 41 checks, including six native ISO/UDF readbacks, cancellation,
source mutation, read/fsync failures, synthetic actual-size rejection, failed
publication after part one, existing-target preservation and extractor preflight.
Small generated archives are split with a linker-wrapped part target; the wrapper
asserts the production 4094 MiB/FSYNC arguments. The real 4094 MiB call remains
covered by the earlier small-input qualification. This is not a large-input or
mounted-FAT32 qualification. No Windows installation/customization test was added.

## Helper selection and space reservation (CP11ae)

The helper now selects splitting from a fresh `CheckIsoWriteImage` result before
any partition writes. The request carries `enhancedInstall`, the separate Win32
Alt+E preference; Ctrl+Alt+E expert mode continues to control options such as
S Mode. The existing shared `IsFat32Compatible`/filesystem rules decide whether
the single-large-WIM exception applies. Linux additionally requires a known
Windows 10/11 UEFI installer with one conventional `/sources/install.wim`, then
runs the native resource/tree/name checks. The request cannot supply a trusted
split decision. NTFS and small WIMs remain ordinary copies.

The strict protocol accepts this boolean only with a native source request and
omits it when false. Older default requests retain their shape. An old helper
rejects the added field rather than silently ignoring enhanced installation.
GUI requests still leave the preference false. No extra authorization is added;
the helper's existing cleanup and READY requirements remain in force.

For sizing, the adapter asks upstream `wimlib_split` to construct its plan and
aborts at the first BEGIN_PART notification, before any output open/write. The
callback returns the part count and compressed-resource total. No local split
algorithm, upstream patch or whole-payload scan is needed. Let `S` be the source
WIM bytes, `R` the planned resource bytes and `N` the part count. The reservation
is `S + N × max(S − R, 2 MiB)`, with invalid totals and overflow rejected. The
2 MiB minimum is derived from Rufus's 4094 MiB part target. Reserving the original
non-resource overhead for each part is deliberately conservative; original
padding and integrity data may increase it.

This is an enforced output budget, not an exact serialization or filesystem
allocation prediction. Actual SWM bytes must fit it; unexpected expansion fails
and cleans the current split instead of exceeding the accepted budget. FAT32's
per-file bound is checked independently. `ISO_EXTRACT_RESULT.required_bytes`
replaces the source WIM's logical size with its reservation while retaining
`bytes_written` as logical source progress. Ordinary copies keep the two totals
equal. Preflight compares the reservation with partition size. After mounting,
the extractor checks it against `f_bavail` using overflow-safe block arithmetic
before creating entries. Filesystem directory/cluster overhead, config expansion
and temporary customization space are not fully modeled by this change; runtime
I/O/ENOSPC checks remain essential.

Small fixtures verify read-only planning for all six existing ISO/UDF compression
cases, then complete split/readback after the planning abort. They also check
reservation overrun, unavailable/invalid free-space reports, enhanced-mode policy,
unchanged default/NTFS/small-WIM behavior, insufficient reserved space, and
ambiguous/nested/colliding archives. Policy tests inject the large-WIM scan flag
on small valid generated archives; they do not qualify a real 4 GiB boundary.
The next checkpoint should exercise a disposable FAT32 volume and the production
helper at that boundary before adding the GUI preference. Do not start another
Windows installation/customization trial or claim BIOS support from the Alt+E
name. Win32 source/build and upstream wimlib remain unchanged.

## Real FAT32 split qualification (CP11af)

The production helper successfully creates GPT/FAT32 media from the supplied
Windows 11 25H2 ISO on a disposable 16 GiB USB disk under KVM. The request sets
enhanced installation, retains zero customization flags, and uses one real
polkit authorization. This is a helper/process qualification; the GUI preference
has not been exposed yet. A read-only 9p share provides the original ISO without another
installation-sized copy. Source identity checks remain enabled.

The 7,578,075,168-byte install.wim produces `install.swm` (4,284,319,873 bytes) and
`install2.swm` (3,229,618,068 bytes). Both fit FAT32. The complete output's logical
file sizes total 8,401,820,637 bytes, below preflight's 8,570,150,808-byte reservation.
The helper reports the original 8,465,957,864 source bytes for progress, completes
sync and private unmount, and exits successfully before readback starts.

Independent readback write-protects the virtual disk and partition, checks FAT32
with `fsck.fat -n`, and mounts read-only. System wimlib 1.14.5, without the Rufus
reader extension or split adapter, decompresses and verifies all 11 images and
all resource data across the two parts. Image XML, 11 metadata hashes and 95,208
file-resource hashes/sizes match the original archive. The remaining 975 ISO
files retain their independent source SHA-256 hashes; directories also match.
Both GPT copies and guard bytes pass, and the whole-disk checksum is unchanged
by verification. One authorization is confirmed in the D-Bus log.

Logs, manifests, inventories and the authentication screenshot are retained;
the USB/system disks and generated boot files were removed. The original ISO's
SHA-256 is unchanged. This is media-creation/data-integrity qualification, not a
Windows Setup boot or SWM-consumption trial. CP11n remains the sole customization
qualification. Production, Win32 and upstream-library code were unchanged in
this checkpoint; the new code consists of reusable test probes and a VM recipe.

## Qt enhanced-installation preference (CP11ag)

Alt+E now toggles enhanced installation in the Qt frontend. Like Win32, it saves
`EnableWindowsDualUefiBiosMode`, displays a 3.5-second status message, and rebuilds
filesystem/cluster choices. It retains the selected filesystem when still allowed;
large-WIM Windows images continue to default to NTFS. Enabling the preference
allows FAT32 through the shared Rufus policy, then the user selects FAT32 explicitly.
Disabling it returns an incompatible FAT32 selection to NTFS. Cluster sizes return
to the selected filesystem's shared default. Fresh image selection retains the
ordinary shared image preference rather than inheriting the preceding image's
filesystem choice.

The selection controller owns the preference because it affects both the form
and the next request. It copies the value into the ISO request at START; the
retained source identity is unchanged. The existing ISO controller passes it
through fresh preflight, Windows options and confirmation to the existing helper.
No split instruction comes from the GUI; helper validation still decides.
Alt+E is ignored during analysis and all active creation states, including
preflight, options/confirmation, authorization, writing and cancellation.
The independent Ctrl+Alt+E expert preference and S Mode choices are unchanged.

Linux displays “Enhanced installation mode enabled (UEFI only)” rather than the
Win32 “Dual UEFI/BIOS mode” text. GPT/UEFI remains selected. This intentional
wording/target difference avoids promising the unimplemented BIOS/MBR install
path. It does not broaden the supported Windows versions or archive layouts.
Older/unknown Windows images and invalid split sources retain their existing
preflight restrictions.

Focused Widgets tests exercise the real shortcuts, saved preference, filesystem
selection, busy-state refusal, large/small WIM and FAT32/NTFS request paths, and
unsupported Windows refusal before authorization. Small generated images use
the existing test-only large-WIM scan flag; the child helper independently checks
the encoded preference, filesystem and zero customization flags. The child never
writes a target. CP11af remains the real large-WIM, mounted-FAT32 qualification;
these GUI tests do not repeat that workload or claim a Windows installation test.

## FAT32/SWM boot into Windows Setup (CP11ah)

The real Windows 11 ISO was recreated through the production helper with zero
customization flags, then independently verified as in CP11af. Both SWM parts,
all 11 images and all 975 ordinary files pass readback again. One authorization,
successful sync/private unmount and unchanged source ISO hash are confirmed.
The source stayed on its read-only share; no ISO or installation-tree copy was
created. Only the verified USB was retained for the immediate boot check.

`windows_setup.py --boot-only` attaches its disposable USB overlay, fresh OVMF
firmware and software TPM, with no installation disk or network. The firmware
loads the FAT32 USB directly, without UEFI:NTFS. Windows 11 Setup reaches language
selection and displays all 11 editions. WinPE reports `PEFirmwareType = 2`, sees
both expected SWM sizes on `C:`, and Microsoft's `dism /Get-WimInfo` completes
successfully through index 11. No installation was started. Edition discovery
does not prove that Setup can apply every resource across both parts; independent
system-wimlib verification covers their data integrity.

After `wpeutil shutdown`, the runner ended with status 143 before writing its
normal final checksum record; the cause is unknown. QEMU and TPM processes were
absent and a separate full SHA-256 check confirmed the backing USB was unchanged.
Small runner shutdown checks with a child process and a diskless QEMU/TPM pass.
Screenshots, exact actions, manifests, hashes and this limitation are retained.
The USB, overlay, guest system and firmware/TPM state were removed. Secure Boot
enforcement, BIOS and physical USB testing remain outside this checkpoint.

CP11ai adds supervisor exit records and SIGTERM cleanup, with ten small process
scenarios and normal/interrupted diskless QEMU/TPM checks. This closes the missing
interruption-handling and diagnostic paths; it does not establish the cause of
CP11ah's earlier exit. No Windows media or installation test was repeated.

## Next small checkpoints

CP11g passes complete ISO9660 and UDF extraction of a **4 GiB + 1 MiB + 123-byte**
file with both providers, including full SHA-256 readback and exact copied bytes.
The NTFS-3G runs use ASan/UBSan and KVM; the new-kernel-driver runs use the normal
build and TCG, with independent readback resumed after the initial outer deadline.
Each job requires exactly one authorization. Those fixtures contain generated
data; CP11k adds the real Windows creation and installation qualification above.

Continue native FAT32 split integration with focused component tests. The
edition/language reader is connected to preflight and the edition
display. CP11z connects source/edition validation to helper application, and
CP11aa completes the GUI selection/confirmation flow for supported images.
CP11ab adds the expert-mode S Mode choice through the existing shared writer.
Explicit archive binding for other layouts remains deferred as described above.
The single privacy media-creation test
above fulfills the agreed customization qualification scope. Additional
customization installation tests and Windows-version testing remain deferred,
as do Secure Boot enforcement, physical USB and Win32 application validation.

For NTFS installation media, WIM splitting is unnecessary. CP11ac qualifies the
native operation, CP11ad connects checked extraction splitting, and CP11ae adds
helper policy and space reservation. CP11af qualifies the real greater-than-4-GiB
WIM through the production helper on disposable FAT32, and CP11ag exposes the
separate enhanced-installation preference in Qt. CP11ah qualifies UEFI boot and
Windows Setup edition discovery from that FAT32/SWM media, with zero customization
flags and no installation. The split-archive application step remains deferred;
do not start another Windows installation or customization matrix. Keep the
existing shared reader and original progress behavior. CP11ai supplies process
exit records before cleanup and checks interrupt handling with small fixtures.
Continue the port's next native backend checkpoint without another
installation-sized fixture for this already observed boot path.


## Windows BIOS/dual-boot follow-up (CP22a)

The [BIOS/dual-boot reuse audit](linux-windows-bios-reuse.md) traces the original
Windows MBR/PBR dispatch and shared policy against the current Linux helper.
The original standard NTFS PBR and Rufus/Windows 7 MBR bytes are already reusable;
NTFS layout eligibility, frozen request, helper dispatch and Qt selection remain
staged follow-up work. This audit adds no enabled creation mode. FAT dual boot,
WinPE transformations/masquerading and Windows 7 fallback extraction remain
explicit parity work, separate from ordinary modern NTFS installer creation.
