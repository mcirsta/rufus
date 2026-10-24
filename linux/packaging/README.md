# Linux desktop package

The current packaged preview is `4.15.2404-linux-preview.48`. The version is read
from the upstream Windows resource and combined with the Linux preview revision.
`rufus-linux --version` and `rufus-linux --help` work without a display server.
Portable INI storage, startup options and automatic/persistent logs are described
in the [desktop guide](../../docs/linux-desktop-settings.md).
The About button shows the build version, original GPL license and upstream
credits. Current supported routes are listed in the installed
`share/doc/rufus-linux-preview/docs/linux-support.md`.

E2fsprogs is a required runtime dependency for libext2fs and persistence formatting.
UDF data formatting requires optional udftools `mkudffs`; see the
[formatting guide](../../docs/linux-filesystem-formatting.md) for revision limits.

The menu entry is **Rufus (Linux preview)**. It starts `rufus-linux` as the
desktop user. Device creation requests authorization through the session's
polkit agent after confirmation; the GUI does not run as root. The same
authorized helper handles the entire ISO creation job and its cleanup.

The desktop entry, Qt desktop identity and installed icon name use
`org.rufus.Rufus`. The menu icons are unchanged upstream PNGs; the window keeps
the existing embedded Rufus icon. The launcher opens the ordinary selection
window. Image-file associations are not registered: use SELECT to load an image.

## Build and stage

Use the [build prerequisites](../README.md#build-and-test), verified
[wimlib source](../../docs/linux-wimlib-prototype.md#reproduction) and optional
[Syslinux source](../syslinux/README.md). All application/dependency compilation
uses CMake; source downloads are a separate preparation step.

From the repository root, with new output directories:

```sh
cmake -S linux -B build/linux-package -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
  -DCMAKE_INSTALL_LIBEXECDIR=lib/rufus-linux-preview \
  -DBUILD_TESTING=OFF \
  -DRUFUS_WIMLIB_UPSTREAM=/path/to/wimlib-1.14.5 \
  -DRUFUS_SYSLINUX_UPSTREAM=/path/to/syslinux-6.04-pre1 \
  -DRUFUS_SYSLINUX4_UPSTREAM=/path/to/syslinux-4.07
cmake --build build/linux-package
DESTDIR="$PWD/package-root" cmake --install build/linux-package
desktop-file-validate package-root/usr/share/applications/org.rufus.Rufus.desktop
```

Ninja chooses its default parallelism. `desktop-file-validate` comes from
desktop-file-utils and is a packaging check, not an application dependency.
Leaving either Syslinux source path empty omits that optional native installer.
The advanced Qt choices remain visible; a missing selected provider stops
preparation before authorization. Another version is never substituted.
`RUFUS_BUILD_GUI=OFF` omits the GUI, desktop entry and menu icons. With tests
disabled too, only the existing Qt Core reporting component is built (plus
Syslinux if explicitly selected).

For the prefix and helper directory above, the staged payload is:

| Installed path | Purpose |
| --- | --- |
| `/usr/bin/rufus-linux` | Unprivileged Qt Widgets application |
| `/usr/lib/rufus-linux-preview/rufus-prepare-helper` | Whole-job helper, invoked by pkexec |
| `/usr/share/polkit-1/actions/org.rufus.PrepareDevice.policy` | One uncached administrator authorization per helper job |
| `/usr/share/applications/org.rufus.Rufus.desktop` | Menu launcher |
| `/usr/share/icons/hicolor/SIZExSIZE/apps/org.rufus.Rufus.png` | Original icons at 16, 24, 32, 48, 64, 128, 256 and 512 pixels |
| `/usr/lib/rufus-linux-preview/rufus-syslinux` | Optional qualified native Syslinux 6.04-pre1 installer |
| `/usr/lib/rufus-linux-preview/rufus-syslinux4` | Optional qualified native Syslinux 4.07 installer |
| `/usr/lib/rufus-linux-preview/rufus-extlinux` | Optional native Syslinux 6.04-pre1 provider for NTFS |
| `/usr/share/licenses/rufus-linux-preview/LICENSE.txt` | Original GPL v3 license |
| `/usr/share/doc/rufus-linux-preview/` | Supported media, parity backlog and package guide |
| `/usr/share/licenses/rufus-syslinux/`, `/usr/share/licenses/rufus-syslinux4/` | Selected installers' upstream licenses and provenance |

Configure the intended prefix and any GNUInstallDirs overrides **before**
building. GUI/helper/installer locations and the policy's executable annotation
contain configured absolute paths. `cmake --install --prefix` alone cannot
relocate that runtime. The desktop entry resolves `rufus-linux` through the
desktop session's PATH; its binary directory must be there. Polkit must discover
the policy in its system actions directory, normally the `/usr` path above.

`DESTDIR` creates package contents without installing them on the host. Use the
distribution's packaging tools to install that payload and supply dependencies
and license notices. CMake provides a CPack archive, an Arch/CachyOS recipe and explicit DEB/RPM
generators as described below. AppImage delivery remains future work.
Desktop/icon cache updates belong to the distribution's normal package triggers,
not a CMake command that might update the host while staging. A staged GUI can
be smoke-tested by prepending its bin directory to PATH, but creation requires
the helper and policy at their actual installed paths. All DD jobs use the same
helper as ISO extraction and formatting. Upgrade the GUI/helper together.

## Build release packages

The configured build produces a compressed install tree and its SHA256:

```sh
cpack --config build/linux-package/CPackConfig.cmake -B build/releases
```

This archive uses the configured `/usr` paths and system dependencies; it is
an install tree for packagers, not a self-contained portable application.

On Arch or CachyOS, use the generated `PKGBUILD` to make a native package from
the same Release build. Run it in a separate work directory:

```sh
mkdir -p build/arch-package build/releases
cp build/linux-package/PKGBUILD build/arch-package/PKGBUILD
cd build/arch-package
PKGDEST="$PWD/../releases" makepkg --noconfirm
```

The recipe builds and stages the configured binaries, records core runtime
dependencies and lists filesystem tools as optional dependencies. It requires
`CMAKE_INSTALL_PREFIX=/usr`, `CMAKE_BUILD_TYPE=Release` and `BUILD_TESTING=OFF`.
The package includes the GUI, one matching helper/policy pair, selected native
installers, icons, license and support guide. Install the resulting
`rufus-linux-preview-*.pkg.tar.zst` using the distribution package manager.
Building either package does not install it on the host.

## Debian and Fedora native packages

Build separately in the intended distribution. The Arch/CachyOS binary and its
system-dependent generic archive do not establish Debian/Fedora compatibility.
The pinned `Containerfile.debian` and `Containerfile.fedora` describe the native
build dependencies for Debian 13 and Fedora 44 on x86-64. Verified wimlib and
Syslinux source trees are supplied separately, as in the build command above;
CMake does not download sources. Use separate output directories for each host.

After configuring and building the matching `/usr` Release build:

```sh
# Inside the Debian build environment:
cpack --config build/linux-package/CPackConfig.cmake -G DEB -B build/releases

# Inside the Fedora build environment:
cpack --config build/linux-package/CPackConfig.cmake -G RPM -B build/releases
```

DEB uses `dpkg-shlibdeps` and RPM uses its native ELF dependency generator.
Both include the same GUI, helper, selected native installers, policy, icons and
guides. Declared service dependencies cover UDisks2 and polkit; filesystem tools
remain optional per the runtime table below. Debian names its polkit components
`polkitd` and `pkexec`. Qt platform plugins are required for a desktop launch.
The package version sorts preview builds below the corresponding stable release:
Debian uses `4.15.2404~preview48-1`; RPM uses
`4.15.2404-0.preview48.1`. The application's displayed version is identical on
all distributions.

GitHub release filenames replace the Debian version's tilde with a period; the
package's internal version retains the tilde shown above.

Install with the distribution's package manager so it resolves dependencies:

```sh
# Debian 13:
sudo apt install ./rufus-linux-preview_4.15.2404.preview48-1_amd64.deb

# Fedora 44:
sudo dnf install ./rufus-linux-preview-4.15.2404-0.preview48.1.x86_64.rpm
```

These commands are for the user's installed system. Building and staging the
packages does not install anything on the build host. Keep the GUI unprivileged;
the normal desktop polkit agent authorizes the helper when a job begins.

CP78 qualifies native Debian 13 and Fedora 44 x86-64 builds and their fresh
installation, upgrade and removal transactions. Each environment passes
headless help/version, installed helper/polkit/resource paths, five executable
library/version checks, eight configured provider checks and an unprivileged
offscreen GUI with saved French selection and single-instance refusal. User
settings survive package upgrade and removal. The first Fedora check found the
base container's `tsflags=nodocs` policy; an explicit documentation-install
override fixes the test environment without changing the application.

The B8 package transaction qualification is recorded separately from media
qualification. A fresh base-image environment checks the native package's
runtime dependencies and installed paths, then installs optional formatters and
checks their configured paths/version output. The upgrade fixture contains the
same candidate payload with a lower package release; it verifies package-manager
replacement and preservation of user settings. It is not a previously published
Debian/Fedora release. Rootless container checks do not qualify a distribution's
physical USB hardware, running polkit authentication agent or kernel filesystem
drivers. Refer to the release evidence for the actual qualified environments.

## AUR source-build recipe

A separate source-build AUR recipe and generator are in
[the AUR packaging guide](https://github.com/mcirsta/rufus/tree/linux-preview.48/linux/packaging/aur). The recipe uses pinned upstream wimlib/Syslinux
sources, includes the matching GUI/helper/polkit pair, and generates
`PKGBUILD`/`.SRCINFO` from the actual release archive checksum. It is separate
from the local-build `PKGBUILD` above. AUR submission is prepared separately from
GitHub release publication; no AUR upload has been performed.

## Runtime requirements

| Requirement | Used for |
| --- | --- |
| Qt6 Core, Gui, Widgets and DBus, their dependencies, and an X11 or Wayland platform plugin | GUI, analysis and job communication; include Qt's ICO reader for the embedded window icon |
| libfdisk and Hivex shared libraries | Partition operations and Windows offline-registry handling |
| zlib-compatible shared runtime selected by CMake | Gzip analysis and confirmed DD creation through one helper; CP24e/CP25d qualify ordinary Qt and guest-kernel flows |
| System D-Bus and UDisks2 | Discovery and native device validation/access |
| Polkit service, `/usr/bin/pkexec`, and a running desktop authentication agent | Native privilege prompt; helper startup explicitly disables pkexec's terminal-agent fallback |
| `mkfs.fat` from dosfstools and kernel `vfat` support | FAT16/FAT32 creation and formatting |
| Compatible `mkfs.exfat` from exfatprogs and libblkid 2.39+ | Non-bootable exFAT formatting; qualified with exfatprogs 1.4.3 |
| `mkudffs` from udftools and kernel `udf` support | Non-bootable UDF formatting; qualified with udftools 2.3 for revisions 1.02/1.50/2.00/2.01 |
| `mkisofs` from cdrtools | Optional UDF-only ISO export; Debian/Fedora package qualification does not supply or qualify a cdrtools build |
| `mkntfs` from ntfsprogs-plus (preferred) or NTFS-3G | NTFS formatting with the selected geometry/cluster size |
| New kernel `ntfs` driver, or `lowntfs-3g` plus kernel `fuseblk` | NTFS file copying; current selection checks kernel release 7.1+ and driver availability before choosing the new driver |
| Packaged `rufus-syslinux`, `mcopy` and `mattrib` from mtools | Qualified Syslinux MBR/FAT16/FAT32 ISO creation and standalone Syslinux 6.04 |
| Packaged `rufus-syslinux4`, `mcopy` and `mattrib` from mtools | Standalone Syslinux 4.07 on MBR/FAT16/FAT32; image-based v4 compatibility remains separate work |

Packagers should derive the remaining shared-library dependencies from the built
ELF binaries. The selected wimlib reader and Rufus ISO/UDF reader are compiled
into this build; a separately installed system wimlib/libcdio does not supply
the application's reader extension.

The formatter and NTFS-3G paths are selected at configuration time with
`RUFUS_MKFS_FAT`, `RUFUS_MKFS_EXFAT`, `RUFUS_MKUDFFS`, `RUFUS_MKNTFS` and `RUFUS_NTFS_3G`. Native
Syslinux also uses a fixed mtools directory; CMake requires detected `mcopy` and `mattrib` to share
that directory. Package those exact tools at those paths. No system Syslinux
substitute is chosen automatically. Missing native tools are checked before
writes. Raw and ISOHybrid DD also require the preparation helper, but do not
need formatters/installers. Missing helpers gate all creation modes. Hybrid
images keep the usual ISO/DD dialog with ISO recommended; no automatic mode or
authorization-route fallback occurs. An NTFS write/corruption error does not
trigger a driver fallback.

The GUI can start without UDisks2 and report that discovery is unavailable.
Working device creation requires the services and tools above. A KDE or GNOME
session's existing polkit agent can handle the prompt; there is no Rufus password
dialog or stored password. Filesystem drivers and FUSE support are kernel/runtime
dependencies, not extra prompts during a job.

## Verification and scope

Linux CI stages the CMake installation and validates the desktop file. CP13
also checks the installed icon hashes and executable/policy paths, and launches
the staged desktop entry in an isolated X11 display with no host system bus or
physical device access. The [CP12o guest recipe](../tests/vm/README.md#installed-runtime-and-widgets)
separately qualifies the installed helper and native authentication workflow.
X11 launcher matching does not establish native Wayland/compositor qualification.
See the [supported media table](../../docs/linux-support.md) for enabled creation
routes and the [port plan](../../docs/linux-port-plan.md) for upcoming work.

The entry follows the [freedesktop desktop-entry specification](https://specifications.freedesktop.org/desktop-entry/latest/recognized-keys.html).
Qt's [desktopFileName](https://doc.qt.io/qt-6/qguiapplication.html#desktopFileName-prop)
binds the application to that entry. No Windows build or resource is modified.


The internal XZ source decoder compiles Rufus's original XZ Embedded sources
and uses existing zlib CRC32 and shared SHA256. It adds no external xz/liblzma
runtime dependency. Independent component tests require Python's lzma module
and the xz CLI. CP25b qualifies the writer/helper on private files, and CP25c
adds real guest-kernel helper/readback/bootable-image coverage. CP25d enables
ordinary gzip/XZ SELECT and confirmed USB creation; both use the same packaged
helper and their in-process decoders. Install neither the hook-disabled
comparison probe nor other test binaries.


CP26a compiles the original BZIP2 core through a native retained-source adapter.
It adds no bzip2 executable or libbz2 production dependency. Independent tests
use Python's bz2 module and a test-only original-wrapper probe; package neither
the probe nor test fixtures. CP26d enables ordinary BZIP2 creation;
ordinary compressed creation is gzip/XZ/BZIP2.


CP26b's BZIP2 writer/helper uses the existing packaged single-helper engine and
adds no runtime dependency. A Qt test checks the available backend and gated
selection using the independent bzip2 CLI; the GUI does not invoke
it in production. CP26d qualifies ordinary creation/controller/READY as well.


CP26c qualifies the unchanged packaged BZIP2 helper/decoder in normal/sanitizer
69-job guests, including real claims/faults, full poisoned restore/readback and
BIOS boot. The independent bzip2 encoder is staged only in disposable tests.
CP26d enables ordinary BZIP2 creation through that same helper; enabled formats
are gzip/XZ/BZIP2. See CP26c/CP26d for fixture limits.


CP26d enables *.bz2 in the ordinary worker/picker and confirmed creation flow.
Both existing production helper binaries are unchanged from CP26c. Native
BZIP2 decoding remains embedded; external bzip2/xz executables are independent
test encoders and are not invoked by the production GUI/helper. Normal/sanitizer
80/81 host and focused full-size 11-job controller guest runs pass.


CP27a embeds the original LZMA-Alone decoder through a native source adapter;
production adds no liblzma or xz executable dependency. Python's lzma module and
the original-wrapper probe are independent test providers, not packaged runtime
components. Preserve the original 1-GiB dictionary/property range. This internal
source foundation supports CP27d ordinary LZMA media creation.


CP27b adds LZMA to the existing native writer and compressed-write-device-v1
protocol, using the same packaged helper and embedded original decoder. No
external liblzma/xz runtime dependency or extra privileged process is added.
Independent Python/xz encoders and link-only kernel/protocol fixtures stay in
tests. CP27c qualifies actual guest paths and CP27d ordinary GUI creation.
Before CP29e, GUI compression was gzip/XZ/BZIP2/LZMA/LZW (CP28d).


CP27c qualifies the unchanged packaged LZMA helper/decoder in both focused
38-job guests, including actual claims/source I/O/faults, full poisoned restore,
independent readback and BIOS boot. The xz encoder and fault probe are staged
only in disposable tests. CP27d enables ordinary LZMA GUI admission; no new
production dependency, helper process or authorization was added.


CP27d adds *.lzma to the ordinary worker/picker and existing confirmed creation
flow. Both production helpers are unchanged from CP27c. Independent xz fixtures
stay in tests; production decoding remains embedded. Normal/sanitizer 96/99
host and full-size 11-job GUI guest qualifications pass.


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


## CP29c — internal ZIP creation foundation

The built-in ZIP source can now feed frozen decoded plans, the native writer
and the strict helper packet. Simulated device tests cover all five original
entry methods. ZIP remains absent from ordinary Qt selection pending real
guest and complete controller/READY qualification. No additional runtime ZIP
program is required. See CP29c for the test record.


## CP29d — ZIP guest helper evidence

The unchanged production helper passes normal and sanitizer 512-byte-sector
QEMU USB jobs for all five original ZIP entry methods, including CRC refusal,
claim/fault cleanup and full bootable deflate restore. No ZIP executable is
added to runtime packaging. Ordinary GUI ZIP creation remains gated pending
selection, confirmation and joined READY qualification. See CP29d.


## CP29e — packaged ZIP creation route

ZIP now uses the ordinary picker/worker and the existing single packaged
helper for confirmed DD creation. The first central-directory entry is
decoded by embedded providers, so no external unzip/bzip2/xz executable is
added to the runtime package. Both normal and sanitizer 64-MiB guest controller
restores pass one launch, joined READY, independent readback and BIOS boot.
Current GUI compression is gzip/XZ/BZIP2/LZMA/LZW/ZIP. See CP29e for the
evidence and first-entry/512-byte guest limits.

## CP33 — packaged fixed/dynamic VHDX creation

Preview 2 adds `.vhdx`/`.VHDX` to ordinary selection and confirmed DD creation.
The native reader feeds the existing packaged helper and writer; it adds no
runtime QEMU or other virtual-disk dependency. QEMU is only an independent test
encoder. Pending logs and parent chains are rejected before writing. The
release README is generated from the same preview revision as the binaries
and package names. See CP33 for the combined qualification and size limits.

## CP34 — packaged full FFU creation

Preview 3 adds full single-store FFU reconstruction to ordinary selection and
confirmed creation. V1, single-store V2 and V1-compressed none/plain-XPRESS
payloads use an embedded native reader and the existing helper/writer. No DISM,
Wine, .NET, Img2Ffu or Python runtime dependency is added. Those independent
tools only generate/check disposable tests. Restoration keeps the manifest's
captured size; remaining FFU compression methods, portable resizing and trust
validation remain in the parity backlog. See the support guide and CP34.

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
limits are in the [support guide](../../docs/linux-support.md). Read-only device
discovery and the remaining container/image operations stay in the parity backlog.

## CP37 — read-only device discovery and saving, preview 6

Eligible USB media now appear even when read-only. The Device list marks them
`[Read-only]`; the save action remains available for VHD, VHDX, FFU and ISO,
while START and format controls stay disabled. Capture keeps existing write
protection and uses the same read acquisition, verification, atomic publication
and joined cleanup as writable sources. Known access changes preserve source
selection and stop a creation job when the device becomes read-only. Unknown
access state remains excluded. See the support guide for source/format bounds.

## CP43 — partial FFU updates, preview 12

Ordinary FFU selection now accepts conditional partial single-store updates.
Every declared existing-device comparison must pass before any writes; updates
preserve omitted bytes and apply their metadata as declared. New data is read
back before joined READY. See the support guide for codec, geometry and base
validation limits. Optimized resizing, multiple stores and signer trust remain
in the parity backlog.

## CP44 — image entry and marker override, preview 13 (2026-10-20)

Close saved Alt+M override, raw signature recognition regardless of suffix,
`.usb`/`.wic`/`.bzip2`, local first-file drops, startup `-i`/`--iso` or positional
paths and desktop file arguments in one batch. Frozen typed requests and native
preflight retain source/geometry/target checks. Focused GUI/protocol checks,
one combined normal/sanitizer route, one Qt gate and two inspected packages
qualify delivery. The support guide records exact limits; the other Windows
parity groups remain active.

## CP45 — device zeroing, preview 14 (2026-10-20)

Alt+Z and Ctrl+Alt+Z now perform source-free full and fast clearing through one
confirmed/authorized helper. Exact Windows fast-zero semantics, full readback,
zero-write blank jobs and joined READY share one native normal/sanitizer corpus,
one combined guest per build and one Qt gate. The support guide records the
geometry and memory boundaries. Full formatting and bad-block/fake-capacity
tests remain separate parity work, together with the other completion groups.

## CP46 — compressed FFU capture, preview 15 (2026-10-20)

FFU saving uses LZNT1 compression and native literal fallback, with exact-sector
hashing/readback and atomic publication. Zero-filled regions remain explicitly
mapped. Opaque GPT names stay in captured sectors without unsafe manifest text.
Normal/sanitizer grouped components, combined guests, one Qt gate and inspected
Arch/CachyOS/TGZ packages form the delivery batch. See the support guide for bounds
and qualification limits. Full formatting remains disabled pending qualification;
optimized FFU resizing, multiple stores, signer trust and full parity remain open.

## CP47 — full/quick formatting, preview 16 (2026-10-20)

Quick format is now selectable for eligible native formatting jobs. Full mode
clears/verifies FAT volumes, requests native NTFS/exFAT full formatting and fully
initializes ext journals. Preserve Windows's large-FAT32/DD control rules and
cancelable long-running helpers. Normal/sanitizer grouped components, combined
guests, one Qt gate and inspected packages qualify the batch. See the support
guide for FAT I/O refusal, filesystem semantics and qualification boundaries.
Bad-block/fake-capacity checks and the complete parity backlog remain active.


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

Preview 24 adds optimized FFU target sizing, NTFS growth and recovery updates.
`ntfsresize` is an optional runtime provider, supplied by ntfsprogs-plus or NTFS-3G;
missing support is reported before target writes. See the
[support guide](../../docs/linux-support.md#optimized-ffu-deployment-cp55) and
[batch record](../../docs/linux-port-plan.md#cp55--optimized-ffu-deployment-preview-24-2026-10-20).

## CP56 — FFU closeout, preview 25

Preview 25 completes the FFU feature group and corrects capture compression to
plain XPRESS for the checked Windows desktop provider. The installed
[FFU audit](../../docs/linux-ffu-parity.md) records the call chain, removed
incorrect mapping requirement, prior evidence and qualification boundary.
Full Windows parity remains active outside FFU.

## CP57 — supplementary ZIP archives, preview 26

Ctrl+SELECT now attaches a ZIP overlay for file-based creation/formatting.
The same authorized job validates sources and CRCs, copies the files last
and waits for sync/cleanup/helper exit before READY. DD/ext jobs omit it.
See the [ZIP guide](../../docs/linux-supplementary-zip.md) and CP57 batch evidence.

## CP58 — Windows To Go and standalone WIM/ESD, preview 27

Preview 27 closes Windows To Go and standalone WIM/ESD as one batch: ordinary
archive/edition selection, native NTFS metadata including EA/EFS, GPT ESP/MSR or
MBR BIOS/UEFI, BCD/boot files, installed SAN/S-mode and one-helper joined READY.
Normal/sanitizer components cover 284 cases per build. Combined guests cover all
four source forms and one native EFI handoff per build; one full Qt gate passes.
Windows execution/installation and decryption remain deferred. See the
[To Go guide](../../docs/linux-windows-to-go.md) for precise coverage and evidence. Full
Windows parity remains active outside this completed feature group.


## CP65 / Preview 34 — device discovery

Running-application disks are excluded and existing mounted extended labels
are shown with Multiple Volumes captions. Discovery is unprivileged and does
not mount media. See the [device-discovery guide](../../docs/linux-device-discovery.md)
for native backings, original behavior, metadata safeguards and qualification.


## CP66 / Preview 35 — native image downloads

SELECT/DOWNLOAD now offers the signed Fido Windows and UEFI Shell choices,
browser handoff or streamed ISO saving and automatic selection. No device
authorization is requested for downloading. See [native downloads](../../docs/linux-image-downloads.md)
for trust, cancellation, native filesystem publication/recovery and evidence.


## CP67 — application language selection and catalog reuse (Preview 36)

The [language batch](../../docs/linux-localization.md) reuses the unchanged original
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

The [application-update batch](../../docs/linux-application-updates.md) groups native release
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


## CP69 — ISO read options and timestamps (Preview 38)

The [ISO controls batch](linux-iso-options.md) groups Alt+I/J/K/S/T: session
ISO/Joliet/Rock Ridge and size checks, saved timestamp preservation, coherent
selected-source analysis/preflight/extraction and the explicit MS-DOS 64 KiB
cluster override. Physical capacity and actual filesystem bounds remain enforced.
Ordinary ISO9660/UDF copies preserve access/modification times when enabled;
creation time uses native filesystem behavior. Directory times are applied after
children so copying cannot immediately overwrite them. See the guide for the
Windows behavior, Linux differences and qualification limits.


## CP70 — NTFS compression and indexing (Preview 39)

The [NTFS options batch](linux-ntfs-options.md) groups session Alt+N compression,
saved Alt+Q indexing, frozen NTFS-only options, native formatter defaults and
boot-file compression exceptions. Larger clusters continue uncompressed with
a diagnostic. Compressed file creation preselects the qualified NTFS-3G provider
before writes; ordinary NTFS creation keeps the kernel provider. No write or
corruption error switches drivers. The guide records native directory-default
restoration, provider evidence and remaining UDF/ReFS indexing-hint work.


## CP71 — advanced device selectors (Preview 40)

Alt+G/Alt+W and Ctrl+Alt+F now control recognized native virtual, VMware and
non-USB removable disks; native MMC cards share the existing card filter.
Selected classes are frozen and independently revalidated by the helper.
See [native discovery](docs/linux-device-discovery.md) for exact provider scope,
protection and grouped qualification. The complete parity goal remains active.
