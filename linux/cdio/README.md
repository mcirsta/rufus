# Native bundled image-reader build

The Windows/Rufus reader sources under `src/libcdio/` remain unchanged. CMake
copies them into this target's private build directory and applies
`native-warnings.patch` with no patch fuzz.

The patch makes I/O counts, fixed-width formatting and length comparisons match
their native types. Fixed-width ISO strings are copied and space-filled without
requesting a terminating NUL. A negative XA system-use length returns before
reading XA attributes.

The private C reader retains the original variable payload fields through
`CDIO_NATIVE_READER`. Public C/C++ headers expose the same fixed record prefixes;
C callers keep a standard flexible filename tail. GNU zero-length payload arrays
are not exposed to public callers. The `rufus_cdio_header_layout` check compares
40 sizes and offsets between the private C reader, public C and public C++ views.
The on-disk decoder and private C layout retain their original definitions.
