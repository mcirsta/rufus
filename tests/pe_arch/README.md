# Shared PE architecture recognition

`src/pe_arch.c` retains Rufus's machine-to-architecture mapping and recognizes
the DOS/PE signatures and COFF machine field within an explicit byte span.
This is architecture recognition, not executable, EFI or signature validation.

```sh
cmake -S tests/pe_arch -B /tmp/rufus-pe-arch -G Ninja
cmake --build /tmp/rufus-pe-arch
ctest --test-dir /tmp/rufus-pe-arch --output-on-failure
```

The 289 checks cover every existing mapping, unknown machines, unaligned input,
all truncation points, negative/oversized/overlapping PE offsets, signature
failures and unchanged input. The component is included in Linux CTest and the
Windows component workflow. Native Windows execution remains deferred.
