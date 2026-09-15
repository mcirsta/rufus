# Shared image-policy tests

These tests compile `src/image_policy.c` with Rufus's three original `cregex`
implementations. They exercise the distribution, GRUB and ISOHybrid rules moved
from `ImageScanThread`; they need only a C11 compiler and CMake 3.21+.

```sh
cmake -S tests/image_policy -B /tmp/rufus-image-policy-build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/rufus-image-policy-build
ctest --test-dir /tmp/rufus-image-policy-build --output-on-failure
```

On native Windows, use a Visual Studio developer terminal and configure with
`cmake -S tests/image_policy -B build/image-policy -A x64` (or `-A Win32`), then
build with `--config Debug` and pass `-C Debug` to CTest. The
[Windows component workflow](../../.github/workflows/image-policy.yml) covers
both architectures. The existing VS and MinGW workflows remain responsible for
full application builds.

The Linux build includes this directory when `BUILD_TESTING` is enabled.
The 201 cases cover label/version boundaries, prefix/case matching, UTF-8,
GRUB filesystem combinations, large-file flag encodings, supported boot
methods and DD detection states. Every case checks that unrelated report data
survives and repeated application returns the same result. The host logging
entry point is supplied by the test, and unexpected diagnostics fail it.

Only complete scan reports are valid input. A report with `is_iso == 0` is left
unchanged. The policy updates `rh8_derivative` and `disable_iso`, and returns
whether DD mode is required; the frontend owns notification and the subsequent
`is_iso` transition. These tests do not scan image files or establish that a
medium boots. Generated-ISO integration is the next checkpoint.
