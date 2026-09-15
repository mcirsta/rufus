# Shared GRUB inspection

Build the standalone C component tests without Qt or libcdio:

```sh
cmake -S tests/grub_inspect -B /tmp/rufus-grub-inspect-build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/rufus-grub-inspect-build
ctest --test-dir /tmp/rufus-grub-inspect-build --output-on-failure
```

The same project can be configured with a native Visual Studio generator for
later Windows validation. No Windows execution has been performed here.

There are 36 portable cases, plus two Linux cases using linker injection to
check allocation failure and retry. They cover the two banner spellings, CentOS
newline handling, last marker within a file, preservation of a previously chosen
version, the zero-version exclusion, build suffix ordering, label normalization,
truncation, search margins and bounded termination. Filesystem tests cover the
11-character limit, case-sensitive uniqueness/order, owned strings, and the
case-insensitive FAT/exFAT/NTFS report mask. Boundary tests run against the shared
implementation only.

The functions in `src/grub_inspect.c` take an explicit report or string array;
the host supplies the existing `uprintf` endpoint. There are no global report,
Qt, file-reader or installer dependencies. The original label sanitizer now
lives here as a private function, with its transformation sequence preserved.

`linux/tests/test_grub_image.py` generates an ISO with eleven synthetic content
files and exercises `ReadISOFileToBufferExt` followed by these functions. It
checks version selection across files, filesystem accumulation, reader-added
termination, unknown versions and ordinary read failures. Run this test and the
component suite from the main Linux build with:

```sh
ctest --test-dir /tmp/rufus-linux-qt6-build -R 'rufus_grub_' --output-on-failure
```

These fixtures contain marker bytes, not executable GRUB modules or EFI images.
They establish recognition behavior; PE validation, complete scan integration,
real-loader fixtures and QEMU boot tests remain separate work. A temporary
Linux harness of the original functions passed the same generated-image
expectations, without running the new boundary/failure tests against it.
