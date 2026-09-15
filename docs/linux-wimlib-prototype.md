# Upstream wimlib reader (CP3g3b–CP3g3d)

**Decision, 2026-09-14:** use upstream wimlib source with a small read-only
input extension for Linux. This is the user's preferred direction. A dedicated
wimlib fork remains an option if maintaining the extension alongside releases
becomes easier there. No new remote fork is needed for this checkpoint.

**Current status, CP3g3d:** the extended upstream **1.14.5 release** is now the Linux
application dependency. SELECT opens WIMs
through Rufus's shared ISO/UDF reader and returns the same archive/Windows
metadata as the system library reading ordinary files. It uses the scan's retained
selected handle and the first recorded installation path / WIM image 1. Unknown
and unreadable metadata stay partial; cancelled and obsolete results are discarded.
The existing Widgets form is unchanged. The original measurements below record CP3g3b;
the [checkpoint log](testing-and-checkpoints.md#cp3g3c-select-windows-metadata-2026-09-14)
records the SELECT integration and subsequent release qualification.

## Dependency choice

Rufus's bundled wimlib is modified, including its libcdio input extension,
Windows/MSVC portability, logging and progress handling. Comparing its C/header
files with its declared upstream base found 76 identical files and 58 changed
files, excluding the generated configuration. Bringing back the full upstream
native source set would also add nine omitted C files (5,279 lines), including
Unix capture/apply and public operations that Rufus does not build.

The initial bundled-source build attempt exposed additional native portability
work, including Windows-only initialization locking, CRT names and typed SIMD
arguments. That attempt was not qualified. We selected upstream source instead
of adding these adaptations to the Windows fork. The final target uses upstream
POSIX threading, file I/O and Unix sources directly, without Windows API aliases.

The original comparison base was upstream commit
[`4a34203cde54c317f0d277a4c64e19916547bf10`](https://github.com/ebiggers/wimlib/commit/4a34203cde54c317f0d277a4c64e19916547bf10),
the exact commit recorded by Rufus's `1.14.4-3-g4a34203c` configuration. This is a
comparison pin. CP3g3d replaces it with the official **1.14.5 source release**,
listed as current by [upstream](https://wimlib.net/) on 2026-09-14. The
[release announcement](https://wimlib.net/forums/viewtopic.php?t=1133) supplies
the archive checksum used below. The independent ordinary-file comparator uses
the existing native 1.14.5 library. The original comparison pin is no longer
accepted by the build.

Unmodified distro wimlib still has no public arbitrary-reader opener. Its normal
file API remains useful as a comparator, but the ISO/UDF path requires this
source extension. The eventual application should use one extended upstream
provider for both ordinary and embedded WIMs; CP3g3c does so. Do not link two
wimlib providers into one process. System comparison executables remain separate
and require `RUFUS_COMPARE_SYSTEM_WIMLIB=ON`.

## Size and ownership of the extension

`linux/wimlib/native-input.patch` changes **five upstream files: 44 added lines
and 5 removed lines**. `native_input.c` provides a 76-line concrete adapter and
`native_input.h` declares its interface. There is no generic callback registry
or Qt dependency. The existing CMake source list/configuration and SHA-256 input
manifest are build support, not another WIM implementation.

| Location | Change |
| --- | --- |
| `wimlib/file_io.h` | A borrowed reader pointer next to the existing descriptor and position. Ordinary descriptors initialize it to null. |
| `wim.c` | Accept a selected reader and its file size through the existing open path; reject modification of that input. |
| `file_io.c` | Route sequential/random reads and seeks through the reader; reject writes. |
| `header.c` | Read a pipable WIM's final header through the reader rather than calling `lseek()` on a descriptor. |
| `xml.c` | Allow an explicit reader to satisfy the input-presence check without a filename. XML parsing is unchanged. |

WIM header/resource/XML parsing stays upstream. `src/wim_metadata.c` remains the
shared Rufus interpretation of archive facts and Windows version fields.
`src/iso_file.c` remains the shared selected-filesystem implementation. Its
structure acquired a tag so the extension can forward-declare it without pulling
libcdio headers into every wimlib source file. No bundled Windows wimlib files or
Windows build manifests changed in this checkpoint.

`OpenWimFromIsoReader` borrows an already-open reader. The caller initializes
wimlib before starting workers, opens the reader in exactly one selected view,
queries metadata, frees the WIM, then closes the reader and filesystem. CP3g4a
also qualifies bounded `ReadWimFile` calls while this ownership chain is alive.
Do not use it for exports or retained resources yet; exported resources can
extend wimlib's internal reference lifetime. Calls using
one selected handle must remain serialized.

Sequential reads advance position only on success; random reads leave it alone.
Negative/out-of-range reads fail before backend I/O, with no EOF success masking
missing bytes. Zero bytes at EOF are valid. The reader retains its fixed 32 KiB
cache and maximum sixteen-block physical reads. Cancellation is checked before
opening, on reads and after opening; a cancelled open frees its WIM and clears
the output. Failures never close a caller-owned filesystem or reader.

The extension does not bound all allocations or CPU work inside wimlib. It
cannot interrupt one synchronous libcdio call or a period of parsing without
reads. There are no filesystem mounts, whole-WIM temporary copies or process-wide
POSIX interception. FUSE and NTFS-3G backends are disabled in this build.

## Reproduction

GUI and full test builds now require the source explicitly; CMake performs no
downloads. The former `RUFUS_WIMLIB_PROTOTYPE` option is no longer used.
Download/extract outside the Rufus source tree:

```sh
curl -fL https://wimlib.net/downloads/wimlib-1.14.5.tar.gz -o /tmp/rufus-cp3g3d-wimlib-1.14.5.tar.gz
echo '84221a3abd5b91228f15f8e6065c335a336237b5738197b75bf419eea561a194  /tmp/rufus-cp3g3d-wimlib-1.14.5.tar.gz' | sha256sum --check && mkdir -p /tmp/rufus-cp3g3d-upstream && tar -xf /tmp/rufus-cp3g3d-wimlib-1.14.5.tar.gz -C /tmp/rufus-cp3g3d-upstream
```

Archive SHA-256:
`84221a3abd5b91228f15f8e6065c335a336237b5738197b75bf419eea561a194`.
The checked-in manifest additionally validates every consumed upstream input:
64 C sources and 73 headers. Only build-directory copies are patched, with zero
patch fuzz. Changed inputs fail configuration and require explicit patch review;
the external upstream source tree stays untouched. The build needs `patch` and
the normal Linux dependencies. System wimlib is optional, for comparator tests.

From the Rufus checkout, using the rootless dependency prepared in CP3g2:

```sh
export LD_LIBRARY_PATH=/tmp/rufus-cp3g2-wimlib/usr/lib
cmake -S linux -B /tmp/rufus-linux-qt6-build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=/tmp/rufus-cp3g2-wimlib/usr -DRUFUS_COMPARE_SYSTEM_WIMLIB=ON -DRUFUS_WIMLIB_UPSTREAM=/tmp/rufus-cp3g3d-upstream/wimlib-1.14.5
cmake --build /tmp/rufus-linux-qt6-build
ctest --test-dir /tmp/rufus-linux-qt6-build --output-on-failure
```

Omit the comparison option and prefix/environment overrides when only the
application's upstream provider is wanted. CTest
uses the configured genisoimage/mkisofs fixture generator. Ninja uses default
parallelism. The reader executable links only the upstream static
library, bundled libcdio and native runtime; the system comparator is a separate
executable. No installation, root access, Windows executable or USB is needed.

## Original CP3g3b results

- **33/33 full CTest suites pass normally** (19.17 seconds) and **33/33 with
  ASan, UBSan and leak detection** (33.07 seconds), without sanitizer diagnostics.
- The upstream provider passes the existing **94 ordinary-file fixtures and
  647 metadata checks**.
- **132 direct transport checks** cover sequential position, backward/random
  reads, >4 GiB offset arithmetic using a test double, EOF, range rejection,
  read-only policy, error/cancellation translation and repeated failed-open
  cleanup. This is not a multi-gigabyte real-image test.
- **95 functional probes** compare ordinary system/upstream results with
  RR/Joliet/UDF metadata: normal, pipable, solid and integrity WIMs, UTF-8 paths,
  corrupt/truncated/empty input, different WIMs at one path across views, missing
  paths without fallback, and fragmented short/long UDF allocation descriptors.
  Failure and zero-byte short reads are injected at every observed physical
  refill in the normal generated WIM, with cancellation at all those boundaries.
- Both genisoimage and cdrtools mkisofs pass, normally and under sanitizers.
  Source-pin rejection and a fresh reporting-only build also pass.

Build logs are `/tmp/rufus-cp3g3b-build.log` and
`/tmp/rufus-cp3g3b-sanitizers-build.log`; CTest logs are in each build's
`Testing/Temporary/LastTest.log`. The checkpoint remains local and uncommitted.
Remote CI, real Windows, extraction, media writing and boot qualification remain
unexecuted.

**CP3g3c is implemented.** The worker initializes the library before moving to a
thread, retains the scan's selected filesystem, performs the Windows query, then
closes readers and the filesystem before publishing. Metadata facts and their
known/unknown/failed state travel with the existing request number.

## CP3g3d: release qualification

The application now uses the official 1.14.5 release archive. Of the 137 inputs,
59 differ from the original comparison pin: 56 differ only in license headers;
the other three update an inode-capture warning, the public version number and
a Windows stat alias. `configure.ac` is unchanged. The reader's WIM opening,
I/O and XML interfaces require no adaptation: the same five-file patch applies
with zero fuzz and no offsets, and the concrete adapter is unchanged.

The build manifest verifies the release's 64 C sources and 73 headers before
patching build copies. The runtime reports `1.14.5-rufus-reader`; the existing
reader contract now verifies both numeric and string versions. CI uses the
official archive and published checksum. Upgrade checks and test results are in
the [CP3g3d checkpoint](testing-and-checkpoints.md#cp3g3d-wimlib-1145-release-2026-09-14).

CP3g3d covers the existing metadata and reader operations. CP3g4a subsequently
qualifies bounded single-file extraction without changing this patch; see the
[extraction and PE boundary](linux-windows-image-metadata.md#cp3g4a-bounded-single-file-extraction-and-shared-pe-recognition).
Retained resources, Secure Boot and actual booting remain separate checkpoints.
Keep the reader patch separately reviewable; a dedicated fork
remains optional, and this release upgrade did not require one.
