# Shared partition placement tests

These portable C tests compare the arithmetic extracted from `drive.c` with an
independent transcription of the original formulas. The matrix includes modern
and old-BIOS offsets, GPT backup space, extra-partition reservations, several
logical-sector/track/cluster sizes, the zero-cluster fallback and overflow or
empty-range rejection. Both Windows `CreatePartition` and Linux's GPT image
adapter call the shared functions. This suite does not validate Windows disk APIs.

From the repository root:

```sh
cmake -S tests/partition_layout -B /tmp/rufus-partition-layout -G Ninja
cmake --build /tmp/rufus-partition-layout
ctest --test-dir /tmp/rufus-partition-layout --output-on-failure
```

The suite is also part of the main Linux CTest run. Its standalone CMake project
can be used with a Windows compiler later; the Windows application keeps its
existing build systems.
