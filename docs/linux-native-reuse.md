# Native Linux implementations before API adaptation

CP74 implements source-matched Syslinux resource selection using the existing
patcher and native providers, with private portability/resource adapters. See
[the ISO/EFI resource guide](linux-iso-efi-resources.md). The dated audit below
records the earlier reuse investigation.

Updated: 2026-09-29. Rufus baseline:
`18ae93bf37f94764d77216cff4168e0549a14c4d`.

Prefer an existing native Linux implementation of an operation when it meets
Rufus's requirements. Keep Rufus's media detection, version selection and
workarounds shared. A small API adapter is useful for the remaining gaps; it
should not cause us to port a Windows implementation of work already available
on Linux. Compiling Rufus's bundled portable C on Linux also counts as native
reuse, and can preserve changes absent from distribution packages.

## Syslinux: a concrete candidate

The upstream project supplies Linux `syslinux`/`syslinux-mtools` and `extlinux`
installers. They install the bootloader and update filesystem boot information;
the former use an unmounted FAT filesystem, while `extlinux` takes a path on a
mounted supported filesystem. See the pinned
[upstream installer documentation](https://kernel.googlesource.com/pub/scm/boot/syslinux/syslinux/%2B/refs/tags/syslinux-6.03-pre20/txt/syslinux.txt).
These interfaces make native installation worth testing before adapting Rufus's
Windows volume, file-extent and sector-writing code.

However, `src/syslinux.c:InstallSyslinux` does more than invoke a single fixed
installer: it selects embedded v4/v6 or image-specific downloaded `ldlinux`
payloads, sets variable sector geometry, handles FAT and NTFS differently,
and installs accompanying modules. `src/syslinux/libinstaller/syslinux.h`
exposes those selected payload buffers and variable sector sizes. A host
installer cannot be assumed to use Rufus's chosen payload or support every
filesystem/sector combination.

The preferred sequence for the installation checkpoint is:

1. Test a native installer on a disposable FAT image, with matching loader and
   module versions. Verify installed files, boot-sector changes, and booting
   under QEMU using a small controlled boot fixture.
2. Make the supported version/filesystem/sector constraints explicit. Check
   legacy image modules, image-specific payload selection and NTFS separately.
3. Where the installed tool cannot meet those requirements, evaluate a matching
   native Syslinux build or reuse Rufus's bundled `libinstaller` algorithms with
   Linux I/O. Do not replace the image's module set with an unrelated host set.

This changes the earlier assumption that porting the bundled installer and
`libfat` I/O must be the first approach. They remain available fallbacks and may
also serve other image-inspection operations.

## Other operations

| Operation | Preferred Linux treatment | Rufus behavior to retain |
| --- | --- | --- |
| FAT/NTFS/exFAT/ext filesystem creation | Native filesystem tools/services, through the planned privileged operation boundary. | Filesystem choice, labels, cluster sizes and boot-media constraints. |
| WIM inspection, extraction and splitting | Native wimlib API or its tools. | Installer selection, large-file flags, splitting decisions and destination naming. |
| ISO/UDF reading | Bundled libcdio compiled natively for now. | Rufus's synthetic El Torito paths and other verified reader changes. |
| Bootloader assets and configuration | Reuse the selected image/Rufus assets and shared rules. | Architecture, version compatibility, configuration patches and boot quirks. |

wimlib supplies both a cross-platform C library and command-line tools for WIM
operations, including splitting. This was already planned for Linux and remains
preferable to carrying the Windows WIM interface over. See the
[wimlib project documentation](https://wimlib.net/).

The libcdio comparison is an example where substituting the installed native
library loses behavior: the tested system 2.4.0 reader did not resolve Rufus's
synthetic `[BOOT]` path, while the bundled source compiled natively did. The
[comparison probe](../linux/probes/libcdio/README.md) records that evidence.

On this host, `mkfs.fat`, `mkfs.ext4`, `mkfs.ntfs` and `bsdtar` were found on PATH;
`syslinux`, `extlinux` and `wimlib-imagex` were not. This is an availability check,
not installation or functional validation. No packages or bootloaders were
installed and no devices were formatted during this checkpoint.

CP3c4 now shares Syslinux version recognition in `src/syslinux_version.c`, tested
both directly and through generated ISO reads. This preserves the version and
build suffix needed to assess payload compatibility. It does not select or run
the native installer. CP3c7 now shares configuration/loader selection and the
embedded-v4/v6 numeric fallback in `src/syslinux_scan.c`. Presence and detection
are explicit report fields, while the selected version and inferred flag record
the separate installation choice. Unknown detection alone does not reject the
image. Component tests and seventeen generated ISO files cover the selection
policy. Native installation must still establish that the chosen payload and
filesystem/sector requirements can actually be supported. See the [agreed plan](linux-port-plan.md#syslinux-detection-and-installation-selection--agreed-2026-09-13).
CP12a now qualifies a native CMake build of the unchanged upstream 6.04-pre1
mtools installer. Its `.sys` and `.bss` match Rufus's bundled v6 resources exactly;
the build pins those hashes and the same-release `ldlinux.c32`. Three FAT32
file-image installations and two BIOS volume boots pass. No root, mount or
physical USB is involved. A valid 4096-byte-sector volume is rejected unchanged.
See the [probe, provenance and limitations](../linux/probes/syslinux/README.md).

The native route is viable for this pair, but it is not yet a drop-in replacement:
it embeds fixed payloads, omits the build suffix from `--version`, moves both
loader files with `--directory`, and invokes mtools through internal shell
commands. Rufus keeps `ldlinux.sys` at the root and independently selects its
base directory and modules. CP12b now checks the selected build or inferred
fallback against exact companion bytes and qualifies the shared root-config
redirect in four extracted ISO boots, including real menu modules and relative
paths. The native root installation adds its matching root `ldlinux.c32`; image
modules remain unchanged. Existing root companions must also match before
installation. Unknown detection stays separate from the inferred installation
choice; an unrelated host module set is still not a substitute. CP12c adds an
explicit grouped-installer mode to the existing command runner. A dedicated
worker retains the target, supplies a private environment/directory, adopts
orphaned mtools descendants, stops and reaps the group before final sync, and
rejects success with leftover children. This is qualified with disposable files,
failure injection and the four extracted-ISO boots. The actual native invocation
is still probe-only. CP12d now checks initial config/search-directory eligibility
using the retained ISO/UDF view, shared extraction rules and exact selected/root
companions. It rejects routes whose first config differs, while allowing harmless
extra configs; it does not interpret arbitrary config commands or certify every
referenced module. Source identity and general extraction checks remain required.
CP12e combines source/target checks, supervised installation and cleanup in a
regular-file adapter. It reuses bundled libfat and Rufus's Syslinux patcher to
reconstruct the installed loader/ADV and VBR from the actual sector map, then
compares them byte-for-byte. It also verifies the companion, original BPB and
unchanged backup VBR. Six shared integer helpers now use memcpy for defined
unaligned access after UBSan found the original casts; the patching algorithm
and Windows build remain unchanged. Optimized native loads/stores are unchanged.
CP12f adds MBR file-image creation and verification in the existing libfdisk
partition component. Partition placement reuses the shared functions; the 440-byte
Syslinux MBR payload is included directly from the same ms-sys header as Windows.
It also equals the pinned upstream 6.04-pre1 MBR, despite the header's 6.02 comment.
No second bootstrap implementation or full ms-sys Windows I/O adapter is needed.
One active FAT32 LBA partition and two whole-disk fixture boots are qualified.
The native library supplies DOS table serialization/CHS; Win32 runtime and legacy
CHS-only BIOS qualification remain deferred. CP12g moves the unchanged native
installer build into a shared Linux CMake directory, used by both the main build
and standalone tests. The optional `SyslinuxRuntime` component packages a private
`rufus-syslinux` with the exact pinned payloads, license and provenance notice.
Two whole-disk boots pass using the staged executable. Privileged helper ownership,
old-metadata cleanup and representative distribution booting remain before enabling BIOS.

## USB HDD classification (CP17d4)

Reuse the original `IsHDD` score and `hdd_vs_ufd.h` tables rather than introducing
another Linux HDD/flash heuristic. Its production path uses device facts and
string/ID rules, not SMART commands. A small native sysfs/libudev facts adapter
can replace Windows enumeration without needing Wine or a new SMART library.
See the [audit and extraction boundaries](linux-hdd-classification.md), including
original-function reference tests, card exceptions, known/unknown facts and the
requirement that default-visible fixed media still uses protected acquisition.


## FreeDOS resources, boot records and locale policy (CP19a–d2a)

Reuse the 27 checked-in FreeDOS files, including Rufus's FORCELBA-enabled kernel,
and the existing ms-sys FAT16/FAT32 boot-record routines. A test-only native
stdio adapter compiles and exercises those routines unchanged, without the
Windows ms-sys I/O module. The complete payload inventory is 400,604 bytes.

The [FreeDOS reuse audit](linux-freedos-reuse.md) records the MBR choice,
primary/backup FAT write ranges, label/attribute rules and locale split. CP19b
qualifies the bounded native adapter on formatted regular files, reusing both
the unchanged boot writers and existing FAT geometry/descriptor validation.
It stages both FAT32 copies before writing and flushes/verifies the retained fd.
The directory label and all bytes outside the boot prefixes remain unchanged.
The existing native formatter, partition writer, private mount and one-helper
lifecycle remain the integration points. Share DOS resource/locale data and
content generation when needed; keep Windows host keyboard queries and file I/O in
small platform adapters. There is no reason to introduce another formatter,
a general Windows API layer or a separate FreeDOS job framework.

CP19c1 extracts the DOS tables, driver/code-page selection and CONFIG/AUTOEXEC
writers into `src/dos_locale_policy.c`. The original Windows host wrapper now
uses it, and Linux compiles the same source with standard binary FILE streams.
The native comparison covers both FreeDOS and MS-DOS bytes, preserving driver
order, the 850-to-858 upgrade/fallback distinction, CRLF and OEM menu characters.
The shared EGA12 filename correction references the actual bundled `.CPX` file.
CP19c2 shares the resource order, destinations and progress milestones in
`src/freedos_files.inc`. Windows derives its resource IDs from that list; Linux
CMake embeds the same existing files. The native copy component uses kernel
FAT attribute ioctls and the existing empty-directory guard, moved unchanged
into `target_io` for both it and ISO extraction. Real kernel FAT16/FAT32 copying
and independent payload/attribute readback pass. CP19c3a uses the same native
exclusive write/sync/readback path to publish the shared startup writers' bytes.
It preserves CONFIG/AUTOEXEC order, the US/437 exception and progress accounting;
it adds no content generator or job layer. CP19c3b exposes the existing driver
and code-page lists for an explicit Qt choice, initially US/437. The user chose
manual selection over Linux host detection on 2026-09-29; no desktop query or
additional host mapping table is needed. CP19d1 also reuses the original Rufus
and Windows 7 MBR payloads through the existing native partition writer, with
explicit selection and readback. CP19d2a carries exact DOS choices through the
existing request and derives its six copy steps from shared progress policy;
the helper refuses the route before acquisition until its device path is joined.
Complete helper/GUI integration remains.


## GRUB2 — reuse the prepared Rufus payloads (CP21a)

The [GRUB2 audit](linux-grub2-reuse.md) confirms a smaller integration than the
Syslinux provider: `WriteMBR` uses the existing 432-byte GRUB MBR and `WriteSBR`
places the selected core at byte 512. The current embedded 2.14 pair already has
its fixed sector locations, prefix, modules and patches. `grub-install` would
regenerate/install a host-selected core and module set, which is unnecessary for
this prepared Rufus route. A native CMake probe compiles the unchanged ms-sys MBR
routine and boots unchanged core bytes on generated FAT16/FAT32 USB images,
including normal and sanitized host runs. No native GRUB package was installed.

Reuse the payloads and extend the existing bounded partition/boot-record writer;
keep payload selection and GUI interaction separate from native I/O. Preserve
standard PBR writes, Syslinux precedence, image module/version compatibility and
Rufus's original download/fallback choices. A bare rescue prompt plus a filesystem
read is qualified; an ISO's normal modules/menu/OS still needs its own source and
helper integration. The probe is test-only and adds no production code.
