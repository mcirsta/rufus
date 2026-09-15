# Shared Syslinux version detection

Run the portable C tests without Qt or libcdio:

```sh
cmake -S tests/syslinux_version -B /tmp/rufus-syslinux-version-build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/rufus-syslinux-version-build
ctest --test-dir /tmp/rufus-syslinux-version-build --output-on-failure
```

The 46 cases cover marker prefixes, the original search margins and minimum
span, first usable version, numeric encoding, duplicate-version removal, suffix
trimming/sanitization, missing termination, invalid versions and null arguments.
They also read both original bundled payloads as data: v4.07 with `/2013-07-25`
and v6.04 with `/pre1`. Their provenance is in
[the existing resource notice](../../res/syslinux/readme.txt). No loader executes.

`GetSyslinuxVersion` retains its signature and modifies a writable input buffer
to normalize the extension. A nonempty extension borrows that buffer's storage;
copy it before freeing the buffer and never free the extension separately.
Unknown detection returns zero and an empty extension. The span must include
the banner's terminator: pass `byte_count + 1` for the shared content reader's
buffer, or the actual resource length for a resource with an internal terminator.

The main Linux suite also generates an ISO, reads its files through
`ReadISOFileToBufferExt`, and parses their banners. It tests the bundled payloads,
synthetic distro suffixes, termination supplied at EOF, unknown versions and
ordinary read failures. Select both suites with:

```sh
ctest --test-dir /tmp/rufus-linux-qt6-build -R 'rufus_syslinux_' --output-on-failure
```

Native Windows validation remains deferred. The standalone project and prepared
component CI also support future native Windows runs; select `-A x64` or `-A Win32`,
build with `--config Debug` and pass `-C Debug` to CTest on that platform.

These tests identify version facts. They do not choose between conflicting
loaders, select a fallback or native installer, install files, or validate booting.
