# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate actual UDF filesystems and exercise shared analysis and byte reads."""
import argparse
import binascii
from pathlib import Path
import subprocess
import tempfile

from test_iso_analyze import analyze
from test_grub_image import payload as grub
from test_syslinux_scan import banner as syslinux
from test_efi_image import banner as efi


def make_image(directory, name, entries, mkisofs, options=()):
    tree = directory / name
    tree.mkdir()
    for path, content in entries.items():
        file = tree / path.lstrip("/")
        file.parent.mkdir(parents=True, exist_ok=True)
        file.write_bytes(content)
    image = directory / f"{name}-ș.iso"
    subprocess.run([mkisofs, "-quiet", "-udf", "-R", "-J", "-input-charset", "utf-8",
                    "-V", name, *options, "-o", str(image), str(tree)],
                   check=True, capture_output=True, timeout=10)
    return image


def read_file(executable, image, path, expected, mode="rr"):
    result = subprocess.run([str(executable), str(image), path, mode],
                            capture_output=True, timeout=10)
    assert result.returncode == (1 if expected is None else 0), (path, result.stderr)
    assert result.stdout == (b"" if expected is None else expected), (path, result.stderr)


def rename_udf_entry(image, old, new):
    # Rename one generated FID, leaving the ISO/RR/Joliet views unchanged.
    # Equal encoded lengths preserve placement; repair its CRC and tag checksum
    # so this remains an ordinary valid UDF fixture, not malformed input.
    old = b"\x08" + old.encode("ascii")
    new = b"\x08" + new.encode("ascii")
    assert len(old) == len(new)
    data = bytearray(image.read_bytes())
    renamed = 0
    for offset in range(0, len(data) - 38, 4):
        if data[offset:offset + 2] != b"\x01\x01":  # File Identifier Descriptor
            continue
        if data[offset + 19] != len(old):
            continue
        imp_size = int.from_bytes(data[offset + 36:offset + 38], "little")
        start = offset + 38 + imp_size
        if data[start:start + len(old)] != old:
            continue
        data[start:start + len(old)] = new
        crc_size = int.from_bytes(data[offset + 10:offset + 12], "little")
        crc = binascii.crc_hqx(data[offset + 16:offset + 16 + crc_size], 0)
        data[offset + 8:offset + 10] = crc.to_bytes(2, "little")
        data[offset + 4] = 0
        data[offset + 4] = sum(data[offset:offset + 16]) & 255
        renamed += 1
    assert renamed == 1, (image, old, renamed)
    image.write_bytes(data)


def check_combined(scanner, reader, directory, mkisofs):
    entries = {
        "/isolinux/isolinux.cfg": b"default linux\n",
        "/isolinux/isolinux.bin": syslinux("6.04 pre1"),
        "/isolinux/ldlinux.c32": bytes(512),
        "/boot/grub/i386-pc/normal.mod": grub("2.06"),
        "/EFI/BOOT/grubx64.efi": grub("2.12", filesystems=("fat", "ntfs")),
        "/EFI/BOOT/bootx64.efi": efi(b"#### LoaderInfo: systemd-boot 261.3 ####\0"),
        "/ș-Ж-é/readme.txt": bytes(range(256)) * 17 + b"tail",
        "/one-byte": b"x", "/one-block": bytes(2048), "/empty": b"",
    }
    image = make_image(directory, "UDF_COMBINED", entries, mkisofs)
    blocks = sum((len(data) + 2047) // 2048 for data in entries.values())
    values, logs = analyze(scanner, image, status=3, pending=32, TREE=1, ISO=1,
                          FILESYSTEM=2, VOLUME_SIZE_KNOWN=1, LABEL="UDF_COMBINED",
                          FILES=len(entries), BLOCKS=blocks, EXTRA=0, MISMATCH=0,
                          PROJECTED=int(blocks * 2048 * 1.0099999904632568),
                          CONFIG="/isolinux/isolinux.cfg", DETECTED=0x0604,
                          SELECTED=0x0604, INFERRED=0, GRUB=129, GRUB_FS=5, READS=4)
    assert "Detected systemd-boot version: 261.3" in logs, logs
    for path, content in entries.items():
        read_file(reader, image, path, content if content else None)
    for path in ("/", "/isolinux", "/not-present", "/ISOLINUX/isolinux.bin"):
        read_file(reader, image, path, None)
    for point in (1, 2, 5, 15):
        analyze(scanner, image, status=2, fault=f"cancel:{point}")
    for point in range(1, int(values["READS"]) + 1):
        analyze(scanner, image, status=2, fault=f"cancel-read:{point}")
        analyze(scanner, image, status=3, pending=36, fault=f"fail-read:{point}")
    for point in (1, 2, 3):
        analyze(scanner, image, status=1, fault=f"fail-array:{point}")
    for fault in ("fail-dir:1", "fail-dir:3", "fail-open-dir:1", "fail-open-dir:3"):
        analyze(scanner, image, status=1, fault=fault, READS=0)
    analyze(scanner, image, status=1, fault="fail-fs")
    return image


def check_views(scanner, reader, directory, mkisofs):
    entries = {"/syslinux.cfg": b"default udf\n", "/bootmgr": b"ISO VIEW ONLY",
               "/MiXeD/é.txt": b"UDF names stay exact"}
    image = make_image(directory, "UDF_FIRST", entries, mkisofs)
    rename_udf_entry(image, "bootmgr", "ignored")
    rename_udf_entry(image, "syslinux.cfg", "SYSLINUX.CFG")
    for mode in ("rr", "joliet", "plain"):
        analyze(scanner, image, mode=mode, TREE=1, FILESYSTEM=2, FILES=3,
                CONFIG="/SYSLINUX.CFG", DETECTED=0, SELECTED=0x0407, INFERRED=1)
        read_file(reader, image, "/SYSLINUX.CFG", entries["/syslinux.cfg"], mode)
        read_file(reader, image, "/MiXeD/é.txt", entries["/MiXeD/é.txt"], mode)
        read_file(reader, image, "/bootmgr", None, mode)
        read_file(reader, image, "/syslinux.cfg", None, mode)
    # Remove only the companion ISO descriptors. UDF remains a complete real
    # filesystem; lack of a PVD must not select the ISO view or invent a size check.
    data = bytearray(image.read_bytes())
    for sector in range(16, 256):
        offset = sector * 2048
        if data[offset + 1:offset + 6] == b"CD001":
            data[offset:offset + 2048] = bytes(2048)
    standalone = directory / "udf-without-iso.iso"
    standalone.write_bytes(data)
    analyze(scanner, standalone, FILESYSTEM=2, VOLUME_SIZE_KNOWN=0, FILES=3,
            MISMATCH=0, CONFIG="/SYSLINUX.CFG")
    read_file(reader, standalone, "/SYSLINUX.CFG", entries["/syslinux.cfg"])


def check_variants(scanner, directory, mkisofs):
    cases = (
        ("UDF_EMPTY", {}, 0, {"FILES": 0, "PROJECTED": 0}),
        ("UDF_UNKNOWN", {"/isolinux.cfg": b"default linux", "/isolinux.bin": bytes(512)},
         0, {"DETECTED": 0, "SELECTED": 0x0407, "INFERRED": 1}),
        ("UDF_UNKNOWN_V6", {"/isolinux.cfg": b"default linux", "/isolinux.bin": bytes(512),
                            "/ldlinux.c32": bytes(512)},
         0, {"DETECTED": 0, "SELECTED": 0x0604, "INFERRED": 1}),
        ("UDF_UNREADABLE", {"/isolinux.cfg": b"default linux", "/isolinux.bin": b""},
         4, {"DETECTED": 0, "SELECTED": 0x0407, "INFERRED": 1}),
        ("UDF_WINDOWS", {"/bootmgr": bytes(512), "/sources/install.wim": bytes(512)},
         16, {"FILES": 2}),
        ("UDF_EMBEDDED", {"/boot/efi.img": bytes(512)}, 8, {"FILES": 1}),
        # Enough entries to cross multiple directory sectors.
        ("UDF_MANY", {f"/files/entry-{i:03d}": bytes(i % 3) for i in range(140)},
         0, {"FILES": 140, "BLOCKS": 93}),
        ("UDF_DEEP", {"/" + "/".join(f"level{i}" for i in range(12)) + "/data": b"x"},
         0, {"FILES": 1, "BLOCKS": 1}),
    )
    for name, entries, pending, expected in cases:
        image = make_image(directory, name, entries, mkisofs)
        analyze(scanner, image, status=3 if pending else 0, pending=pending,
                FILESYSTEM=2, TREE=1, **expected)
    for name, path in (
        ("UDF_LONG_PATH", "/" + "/".join("a" * 60 for i in range(5)) + "/file"),
        ("UDF_LONG_CONFIG", "/" + "a" * 60 + "/" + "b" * 60 + "/syslinux.cfg"),
    ):
        image = make_image(directory, name, {path: b"x"}, mkisofs)
        analyze(scanner, image, status=1, READS=0)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("scanner", type=Path)
    parser.add_argument("reader", type=Path)
    parser.add_argument("reader_contract", type=Path)
    parser.add_argument("--mkisofs", required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="rufus-udf-") as temporary:
        directory = Path(temporary)
        image = check_combined(args.scanner, args.reader, directory, args.mkisofs)
        subprocess.run([str(args.reader_contract), str(image)], check=True, timeout=10)
        check_views(args.scanner, args.reader, directory, args.mkisofs)
        check_variants(args.scanner, directory, args.mkisofs)
    print("UDF fixtures: real traversal, byte reads, UDF-first views, Unicode,")
    print("bootloader metadata, cancellation and failure cleanup pass.")


if __name__ == "__main__":
    main()
