# Native bundled decoder build

`native-warnings.patch` is applied to private build copies of `src/bled/`.
The original Windows decoder sources and compressed-data algorithms remain
unchanged.

The patch compares unsigned BZIP indices in their native type, keeps negative
legacy decoder error results signed, and makes legacy BZIP variables retained
across `longjmp` volatile. It also compares the bounded two-byte input refill
result using its signed return type. Existing differential BZIP/LZMA/LZW cases
continue to exercise the patched native copies against independent providers.
