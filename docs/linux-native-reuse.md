# Native Linux implementations before API adaptation

Updated: 2026-09-13. Rufus baseline:
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
Native installer integration remains a separate
checkpoint with file-image and boot tests; privilege handling continues to use
the planned unprivileged frontend and system authorization.
