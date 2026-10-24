# SPDX-License-Identifier: GPL-3.0-or-later
"""Qualify FAT16 on Rufus GPT or MBR extents with disposable regular files only."""
import argparse
from pathlib import Path
import struct
import subprocess
import tempfile

import test_fat_format as fat
import test_partition_image as gpt
import test_mbr_image as mbr

MIB = 1024 * 1024


def check_partition(args, root, disk_size, cluster):
    if args.mbr:
        image = mbr.make_image(root, "disk.img", disk_size)
        mbr.invoke(args.probe, image, disk_size, cluster, file_system=16)
        metadata = mbr.inspect_mbr(image, disk_size, cluster, sfdisk=args.sfdisk, file_system=16)
        offset, size = mbr.extent(disk_size, cluster)
        with image.open("rb") as source:
            prefix = source.read(offset)
            source.seek(offset + size)
            tail = source.read()
    else:
        image, guards = gpt.make_image(root, "disk.img", disk_size, cluster)
        gpt.invoke(args, image, disk_size, cluster)
        metadata = gpt.inspect_gpt(args, image, disk_size, cluster)
        offset, size = gpt.extent(disk_size, cluster)
    formatter = argparse.Namespace(probe=args.format_probe, mkfs=args.mkfs, fat_bits=16)
    report = fat.invoke(formatter, image, offset, size, cluster)
    # Partition metadata and all surrounding bytes survive formatting. The
    # formatter independently verifies libfat readback for either table.
    if args.mbr:
        assert metadata == mbr.inspect_mbr(image, disk_size, cluster, sfdisk=args.sfdisk, file_system=16)
        mbr.invoke(args.probe, image, disk_size, cluster, operation="check", file_system=16)
        with image.open("rb") as source:
            assert source.read(offset) == prefix
            source.seek(offset + size)
            assert source.read() == tail
    else:
        assert metadata == gpt.inspect_gpt(args, image, disk_size, cluster)
        gpt.check_preserved(image, disk_size, guards, (offset, size))
    with image.open("rb") as source:
        source.seek(offset)
        boot = source.read(512)
    sector, spc, reserved, fats, entries, short_total = struct.unpack_from("<HBHBHH", boot, 11)
    fat_sectors = struct.unpack_from("<H", boot, 22)[0]
    total = short_total or struct.unpack_from("<I", boot, 32)[0]
    assert sector == 512 and spc * sector == cluster and fats == 2
    assert boot[510:] == b"\x55\xaa" and boot[54:62] == b"FAT16   "
    assert struct.unpack_from("<I", boot, 39)[0] == 0x1234abcd
    assert boot[43:54] == b"RUFUS TEST "
    assert struct.unpack_from("<I", boot, 28)[0] == offset // sector
    clusters = (total - reserved - fats * fat_sectors - entries * 32 // sector) // spc
    assert 4085 <= clusters <= 65524
    assert total * sector == report["BYTES"] and 0 <= size - total * sector < 63 * sector
    fat.invoke(formatter, image, offset, size, cluster, fault="check-only", started=False)
    volume = root / "volume.img"
    fat.copy_volume(image, volume, offset, size)
    checked = subprocess.run([args.fsck, "-n", str(volume)], capture_output=True, timeout=15)
    assert checked.returncode == 0, checked.stdout + checked.stderr
    listing = subprocess.run([args.mdir, "-i", f"{image}@@{offset}", "::"], capture_output=True, timeout=10)
    assert listing.returncode == 0 and b"No files" in listing.stdout, listing.stdout + listing.stderr
    volume.unlink()
    image.unlink()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("probe", type=Path)
    parser.add_argument("format_probe", type=Path)
    parser.add_argument("--mbr", action="store_true")
    for name in ("sfdisk", "mkfs", "fsck", "mdir"):
        parser.add_argument("--" + name, required=True)
    args = parser.parse_args()
    # Native policy golden choices at each original Rufus capacity step. Check
    # every offered cluster here, including the 64 KiB single-partition layout.
    cases = [(16*MIB, (512, 1024, 2048)), (32*MIB, (1024, 2048, 4096)),
             (64*MIB, (2048, 4096, 8192)), (128*MIB, (4096, 8192, 16384)),
             (256*MIB, (8192, 16384, 32768)), (512*MIB, (16384, 32768, 65536)),
             (1024*MIB, (32768, 65536)), (2048*MIB, (65536,)),
             (4096*MIB-512, (65536,))]
    count = 0
    table = "MBR" if args.mbr else "GPT"
    with tempfile.TemporaryDirectory(prefix="rufus-fat16-partition-") as temporary:
        for size, clusters in cases:
            for cluster in clusters:
                check_partition(args, Path(temporary), size, cluster)
                count += 1
    print(f"{count} FAT16 {table} layouts passed native readback, sfdisk, fsck.fat and mtools; disposable files only.")


if __name__ == "__main__":
    main()
