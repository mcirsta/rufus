# SPDX-License-Identifier: GPL-3.0-or-later
"""Inspect native-tool FAT fixtures embedded in ordinary ISO image files."""
import argparse
from pathlib import Path
import subprocess
import tempfile

from test_iso_analyze import analyze, make_image
from test_udf_images import make_image as make_udf

ARCHES = ("ia32", "x64", "arm", "aa64", "ia64", "riscv64", "loongarch64", "ebc")


def run(command):
    result = subprocess.run([str(arg) for arg in command], capture_output=True, timeout=15)
    assert result.returncode == 0, (command, result.stdout, result.stderr)
    return result


def make_fat(directory, name, args, files=(), fat=12, sector=512, empty=False, filler=0):
    image = directory / f"{name}.fat"
    kib = 34816 if fat == 32 else 16384 if fat == 16 else 4096 if sector == 4096 else 1440
    run([args.mkfs_fat, "-C", "-F", fat, "-S", sector, "-s", "1", "-n", "EFI_TEST",
         "-i", "12345678", image, kib])
    if filler:
        payload = directory / "filler"
        payload.write_bytes(bytes(filler))
        run([args.mcopy, "-i", image, payload, "::/filler"])
    if files:
        run([args.mmd, "-i", image, "::/EFI", "::/EFI/BOOT"])
    payload = directory / "loader"
    payload.write_bytes(b"" if empty else b"EFI filename fixture, not an executable.\n" * 16)
    for name in files:
        run([args.mcopy, "-i", image, payload, "::/EFI/BOOT/" + name])
    return image.read_bytes()


def inspect(probe, image, found=False, status=0, fault="none", path="/boot/efi.img", view="iso"):
    result = subprocess.run([str(probe), str(image), path, fault, view],
                            capture_output=True, text=True, timeout=10)
    assert result.returncode == status, (image.name, fault, result.stdout, result.stderr)
    values = dict(line.split("=", 1) for line in result.stdout.splitlines())
    assert values["FOUND"] == str(int(found)), (image.name, values, result.stderr)
    assert int(values["MAX_BLOCKS"]) <= 16, values
    if view == "udf":
        assert values["ISO_READS"] == "0", values
    if fault.startswith("fail-"):
        assert values["INJECTED"] == "1", values
    return values, result.stderr


def check_formats(directory, args):
    first = None
    for fat, sector, filler in ((12, 512, 0), (16, 512, 0), (32, 512, 0),
                                (12, 4096, 0), (32, 512, 33554432)):
        name = f"{fat}-{sector}-{filler}"
        data = make_fat(directory, "fat-" + name, args,
                        ["boot" + arch + ".efi" for arch in ARCHES], fat=fat, sector=sector, filler=filler)
        image = make_image(directory, "iso-" + name, {"/boot/efi.img": data}, args.bsdtar)
        values, logs = inspect(args.probe, image, found=True)
        for arch in ARCHES:
            assert f"'boot{arch}.efi'" in logs, (arch, logs)
        assert int(values["BLOCKS"]) < 128, values  # Small reads, even for the 34 MiB FAT32 file.
        analyze(args.scanner, image, status=3, pending=32, EFI=0x8000, TREE=1)
        if first is None:
            first = (data, image, values)
    data, image, values = first
    for point in range(1, int(values["READS"]) + 1):
        inspect(args.probe, image, status=3, fault=f"fail-read:{point}")
    for point in (1, 3, 6, 12):
        inspect(args.probe, image, status=2, fault=f"cancel:{point}")
    for fault, status in (("fail-context", 1), ("fail-open", 3), ("fail-cache", 3)):
        inspect(args.probe, image, status=status, fault=fault)
    inspect(args.probe, image, found=True)  # Recovery after the injected failures.
    return data


def check_absence(directory, args):
    for name, files, empty in (
        ("no-efi-dir", (), False),
        ("other-name", ("random.efi",), False),
        ("empty-loader", ("bootx64.efi",), True),
    ):
        data = make_fat(directory, name, args, files, empty=empty)
        image = make_image(directory, name, {"/boot/efi.img": data}, args.bsdtar)
        inspect(args.probe, image)
        analyze(args.scanner, image, EFI=0, TREE=1)
    # A correctly named directory is not an EFI file.
    image_path = directory / "directory-loader.fat"
    make_fat(directory, "directory-loader", args, ["random.efi"])
    run([args.mmd, "-i", image_path, "::/EFI/BOOT/bootx64.efi"])
    image = make_image(directory, "directory-loader", {"/boot/efi.img": image_path.read_bytes()}, args.bsdtar)
    inspect(args.probe, image)
    analyze(args.scanner, image, EFI=0)
    # Force the searched directories near EOF, beyond the initial 32 KiB cache.
    data = make_fat(directory, "near-end", args, ["bootx64.efi"], filler=1430000)
    image = make_image(directory, "near-end", {"/boot/efi.img": data}, args.bsdtar)
    inspect(args.probe, image, found=True)
    analyze(args.scanner, image, status=3, pending=32, EFI=0x8000)


def partitioned(data, start=2048, active=0x80, partition_type=1, count=None):
    image = bytearray(start * 512) + data
    image[510:512] = b"\x55\xaa"
    image[446] = active
    image[450] = partition_type
    image[454:458] = start.to_bytes(4, "little")
    image[458:462] = (len(data) // 512 if count is None else count).to_bytes(4, "little")
    return image


def check_partition_and_limits(directory, args, data):
    for name, content, success in (
        ("active-mbr", partitioned(data), True),
        ("unaligned-mbr", partitioned(data, start=1), False),
        ("inactive-mbr", partitioned(data, active=0), False),
        ("other-partition", partitioned(data, partition_type=0x83), False),
        ("short-partition", partitioned(data, count=1), False),
        ("outside-partition", partitioned(data, count=len(data)), False),
        ("truncated-fat", data[:len(data) // 2], False),
        ("invalid-fat", bytes(4096), False),
        ("empty-image", b"", False),
    ):
        image = make_image(directory, name, {"/boot/efi.img": content}, args.bsdtar)
        inspect(args.probe, image, found=success, status=0 if success else 3)
        analyze(args.scanner, image, status=3, pending=32 if success else 8,
                EFI=0x8000 if success else 0, TREE=1)
    image = make_image(directory, "missing-image", {"/boot/data": b"data"}, args.bsdtar)
    inspect(args.probe, image, status=3)
    inspect(args.probe, image, status=3, path="/boot")
    analyze(args.scanner, image, EFI=0)
    # The coordinator now reads embedded FAT through the selected UDF filesystem.
    image = make_udf(directory, "UDF_EFI", {"/boot/efi.img": data}, args.mkisofs)
    analyze(args.scanner, image, status=3, pending=32, FILESYSTEM=2, EFI=0x8000)


def check_el_torito(directory, args, data):
    tree = directory / "eltorito-tree"
    tree.mkdir()
    (tree / "efi.img").write_bytes(partitioned(data))
    image = directory / "eltorito.iso"
    sectors = (tree / "efi.img").stat().st_size // 512
    run([args.bsdtar, "--format", "iso9660", "--options",
         f"iso9660:boot=efi.img,iso9660:boot-type=hard-disk,iso9660:boot-load-size={sectors}",
         "-cf", image, "-C", tree, "."])
    # libarchive's hard-disk catalog records one load sector. A FAT volume
    # extending beyond that synthetic range must stay pending, not read past it.
    inspect(args.probe, image, status=3, path="/[BOOT]/0-Boot-HardDisk.img")
    analyze(args.scanner, image, status=3, pending=8, EFI=0)
    # No-emulation supports an explicit complete range in this metadata fixture.
    image = directory / "eltorito-complete.iso"
    run([args.bsdtar, "--format", "iso9660", "--options",
         f"iso9660:boot=efi.img,iso9660:boot-type=no-emulation,iso9660:boot-load-size={sectors}",
         "-cf", image, "-C", tree, "."])
    inspect(args.probe, image, found=True, path="/[BOOT]/0-Boot-NoEmul.img")
    analyze(args.scanner, image, status=3, pending=32, EFI=0x8000)


def check_reader_failures(directory, args, data):
    # These invalid-directory cases run only against the corrected shared reader.
    # Start with a real FAT12 filesystem and locate its generated short entries.
    assert data[13] == 1 and int.from_bytes(data[11:13], "little") == 512
    reserved = int.from_bytes(data[14:16], "little")
    fat_sectors = int.from_bytes(data[22:24], "little")
    root_sector = reserved + data[16] * fat_sectors
    root_entries = int.from_bytes(data[17:19], "little")
    first_data = root_sector + (root_entries * 32 + 511) // 512
    root = data[root_sector * 512:(root_sector + 1) * 512]
    efi_offset = root.find(b"EFI        ")
    assert efi_offset >= 0 and efi_offset % 32 == 0
    efi_cluster = int.from_bytes(root[efi_offset + 26:efi_offset + 28], "little")
    efi_start = (first_data + efi_cluster - 2) * 512
    boot_offset = data.find(b"BOOT       ", efi_start, efi_start + 512)
    assert boot_offset >= 0
    boot_cluster = int.from_bytes(data[boot_offset + 26:boot_offset + 28], "little")
    boot_start = (first_data + boot_cluster - 2) * 512
    for name, successor in (("cycle", boot_cluster), ("free-chain", 0), ("reserved-chain", 1)):
        broken = bytearray(data)
        # Full directory with deleted entries, so the FAT chain must be consulted.
        broken[boot_start:boot_start + 512] = (b"\xe5" + bytes(31)) * 16
        for copy in range(data[16]):
            offset = (reserved + copy * fat_sectors) * 512 + boot_cluster + boot_cluster // 2
            pair = int.from_bytes(broken[offset:offset + 2], "little")
            pair = (pair & 0x000f) | (successor << 4) if boot_cluster & 1 else (pair & 0xf000) | successor
            broken[offset:offset + 2] = pair.to_bytes(2, "little")
        image = make_image(directory, name, {"/boot/efi.img": broken}, args.bsdtar)
        inspect(args.probe, image, status=3)
        analyze(args.scanner, image, status=3, pending=8, EFI=0)
    broken = bytearray(data)
    broken[22:24] = bytes(2)
    broken[36:40] = bytes(4)
    image = make_image(directory, "no-fat", {"/boot/efi.img": broken}, args.bsdtar)
    inspect(args.probe, image, status=3)  # libfat_open must release its cached boot sector.


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("probe", type=Path)
    parser.add_argument("scanner", type=Path)
    parser.add_argument("--bsdtar", required=True)
    parser.add_argument("--mkisofs", required=True)
    parser.add_argument("--mkfs-fat", required=True)
    parser.add_argument("--mcopy", required=True)
    parser.add_argument("--mmd", required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="rufus-embedded-efi-") as temporary:
        directory = Path(temporary)
        data = check_formats(directory, args)
        check_absence(directory, args)
        check_partition_and_limits(directory, args, data)
        check_el_torito(directory, args, data)
        check_reader_failures(directory, args, data)
    print("Embedded EFI: real FAT12/16/32/4K images, names, MBR, bounded reads,")
    print("absence/partial results, ISO/UDF coordination and cleanup pass.")


if __name__ == "__main__":
    main()
