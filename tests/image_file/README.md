# Shared ISO file classification

Build and run without Qt, image libraries or USB hardware:

```sh
cmake -S tests/image_file -B /tmp/rufus-image-file-build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/rufus-image-file-build
ctest --test-dir /tmp/rufus-image-file-build --output-on-failure
```

On native Windows use `-A x64` or `-A Win32` instead of `-G Ninja`, build with
`--config Debug` and pass `-C Debug` to CTest. The shared-rules workflow covers
both architectures. Linux CTest includes this directory automatically.

The checks cover file-property reset, config names, old `.c32` thresholds,
EFI bootloader types and architectures, case preservation, the small-EFI-file
workaround, installer path limits, WIM/ESD/SWM size flags, counter saturation,
WinPE masks, distribution directory markers and path truncation. Large-file
tests pass synthetic sizes to the classifier; they allocate no multi-GB files.

`SanitizeImagePath` shares the original extraction replacements for `* ? < > : |`.
Tests cover all byte values, exact replacements, UTF-8 preservation, nested paths,
Windows drive prefixes and unchanged-name reporting. Only the caller's writable
output string is changed, with no allocation or normalization beyond those six
characters. This helper does not validate paths or decide collision behavior.

`GetIsoFileProps` supplies file facts for both scan and extraction callers.
`CheckIsoScanFile` accumulates report findings without I/O. It does not complete
a scan or authorize a write. Config/isolinux path lists, total block counts,
traversal and extraction remain caller-owned.

On Linux, CMake selects `linux/platform/crt_strings.h` for the two CRT string
comparisons. Windows uses its native CRT. No Win32 or Qt headers enter the shared
module. The existing bounded path-copy behavior and report encodings are retained.

The separate [generated-ISO fixture test](../../linux/probes/libcdio/README.md)
reads real directory records through the bundled reader and classifies their
names and sizes. It uses known paths, not a second production tree walker.
