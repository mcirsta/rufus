# Linux preview: supported media and limits

Current GUI through **CP78 (2026-10-24)**, including source-eligible Windows
FAT16/FAT32/NTFS BIOS/dual and UEFI creation, older/unknown BOOTMGR and PE,
ReactOS/Kolibri ISO routes and GRUB2 MBR/FAT16/FAT32/NTFS
ISO creation, required UEFI:NTFS for EFI-capable NTFS images, and standalone
GRUB2 FAT16/FAT32/NTFS/ext2/ext3. The implemented
routes below use the Qt Widgets application. Qualification examples use simulated
USB devices and do not establish compatibility with every image or physical PC.
ZIP creation is available for the first central-directory entry when its
decoded image has the required boot marker and fits the selected device.
The DD picker also accepts qualified Zstd, VTSI and fixed/dynamic VHD/VHDX images.
Full FFU images can also be deployed to the same or a larger disk; optimized
images can fit smaller media and grow their designated NTFS partition. V2 images
with multiple stores prompt for the store to write to the selected device.
Partial FFUs validate the selected store's declared base comparisons before
applying sparse updates to an existing disk. [CP56 closes FFU feature work](linux-ffu-parity.md).
CP32 supplies the first versioned Release package for Arch/CachyOS, a generic
install archive and working About/license information. See the
[package guide](../linux/packaging/README.md) and
[delivery finish lines](linux-release-plan.md).

CP21ad qualifies combined GRUB/NTFS + UEFI:NTFS native creation, readback,
BIOS Linux boot and x64 EFI application handoff with the kernel and NTFS-3G
providers. CP21ae enables that qualified combined choice in Qt through the
existing writer/helper, with shared filesystem/partition policy and progress.

## Preview47 release qualification

Reuse of the original Rufus translations is qualified across all 38 languages.
Linux-specific messages without an equivalent catalog entry remain English;
full new translations are a follow-up. See the [localization guide](linux-localization.md).

Native x86-64 packages are prepared for Arch/CachyOS, Debian 13 and Fedora 44,
with a separate AUR recipe building from the GitHub source tag. Debian/Fedora
fresh install, package-release upgrade and removal are qualified in isolated
containers; no historical published Linux package existed for an application-version
migration check. The generic archive requires compatible system libraries.
See [packaging](../linux/packaging/README.md) and [release evidence](linux-release-evidence.md).

No usable writable physical USB or physical 4Kn disk is available in this
environment. Physical write/unplug/reconnect, complete i686/aarch64 application
hosts and non-x64 Windows To Go device/firmware creation remain unqualified.
Existing simulated-device and boot results are retained with their exact scope.

## Filesystem metadata limitations

CP77 / Preview46 preserves Alt+Q's saved indexing choice on supported UDF jobs.
The Linux provider cannot write the Windows UDF indexing attribute: disabling
indexing logs an optional unsupported diagnostic and formatting continues. Enabling
indexing skips that disabling attempt. No indexer is started and no UDF flag is
invented. NTFS retains its actual root-attribute implementation. UDF 1.02–2.01
formatting, labels/icons and ZIP overlays remain supported.

The user deferred UDF 2.50/2.60, ReFS formatting and Dev Drive recognition.
Existing native target protections remain, but they do not identify Windows Dev
Drives. See the [metadata policy](linux-filesystem-metadata-policy.md).

## Sector and filesystem geometry

CP76 / Preview45 adds checked 512/512e/4Kn formatting through discovery, accepted
requests, MBR/GPT/SFD, all seven native formatters and independent readback. FAT32
includes 64 KiB clusters. UDF selects logical blocks from the actual device and
checks its exact provider maximum; a 512e device retains 512-byte UDF blocks.
Native BIOS/Syslinux/SBR and bundled UEFI:NTFS routes retain explicit 512-byte
limits before writes. See the [geometry guide](linux-sector-geometry.md).

## Legacy Windows, ReactOS and Kolibri ISO routes

CP75 / Preview44 follows the original primary/secondary dispatch for Windows
FAT16, older and unknown BOOTMGR/PE, and mixed legacy setup sources. ReactOS ISO
creation uses source-selected Syslinux and original mboot/config ordering.
Kolibri FAT32 uses its original MBR/PBR and root MTLD_F32 with actual hidden/system
attributes; NTFS follows its source-dependent ordinary boot route. FAT16 Windows
uses the original standard PBR and does not gain a BOOTMGR BIOS loader. See the
[legacy ISO guide](linux-legacy-iso-routes.md) for source restrictions, native
NTFS/CheckDisk difference and finite qualification evidence.

## ISO extraction, EFI and boot resources

CP74 / Preview43 adds all eight source-selected EFI fallback architectures,
embedded FAT EFI copying and Mint broken-fallback repair, source-only NTFS Windows
links and original FAT/UDF nonregular rules, source-matched Syslinux 3/4/5/6 resource
contracts and signed obsolete COM32 replacements, Slax EFI relocation and safe
missing-parent recovery. Local cached issuer metadata supplements embedded
certificates. Private native Syslinux accessors now build on aarch64 and run
under sanitizers. NTFS reparse creation needs lowntfs-3g before writes. See the
[ISO/EFI resource guide](linux-iso-efi-resources.md) for source/signature rules,
provider differences, bounds and qualification limits.

## Device and source operations

CP73 / Preview42 adds Alt+O exact raw optical ISO capture, Alt+C selected USB
reset, Alt+P ESP/data type toggling and Alt+, session device-locking control.
Original IgnoreUsb/IgnoreDisk exclusion slots are applied before listing and
writing. Explicit virtual-device discovery now accepts positively identified
file-backed loop and local NBD disks. Source/home protection follows Btrfs,
loop/stacked devices and resolvable overlay/FUSE storage; opaque backing stays
unavailable. Single-member Btrfs automatic unmount is supported when its native
mount and selected backing disk match exactly. Windows-only VHD/VHDX parent
locators can use a per-child `.parents` map. Read the
[device/source guide](linux-device-source-operations.md) for syntax, native
reset differences and qualification limits.

## Desktop controls

CP72 / Preview 41 adds cache clearing (Alt+D), saved worker scheduling
(Alt plus/minus), USB diagnostics (Alt+.), visible-log Ctrl+A, saved dark/light
appearance (Ctrl+Alt+D), bare-Alt percentage/rate/remaining-time views, advanced
forced release checks and verbose update logs. Read the
[desktop controls guide](linux-desktop-controls.md) for native scheduling,
cache preservation and package-verification behavior. Full native-only
translation coverage remains tracked separately as I05.

## NTFS options

CP70 adds **Alt+N** session NTFS compression and **Alt+Q** saved indexing.
Compression with copied files requires NTFS-3G. Ordinary NTFS jobs retain the
kernel provider; larger clusters continue uncompressed as on Windows. See
[NTFS options](linux-ntfs-options.md) for provider and boot-file behavior.

## ISO read options

CP69 adds **Alt+I** ISO/DD selection, **Alt+J/K** Joliet/Rock Ridge support,
**Alt+T** saved timestamp preservation and **Alt+S** size heuristics/MS-DOS
64 KiB cluster override. The other four switches are session preferences.
Read [ISO controls](linux-iso-options.md) for defaults, rescanning and limits.

## Download an image

CP66 adds **DOWNLOAD** in the SELECT button's arrow menu. Choose the Windows or
UEFI Shell version/release/edition, language and architecture, then save the ISO
or open its link in your browser. Saved images are selected automatically after
sync and worker exit. Ctrl+SELECT still chooses the additional ZIP. Discovery and
saving are unprivileged; an interrupted transfer preserves an existing file.
See [native downloads](linux-image-downloads.md) for the signed provider, native
NTFS publication/recovery behavior and qualification.

## Portable settings, startup and logs

CP59 adds portable adjacent `rufus.ini` storage (`--portable` or the original
portable filename), saved advanced panels, `-x` extra devices, `-f` filesystem
preference and `-w` instance waiting. Ctrl+L toggles the log; Ctrl+P saves
persistent-log mode; Alt+R resets normal user settings and Alt+U changes device
size units. The full session can be exported despite the bounded visible tail.
An accepted idle close saves `files/rufus.log` in the existing user data folder,
or `rufus_files/rufus.log` beside the portable executable. See the
[desktop guide](linux-desktop-settings.md) for original behavior, native
differences, failure handling and exact startup options.

## Device discovery

CP65 excludes the disk Rufus runs from, including identified partition, layered,
loop and Btrfs backings. Already mounted volumes can supply their extended
`autorun.inf` label; multiple mounted volumes have a matching caption. No mount
or authorization is requested for discovery. Unmounted/unreadable metadata uses
the filesystem label. See [device discovery](linux-device-discovery.md).

## Application update checks

CP68 adds native release checks to **Settings**, the original beta preference,
**Alt+Y** forced discovery, cancellation and deferred notifications. **Open release
page** hands installation to the package manager/release page. Existing Secure
Boot consent remains unchanged; enable automatic application checks explicitly.
The port publisher currently has no published native releases, so a successful
empty check is reported separately from “up to date.” See
[native application updates](linux-application-updates.md).

## Application language

CP67 enables the language button with the original catalog's 38 choices, saved
selection, `-l` / `--locale` and right-to-left layout. Start in English and choose
explicitly; a saved language takes precedence over the startup argument, as on
Windows. Switching preserves the selected device/image/filesystem and session
log. Text without an original catalog counterpart remains English. See
[native application languages](linux-localization.md) for catalog validation,
external `rufus.loc`, fallback behavior and remaining translation coverage.

## Standalone MS-DOS and ReactOS

CP64 adds normal **MS-DOS** and advanced **ReactOS** on MBR/FAT16/FAT32.
MS-DOS asks for explicit DOS keyboard/code-page choices and, when needed,
permission to download the fixed hash-checked Microsoft resource. The DLL is
read only as data and cached; its 13 files and original patches are copied in
one authorized job. ReactOS prepares boot records for separately supplied loader/
system files. See the [legacy boot guide](linux-legacy-boot.md) for exact behavior,
metadata differences and qualification. DOS requires BIOS or UEFI-CSM.

## Secure Boot policy updates

CP63 adds **Settings** for opt-in automatic policy checks, Disabled/Daily/Weekly/
Monthly intervals, **Check now** and cancellation. Enabled sessions refresh the
small Rufus lists; DBX checks follow the chosen interval and prompt before
newer databases are downloaded. Valid DBX caches survive application restarts,
with bundled data as fallback. Select the image again after an update to use
its new policy. These files affect Rufus image checks and do not change firmware.
See the [policy update guide](linux-policy-updates.md) for storage, error handling
and the original Windows behavior. CP66 later adds native image-download selection. CP68 adds native application release checks and
release-page handoff; distribution installation remains delivery work.

## Add files from a supplementary ZIP

Select the boot image, then hold **Ctrl** while pressing **SELECT** to attach a
ZIP. Its files are copied after normal file-based creation or formatting and can
overwrite earlier files. The confirmation shows the ZIP and unpacked totals.
FAT16/FAT32, NTFS, exFAT and UDF use the same authorized job; DD and ext modes omit it.
Choosing another image clears the attachment. Ctrl+SELECT followed by Cancel
also clears it. Source protection, CRC checks, sync and cleanup precede READY;
an incomplete supplement remains FAILED. See the [ZIP guide](linux-supplementary-zip.md).

## Windows To Go and standalone WIM/ESD

Select a Windows ISO and choose **Windows To Go**, or select a complete standalone
WIM/ESD, which offers To Go directly. Choose the install archive when several are
present, then the Windows edition. The native image index is retained even when
edition labels repeat. NTFS is required. GPT creates a 260-MiB ESP, 128-MiB MSR
and Windows partition; MBR offers BIOS or UEFI with UEFI:NTFS.

The one authorized job applies Windows metadata, generates BCD from the selected
image's template, copies its boot files and applies chosen installed settings.
EFS content stays encrypted. Sparse encrypted streams require clusters small
enough to represent their holes; an incompatible choice refuses before writes.
Internal-drive offline policy and expert S-mode are available; Windows regional
copying stays visible and disabled. READY waits for sync, cleanup and helper exit.

CP58 covers four synthetic source/device routes in normal and sanitizer Linux
guests, independent metadata/boot-file readback and a native EFI test handoff.
Windows startup/installation and decryption were not executed. See the
[To Go guide](linux-windows-to-go.md) for exact coverage and limits.

## Live USB persistence

Compatible Syslinux or GRUB images expose **Persistent partition size**. Set it
with the slider or MB/GB/TB controls; 0 disables it and the minimum enabled size
is 256 MiB. Persistence selects ISO copy mode. Selecting another image or a
source-free boot choice resets it; changing the device or format clamps it to
valid partition/filesystem geometry and the image's space allowance.

The same authorized helper creates and verifies an ext persistence partition
between the main FAT16/FAT32/NTFS partition and any UEFI:NTFS partition. Ext3 is
the default; the explicit `UseExtVersion` preference accepts 2, 3 or 4. Casper
images use `casper-rw`. Other eligible Linux images use `persistence` containing
`persistence.conf` with `/ union`. Boot configuration changes follow Windows
Rufus's ordered preseed, Casper and live-boot rules. Images with other persistence
conventions can still require distribution-specific configuration.

CP42 qualifies ext2/3/4 with native virtual-device creation/readback, a real
GPT/NTFS/ext4/UEFI:NTFS three-partition backend case, a formatter-failure case,
and ordinary Qt creation with one authorization and READY after cleanup/exit.
A generated Syslinux Linux-kernel fixture boots the final USB twice and verifies
that its persistence file survives. This is not an official-distribution overlay
or physical-hardware qualification. Existing shared source/claim/fault evidence
is reused. E2fsprogs supplies the required runtime library and `mke2fs` formatter.

## Save a device to an image

Select the USB device, close files using it, then use the save icon beside
Device. An input image is not required. The dialog remembers the preferred
format and starts with VHDX. FFU is offered for a GPT device; ISO is offered
when filesystem metadata is available.

| Output | Capture boundary |
| --- | --- |
| UDF-only `.iso` | Exports the first eligible filesystem by partition offset (or a whole-device filesystem), preserving files, hidden entries, empty directories and Unicode names. Uses optional cdrtools `mkisofs`, then removes its ISO-9660 recognition descriptors while retaining the UDF 1.02 tree. File contents and paths are verified before publication. No automatic El Torito boot setup. |
| Fixed `.vhd` | Complete device bytes plus the VHD footer; 512-byte logical sectors, up to 2040 GiB. |
| Dynamic `.vhdx` | Complete logical device contents with zero 32-MiB blocks omitted; 512- or 4096-byte logical sectors, up to 64 TiB. No parent or replay log is generated. |
| Full `.ffu` | Plain XPRESS-compressed single-store capture, including every device block. Valid primary GPT with non-overlapping partitions, including empty GPT disks; unnamed/Unicode names are retained in the raw GPT (optional ASCII manifest names are omitted when needed). Supports 512- or 4096-byte logical sectors and exact sector-aligned capacity. CP50 supports manifests spanning multiple chunks and GPT entry tables up to 4 MiB. CP41 chooses 1-KiB to 16-MiB chunks within native table/buffer budgets; see the dense capture bound below. No signing certificate or portable resizing is generated. |

The GUI and ISO encoder stay unprivileged. An unmounted raw capture uses one
interactive UDisks read acquisition. Mounted captures and all ISO exports use
one authorized `--save-device` helper: it validates the frozen physical source,
output backing and caller home; matches fresh UDisks/kernel mount inventories;
unmounts without force/lazy flags; and retains source access through the job.
The helper accepts no destination pathname or write descriptor. Unix peer
credentials, a per-job token and close-on-exec descriptor transfer bind the
read-only source to the waiting caller. A cancelled authorization or failed
unmount does not fall back to another authorization route.

Raw captures retain an exclusive read-only whole-device handle. ISO exports
use a private read-only mount of the selected filesystem. FAT/exFAT are mounted
for the caller; ext3/ext4 disable journal replay; NTFS uses the configured
kernel driver and, only when that driver is unavailable, the NTFS-3G fallback.
The private source mount and any NTFS process must exit before publication.
Source filesystems remain unmounted afterward. Mounted system/protected,
layered, swap, aliased, nested or
busy sources and output on the source disk are refused.

Eligible read-only USB media are listed with **[Read-only]** and can be saved
in all four formats within the same format/geometry limits. START and format
controls remain disabled; the tooltip points to the save action. Discovery
requires a known boolean read-only state and still applies the existing
USB/media/protected-use checks. Known read-only/writable capability changes
retain source selection and cancel a pending or active creation job when it
becomes read-only. Capture never clears kernel or hardware write protection.

The destination directory must already exist and be writable by the desktop
user. A private temporary image is decoded or checked against a file manifest,
and the source is reread before publication. Cancellation or failure before
publication preserves an existing backup. Replacement requires the destination
filesystem's atomic rename-exchange support. A final sync or cleanup failure
remains FAILED even if a published image already exists. Successful full READY
waits for descriptor closure, private-mount/process cleanup and joined worker
and helper exit. The operation does not eject or remount the USB.

ISO export requires `cdrtools` (`mkisofs`). The source must contain ordinary
readable files/directories; links, special entries, invalid UTF-8 names and
nested mounts are refused. Traversal is bounded to 128 directory levels,
262144 entries and 64 MiB of path metadata; the provider's image/file-size
limits still apply (8 TiB image bound). Any provider renaming, skipped data or
unreadable file fails verification. UDF names/labels must round-trip exactly
through the encoder/reader. The source filesystem label is retained up to
32 characters, with `RUFUS` for an empty label. For multiple filesystems, Linux
uses partition offset because it has no Windows drive-letter ordering. An ISO
contains the filesystem's files; choose a raw format to retain disk partition
and boot records.

CP35 qualifies the three raw capture formats. CP36 batches ISO export and
source unmount: focused normal/sanitizer cases include a real file larger than
4 GiB; combined guest jobs cover mounted VHDX, FAT and NTFS-3G ISO export,
normal unmount, busy refusal, cancellation, cleanup failure and held helper
exit. Independent kernel UDF readback compares every exported path/content;
one saved VHDX restore retains the existing whole-disk/FAT/BIOS qualification.
CP37 qualifies all four saves from a kernel-read-only device through production
discovery, mounted VHDX/ISO source handling, metadata and kernel write refusal,
source/RO-state preservation and one captured VHDX restore/boot. The guests use
512-byte sectors. Physical media, actual desktop authorization
UI and Windows mounting/DISM deployment remain unqualified.

## Creation modes

| Input | Choices in the GUI | Requirements and recorded evidence |
| --- | --- | --- |
| No image — **Non bootable** | **MBR** or **GPT**, **FAT16**, **FAT32**, **NTFS** or **exFAT**, **BIOS or UEFI**, quick format | FAT uses dosfstools; NTFS uses the configured native mkntfs; exFAT requires exfatprogs metadata verification/no-discard/partition-table controls and libblkid 2.39+. CP16d/f/h qualified complete jobs on simulated USB; CP16e/g/i expose Qt selection, confirmation and completion. Choices follow native geometry and original Rufus defaults. CP76 qualifies 512/512e/4Kn logical/physical geometry. CP16k/l/m qualify the MBR backends; CP16n exposes them in Qt. MBR is bounded by 2^32 logical sectors (2 TiB at 512 bytes, 16 TiB at 4096); each format also applies its own limits. These jobs create no bootloader; SFD is listed separately below. |
| No image — **Non bootable**, **Super Floppy Disk** | **FAT16**, **FAT32**, **NTFS** or **exFAT**, **BIOS or UEFI**, quick format; no partition table | CP16o/p/q qualify whole-device formatting with native dosfstools, stale partition removal, one helper launch and final sync. CP16r/t expose it in Qt where native FAT geometry fits; CP16s qualifies FAT16 native/helper jobs. CP16u qualifies NTFS SFD in the backend and CP16v exposes it in Qt. CP16w qualifies exFAT SFD in the backend and CP16x exposes it in Qt. CP76 qualifies 512/512e/4Kn geometry through whole-device formatter/readback. |
| No image — **UEFI:NTFS** | Under **Show advanced drive properties**: **GPT** (default) or **MBR**, **NTFS** or **exFAT**, **UEFI (non CSM)** | Creates a blank data volume plus Rufus's unchanged embedded UEFI:NTFS loader. Uses the same native formatters as the non-bootable routes. CP18a–d qualify GPT/MBR helper jobs and payload readback on simulated USB; CP18e exposes the Qt selector and tests its defaults/lifecycle. MBR is available up to 2 TiB; only 512-byte logical sectors are qualified. This alone does not install an operating system. |
| No image — **FreeDOS** | **MBR**, **BIOS (or UEFI-CSM)**, **FAT16** or **FAT32**, explicit keyboard/code-page choice | Reuses Rufus’s original MBR/PBR code, bundled payload and startup writers. Requires native dosfstools and a qualified 512-byte-sector FAT layout. CP19d2b qualifies complete helper creation/readback; CP19d2c boots FAT32/Rufus-MBR with RO/852 and FAT16/Windows-7-MBR with US/437 under SeaBIOS. CP19e enables the normal Qt choice with the default Rufus MBR. Advanced BIOS fixes and other geometries remain work. |
| No image — **Syslinux 4.07** or **Syslinux 6.04** | Under **Show advanced drive properties**: **MBR**, **BIOS (or UEFI-CSM)**, **FAT16** or **FAT32**; **NTFS** also available for v6 | Requires the selected packaged native installer; FAT uses dosfstools and mtools, NTFS uses the native formatter and mount driver. CP20a–h qualify checked installation, one-helper jobs on simulated USB, independent readback, bare BIOS boots and Qt selection/progress/completion. Only 512-byte logical sectors are qualified; CP74 qualifies portable v4 accessors and aarch64 build/emulation; physical aarch64 writes remain unqualified. This creates a bare loader without configuration, kernel or OS. CP20i–m also qualify and expose standalone v6 on NTFS with the kernel driver or NTFS-3G fallback. |
| No image — **Grub 2.14** | Under **Show advanced drive properties**: **MBR**, **BIOS (or UEFI-CSM)**, **FAT16**, **FAT32** or **NTFS**; **ext2/ext3** under advanced format options | Reuses Rufus's embedded core and boot records with native formatting. CP21b–l cover helper creation, bare BIOS loader boots and Qt selection. A bare loader has no OS/kernel/configuration. |
| Uncompressed disk image (`.img`, `.usb`, `.wic`, or other suffix) | DD; the image supplies its partition table and filesystems | Must contain complete 512-byte sectors and Rufus's `55 AA` boot marker unless Alt+M was enabled before selection, fit the device, and satisfy its native sector alignment. WIM candidates remain separate. CP8b verifies the written bytes, cancellation/removal and a generated BIOS boot image. |
| Gzip/XZ/BZIP2/LZMA/LZW/ZIP/Zstd/VTSI/VHD/VHDX (`*.gz`, `*.xz`, `*.bz2`, `*.lzma`, `*.Z`, `*.z`, `*.zip`, `*.ZIP`, `*.zst`, `*.ZST`, `*.vtsi`, `*.VTSI`, `*.vhd`, `*.VHD`, `*.vhdx`, `*.VHDX`) | DD; the decoded logical image supplies its layout/filesystems | CP24e–CP33 validate decoded size/hash asynchronously, check target geometry/capacity before confirmation, and use one helper with verified readback and joined READY. ZIP reads its first central-directory entry with stored, deflate, BZIP2, LZMA or XZ. VTSI preserves sparse target gaps; fixed/dynamic/differencing VHD/VHDX reconstruct the logical disk through retained parent files; dirty VHDX logs replay in a private read overlay (CP38). Requires the `55 AA` marker unless Alt+M was explicitly enabled before selection (CP44). Real guest coverage is 512-byte sectors; focused geometry tests include 4096/65536. |
| Full or partial FFU (`*.ffu`, `*.FFU`) | Native deployment at the selected capacity | CP34/CP39/CP40/CP43/CP53/CP54/CP55/CP56 support full and partial V1, V2 and V1-compressed none/plain XPRESS/LZNT1/Huffman-XPRESS; validates all SHA256 chunks before output; resolves repeated/overwritten/end-relative blocks against the target and preserves unlisted bytes. Partial updates require all declared base comparisons to match before any writes and apply their metadata as declared. Full-image GPT metadata is relocated on a larger disk. Existing helper/readback/joined READY applies. Grouped buffers and multi-block end-relative runs are supported. V2 multi-store containers require an explicit store choice; all stores are authenticated, and only the chosen store is deployed. Optimized full images fit smaller/larger targets with NTFS growth and recovery-location updates; catalogs follow the desktop signature policy. CP56 closes the [FFU feature audit](linux-ffu-parity.md); automatic multi-drive mapping is not a Windows Rufus desktop capability. See the detailed FFU boundary below. |
| ISOHybrid image | Rufus's ISO/DD dialog when both modes are allowed; **ISO is recommended and selected by default** | DD copies the image unchanged and does not run a formatter or native Syslinux installer. Policy can require DD, in which case formatting controls are disabled and the choice dialog is skipped. GUI/byte-copy tests cover these paths; ISO mode must satisfy a route below. |
| Linux ISO/UDF with a regular x64 EFI fallback | **GPT**, **UEFI (non CSM)**, **FAT16**, **FAT32** or **NTFS** when Rufus permits it | Requires a readable `/EFI/BOOT/BOOTX64.EFI` in the selected image tree. FAT16 is offered only on supported small targets and uses the same vfat copy path; a generated GPT/FAT16 image booted under x64 OVMF in CP15e. NTFS adds UEFI:NTFS. Alpine **3.22.5 x86-64** booted from GPT/FAT32 in CP10p; NTFS firmware handoff and large-file copying have separate generated-fixture coverage in CP11c/g. Other Linux distributions are not qualified by those examples. |
| Compatible Linux Syslinux ISO/UDF | **MBR**, **FAT16**, **FAT32** or **NTFS**; **BIOS or UEFI** when regular EFI files are present, otherwise **BIOS (or UEFI-CSM)** | Uses the source-selected Syslinux 3/4/5/6 resource contract and matching packaged native provider, with compatible configuration/modules (CP74). Eligible hybrids default to MBR; GPT remains selectable when regular EFI files are present. FAT16 is available on supported targets below 4 GiB; FAT32 remains the image preference when it fits. NTFS uses the qualified native extlinux provider; EFI-capable images also receive Rufus's UEFI:NTFS partition. CP20p/q qualify the NTFS helper with the kernel driver and NTFS-3G fallback, including generated BIOS/x64 UEFI boots; CP20r exposes the Qt route. Generated MBR/FAT16 media passed native helper creation, readback and BIOS boot in CP15i; CP15j enables its GUI route. Alpine **3.22.5 x86-64** passed MBR/FAT32 installed-helper creation and both BIOS/UEFI boot in CP12o/p. |
| Compatible Linux GRUB2 ISO | **MBR**, **FAT16**, **FAT32**, or **NTFS**, **BIOS (or UEFI-CSM)**; **BIOS or UEFI** when regular EFI files are present | CP21w qualifies a pinned GRUB 2.12 core/module/configuration fixture through native helper creation, all-file readback and BIOS boot into Linux userspace. CP21x enables the GUI route, original version/download/fallback decisions, retained core transfer, copy percentages and READY after helper exit. CP21y qualifies native FAT16 creation/readback/BIOS boot; CP21z enables its Qt choice with native geometry and original filesystem defaults. Small BIOS targets default to FAT16 when supported, while an explicit FAT32 choice survives a rescan. Syslinux retains Rufus's precedence. EFI-capable images can still select GPT and use the existing EFI copy route. CP21aa qualifies plain BIOS/MBR/NTFS native creation/readback/boot with the kernel driver and NTFS-3G fallback. CP21ab enables its BIOS-only Qt choice using shared format policy, native cluster selection, retained core transfer and original NTFS progress allocation. NTFS becomes the default when the image policy excludes FAT; manual filesystem/cluster choices are honored. CP21ae also enables MBR/NTFS for EFI-capable GRUB images, automatically adding the required UEFI:NTFS partition. CP21ad supplies combined native creation/readback, BIOS Linux boot and x64 EFI test-application handoff evidence with both NTFS providers. GPT/UEFI and DD choices remain available under their existing policy. Windows image mode does not offer ext filesystems; those belong to the standalone GRUB choice. This does not certify arbitrary distribution modules/EFI loaders, other EFI architectures or Secure Boot. |
| Known Windows 10/11 x64 installation ISO/UDF | **Standard Windows installation**, **GPT**, **NTFS**, **UEFI (non CSM)** | Uses the bundled UEFI:NTFS boot partition. The supplied Windows **11 25H2** ISO was copied, independently verified and installed as Windows 11 Pro in a VM, which booted to its first-use screen with the USB detached (CP11k). Windows 10 is accepted by preflight but has no corresponding real-ISO installation qualification. |
| Known Windows 10/11 installation ISO/UDF with root BOOTMGR | **MBR**, **NTFS**, **BIOS (or UEFI-CSM)**; **Alt+E** also permits eligible **FAT16/FAT32**, with **BIOS or UEFI** when EFI files are present | CP22d2a/b qualify native NTFS creation/readback and generated BIOS/x64 EFI handoff with both NTFS providers and original MBR choices. CP22i qualifies native FAT32 creation/readback and generated BIOS/direct x64 EFI handoff; CP22j exposes it in Qt. **Alt+A** retains the original MBR choice. Enhanced EFI-capable mode needs a checked x64 fallback application. CP75 adds original FAT16 eligibility/records; its standard FAT16 PBR does not load BOOTMGR. Physical/Secure Boot and Windows Setup validation of these BIOS/dual routes remain unqualified. |
| Supported legacy WinPE setup ISO/UDF (i386, AMD64 or minint) | **MBR**, **NTFS**; original **Alt+E** eligibility also permits **FAT16/FAT32** | CP75 extends the existing bounded SetupWinPE preparation to original BOOTMGR/EFI/install-archive and selected Syslinux/GRUB mixtures. SIF encodings, existing root loader equality and detector ownership remain checked before writes; ordinary additional root loaders are preserved. Final PBR and MBR/0x81 choice follow original independent dispatch. UEFI:NTFS is included whenever source policy requires it. The grouped generated-source/media gate preserves one authorization and joined READY; actual installed WinPE/AIK and Windows CheckDisk remain unqualified. See [B4 details](linux-legacy-iso-routes.md). |
| Windows 7/8/8.1, older/unknown BOOTMGR installers, or BOOTMGR/WIM PE without an install archive | **MBR/NTFS/BIOS**; source-eligible **GPT/UEFI/FAT16/FAT32/NTFS** and **Alt+E MBR/FAT16/FAT32 dual** | CP75 removes the ordinary copy requirement for recognized version/install metadata. Vista stays normalized unknown. Exact fallback EFI validation and original image-1 Windows EFI repair remain required where applicable. Modern customization stays tied to the original offered options. FAT16 uses the standard original PBR, which expects IO.SYS/MSDOS.SYS; generated firmware qualification uses its native EFI route. See [B4 details and limits](linux-legacy-iso-routes.md). |
| ReactOS ISO/UDF | **MBR/FAT16/FAT32/NTFS**, selected Syslinux primary | CP75 uses embedded 4.07 when the source has no Syslinux version/config; actual versions use the existing resource contract. Original mboot/config files precede extraction and source files can replace them. The final original MBR is selected independently, including saved Alt+A. Selected GRUB SBRs follow original precedence. |
| KolibriOS ISO/UDF | **MBR/FAT32** or **NTFS**, source-dependent EFI/UEFI:NTFS | CP75 copies HD_Load/USB_Boot/MTLD_F32 to root with checked hidden/system attributes. FAT32 uses the original Kolibri MBR/PBR. FAT16 PBR installation remains refused. The native five-job gate checks bytes, attributes, original records and representative boot handoff. |
| FAT32-compatible Windows 10/11 x64 installation ISO/UDF | **GPT**, **FAT32**, **UEFI (non CSM)**; **Alt+E** also permits qualified **MBR**, **FAT32** | For a large `sources/install.wim`, enable **Alt+E**, then choose FAT32; NTFS remains the default. Select GPT explicitly for UEFI-only media. Splitting requires one conventional installation archive and splittable WIM resources, with sufficient reserved space. Windows 11 25H2 GPT media passed independent readback, UEFI boot, DISM inspection and discovery of all 11 editions in Setup (CP11af/ah). A complete installation from its split SWMs has not been tested. Alt+E defaults qualified Windows BIOS images to MBR and preserves a valid filesystem choice; small-WIM images may retain FAT32. |

CP16i exposes non-bootable GPT/NTFS in Qt using CP16h’s qualified backend. It
creates one data partition without UEFI:NTFS; the existing NTFS image-creation
routes retain their boot partition and payload.

CP18e exposes **standalone GPT/MBR with NTFS or exFAT** in Qt. Expand
**Show advanced drive properties**, then select **UEFI:NTFS**. A source ISO is
not required. The default partition scheme is GPT unless an explicit supported
choice was already made. The existing device filesystem or a valid manual
choice is retained; otherwise NTFS is selected. The target is always UEFI.
Collapsing advanced drive properties restores the image choice, retaining any
loaded ISO. SELECT remains enabled while idle and selecting a new image returns
to image mode, matching Windows.

The existing helper creates a blank data volume plus Rufus's embedded loader,
then syncs and exits before successful READY. No operating system is installed
by this boot choice. Ordinary exFAT formatting retains its single data partition;
the standalone boot request is explicit. ISO-backed MBR creation is separate.

CP16j adds non-bootable MBR table creation/clearing/readback for FAT16, FAT32,
NTFS and exFAT. CP16k qualifies complete MBR/FAT16 and FAT32 device-helper jobs
on simulated USB, including native formatting and readback. CP16l adds the
non-bootable MBR/NTFS helper using the existing native formatter and retained
disk claim, without UEFI:NTFS or mounting. CP16m adds non-bootable MBR/exFAT
using the existing native formatter and claim handoff. All four MBR filesystem
backends are implemented and CP16n exposes them in the Non bootable GUI.
It defaults to MBR, remembers explicit MBR/GPT choices, and switches to GPT
above 2 TiB. Both layouts retain BIOS or UEFI as the target label.

CP60 adds **UDF** and advanced **ext2/ext3** data formatting on MBR, GPT and SFD.
UDF uses optional udftools `mkudffs`, defaults to revision 2.01 and accepts
1.02/1.50/2.00/2.01 through `UseUdfVersion`. It supports Unicode labels,
extended label/icon files and supplementary ZIP contents. Ext2/ext3 require a
device of at least 256 MiB. All three show Default cluster size and retain one
authorized job with sync/cleanup/exit before READY. CP76 qualifies logical sectors
512 through 4096 bytes and physical sector facts. UDF 2.50/2.60 and ReFS formatting
are excluded from this release by user decision. See the [formatting guide](linux-filesystem-formatting.md).

CP16o adds whole-device SFD metadata preparation and FAT32 formatting at byte
zero on disposable files. CP16p qualifies the native block-device formatter
handoff, removal of stale kernel partitions and exclusive reacquisition on
simulated USB. CP16q qualifies the complete source-free SFD/FAT32 helper job,
including cancellation, final sync and waiting for helper exit. CP16r enables
Super Floppy Disk in Qt for non-bootable FAT32, with whole-device cluster
choices and explicit confirmation. CP16s qualifies FAT16 SFD native/helper jobs;
CP16t exposes FAT16 and FAT32 with original Rufus defaults and native whole-device
cluster checks. CP16u qualifies the native/helper NTFS SFD path, retaining the
exclusive claim throughout formatting, readback and final sync. CP16v exposes
NTFS SFD with whole-volume cluster choices and native label rules. Device refresh
still defaults to GPT above 2 TiB; an explicit SFD choice is honored where NTFS
geometry fits. CP16w qualifies exFAT SFD formatting with claim handoff, native
metadata readback and final sync; CP16x exposes it in Qt with whole-device
cluster choices and original Rufus label/default rules. CP76 adds the combined
512/512e/4Kn qualification described in the geometry guide.
FAT16 is omitted near 4 GiB when the whole-device volume exceeds the native
cluster-count limit, even if a smaller MBR/GPT partition would fit. FAT32 remains
available where its geometry permits.

The [checkpoint log](testing-and-checkpoints.md) records the source hashes,
test setup and exact limits behind each example. Alpine's copied EFI loader
prints a known original-ISO-label search warning before its successful boot;
the port does not patch that binary or suppress the warning.

SELECT analyzes the image; START checks the chosen route before authorization.
A displayed filesystem or enabled START button is not proof that the whole
image can be prepared. Incompatible boot files, source changes, incomplete
analysis, unsupported extraction entries and insufficient space stop creation.
Unknown Syslinux detection stays unknown: the native installer/payload checks
must establish compatibility before that route can write.

## Windows BIOS and dual NTFS/FAT32 creation

A known Windows 10/11 installer with root BOOTMGR now offers **MBR** with **NTFS**.
EFI-capable images default to **GPT**, **UEFI (non CSM)**; selecting MBR changes
that target to **BIOS (or UEFI-CSM)**. **Alt+E** defaults the qualified image to
MBR and displays **BIOS or UEFI** when its fallback EFI files are present. Plain
BIOS NTFS omits UEFI:NTFS; dual NTFS adds Rufus's original embedded loader using
the shared partition policy. Alt+E also permits **FAT32** on the qualified MBR
route. FAT32 uses original Windows PE boot records and copies EFI files directly,
without a UEFI:NTFS partition. Small-WIM images retain a valid FAT32 choice;
large-WIM images retain NTFS until FAT32 is selected explicitly. The existing
split-WIM path handles an eligible large install.wim. **Alt+A** switches the default original Rufus MBR
to the original Windows 7 fallback and remembers the choice. These controls
cannot change an active scan or creation request.

Native creation and generated firmware handoff were qualified in CP22d2a/b with
both kernel NTFS and the absent-kernel NTFS-3G provider. CP22e exposes that same
helper through the existing Qt flow. CP22i qualifies native FAT32 creation,
independent readback and generated BIOS/x64 EFI handoff with both MBR choices;
CP22j enables that same helper in Qt. Unknown versions, older EFI/FAT, FAT16,
mixed BIOS loaders and unsupported geometry remain gated; enhanced EFI-capable
mode checks the actual x64 fallback application again at START. GPT/FAT32 remains
selectable for UEFI-only media. No further Windows customization installation
test is added. Successful READY still waits for sync, cleanup and helper exit.

Known Windows **7, 8 and 8.1** installers with root BOOTMGR now offer only
**MBR**, **NTFS**, **BIOS (or UEFI-CSM)**, including sources that also contain
EFI files. **Alt+A** retains both original MBR choices. This is the qualified
plain BIOS route: GPT/UEFI, enhanced/FAT and Windows 7-style EFI repair remain
work. A saved Alt+E preference is retained for other images but does not enter
this request; pressing it shows the limitation without changing that preference.
No Windows User Experience dialog or edition chooser is offered for these
older versions, following Rufus's shared customization rules. Device refresh,
confirmation and final READY reuse the existing helper flow. CP22l2 proves
representative native NTFS creation and generated BIOS handoff, not actual
older Windows Setup/BCD.

## FreeDOS creation

Choose **FreeDOS** in Boot selection, choose the target and FAT settings, then
press START. The keyboard/code-page chooser starts at **US/437** for each job.
Choose explicitly from Rufus's existing DOS tables; Linux desktop/locale settings
are not detected. Cancel stops preparation before authorization or writes.
The selected DOS values and format settings remain fixed through confirmation
and the single authorized helper job. Other selections retain Rufus's original
DOS startup menu with its US/437 fallback.

Copying shows **Copying DOS files...**, with the original six discrete progress
steps. Successful **READY** appears only after file/device sync, private-mount
cleanup and helper exit. Rufus does not eject the USB. A previously loaded ISO
is retained for switching back to image mode, but is not used in a FreeDOS job.

## Standalone Syslinux creation

Expand **Show advanced drive properties** and choose **Syslinux 4.07** or
**Syslinux 6.04**. The latter uses the pinned 6.04-pre1 payload, matching Windows's
displayed name. FAT16/FAT32 choices follow Rufus's filesystem defaults and native
geometry limits. Switching boot choices resets cluster size to the filesystem
default; collapsing advanced properties restores any retained image.

Syslinux 6.04 also offers NTFS through the qualified native extlinux provider;
Syslinux 4.07 remains FAT-only. CP20r also exposes NTFS for compatible Syslinux
images through the CP20p/q helper: BIOS-only images use one partition; x64 EFI
images additionally receive UEFI:NTFS. Image loading still prefers FAT32 where
allowed, and compatible hybrids retain the ISO-recommended/optional-DD dialog.
An unknown image version still requires a qualified installer/payload match.

Each choice checks its own packaged installer before authorization. No other
version is substituted if it is missing. Installation uses one authorized job,
the original progress allocation and successful READY only after final sync and
helper exit. A bare loader is expected to show a missing-configuration message
and `boot:` prompt until an OS/configuration is supplied. Selecting v4 here does
not enable image-based v4 installation or reinterpret an unknown image version.

## Secure Boot issuer and revocation checks

CP62 / Preview 31 checks reported ISO EFI loaders against the original bundled
DBX/SBAT/SVN and certificate policies. Recognized issuer thumbprints and the
revocation reason appear in the log. Revoked loaders trigger the Windows-style
warning at START, before ISO/hybrid-DD mode selection or authorization. Cancel
stops preparation; OK continues to ordinary target confirmation. Expert-only
certificate revocation uses the Expert Mode value when the image was loaded.

This is Rufus policy/metadata analysis. It does not authenticate a publisher,
verify the actual executable signature, inspect target firmware trust, or promise
that a particular computer will boot. Malformed/unreadable or unenumerated
embedded-only EFI entries retain the Secure Boot checks-pending indication.
CP63 adds [remote policy refresh](linux-policy-updates.md); broader firmware/
chain availability remains work.
See the [Secure Boot guide](linux-secure-boot.md).

## Runtime UEFI media validation

CP61 / Preview 30 enables **Runtime UEFI media validation** for eligible
EFI-capable ISO copies on FAT16, FAT32 and NTFS. The session choice follows
Windows eligibility and is frozen before confirmation. DD-only images, Windows
To Go and forced BIOS-only Windows creation cannot enable it.

The same helper copies/repairs the checksum list, preserves original EFI
loaders and installs Rufus's bundled validators. Only files Rufus modifies have
existing source digests recomputed; untouched wrong source digests remain wrong.
Generated split-WIM parts are included. Existing backup conflicts refuse before
device writes. All list/loader sync, readback, cleanup and helper exit precede
successful READY. The final supplementary ZIP can replace validated contents
and must supply a corresponding list if it does so.

The x64 simulated-device batch covers FAT16/FAT32/NTFS creation, independent
readback and a real native EFI validator-to-original handoff. Other firmware
architectures and physical machines remain broader qualification work.
MD5 detects corruption; it does not authenticate publishers or replace Secure
Boot trust/revocation checks. See the [runtime guide](linux-runtime-validation.md).

## Devices and runtime

After the ordinary erase confirmation, DD, ISO and non-bootable creation show
Rufus's additional **Multiple partitions detected** warning when the fresh device
check finds more than one counted partition. Cancel is the default; neither
confirmation starts authorization or writes. Counts follow Windows's auxiliary
partition exclusions. Unrecognized MBR EFI filesystem metadata is conservatively
counted rather than opening the raw disk before authorization. If a single-partition
target becomes multi-partitioned before authorization, start again to review the
warnings. The warnings concern the device's existing layout, not the new layout.


- The default device list includes writable, non-system USB disks accepted by
  **Rufus's shared HDD/card filter**. It uses native capacity, fixed/removable
  status, vendor/model and USB IDs. This can include fixed-media flash drives and
  hide large removable disks. Recognized cards up to 500 GiB bypass HDD scoring;
  larger cards need **List USB Hard Drives** (advanced drive properties or **Alt+F**).
  That option bypasses only the default filter. Optical media, internal disks,
  individual partitions and failed native identity snapshots remain excluded.
  Unknown removal status requires the option unless positive card metadata applies.
  Native names can differ from Windows friendly names, so identical hardware is
  not yet guaranteed to receive identical scores across operating systems.
  The option starts off each session and is locked during image analysis and
  creation, including confirmation. Its warning tooltip matches Windows Rufus.
  Devices are ordered by capacity; the smallest eligible device is initially
  selected. Check the target before START. Turning the option off clears a
  selected disk that fails the default filter; it does not select another disk
  for you. Unplug/replacement and discovery recovery require a fresh selection.
  All DD jobs use the same preparation helper. Fixed or unknown media requires
  native source/home/system/holder protections even with the option off and an
  empty UDisks mount inventory. Opt-in retains these protections too. Native
  classification facts join selection identity and are rechecked before writes.
  Linux's disk sequence must be available; older-kernel fallback remains work.
  CP17d5d qualifies default-visible simulated fixed media and explicit opt-in;
  this is not exhaustive physical-drive or desktop authorization qualification.
- DD, ISO and non-bootable creation (including SFD) can unmount eligible direct-block
  target filesystems after the destructive confirmation, inside the same authorized
  helper. Native mount/device checks protect the source, caller home and system
  storage. Busy mounts, swap, unsupported layered storage and unresolved source/home
  backing fail; there is no forced/lazy unmount or automatic remount. Native checks
  now accept NTFS-3G `fuseblk` mounts using their real block-device identity.
  Both NTFS-3G variants pass native and complete helper VM qualification:
  mounted FAT32 recreation and DD, refusals, cancellation, one authorization,
  sync/cleanup and helper exit before completion. General anonymous FUSE
  backing remains unsupported. Mount parents must
  pass the native ownership/path checks. Mounted and unmounted DD use the same
  helper after a fresh preflight following confirmation. Mount/device changes
  are revalidated by that helper; no fallback or second privilege prompt occurs.
  The consolidation pass preserves DD's sector range (512–65536 bytes, powers of
  two, with image alignment). CP76 separately qualifies formatting through 4096-byte
  logical sectors.
  The 4K helper acquisition check does not qualify every geometry or mounted layout.
  CP17b7a/b qualify native/helper mounted DD on simulated USB; CP17b7c and the
  consolidation tests cover Qt routing, ISO/DD choice, progress, cancellation
  and helper-exit-before-READY behavior.
  CP76 uses the selected format and boot provider's explicit geometry limits; DD
  checks its own sector alignment. All image jobs, including DD, currently require a
  caller-owned, owner-readable regular source file; a readable image owned by
  another account must be copied to a caller-owned file before use.
- Mounted creation qualification covers SFD/FAT32 and a partitioned FAT32 target
  recreated from a generated Syslinux ISO, with independent readback and boot.
  This does not establish every filesystem or real desktop automount combination.
- All creation modes require matching GUI/helper versions and native polkit
  authorization. Installed packages include the generated policy. Raw and
  ISOHybrid DD no longer have a separate UDisks descriptor-writing route.
  Hybrid images keep the same ISO/DD dialog and recommended ISO default; a
  missing helper disables creation before authorization. See the
  [packaging guide](../linux/packaging/README.md#runtime-requirements) for exact
  tool paths and dependencies.
- All writes need UDisks2/system D-Bus and native desktop authorization. FAT16/FAT32
  needs `mkfs.fat`; exFAT needs a compatible `mkfs.exfat` (qualified: exfatprogs 1.4.3); NTFS needs `mkntfs` plus the selected kernel `ntfs` driver or
  `lowntfs-3g` fallback. Syslinux ISO creation additionally needs the packaged
  installer and mtools. A missing tool does not silently select another mode.
- The GUI stays unprivileged. Each creation uses at most one authorization;
  ISO and non-bootable creation keep the same helper through their complete job.
  Full green **READY** follows successful sync, cleanup and helper/worker exit.
  The application does not eject or power off the USB.

Local checks cover Qt **6.2.4** and **6.11.2** on the current x86-64 host, with
offscreen Widgets tests and isolated X11 desktop launch. Ubuntu CI execution,
native Wayland, real desktop automount and physical USB durability remain
separate qualifications. Neither the boot examples nor a READY result establish
Secure Boot enforcement or firmware compatibility on an arbitrary machine.

## Windows customization

Eligible images expose the implemented hardware-requirement/online-account
bypasses, local account, privacy, BitLocker and quality-of-life options, CA 2023
bootloaders and SkuSiPolicy. S Mode follows Rufus's **Ctrl+Alt+E** expert toggle.
Available choices still depend on Rufus's image/build rules.

Silent installation is implemented for a sole conventional `sources/install.wim`
or `sources/install.esd`, with an explicit edition, local account, privacy choice,
regional settings and the original three acknowledgements. The option to copy
the current Windows user's regional settings remains visible but disabled on
Linux. **Choose regional settings…** accepts explicit values; Linux host defaults
are not substituted.

These paths have component/GUI tests. CP11n's **Skip privacy questions** creation
and independent readback remain the single agreed real-image customization
qualification; the effect inside installed Windows is unverified. The ordinary
Windows 11 installation in CP11k did not exercise customization choices.

## GRUB4DOS standalone and ISO creation

Under advanced drive properties, standalone Grub4DOS 0.4.6a offers MBR/BIOS with
FAT16, FAT32 or NTFS. It uses Rufus's original loader and boot records; a bare
loader has no operating system. Native creation/readback/BIOS checks cover these
choices, including both NTFS providers.

Ordinary image selection also supports a plain BIOS ISO with its own root grldr
and the shared GRUB4DOS secondary-loader choice. Eligible small targets offer
FAT16/FAT32/NTFS with Rufus's FAT16 default; manual NTFS/FAT32 survives reload.
Geometry removes unusable choices. The image owns its loader/menu, with no
standalone payload substitution or version inference. CP22n57/60/63 qualify
native Qt creation, independent readback and representative original loader/menu
BIOS execution, including kernel ntfs and NTFS-3G. Successful READY waits for
sync/private unmount and all process joins; incomplete NTFS format reports fail.
A combined x64 EFI/BIOS image additionally offers MBR/FAT16/FAT32/NTFS with BIOS or UEFI;
GPT remains the shared default for non-hybrid EFI images and its existing UEFI
choices remain available. CP22n66/68 qualify normal/sanitizer ordinary Qt
FAT creation, five-file readback, both firmware boots and joined READY. CP22n70/71
qualify combined NTFS with its required trailing UEFI:NTFS partition, both native
NTFS providers and ordinary Qt creation. Shared FAT16 small-device defaults and
manual FAT/NTFS choices survive reload. An image requiring NTFS can also select
MBR and its paired BIOS/UEFI route. The image retains its own loader and fallback;
the standalone loader is not downloaded or substituted. Native examples use
512-byte sectors and 4-KiB NTFS clusters. A representative boot does not
establish compatibility with every loader version or operating-system chainload.

## Not implemented or not qualified

**Deferred by user decision (2026-10-24):** UDF 2.50/2.60 metadata-partition
creation (B6). Existing UDF 1.02, 1.50, 2.00 and 2.01 formatting remains supported,
with 2.01 as the default. Unsupported revisions fail before authorization/writes;
no lower revision is silently substituted. See the
[documented gap](linux-filesystem-formatting.md#deferred-udf-250260-gap--user-decision-2026-10-24).

ReFS formatting (I15) and Dev Drive detection (I10) are also excluded from
first-release requirements by user decision. Both remain unimplemented. The UDF
indexing attribute is also unavailable; CP77 qualifies its explicit optional
failure behavior, not on-disk metadata equivalence.

**Remaining release work:** full native translations and Q01–Q04 qualification.
See the [current register](linux-parity-register.json) for exact acceptance.
Legacy creation routes are described above and in the [B4 guide](linux-legacy-iso-routes.md).
Actual older Windows Setup/WinPE/AIK, physical media/firmware and full original
Windows API execution remain unqualified within the agreed testing scope.


Full Windows parity remains incomplete. SFD, image-download integration,
quick/full formatting, device saving and extended label/icon creation are
available as described in this guide. B6, ReFS formatting and Dev Drive detection are deferred; the
[release review](linux-release-plan.md#useful-release-review--2026-10-24)
records the agreed release scope and remaining work. See the
[GUI audit](linux-gui-parity.md#explicit-limits-and-follow-up-work) for individual
controls and remaining Win32 behavior differences.

Physical Secure Boot firmware enforcement, additional host/firmware architectures
and broader distribution/image compatibility remain unqualified. Native execution of the
original Windows Rufus application is still deferred. A successful Linux or
Windows guest boot does not replace that upstream validation.


## Image checksums

Select an accepted `.img` or ISO image, then click the checksum icon next to Boot
selection. It computes MD5, SHA1 and SHA256; **Alt+H** in the main window enables
or disables SHA512 and saves that choice. The result dialog lets you select and
copy each value. No USB selection or authorization is needed.

**CANCEL** stops hashing and restores the window after cleanup. A source changed
since selection must be selected again. This operation reads the source and does
not write a USB. CP23b qualifies the worker, dialog, cancellation and notification
with normal/sanitizer and scaled-font tests.


## Compressed-image implementation status

Ordinary SELECT and confirmed DD creation support gzip (`*.gz`), XZ (`*.xz`),
BZIP2 (`*.bz2`), LZMA-Alone (`*.lzma`) and LZW (`*.Z`/`*.z`) in CP24e/CP25d/CP26d/CP27d/CP28d. Initial Unix-compress magic inside `.gz` uses LZW while retaining gzip filename association.
Analysis validates the complete retained stream and freezes actual decoded size
and SHA256 on a cancellable worker. START reads the selected device's logical
sector size, checks padding/capacity and revalidates source/geometry before the
single helper authorization. The helper independently reconstructs the plan,
checks source ownership, retains the real claim through copy/sync/readback and
checks closure before completion. READY waits for helper exit too. Confirmation
shows the written size and stored compressed size. Strict `55 AA` marker policy
remains; force-marker UI is pending. Distribution packaging includes the selected
zlib-compatible runtime; no external gzip executable is required.

Normal/sanitizer components and two real-kernel disposable guests qualify this
flow, including complete Qt creation, independent hashes/readback and BIOS
Syslinux/boot-sector execution. Guest UDisks/pkexec and front-end UID are fixtures;
physical media, host polkit, other real sector sizes and installed-OS/kernel boot
are not established. The full SELECT scan adds a fifth complete decode pass to
the four-pass helper, with bounded buffers and no expanded production image.
Later checkpoints add ZIP, Zstd, VTSI and fixed/dynamic VHD. VHDX/FFU,
differencing VHD parent chains and image saving remain in scope. See CP24e
through CP31g for the exact provider tests and limits.


The internal XZ decoder uses Rufus's original LZMA2 and host BCJ implementation,
retaining its 64-MiB dictionary limit and first-stream behavior. Native reading
validates CRC64/SHA256 checks that Windows skips and rejects reserved checks.
CP25b qualifies the native writer/helper protocol on private regular files;
CP25c adds 45 real guest-kernel jobs in normal and sanitizer builds, including
512-sector claims, dirty failures, readback and complete bootable-image restore.
CP25d enables ordinary XZ SELECT/USB creation through the same worker, frozen
plan and confirmed controller, with 64 analysis/63 focused Widgets results and
both 47-job normal/sanitizer guest runs including ordinary gzip/XZ creation.
Production uses existing zlib CRC32 and shared Rufus SHA256;
external xz/liblzma are independent test providers. Source tests count actual
source/output beyond 4 GiB. Guest BIOS Syslinux/menu/boot-sector execution is
qualified; physical media, other actual sectors, host polkit and installed-OS/
kernel boot remain unqualified.


CP26a adds an internal read-only BZIP2 source provider using Rufus's original
Huffman/BWT/RLE decoder. It validates block/combined CRCs, concatenated members,
64-bit output/source/tail counts, source identity and bounded per-call memory.
Unlike the original wrapper, it rejects a corrupt empty-member combined CRC.
Both normal/sanitizer source and regression suites pass, including real output
above 4 GiB. CP26d enables BZIP2 ordinary SELECT/USB creation through the
existing worker, plan and confirmed controller. Enabled creation is gzip/XZ/BZIP2.


CP26b qualifies BZIP2 frozen plans and the existing native writer/strict helper
on regular-file and simulated-native targets: 138 writer cases, 56 helper jobs,
33 protocol results and all 65 analysis results pass in normal/sanitizer builds.
The original decoder and claim/cleanup/controller bodies are unchanged.
CP26d enables ordinary BZIP2 SELECT/USB creation through the same controller;
the supported creation table above enables gzip/XZ/BZIP2.


CP26c adds real guest-kernel BZIP2 helper/claim/failure/full-restore/readback and
BIOS boot qualification. Both normal/sanitizer recipes pass 69 jobs, retaining
47 gzip/XZ jobs and adding 22 BZIP2 jobs. The complete disk is poisoned before
BZIP2's 64-MiB restore; independent guest/host/raw/gzip/XZ/BZIP2 hashes agree.
Production and the native fault probe are unchanged by that guest checkpoint.
CP26d adds ordinary BZIP2 SELECT/creation and joined READY. Guest metadata/
pkexec/UID fixtures, 512-byte sectors and tmpfs backing do not establish physical
media, host polkit, other actual sectors, persistence or installed-OS/kernel boot.


CP26d enables ordinary BZIP2 selection and confirmed creation. Both normal and
sanitizer runs pass 80 complete analysis/81 focused Widgets results, plus the
full-size BZIP2 controller/production-helper/11-job guest route, independent
readback/filesystem/files and BIOS boot. Both helpers are byte-identical to
CP26c's 69-job qualification. Results wait for sync, checked cleanup and joined
exit before full green READY and one alert. No new production decoder dependency.


CP27a adds an internal read-only LZMA-Alone source provider using the original
range/model/dictionary decoder, all 225 properties and original 1-GiB dictionary
cap. It counts actual 64-bit output, reports the original first-stream tail and
rejects known-size mismatch or unwritten history. LZMA-Alone has no container
checksum. Both normal/sanitizer source, real >4-GiB output/limit and admitted
format regressions pass. CP27d enables ordinary LZMA creation through the
qualified controller;
the supported table enables gzip/XZ/BZIP2/LZMA.


CP27b qualifies LZMA frozen plans, the existing native writer and strict helper
on private file/simulated-native targets. Normal/sanitizer runs pass 145 writer
cases, 60 helper jobs, 33 protocol results and 81 complete analysis results with
all admitted-format regressions. Original known-size/no-complete-EOS, dictionary
range and first-stream tails are preserved. Valid-stream hash substitutions,
format/identity/claim/sync/readback/close/cancel failures are checked.
CP27c qualifies real LZMA guest/kernel paths; CP27d adds ordinary controller/
READY qualification. Supported ordinary creation is gzip/XZ/BZIP2/LZMA.
No new runtime decoder dependency is added.


CP27c qualifies real guest-kernel LZMA helper/claims/source and dirty failures,
mounted protection/cancel, checked cleanup and complete poisoned 64-MiB restore.
Both normal/sanitizer 38-job runs pass with independent hash/filesystem/file and
BIOS checks. Actual sectors are 512 and host backing is tmpfs; metadata/pkexec
are fixtures, with real guest-root helper/kernel. This does not establish
physical media, desktop polkit, other actual sectors or persistence.
CP27d adds LZMA GUI creation; enabled SELECT formats are gzip/XZ/BZIP2/LZMA.


CP27d enables ordinary LZMA selection and confirmed creation with full retained
validation, actual target geometry, one helper and joined READY. Both builds
pass 96 complete analysis/99 focused Widgets and full-size 11-job GUI/helper/
readback/BIOS guests. LZMA-Alone retains its original dictionary/property range,
known-size/EOS/tail behavior and native size/history guards; it has no container
CRC. Production helpers are unchanged from CP27c. Physical/desktop polkit/other
actual sectors/persistence/installed-OS limits remain as above.


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


## CP29a — internal ZIP source qualification

The retained-source reader can inspect ZIP archives whose first central-directory
entry uses stored (0) or raw deflate (8), including bounded ZIP64 sizes. Both
normal and sanitized host suites pass; an actual >4-GiB decoded stream was
checked without retaining the expanded image. This reader does not enable ZIP
in SELECT or USB creation. Methods 12/14/95, writer/helper, real guest and
ordinary Qt confirmation/READY remain pending. The creation table above is
unchanged. See CP29a in the checkpoint log for exact evidence.


## CP29b — internal ZIP methods 12, 14 and 95

The retained-source reader also decodes first-entry BZIP2, LZMA and XZ ZIP
streams, with bounded slices and ZIP CRC. ZIP-LZMA handles EOS and no-EOS
archives. Both normal and sanitized host suites pass. ZIP still cannot be
selected for ordinary USB creation: decoded plans, writer/helper, real guest
and GUI confirmation/READY are pending. The creation table remains unchanged.
See CP29b in the checkpoint log for exact evidence and limits.


## CP29c — ZIP writer and strict helper foundation

The native writer now freezes the decoded ZIP SHA256, verifies the retained
archive again before writing, writes bounded padded sectors, syncs and checks
the target by readback. The strict helper packet permits ZIP raw device jobs,
and simulated claim/cleanup tests cover all five original entry methods on
owned files. Ordinary ZIP selection remains unavailable until the real guest
device route and confirmed Qt creation/READY are qualified. No physical USB
device was used. See CP29c for the test record.


## CP29d — ZIP guest device qualification

All five original ZIP entry methods now pass real guest USB helper writes and
CRC refusals. The guest also checks source/sector identity, exclusive claim,
cancel/sync/readback/close errors and a full 64-MiB bootable deflate restore.
Normal and sanitizer runs pass independent disk/file hashes and BIOS boot.
These are disposable 512-byte-sector QEMU devices; ordinary Qt ZIP selection
and confirmed READY remain unavailable. See CP29d for evidence.


## CP29e — ordinary ZIP creation

The image picker now offers `.zip` and `.ZIP`. A ZIP archive can be selected
for DD creation when its first central-directory entry decodes to a bootable
disk image with the `55 AA` marker and fits the target after sector padding.
The supported entry methods are stored, deflate, BZIP2, LZMA and XZ. The
ordinary Qt route freezes the decoded identity, confirms the selected target,
uses one authorized helper, and waits for verified sync/readback, cleanup and
helper exit before READY. Normal and sanitizer guest deflate restores pass
independent disk/file readback and BIOS boot. Guest coverage uses disposable
512-byte-sector QEMU media; see CP29e for evidence and limits.

## CP30d–CP31g — Zstd, VTSI and VHD creation

The ordinary picker also offers Zstd (`.zst`/`.ZST`), VTSI
(`.vtsi`/`.VTSI`) and fixed/dynamic VHD (`.vhd`/`.VHD`). Each route freezes
the decoded logical SHA256 before START, confirms the selected target and
uses one production helper with readback and joined READY. VTSI writes only
its listed sparse extents and preserves target gaps. Fixed/dynamic VHD writes
the reconstructed logical disk, including zero sectors. CP38 adds retained
differencing parent chains; see the current boundaries below.

Normal and sanitizer disposable QEMU guests pass full bootable restores and
independent disk/FAT-file readback for all three formats. VHD also passes a
small dynamic job with target tail preserved. These examples cover
512-byte-sector guest media, not every physical device or installed OS.
See CP30d, CP31d and CP31g for tests and supported boundaries.

## CP33 — fixed/dynamic VHDX creation

The picker accepts `.vhdx`/`.VHDX` for ordinary confirmed DD creation. The native
reader validates redundant CRC-32C headers/region tables, required metadata and
nonoverlapping BAT extents before exposing the logical disk. Fixed and dynamic
images use the existing retained source, frozen SHA256, single helper and
verified READY path. QEMU-generated 512-byte-sector images pass independent
source/native-write checks and ordinary Qt guest creation/readback/BIOS boot;
the source corpus also covers 4096-byte logical-sector metadata.

CP38 extends this reader to parent files and journal replay; the preview never
repairs or modifies a caller's source. The BAT extent-validation budget is 64 MiB; very large tables can be
unsupported even within the format's 64-TiB size limit. Real guest destination
coverage remains 512-byte logical sectors. See CP33 for the batch record.

## CP34 — full FFU at captured size

The picker accepts `.ffu`/`.FFU`. Supported inputs are full single-store V1,
single-store V2, and V1-compressed images with uncompressed, plain XPRESS,
LZNT1 or Huffman-XPRESS payloads (algorithm 0 through 4; CP39 extends CP34). The reader validates the security/image/store
roots, manifest dimensions, variable descriptor bounds and every SHA256 chunk,
including payloads later overwritten by final GPT data, before logical output.
It preserves descriptor order for repeated destinations, resolves end-relative
blocks at the manifest's captured size and reconstructs omitted blocks as zero.
The existing source identity, frozen decoded SHA256, target checks, authorization,
readback, sync, cleanup and joined READY rules apply.

CP40 separates logical analysis from deployment. Analysis still reconstructs
zero-filled gaps at the captured size and freezes both that decoded SHA256 and
the FFU metadata/payload hash-table digest. Installation writes the final mapped
extents only, preserving unlisted target bytes. End-relative runs are resolved
at the actual selected capacity, including capacities not aligned to an FFU
block. Overlapping beginning/end-relative runs retain descriptor overwrite
order. A smaller target is refused. The manifest and target logical sector
sizes must match.

On a larger GPT target, both captured headers and partition tables must pass
geometry, CRC and consistency checks before the first write. The primary and
backup header locations, last usable sector, backup table location, header CRCs
and protective MBR length are updated. Partition entries and filesystem sizes
remain unchanged. The backup table is copied to its new location. Old metadata
outside the final extents can remain in unlisted regions; this is not an erase
operation. GPT entry tables are bounded to 4 MiB in total. Entry sizes are
multiples of 128 bytes; count and entry size share this byte budget. CP50 removes
the separate 4096-entry and 1024-byte-entry limits for capture and relocation.

Windows Rufus invokes DISM `/Apply-Ffu`, which additionally deploys optimized
portable FFUs with eligible partition/filesystem resizing. Optimized resizing
remains parity work. This release qualifies ordinary full images on equal or
larger targets; it does not qualify optimized shrinking/expansion.

CP43 adds partial updates as described below; multiple stores return ENOTSUP. CP39 supports grouped
compressed descriptors up to 16 MiB decoded and multi-block end-relative runs;
an end-relative index identifies the run's first block and the run must fit
forward within the captured disk. Captured sizes must align to their 512/4096-byte manifest sectors. CP41
resolves overlapping beginning/end grids at the exact byte capacity, including
a final sector that falls inside an FFU chunk. Hash and
block chunk sizes must match and be powers of two from 1 KiB through 16 MiB.
Manifest data is limited to 1 MiB; descriptor/hash/map/payload-index allocations
each have a 64-MiB budget. SHA256 chunk checks do not authenticate a catalog
signer; catalog trust remains separate work. The source is never changed.

Independent Img2Ffu V1/V2 GPT captures reconstruct byte for byte, and an
independent XPRESS decoder agrees on extended matches. The focused corpus
covers native regular-file refusal/readback and 4096-sector metadata. Guest
coverage uses disposable 512-byte-sector media and synthetic FFUs carrying the
existing BIOS fixture; it does not establish Windows installation or physical
firmware compatibility. See CP34 for the combined batch evidence.

## CP38 — VHD/VHDX parents and VHDX log replay, preview 7

The original limits below describe CP38. [CP73](linux-device-source-operations.md#relocated-vhdvhdx-parents)
adds explicit parent relocation and merge-time alternate linkage; those cases
are now delivered.

The ordinary image picker and confirmed DD creation accept VHD differencing
images and VHDX parent chains. Each parent is opened once, retained read-only
through the complete helper job and checked for file identity, linkage and
matching virtual size/sector geometry. VHD also checks the recorded parent
modification time; preserve that timestamp when moving a VHD chain. A missing,
stale, cyclic or unreadable parent refuses the
job. The existing decoded SHA256 freezes the complete logical disk, including
inherited bytes. Native target protection covers every parent backing before
unmount and again after claim. All descriptors close before successful READY.

Dirty VHDX version-0 logs are validated for entry CRC-32C, circular sequences,
GUID/sequence linkage, descriptor/data-sector integrity and offsets. The newest
complete sequence is replayed in order into a private read overlay before
region, metadata or BAT interpretation. Zero ranges, overlapping updates and
virtual extension are supported. Source files, headers and logs remain unchanged.

Windows uses `OpenVirtualDisk` and a read-only `AttachVirtualDisk` in
`src/vhd.c:MountImage`. Linux reads these formats natively. Portable chains
must keep their valid Windows-style Unicode relative paths (`\` separators
and `..` work). VHD can fall back to its recorded parent filename. Linux does
not translate Windows drive letters/volume paths/UNC roots, prompt for a
replacement parent, update locator records or accept a symlink as the final
parent component. Keep each parent owned by the invoking user and owner-readable.
Unpaired UTF-16 surrogates are rejected rather than guessed as Linux filenames.
These pathname/ownership differences prevent ambiguous or privileged reads and
keep original images immutable; those Windows-specific relocation cases remain
in the parity backlog.

The chain limit is 32 files including the selected image. Existing 64-MiB BAT
validation budgets remain. Replay supports logs up to 64 MiB each, 128 MiB across
the chain and 16,384 active descriptors, with bounded checksum work and interval
maps. Unknown log versions, locator formats or `parent_linkage2` are unsupported;
malformed or unreplayable dirty logs fail before target writes. Real guest
qualification uses 512-byte destination sectors; component vectors also cover
4096-byte VHDX logical sectors. Full Windows parity remains in progress.

## CP39 — remaining FFU codecs and descriptor runs, preview 8

Full single-store FFUs now accept LZNT1 and Huffman-XPRESS as well as the
existing uncompressed/default/plain-XPRESS forms. The bounded native decoders
validate chunk headers, Huffman tables, bit/byte consumption, match history,
output lengths and cancellation. Huffman uses fresh tables at 64-KiB output
boundaries and preserves match history across chunks. LZNT1 uses 4-KiB chunks
and its changing displacement/length split. Optional end markers and legal
alternate long-length encodings are accepted; unused lookahead padding does
not form part of the decoded image hash.

Compressed descriptors can contain several contiguous blocks, and beginning-
or end-relative destinations can reference multi-block forward runs. Group
buffers are bounded to 16 MiB decoded, with a single cached decode per source
group. Repeated destinations and later overwrites keep their original order;
sparse gaps are zeroed. All metadata and decoded payload chunks are SHA256
checked before logical output, including overwritten payloads. The ordinary
frozen plan, verified DD, one helper and joined READY are unchanged.

FFU saving also accepts unnamed and valid Unicode GPT partition names.
The original GPT bytes, including UTF-16 names, remain exact in the captured
payload. The optional Name field is omitted from the ASCII full-flash manifest
for unnamed/non-ASCII names, following its optional metadata role; names are
never replaced in the GPT. INI line controls and malformed UTF-16 are refused.
Independent Img2Ffu readback verifies unnamed/Unicode captures. This does not
qualify Windows DISM optimization or resizing of those captures.

The combined normal/sanitizer guest route covers LZNT1 and Huffman grouped
buffers, end-relative runs, native readback and one full LZNT1 bootable
restore. Existing writer/helper failure evidence and CP35–CP37 source-acquisition
and transaction evidence are reused. Destination guest coverage remains
512-byte sectors. Portable/optimized resizing, partial base-image validation,
multi-store selection/deployment, catalog signer trust, additional capture
layouts outside the documented bounds and split SFU remain unfinished.

## CP40 — FFU deployment on larger disks, preview 9

The FFU writer applies the authenticated final sparse map at the selected disk
capacity, preserves unlisted bytes, handles partial overlaps between beginning
and end-relative block grids, and relocates valid GPT metadata without resizing
partitions. A generic `layoutSha256` request field freezes VTSI maps or FFU
container hashes. Source identity, one authorization, target claim, fsync,
readback, cleanup and READY after helper exit remain required. Physical FFU
write/readback counters count only final mapped bytes; progress uses target
capacity. Unsupported or inconsistent GPT enlargement fails before writes.

The grouped corpus covers codecs/layouts, 512/4096-byte GPTs, non-block-aligned
targets, frozen-layout refusal and the new transfer fault paths. The combined
normal/sanitizer guest route covers LZNT1/Huffman end-relative writes, larger
GPT deployment and one full bootable restore. Real 4Kn hardware qualification,
optimized resizing, partial/multi-store deployment, catalog signer trust and
remaining capture layouts stay pending. Split SFU handling is not exposed by
Windows Rufus's picker or `/Apply-Ffu` command and is not an additional Rufus
parity requirement; DISM's separate `/SFUFile` workflow is outside that UI.

## CP41 — exact-sector FFU capture and adaptive chunk sizes, preview 10

FFU saving accepts sector-aligned GPT disks whose capacities are not an FFU
chunk multiple. It captures the complete beginning-relative chunks, followed by
one full block anchored at the exact disk end. Overlap is intentional and its
source bytes must remain consistent. The source hash and copy progress count
each original byte once. No device sectors are padded or dropped. Logical
analysis resolves partial overlaps and zero gaps at the exact manifest size;
the existing target-aware sparse writer installs the result.

The fixed 128-GiB capture ceiling is removed. Capture starts at 128-KiB chunks
and increases by powers of two up to 16 MiB when needed to fit the existing
64-MiB descriptor, hash, map and payload-index budgets. Smaller valid disks use
a chunk that fits them. Capture sizing and output preflight share this planner.
The current dense encoder reaches 32 TiB minus 64 MiB on x86-64 under those
budgets; larger dense captures still return unsupported. Large-size planning is
qualified mathematically at its transitions; this release does not claim a
physical multi-terabyte capture test. CP46 adds LZNT1-compressed capture. Full captures
require valid GPT within the documented name/table/manifest limits.

CP50 captures manifests up to 1 MiB across as many chunks as needed. The image
header/manifest region and store/descriptor region are aligned independently;
every metadata chunk participates in the integrity table. Chunk selection accounts
for the maximum manifest before source acquisition, so destination space preflight
covers the actual capture. Empty GPT disks need no synthetic partition. Capture
and larger-target relocation accept entry tables up to 4 MiB, including more than
4096 entries and entries wider than 1024 bytes. Invalid usable ranges or tables
overlapping the primary header/data region are refused before publication.

One grouped normal/sanitizer corpus covers empty GPT, 1024 named partitions,
8192-byte entries, 32768-entry tables, 512/4096-byte sectors, independent chunk
hashes/reconstruction and equal/larger dirty-target readback. One combined guest
per build captures a kernel-read-only disk with an 8192-entry, 2-MiB GPT table
and a 136381-byte manifest, restores it through ordinary Qt, and performs one
representative complete restore/readback/BIOS boot. Windows DISM interoperability,
physical media, optimized resizing, multi-store deployment and signer trust
remain unqualified or unfinished.

The grouped normal/sanitizer corpus checks exact-sector streams/captures,
independent FFU reconstruction and hashes, equal-target readback, 512/4096-byte
sectors, 2/4/8/16-MiB chunks and planner boundaries. The combined guest captures
an unmounted kernel-read-only 8-MiB-plus-1536-byte block source through the native
encoder, restores it through ordinary Qt to a larger disk and checks GPT
relocation. One representative full FFU restore supplies disk/FAT readback and
BIOS boot qualification. Existing capture authorization/publication and shared
writer fault evidence are reused. Optimized resizing, partial/multi-store
updates, catalog signer trust, remaining capture restrictions, real 4Kn hardware
and complete Windows parity remain pending.

## CP43 — partial FFU updates, preview 12

Single-store V1, V2 and V1-compressed FFUs can now declare a partial update.
Their packed validation entries compare existing device bytes before any output;
a mismatch, read error, cancellation, or changed source/target fails before the
first write. Every declared comparison must pass. An image with no comparison
entries imposes no additional base constraints. Validation metadata remains
covered by the frozen layout hash and the existing SHA256 chunk checks.

The writer applies only the final mapped extents, preserving all omitted base
bytes. End-relative destinations use the selected capacity. Partial GPT bytes
are applied exactly as declared; full-image GPT relocation is not synthesized
for updates. The manifest minimum capacity and matching logical sector size
still apply. Readback verifies the new mapped bytes, without repeating old-base
comparisons that the update may have overwritten. Validated FFU containers do
not require an MBR boot marker, matching Windows Rufus's FFU dispatch.

One 61-case normal/sanitizer corpus covers all supported FFU versions/codecs,
512/4096-byte sectors, comparisons spanning transfer buffers, overlapping changed
bytes, late mismatches, malformed/integrity failures, source/target changes,
larger destinations and declared GPT changes. One combined guest per build
performs a full restore, wrong-base refusal with zero writes, and an ordinary
Qt sparse update. Independent whole-disk/FAT readback and one BIOS boot qualify
the shared writer; each Qt job uses one helper and waits for joined READY.
The existing source/claim/helper matrices are reused. Actual Windows DISM,
physical media and desktop authorization UI are not newly qualified.

Optimized partition/filesystem resizing, multiple stores, catalog signer trust
and remaining capture layouts remain in the FFU backlog. Full Windows parity
remains active.

## CP44 — image selection and explicit marker override, preview 13

Use SELECT, drop a local image onto the window, or provide one startup path:
`rufus-linux -i /path/to/image`, `--iso=/path/to/image`, or a positional filename.
The desktop launcher also accepts a file. Multiple dropped files select the
first, following Windows Rufus. Drops use copy semantics and retain the source;
remote URLs, directories and move-only transfers are refused. Writes and image
saving/hashing retain their existing busy gates. A new drop during analysis
replaces the pending selection through the existing cancellation/request logic.

Alt+M toggles and saves `IgnoreBootMarker`, matching Windows. It applies to the
next selection; accepted images keep their frozen choice. The override allows
raw or decoded DD data without `55 AA`, while retaining whole-sector, geometry,
capacity, WIM exclusion, source identity, authorization and readback checks.
It does not establish bootability. A valid ISO retains its usual ISO/DD policy.
Ordinary raw recognition also accepts `.usb`, `.wic` and other suffixes when the
inspected bytes have a marker. A marked non-ISO `.iso` is a raw image, rather
than an ISO tree. The shared decoder now recognizes `.bzip2`, an extension
already offered by the Windows picker. WIM/ESD selection remains visible but
its standalone creation workflow is unfinished.

Focused tests cover Unicode/space paths, first-file drop behavior, saved and
frozen choices, raw and compressed selection, busy/remote/move-only refusal,
and strict typed helper packets. One combined normal/sanitizer guest route
checks default raw/compressed marker refusal with zero writes, explicit raw and
gzip overrides with full-device readback, and one ordinary bootable raw restore.
The shared helper/source/claim matrices are reused. One Qt regression gate and
native/generic Preview 13 packages complete this batch. FFU resizing, multiple
stores, catalog trust and the rest of the parity backlog remain active.

## CP45 — full and fast device zeroing, preview 14

Select the target device, then use **Alt+Z** to write zeros to its complete
capacity or **Ctrl+Alt+Z** for Rufus's fast-zero mode. An input image is not
required. The normal destructive confirmation and any multiple-partition
warning appear before one authorization. Existing source selection is retained
for a later creation job. These commands clear the layout without creating a
filesystem; Quick format and bad-block testing remain separate unfinished work.

Fast mode follows Windows `WriteDrive`: compare 32-MiB regions, leave a region
unchanged when it consists entirely of zero bytes or entirely of FF bytes,
otherwise fill it with FF. After a nonblank region, write the next 15 regions
without comparison before testing again. Partial final regions use the exact
device capacity. Full mode always writes zeros. This is ordinary device writing,
not discard or a secure-erasure command.

The authorized helper retains exclusive access through writes, sync and
readback of the complete result, then rereads the partition table and closes
its descriptors before successful READY. Fast mode verifies skipped regions too;
its physical write/readback counters can be zero for an already blank disk.
Progress still covers the full device and holds at 99 until helper exit.
Cancellation, sync, readback, rescan or cleanup errors remain unsuccessful.
Eligible mounted targets use the existing private unmount flow; devices are
not automatically ejected or remounted. Source/home/system and target identity,
read-only and native geometry protections remain in force.

Native transfers use bounded 1-MiB I/O and 2-MiB buffers. Fast verification keeps
one byte per 32-MiB region, within a 64-MiB map budget (2 PiB device capacity).
Full zeroing has no map requirement. The existing native 512..65536-byte sector
range applies; the combined guest qualification uses 512-byte sectors.

One 38-case normal/sanitizer corpus covers exact tails, zero/FF/mixed regions,
the 15-region backoff, short/interrupted I/O, cancellation, changed capacity,
sync/read/write faults and verification mismatch. Focused GUI/protocol checks
cover source-free strict packets, shortcuts and joined completion with zero
physical writes. One combined guest per build clears mixed and blank devices,
performs full zeroing, then restores a representative bootable disk for
independent whole-disk/FAT readback and BIOS boot. Existing shared source/claim
matrices are reused. Physical devices and desktop authorization UI remain
unqualified by these generated fixtures.

## CP46 — compressed FFU saving, preview 15

Saving `.ffu` now produces a full single-store V1-compressed image using LZNT1.
Each 4-KiB compression chunk falls back to its native literal form when that is
smaller. The encoder uses bounded buffers, checks cancellation between chunks,
and keeps the existing adaptive 1-KiB–16-MiB FFU block planner and native table
budgets. Output preflight uses a worst-case bound including literal headers;
actual output can be much smaller. The sparse 8-MiB fixture produces about
0.40 MiB. All zero bytes have explicit restore mappings, so a dirty destination
receives the complete original content. This replaces CP35/CP41's uncompressed
capture implementation; uncompressed FFUs remain readable.

GPT names with control characters or malformed UTF-16 are retained exactly in
the captured sectors and omitted from optional text-manifest name fields.
Valid GPT/table geometry, overlap checks and the current manifest/table budgets
still apply. Capture retains original data in allocated and unallocated sectors;
it does not exclude free filesystem clusters, change partitions, optimize an
FFU for smaller media, or sign its catalog. LZNT1 follows
[Microsoft MS-XCA](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-xca/5655f4a3-6ba4-489b-959f-e1f407c52f15).
Windows Rufus uses DISM's default compressed capture; see
[DISM Capture-FFU](https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/dism-image-management-command-line-options-s14#capture-ffu).

The batch checks normal/sanitizer encoding, independent Python/Dissect decoding,
512/4096-byte exact-sector capture and dirty restores, buffer bounds, cancellation,
source changes and atomic-output failures. Each combined QEMU route captures a
kernel-read-only source, verifies production compressed output through ordinary
Qt restore onto poisoned media, checks GPT relocation and untouched larger-target
gaps, then qualifies one representative full restore with FAT readback and BIOS
boot. Windows DISM interoperability, physical media and desktop authorization UI
are not newly qualified. Optimized resizing, multi-store deployment and catalog
signer trust remain unfinished.

## CP47 — full/quick formatting, preview 16

Uncheck **Quick format** for a full format on eligible native filesystems. The
choice is frozen before confirmation and stays locked through completion.
DD images clear/disable this control and restore the previous choice afterward.
FAT32 on disks larger than 32 GiB stays quick-only, matching Windows Rufus.

Full FAT16/FAT32 zero and read back the exact volume extent under an exclusive
partition claim, then hand off to the native formatter for metadata. This
includes a final odd sector. Bytes outside that volume are preserved.
Any zero/write/readback error fails the job; Windows's native formatter may mark
unusable clusters and continue. Refusing an incompletely verified clear keeps
Linux READY meaningful. The separate bad-block/fake-capacity workflow is still
pending. Native `mkfs.fat -c --offset` also has a nonzero-offset seek comparison
issue; the bounded clear/readback avoids that path. See
[dosfstools source](https://github.com/dosfstools/dosfstools/blob/master/src/mkfs.fat.c).

NTFS requests native full mode explicitly when the provider offers `--full`.
Legacy NTFS-3G's full default is selected by removing `-Q`, after bounded help
inspection; unknown quick-only defaults are refused before partitioning.
See [NTFS-3G mkntfs](https://github.com/tuxera/ntfs-3g/blob/edge/ntfsprogs/mkntfs.c).
exFAT uses `-f`, retaining native write verification and no discard. ext3/ext4
use `lazy_journal_init=0` for full mode, including persistence; quick mode uses
1. Native allocation may initialize a quick journal too. ext2's full and quick
modes are equivalent. Full ext formatting does not wipe every free data block,
matching Rufus's shared ext formatter behavior.

Full jobs can run longer than the old 120-second limit. Cancellation, child
termination/reaping and final synchronization still apply. Completion requires
metadata checks, private mount cleanup and helper exit. Full format covers the
selected filesystem volume; it is not a physical secure-erase operation.

Qualification combines 31 full/quick/extent/fault cases per normal/sanitizer
build, the previous native formatter/zero regressions, strict protocol/Widgets
choice tests, one Qt gate and one combined guest per build. Guests exercise
FAT16/FAT32/NTFS/exFAT through complete single-helper jobs, ext2/ext3/ext4 on real
virtual partitions with independent checks, then one bootable restore with FAT
readback/BIOS boot. Actual NTFS-3G full formatting, physical media and real desktop
polkit interaction are not newly qualified. Other parity work remains active.


## CP48 — bad-block and fake-capacity testing, preview 17

Enable **Check device for bad blocks** in the advanced format options. Choose
1 pass, 2 passes or 4 passes (SLC, MLC or TLC). These are the original Rufus
pattern tables. The option also works with DD images and device clearing;
Quick format is independent. Testing destroys the device contents before media
creation, so it can take much longer than copying a small image.

Fake-capacity detection is enabled by default. **Alt+B** toggles and saves that
explicit choice. First-pass block identifiers expose devices that wrap writes
onto a smaller physical range. Later passes retain the original pattern bytes.
There is no host-derived setting. A partial FFU requiring existing base bytes
cannot be combined with this destructive test; clear the checkbox to apply it.
This incompatibility is refused before target access or writes.

The test uses 512-KiB blocks, 32-MiB transfers and 64 MiB of data buffers. Each
write pass is flushed before aligned direct-I/O readback. The original exclusive
whole-device claim stays held through the test and the error dialog. Up to 256
unique failing blocks are recorded with their first read, write or corruption
error. **Abort** cancels creation, **Retry** repeats the test, and **Ignore**
continues creation. These choices keep the same authorized helper. Closing the
dialog aborts. A report is stored in the current user's home directory as
`rufus_TIMESTAMP_RANDOM.log`; clean logs are deleted and failed reports remain.
The privileged helper never chooses or opens the report path.

Metadata cleanup follows Rufus's ClearMBRGPT ranges after a test writes data,
including cancellation: a bounded prefix and tail are cleared, flushed and read
back before releasing the claim or creating media. The other test patterns may
remain on an aborted device, as on Windows. Cleanup or sync failure remains
FAILED; successful READY still waits for creation, cleanup and helper exit.
There is no automatic eject or remount.

Two deliberate correctness differences are retained: Linux tests an exact-sector
partial final block rather than dropping it, and TLC retry data stays aligned
to its original logical transfer grid. Restarting a three-byte pattern at every
retry boundary can otherwise misidentify healthy neighboring blocks. The
ordinary successful pattern stream matches Windows's transfer grid.

Qualification uses one grouped 53-case normal/sanitizer corpus for all patterns,
short/odd tails, alias detection, bounded error recovery, cancellation, sync,
cleanup and FFU base requirements. Focused protocol/Widgets checks cover choices,
report logs, stale/duplicate messages and decision lifecycles. One combined guest
per build uses real direct block I/O for fake-capacity detection, Abort/Retry/Ignore,
claim retention and signature cleanup, then a clean TLC test through the ordinary
Qt creation path with full disk/FAT readback and one BIOS boot. The full Qt gate
runs once for the batch. Actual defective/fake hardware, desktop polkit dialogs
and physical persistence are not newly qualified. Other parity work remains active.


## CP49 — extended label/icon files, preview 18

**Create extended label and icon files** is enabled and checked by default,
matching Windows Rufus. It applies to supported FAT16/FAT32/NTFS/exFAT creation,
including SFD and existing boot/ISO/DOS routes. DD and ext clear/disable the control
and restore the previous choice when eligible settings return. It is independent
of Quick format and device testing. No new authorization or public desktop mount
is added.

The job freezes the original user text before normalizing the filesystem label.
It writes `autorun.inf` with a UTF-16LE BOM and Windows CRLF lines, referencing
`autorun.ico` and the original label. The icon is Rufus's original Windows icon
resource. As in SetAutorun, the display label is bounded to 127 UTF-16 units;
Linux drops an incomplete trailing surrogate. Single-line valid Unicode is required.
This metadata supplies the extended name and icon in Windows Explorer. CP65 adds existing mounted autorun-label discovery to the Linux device list;
see [device discovery](linux-device-discovery.md) for matching and fallback rules.

Any existing case-insensitive `autorun.inf` entry is kept, together with the
image's icon. Linux inspects the entry without following links. If metadata is
absent, a staged icon replaces the icon directory entry without following an
image-controlled symlink or modifying an external hardlinked file. A newly
created metadata file is removed if its write or icon publication fails. Metadata
and icon writes are flushed and read back. Unprivileged UI code supplies only the
label; the helper chooses fixed media filenames and its compiled icon resource.

The existing private mount is reused for image/DOS copying. Format-only jobs use
a checked private mount before finalization; whole-device SFD and exFAT mounting
are now integrated. NTFS retains the preferred new kernel driver and native
NTFS-3G fallback. An older system without either read/write provider must uncheck
the option for format-only NTFS creation or install the fallback provider. The
job retains one authorization through sync, unmount and helper exit. It does not
eject or remount the device on the desktop.

Windows SetAutorun reports errors without making them a creation failure. Linux
fails the requested job if metadata creation, readback or synchronization fails,
so READY still proves completion of the selected work. Existing image/DOS copied
byte counts remain unchanged; the helper reports appearance completion separately.

Qualification uses one grouped 30-case normal/sanitizer corpus for encoding,
original icon bytes, preservation, links, short transfers, cancellation, failed
writes/readback/sync/publication and temporary-file cleanup. Focused Qt checks
cover default/eligibility, frozen text and refusal of a successful result without
appearance proof. One combined guest per build performs FAT16/FAT32/NTFS/exFAT
creation on partitioned and whole-device SFD media, independently mounts the
filesystems for readback, exercises both actual NTFS providers and an ordinary
Qt default-option job, preserves uppercase image-owned autorun content, then
runs one full ordinary restore with independent FAT readback and BIOS boot.
The FUSE selection branch is tested by a link-only old-kernel release stand-in;
the FUSE process and its device I/O are real. Guests use disposable 512-byte-sector
media on Linux 7.2. One Qt regression gate and both inspected packages form the
batch. Windows Explorer rendering, physical media and desktop polkit dialogs are
not newly qualified. Other Windows parity work remains active.


## Old-BIOS compatibility (CP51)

Expand advanced drive properties and select **Add fixes for old BIOSes (extra
partition, align, etc.)** for a BIOS-bootable MBR job. The option starts unchecked.
It is disabled and effectively unchecked for GPT, SFD, Non bootable, standalone
UEFI:NTFS and DD-only images. Returning to an eligible mode restores the session
choice. Selecting or changing it is disabled while busy.

Windows reference: `EnableOldBiosFixes` in `src/rufus.c`, `CreatePartition` in
`src/drive.c`, and `GetExtraPartitions` in `src/format_policy.c`. Linux uses the
shared `GetPartitionStartOffset` and main/trailing placement arithmetic. With
512-byte sectors and 63-sector tracks, the start is twice a track rounded up to
the cluster size: 64512 bytes with 512-byte clusters, or 65536 with 4096-byte
clusters. The ext Default sentinel retains the shared 64512-byte placement.
The main partition ends on the original track/cluster boundaries.

An inactive final 0xEA partition occupies one track. UEFI:NTFS takes precedence
over that extra partition when required, while old-BIOS starting alignment still
applies. Persistence stays between the main partition and the final partition.
The choice is frozen in the strict device request and reaches each native layout,
formatter, bootloader, UEFI payload and final WinPE record check. Non-device,
non-BIOS and malformed requests refuse before writing. Existing source protection,
one authorization, exclusive claims, sync/readback, cleanup and READY-after-exit
rules remain in force.

GRUB2/GRUB4DOS payloads must fit before the first partition. Windows also refuses
an oversized SBR (`src/format.c:WriteSBR`); Linux checks the smaller gap before
partitioning. Disable old-BIOS fixes if the selected core cannot fit. This option
changes the media layout; it does not certify compatibility with every old BIOS.
The native 512-byte-sector and 2-TiB MBR limits remain.

CP51 groups 25 file cases for original Syslinux, Rufus, Windows 7, GRUB2, GRUB4DOS
and WinPE-masqueraded MBR choices across supported filesystems. Independent raw
entries check the active main partition, inactive one-track compatibility
partition and exact placement. Cases cover UEFI:NTFS/persistence priority,
compatibility-entry corruption/readback and non-BIOS prewrite refusal. Existing
MBR clearing/I/O, GPT, UEFI payload and SBR matrices are reused once per build.
Combined normal/sanitizer guests check ext2/ext3 on a separate disposable block
device; complete FAT16/NTFS GRUB2 and FAT32 Syslinux helper jobs; an ordinary Qt
FAT32/GRUB2 job with the checkbox enabled, frozen choice, one helper and joined
READY; and independent boot of that legacy-layout GRUB2 disk. One representative
ordinary raw restore retains full FAT/disk readback and BIOS boot coverage.
Actual old physical firmware, other sector geometries, each Windows/FreeDOS ISO
boot path and desktop authorization UI are not newly qualified.


## ISO → ESP (CP52)

For a supported image with regular EFI files and projected content strictly below
1 GiB, select GPT and FAT16 or FAT32, then choose **ISO → ESP** after START.
**ISO Image** stays recommended and selected by default. The original ISOHybrid
ISO/ESP/DD ordering remains; eligible non-hybrid images also receive the original
two-row ESP question. Persistence suppresses DD while retaining the ESP choice,
matching Windows' separate question. Forced-DD images remain DD-only. Source
eligibility is checked again from the retained image before any writes.

The main GPT partition becomes **EFI System Partition**, with type
`C12A7328-F81F-11D2-BA4B-00A0C93EC93B` and zero attributes. It uses the normal main
partition extent, with optional ext persistence afterward. UEFI:NTFS is not
added. MBR, SFD, NTFS/exFAT and source-free jobs cannot request this mode. Existing
native ISO architecture, filesystem, sector and capacity limits still apply.
The choice does not certify arbitrary firmware or Secure Boot.

Windows sets the formatter allocation unit to Default in this mode, after
partition layout has used the selected unit. Linux keeps those two decisions:
layout retains the selection; formatting uses the FAT16 default at actual volume
size or the original `FormatLargeFAT32` default table, now shared without changing
the Windows calculation. FAT32 ESP always uses quick formatting, including when
the form's quick checkbox was off. FAT16 retains its requested full/quick mode.
Confirmation identifies ISO → ESP and the actual default allocation unit. The
form's existing choices remain available for the next job. An invalid native FAT
geometry refuses before writing; Windows' large-FAT32 formatter also rejects
insufficient cluster counts after choosing the default.

The ordinary helper retains target/source protection, authorization, unmounting,
formatting, file copy, optional persistence/configuration, extended label/icon
handling, readback/sync, private-mount cleanup and joined completion. A mode choice
or copied 100% does not produce successful READY.

CP52 groups 17 file cases for data/ESP GPT types/names/attributes, both GPT CRCs,
persistence and selected layout units, an invalid UEFI:NTFS combination, fresh
regular-EFI eligibility and actual formatter defaults. Shared policy tests cover
size/EFI/GPT/FAT boundaries, non-hybrid/hybrid and persistence choices; the FAT32
default table is checked on each boundary. Eight normal/sanitizer component
suites reuse existing FAT, GPT, source and configuration coverage. Focused strict
protocol and Widgets tests cover mode/type refusals, freezing and held completion.

Each combined guest performs four ordinary Qt creations: FAT16 and FAT32, each
with and without 256-MiB ext3 persistence. On a 2-GiB disposable USB, FAT16 keeps
64-KiB layout units but formats with 32-KiB defaults; FAT32 keeps 1-KiB layout
units but formats with 4-KiB defaults. Independent raw GPT/BPB checks and read-only
mounts compare every original file, plus the required persistence configuration.
The final FAT32 ESP/persistence media receives independent host file readback and
a representative x64 OVMF UEFI application handoff. This is not a new Windows
installation, a physical-media qualification or proof of all EFI architectures.


## FFU store selection (CP53)

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


## FFU catalog signatures (CP54)

Preview 23 checks a present FFU catalog before offering its stores or writing a
device. It accepts unsigned catalogs and valid test signatures, including
self-signed or expired test certificates. Invalid signatures and the known
production certificate chains rejected by Windows desktop DISM refuse before
writes. This policy does not establish a trusted publisher. Metadata/payload
chunk hashes, retained-source checks and the frozen catalog identity still
apply. Linux retains catalog-free input compatibility for existing unsigned FFUs;
Windows normally expects an unsigned CTL even without a signer. No host certificate store, online certificate retrieval or revocation
lookup is used. Catalogs are bounded to 16 MiB; saved FFUs remain unsigned.

One grouped normal/sanitizer corpus and independent catalog signer verify the
policy. A genuine Microsoft production CAT refuses before writes. Combined
guests exercise signed multi-store FFUs, ordinary Qt authorization, held-99 and
READY after helper/launcher exit, independent disk/file readback and one BIOS
boot per build. Optimized filesystem/partition resizing and automatic mapping
to additional devices remain pending. Complete Windows parity remains active.

## Optimized FFU deployment (CP55)

Optimized full FFUs can now be restored to a smaller or larger device when it
meets the minimum size shown after analysis. The image's designated NTFS
partition grows to fit; following partitions and recovery locations are updated.
The confirmation shows both captured and minimum sizes. The helper validates
all source/layout metadata before writing, verifies restored sectors, completes
filesystem/recovery maintenance and cleanup, and only then reports READY.

Install `ntfsprogs-plus` (preferred) or `ntfs-3g` for `ntfsresize`. Recovery updates
use the new kernel `ntfs` driver or the existing NTFS-3G fallback. Captured NTFS
volumes inspected during finalization must be clean; corruption/dirty checks are
never bypassed to mount them. The resize utility's own Windows first-boot
consistency-check flag is retained. Optimized geometry uses 128-KiB FFU blocks;
512/4096-byte source sectors are supported by the planner. This batch's real
filesystem/guest qualification used 512-byte sectors and the kernel driver;
existing fallback qualification is reused. CP56 corrects the earlier mapping
requirement in the [FFU audit](linux-ffu-parity.md). See [CP55](linux-port-plan.md#cp55--optimized-ffu-deployment-preview-24-2026-10-20).

## FFU closeout (CP56)

Preview 25 saves full GPT FFUs with plain XPRESS compression (format 3), matching
the checked Windows desktop provider. Existing LZNT1 inputs remain supported.
Capture preserves zeros and GPT bytes, verifies the complete image and publishes
atomically; source acquisition and completion rules are unchanged.

The [FFU parity audit](linux-ffu-parity.md) closes feature implementation and
corrects older statements about automatic multi-target/UEFI-LUN mapping. The
checked Windows Rufus path writes one selected disk; its desktop provider
rejects multiple stores. Linux retains explicit source-store selection.
Documented resource/geometry bounds and physical hardware/other-provider
qualification limits remain. No Windows application execution is claimed.


## Advanced device selectors (CP71 / Preview 40)

Alt+G controls recognized attached virtual disks (enabled by default); Alt+W
opts into VMware disks. Both choices are saved. Ctrl+Alt+F enables non-USB
removable drives for this session and restores the previous HDD choice when
disabled. Native MMC cards follow the existing default card-size filter.
Every new class uses fresh native identity and protected-use checks in the
single authorized job. Read-only devices retain capture-only access. See
[native discovery](linux-device-discovery.md#advanced-device-classes-cp71--preview-40)
for recognized models and safeguards. [CP73](linux-device-source-operations.md)
adds file-backed loop/NBD discovery and operations; other providers remain open.
