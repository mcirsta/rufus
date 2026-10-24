# Rufus GRUB2 reuse probe

CP21a compiles Rufus's original `write_grub2_mbr` and places its unchanged
`res/grub2/core.img` at byte 512 of generated media. It tests the existing Rufus
payload, not a GRUB binary from the host. Nothing in this directory is linked
into or installed with the application. See the [reuse audit](../../../docs/linux-grub2-reuse.md).

Build the main Linux project's `mbr_image_probe` and `fat_format_probe` first.
The test also needs Python, dosfstools, mtools and QEMU with SeaBIOS at
`/usr/share/qemu/bios-256k.bin`. It uses software CPU emulation and no host mounts,
USB devices or GRUB package. Linux build steps use CMake/Ninja only:

```sh
cmake -S linux/probes/grub2 -B /tmp/rufus-grub2-audit -G Ninja \
  -DRUFUS_LINUX_BUILD_DIR=/tmp/rufus-linux-qt6-build \
  -DRUFUS_GRUB_EVIDENCE_DIR=/tmp/rufus-grub2-evidence
cmake --build /tmp/rufus-grub2-audit
ctest --test-dir /tmp/rufus-grub2-audit --output-on-failure
```

The fixed hashes intentionally pin the audited 2.14 payload pair. An upstream
payload change requires a new audit and updated expectations, not automatic
acceptance of an arbitrary `core.img`.

The test checks:

- The original 432-byte MBR array agrees with `boot.img`; its sector-1 pointer
  and the core's sector-2 continuation list agree with the installed ranges.
- All bytes outside the original MBR/signature/core ranges survive, including
  the partition table, disk ID and seeded gap padding.
- Empty, undersized and oversized inputs, including the traditional old-BIOS
  embedding gap, fail without writing.
- 64-MiB FAT16 and FAT32 images boot as emulated USB disks through SeaBIOS, the
  original MBR and core; GRUB displays the expected prefix/root and reads a file
  listing from each volume. Boot uses a snapshot and preserves the backing hash.

It intentionally supplies no `normal.mod`, menu or OS, matching bare standalone
GRUB preparation. Seeing the rescue prompt is not full installation qualification.
Standard Windows PBR writes remain separate adapter work; this probe proves the
GRUB MBR/core handoff with native formatter PBRs. No NTFS, ext, UEFI, image-specific
modules, Secure Boot or hardware qualification is implied.

Large fixture files live in an automatically cleaned temporary directory. QEMU's
snapshot also uses that directory. The chosen evidence directory retains only
small logs, text screens, commands, readback facts and source hashes; a repeated
run replaces those records, so choose a new evidence directory to retain both.

For the host harness's sanitizer run, configure a separate build with
`-DCMAKE_C_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer'` and
`-DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined'`, then run CTest with
`ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1`.
This instruments the probe and original ms-sys code. It does not instrument the
prebuilt boot payload, firmware or formatter programs.

## Production adapters (CP21b)

After building `mbr_image_probe`, `grub2_core_probe`, `freedos_boot_probe` and
`fat_format_probe` in the main CMake build, the same firmware checks can exercise
the production MBR/SBR and standard FAT PBR adapters:

```sh
python3 linux/probes/grub2/verify.py /tmp/rufus-grub2-audit/grub_record_probe \
  /tmp/rufus-linux-qt6-build /tmp/rufus-grub2-native-evidence \
  --native --mkfs /usr/bin/mkfs.fat --mcopy /usr/bin/mcopy
```

Use the installed tools' absolute paths for `--mkfs`/`--mcopy`. The original
audit probe argument is unused in native mode. The output records all four
native probe hashes, adapter sources and the original payloads. The complete
disk comparison now includes standard FAT16/FAT32 records and the FAT32 backup.
The main `rufus_grub2_core`, `rufus_grub2_fat*_mbr_*` and
`rufus_standard_fat_boot` CTests separately cover refusal/failure paths. This is
still a regular-file adapter qualification, not a privileged helper job or a
source-backed menu/OS boot.
