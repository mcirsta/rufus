# Shared EFI bootloader banner reporting

Build the standalone C tests without Qt, libcdio or PE libraries:

```sh
cmake -S tests/efi_boot_info -B /tmp/rufus-efi-boot-info-build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/rufus-efi-boot-info-build
ctest --test-dir /tmp/rufus-efi-boot-info-build --output-on-failure
```

The 34 cases check Shim/systemd-boot markers, all six C-locale whitespace
delimiters, first-match ordering, case sensitivity, empty and NUL-terminated
versions, percent characters, the 32-byte output limit and 64-byte search
margin. They also check a nonterminated byte span, read-only storage, null/short
input and unsigned-byte ctype handling. Each call checks the exact diagnostic
and that the input is unchanged. New contract tests run only against shared code.

`GetEfiBootInfo` still reports through the host's existing `uprintf` endpoint.
It uses the actual file byte count; it does not need the content reader's extra
NUL. No image-report fields are changed. This is the existing Rufus banner
diagnostic, which does not parse PE structures or assess signatures, revocation
or bootability.

The main Linux suite adds nine generated-ISO content fixtures, covering normal
banners, multiple loaders, unknown/short/empty files, and ordinary read failures:

```sh
ctest --test-dir /tmp/rufus-linux-qt6-build -R 'rufus_efi_' --output-on-failure
```

Optional real-file coverage uses explicitly supplied systemd-boot binaries and
an independently verified expected version. On this host, this also passed:

```sh
python3 linux/tests/test_efi_image.py /tmp/rufus-linux-qt6-build/efi_boot_info_probe \
  --systemd-boot /usr/lib/systemd/boot/efi/systemd-bootx64.efi \
  --systemd-boot /usr/lib/systemd/boot/efi/systemd-bootia32.efi \
  --systemd-version 261.3-1-arch
```

Recorded 2026-09-14, with banner contents inspected independently before running
the probe:

| File | Bytes | SHA-256 |
| --- | ---: | --- |
| `systemd-bootx64.efi` | 160256 | `8cbe9ebe87db7c9f7dab2c39dcdcc071ed3debf56bad4ba3bbf6b206b8cf2898` |
| `systemd-bootia32.efi` | 171520 | `9ff14a88684fc67d73b9f7efbb48a3cfe0a2516f24ca34c733acf362c181b582` |

These installed system files were read as data and copied only into temporary
fixtures, which the test deletes. They are not included in the repository or
required by CI. Nothing is installed, executed as an EFI program or booted.
The original function, isolated in a temporary Linux harness, passed the same
synthetic and real-file image expectations. This is fixture compatibility,
not native Windows validation. No real Shim binary was used in this checkpoint.
