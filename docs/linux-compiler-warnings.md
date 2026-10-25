# Native Linux compiler warnings

The native build fixes the 3571 compiler warning lines
(482 unique source/message locations) recorded in
the GCC 16.2.1 Release baseline. The final native Release and Debug sanitizer
builds produce zero compiler warnings. GNU/Clang C and C++ targets use
`-Werror` by default through `linux/CompilerWarnings.cmake`; no new diagnostic
suppression was added. Wimlib's old pointer-sign suppression is removed.

Qt 6.4 is the minimum. Native hashing uses QByteArrayView, and tests select the
available nondeprecated drag-event constructor. CI targets Ubuntu 24.04;
Ubuntu 22.04 is no longer a build target.

## Third-party source handling

Original Windows sources under `src/libcdio`, `src/bled` and `src/syslinux`
remain unchanged. Native builds stage private copies and apply tracked patches
with zero fuzz. Verified external wimlib 1.14.5 and Syslinux 4.07/6.04-pre1 inputs
remain unchanged too; their existing pinned boot payloads are retained.

- Libcdio fixes typed counts, formatting and fixed-width string copies. Public
  C/C++ headers expose the fixed record prefixes while the private C reader
  retains the original variable payload layout. A 40-fact probe checks the
  private C, public C and C++ sizes and offsets.
- Wimlib fixes bounds, intentionally unused parameters and assertions, and
  provides a C accessor for the flexible stream tail. A C/C++ ABI probe checks
  size, alignment, tail offset and actual unnamed/named stream data.
- Syslinux accessors accept byte-addressable pointers and preserve unaligned
  little-endian access through byte operations or memcpy.
- Bled preserves signed decoder errors and makes variables retained across
  longjmp volatile. Existing decoder differential tests remain in use.

Our Linux code is fixed directly: complete aggregate initialization, current Qt
hashing APIs, bounded byte-array access and clear existing control flow.

## Batched qualification

Source `dec391ec0118a004d7d2e823494bcfe6cc55862d` passes 266 distinct no-hardware suites, including
the full Qt write suite, and 34 affected suites with AddressSanitizer,
UndefinedBehaviorSanitizer and leak detection enabled. Passing unchanged suites
were reused; dependency/configuration failures were retried by suite name.
The full local Qt gate's 1,607 passing cases were retained and its 28 private
fixture startup failures were resolved in one focused group (30 passes, one
existing optional external-image skip). Startup has a separate bounded ten-second
deadline; action/write/cancel deadlines are unchanged.
The standalone generated-ISO reader fixture and staged desktop/CLI checks pass.
No new physical-media or Windows installation matrix was run.

The clean CachyOS Release build uses the actual makepkg hardening flags,
FORTIFY_SOURCE=3, C++ library assertions and LTO. It and the final updated build
have zero warnings. The full native test/guest-target build also has zero
warnings. Ubuntu 24.04 CI checks the same source with warnings as errors:
[GitHub qualification run](https://github.com/mcirsta/rufus/actions/runs/40246344865).
Its compiler build passed; the receipt records the remaining test status at
export. The preceding [full Ubuntu gate](https://github.com/mcirsta/rufus/actions/runs/40244491468)
passed; later changes have local focused regression and sanitizer coverage.

Qualification also corrected stale tmpfs/architecture fixture assumptions and
NTFS test inode-handle ownership. Leak checks remain enabled. An old-family
Syslinux image request without selected resources now fails before any writes
instead of falling back to embedded v6 payloads; its existing refusal test is
retained. Valid selected-resource routes remain supported.

[Machine-readable evidence](linux-compiler-warning-evidence.json) records the
source hashes and totals. Private logs and command receipts are retained under
`../.rufus-test-artifacts/warnings-2026-10-26/`; reproducible build copies and
private tool binaries are removed after qualification.

Published Preview48 and its manual AUR recipe still refer to the immutable
Preview48 archive. These source fixes require a subsequent release to reach
that recipe; the published tag and assets were not replaced.
