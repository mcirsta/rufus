# Private native Syslinux installers

`rufus-syslinux` is built from the **syslinux-6.04-pre1** mtools
installer with the CP74 private resource-selection adapter. Both the main Linux build and the standalone qualification use this
directory's CMake recipe. The v6 target compiles ten upstream C files and uses
upstream `bin2c.pl` to embed the release's loader, VBR and companion module. It does not
run upstream Make/Autotools or rebuild BIOS payloads.

CP20k also builds `rufus-extlinux` from eight upstream C files in the same
release, sharing the generated v6 payloads. This is the mounted NTFS provider;
it does not need mtools. CP20l connects its fixed installed path to the standalone
v6/MBR/NTFS helper job, with the same one-authorization cleanup lifecycle. GUI
enablement remains a separate checkpoint. See the [NTFS audit](../../docs/linux-syslinux-ntfs.md).

Source archive:
<https://www.kernel.org/pub/linux/utils/boot/syslinux/Testing/6.04/syslinux-6.04-pre1.tar.xz>

Archive SHA-256 (checked against the upstream HTTPS checksum list; its PGP
signature was not authenticated):

```text
3f6d50a57f3ed47d8234fd0ab4492634eb7c9aaf7dd902f33d3ac33564fd631d
```

Use that verified release tree as `RUFUS_SYSLINUX_UPSTREAM` in the main Linux
CMake build. CMake does not download it. Perl is needed at build time; mtools
is needed when the installer runs. Leaving the cache path empty omits this
optional v6 executables. CP12k connects the installed FAT executable to the complete
one-authorization helper job. CP12l qualifies the real Alpine BIOS path; CP12m
connects eligible MBR/FAT32 creation in Qt. The GUI checks these fixed installed
tool paths before authorization and the helper rechecks them before writes.
Building the optional executable alone does not place it at its installed path.
No automatic system-installer substitution or second privilege prompt is used.

CMake pins these embedded payloads and also compares the first two with
Rufus's existing v6 resources:

| Release path under `bios/` | SHA-256 |
| --- | --- |
| `core/ldlinux.bss` | `cc40ba0349782cb4c9021e54dcc0a4540c3a8b96088b3a5648671926ef44d2f0` |
| `core/ldlinux.sys` | `73b62767a16200b9af193a7d5c94e9c294c6dbb6d5b17c15038c9f3173c9a7bc` |
| `com32/elflink/ldlinux/ldlinux.c32` | `d3472c02263acf9cd1da5db51e263c5484bad13ea68618c403d9cb01ca070aee` |

These checks pin payload identity, not arbitrary installer source. The builder
must verify the source archive above. `--version` prints `6.04` without `pre1`
and is insufficient to choose an interchangeable system installer. There is no
system Syslinux lookup or fallback in this build.

The `SyslinuxRuntime` install component places both v6 executables in
`CMAKE_INSTALL_LIBEXECDIR`, with this notice and upstream `COPYING` under
`CMAKE_INSTALL_DATADIR/licenses/rufus-syslinux`. Keep the original release source,
license notices and this CMake recipe with the distribution's corresponding
source. Both have ordinary executable permissions; neither has a setuid
bit or separate polkit action. The helper invokes its trusted compiled absolute
path inside the existing single authorized job.

To stage only this component without installing anything on the host:

```sh
cmake --build /tmp/rufus-linux-qt6-build --target rufus-syslinux rufus-extlinux
DESTDIR=/tmp/rufus-syslinux-stage cmake --install /tmp/rufus-linux-qt6-build \
  --prefix /usr --component SyslinuxRuntime
```

With tests enabled, `rufus_syslinux_package` stages this component, checks the
installed files and permissions, and boots two disposable whole-disk FAT32/MBR
images using the staged executable through Rufus's checked file adapter. It
also verifies that a changed loader, VBR or companion fails configuration.
It checks the staged NTFS provider's identity, mode and version too. NTFS
installation and bare boots use the separate two-driver guest recipe, with
`--installer` selecting that same staged executable.
The test requires the main build's probes, Python 3.11+, mtools, dosfstools,
bsdtar, NASM and `qemu-system-x86_64`. It removes its staging tree and media.
No root, mount, physical USB or Windows ISO is needed.


CP20a adds `rufus_syslinux_standalone_probe`: the same executable installs on
empty MBR/FAT16 and FAT32 regular files, with independent file/boot-record checks
and a BIOS boot to the bare Syslinux prompt. No ISO, configuration or kernel is
supplied. CP20b runs those boots through `InstallNativeSyslinux` with an explicit
`BT_SYSLINUX_V6` request and no image source. FAT16/FAT32 standalone adapter tests
reuse the image route's supervision, sync, payload readback and cleanup, including
normal and sanitizer failure coverage. CP20c/d connect standalone creation to
the one-helper job and advanced Qt choice.
CP20e–h qualify the v4 build below through checked installation, the native helper
and the advanced Qt choice.
CP20i–k qualify the v6 NTFS provider, checked adapter and Windows file attributes
on both drivers. CP20l qualifies the full helper lifecycle; GUI integration remains. See the
[reuse audit](../../docs/linux-syslinux-standalone.md).


## Syslinux 4.07 provider — CP20e–h

Set `RUFUS_SYSLINUX4_UPSTREAM` to a verified, extracted **syslinux-4.07** release
to build `rufus-syslinux4`. It uses the same CMake recipe, with nine
upstream C files and private byte-safe accessor headers and the original `bin2c.pl`. The older layout places payloads
under `core/`, not `bios/core/`; it has no `ldlinux.c32` companion. Its I/O helpers
are in `mtools/syslinux.c`, rather than v6's separate `syslxrw.c`. No upstream
Make/Autotools or BIOS rebuild is run. `_DEFAULT_SOURCE=1` only enables the modern
glibc equivalent of upstream's `_BSD_SOURCE`. The CP74 private build copies retain
the upstream dependency tree and release payload hashes.

Source: [official 4.07 archive](https://www.kernel.org/pub/linux/utils/boot/syslinux/4.xx/syslinux-4.07.tar.xz).
SHA-256, verified against the official HTTPS `sha256sums.asc` (PGP signature not
authenticated):

```text
8e228c34b8a98166a4d42bf0c8ad0ff767ea9ff38dc03b6543248944be6e1887
```

| Release payload | SHA-256 |
| --- | --- |
| `core/ldlinux.bss` | `8dc6ec58b4f1df391db68d9e2165ccd47d84f4ba64675f87b677077aae5b53fa` |
| `core/ldlinux.sys` | `d5b6310740a596ba4367719a3dff4b3442f6da81cbeafbb0bf3fab026d8bc910` |

Both match Rufus's `ldlinux_v4` resources byte for byte. CMake pins these hashes
and compares them to Rufus too. Build output goes to separate generated files,
so enabling v4 does not overwrite v6 payloads or change its executable. Either
provider can be selected independently; empty paths omit their targets. Neither
provider is substituted for the other, or looked up from the system PATH.

```sh
cmake -S linux -B /tmp/rufus-linux-qt6-build \
  -DRUFUS_SYSLINUX4_UPSTREAM=/path/to/syslinux-4.07
cmake --build /tmp/rufus-linux-qt6-build --target rufus-syslinux4
DESTDIR=/tmp/rufus-syslinux4-stage cmake --install /tmp/rufus-linux-qt6-build \
  --prefix /usr --component Syslinux4Runtime
```

`Syslinux4Runtime` installs ordinary executable `libexec/rufus-syslinux4`, plus
this notice and upstream `COPYING` under `share/licenses/rufus-syslinux4` (subject
to GNUInstallDirs overrides). Keep its original archive/source and CMake recipe
with corresponding source when distributing. CP20f supplies checked regular-file
readback; CP20g connects the existing helper to this fixed path for explicit v4
FAT16/FAT32 operations. CP20h exposes **Syslinux 4.07** before **Syslinux 6.04**
under advanced Qt drive properties, matching Windows's ordering.

CP20e's x86-64 tests reuse the existing process runner on disposable regular
files, including cancellation, pathname replacement, child cleanup and final
fsync. Staged FAT16/FAT32 disks pass independent readback and BIOS boot to the
bare 4.07 prompt, with only a 37888-byte protected `ldlinux.sys`. The loader is
sector-padded before two ADV sectors; Rufus's Windows file length is 37512 bytes.
No configuration/kernel/OS is generated. Unsupported 4096-byte sectors fail
without writes, changed loader/VBR payloads fail CMake, and staging/permissions
are checked. Tests are `rufus_syslinux4_command` and `rufus_syslinux4_package`.

The original CP20e qualification used x86-64 FAT16/FAT32 and normal provider
executables. CP74 supersedes its portability and sanitizer limitations with
private byte-safe libinstaller/libfat accessors, instrumented native media jobs
and an aarch64 provider cross-build plus executable/accessor emulation. Physical
aarch64 media writes remain unqualified.


CP20f reuses `InstallNativeSyslinux` for explicit standalone v4. It compares the
complete patched loader, padding, ADV and primary VBR against the pinned Rufus
payloads, verifies FAT chains and preserves the backup VBR. The process runner,
sync and cleanup are shared with v6. `rufus_syslinux4_fat16_standalone_install`
and `rufus_syslinux4_fat32_standalone_install` add corruption/lifecycle coverage;
both package boots now use this checked adapter. Wrong-version executables fail
readback after writes; this is not executable authentication before writing.
CP20f initially refused image-based v4; CP74 adds its original resource contract.
Earlier adapter tests used `RUFUS_SYSLINUX4_TEST_INSTALLER` with normal binaries;
the CP74 combined route instruments the native providers too.


CP20g qualifies the existing single-helper flow on native simulated USB devices
for both FAT formats. It accepts explicit `prepare-syslinux4-fat16-device-v1`
and `prepare-syslinux4-fat32-device-v1` requests with no source/customization
fields. Missing selected tools fail before writes; another version is never
chosen as fallback. The job keeps its exclusive claim during installation,
shares the original two copy events and requires final disk sync and helper
exit before success. See [the native guest recipe](../tests/vm/README.md#standalone-syslinux-4-and-6-helpers).
CP20h connects the existing controller to both versions: selected-provider
preflight before authorization, version-specific status, original progress and
full READY only after helper exit. Both require MBR/BIOS and qualified FAT16/FAT32
settings. Collapsing advanced properties restores the retained image. Image-based
v4 source support is delivered by CP74; the standalone choice remains explicit.

## CP74 / B3 native extensions

Private build copies add an owned validated resource bundle before upstream main,
allow an absent COM32 companion for old-family NTFS and use portable v4 endian
accessors on all hosts. The dependency trees are unchanged. The original pinned
arrays remain the standalone defaults; source-matched family 5/6 templates are
selected in the unprivileged application and retained for a single helper job.
See the [ISO/EFI resource guide](../../docs/linux-iso-efi-resources.md) for source,
signature, size, alignment and qualification contracts. Distribute these adapters,
CMake transformations and the corresponding upstream release source together.
## Compiler warning patches

Native builds apply `compiler-warnings-v4.patch`,
`compiler-warnings-v6.patch` and `bundled-compiler-warnings.patch` to private
source copies with zero patch fuzz. Original Windows sources and the pinned
upstream installer/boot payloads remain unchanged.

Potentially unaligned little-endian accessors accept byte-addressable `void`
pointers and use byte offsets or `memcpy` for the accesses. Intermediate
integer-pointer casts are removed too, preserving unaligned access semantics
through the wrapper functions. This avoids promising integer alignment for
packed disk fields.
The bundled libfat cache checks a negative read result before comparing the
byte count, and its directory byte offset uses the sector-size type. No loader
format or boot-payload bytes change.
