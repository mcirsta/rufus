# CP3 libcdio comparison probe

Standalone architecture experiment; not linked into `rufus-linux`, not part of
default CTest, and not a supported media-analysis implementation. See the
[boundary discussion](../../../docs/linux-image-analysis-boundary.md).

Requires CMake/Ninja, a C compiler, pkg-config, system libiso9660/libudf development
files, Python 3 and libarchive's `bsdtar`. CP3c2 moved the bundled target into
`linux/cdio/`, with CMake feature checks shared by this probe and the Linux
application build. No device access, mounting or root privileges are involved.

From the Rufus repository root:

```sh
cmake -S linux/probes/libcdio -B /tmp/rufus-cp3-cdio-probe -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/rufus-cp3-cdio-probe
python3 linux/probes/libcdio/verify.py /tmp/rufus-cp3-cdio-probe
ctest --test-dir /tmp/rufus-cp3-cdio-probe --output-on-failure
```

Ninja uses its default parallelism. The two comparison executables compile the same probe
against their respective headers. Only the bundled target and its consumers
define `DO_NOT_WANT_COMPATIBILITY`; this changes public structure layout. The
system target uses the installed library's default ABI, verified here on 2.4.0.
Another system build may require a different API configuration.

`verify.py` uses `bsdtar` to create a small ISO with ordinary file entries and an
El Torito boot-image entry in a temporary directory. The payload is 4096 bytes
of non-executable test data. It verifies labels, file size and a payload prefix,
checks Rufus's synthetic `[BOOT]` path, and tests three ordinary invalid inputs.
Every subprocess has a timeout and the temporary files are removed afterward.
This follows libarchive's documented
[ISO writer support](https://github.com/libarchive/libarchive/wiki/FormatISO9660).

Observed on 2026-09-13 with GCC 16.2.1, system libcdio 2.4.0 and bsdtar 3.8.9:
both readers find the ordinary file; only the bundled reader resolves the
synthetic boot-image path. The script asserts that observed difference. If a
future system library adds compatible behavior, revisit the comparison rather
than treating the assertion as a production regression.

The initial comparison compiled all 20 selected bundled C sources unchanged.
CP3c1 now includes two small reader corrections found by the fixture sanitizer
run: skip absent symlink data when cloning a stat record, and use an unsigned
shift when decoding a 32-bit ISO value. These affect `iso9660_fs.c` and
`cdio/bytesex.h`; the other bundled sources remain unchanged. Existing bundled
compiler warnings remain visible.

## Generated-image classification fixture

`probe_classify` compiles `src/image_file.c` and reads explicitly listed file
records from generated Rock Ridge ISOs. `verify_classification.py` tests 17
entries across two images: mixed-case EFI paths, config and old `.c32` rules,
Windows installer paths, directory workarounds, the small-EFI-file heuristic
and a symlink. It also checks a missing file, a directory passed as a file,
and empty, plain-text and descriptor-truncated images. The executable selects
Rock Ridge explicitly; it does not claim to cover Joliet or UDF traversal.

This test is registered with CTest in this standalone build and is included as
an additional step in Linux CI. It is not included in the application's CMake
dependencies. Its bundled reader now uses the common feature-detected build.
Known-path lookup avoids introducing a duplicate tree walker; full-tree tests
use the shared `src/iso_scan.c` from the main Linux CTest suite.

Both normal and address/undefined-behavior/leak sanitizer runs pass. All file
payloads are synthetic non-executable data. This is not full file-content
verification, a complete image report, Windows validation or a boot test.
