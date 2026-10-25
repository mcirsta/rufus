# Native wimlib warning patch

`compiler-warnings.patch` applies to private build copies after the reader,
NTFS and native include adaptations. The pinned upstream hashes and external
source tree remain unchanged. Patch application uses zero fuzz.

The patch makes numeric bounds and min/max operands use their existing common
types, identifies intentionally unused platform/callback arguments and
assert-only values, and moves `defined()` tests outside a preprocessor macro.
It gives the private language-name table its implicit final terminator, marks
the LZX switch fallthrough, makes the suffix sorter's signed 32-bit workspace
view explicit with compile-time type/width checks, initializes temporary reparse
metadata and removes
xattr upper-bound tests already guaranteed by the stored field widths. Static
assertions retain those xattr format limits. The pointer-capable min/max macros
and the compression/decompression algorithms remain unchanged.

The C++ public view omits the C flexible stream-array member. Native C++ callers
use `WimDirEntryStreams()` from `native_input.h`; its C implementation returns
the actual flexible array, including named streams. `test_wim_dir_entry_layout`
compares C/C++ prefix size, alignment and stream offset, then reads the unnamed
stream and two named streams through the bridge. No fixed one-element stand-in
array or warning suppression is used.

The native target keeps `-Wall -Wextra`; its former pointer-sign suppression is
removed. Root qualification records the clean-build diagnostics and affected
tests together with the other native warning repairs.
