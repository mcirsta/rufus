# SPDX-License-Identifier: GPL-3.0-or-later
"""Prepare superfloppy media and format FAT16/FAT32 at byte zero, using files only."""
import argparse
import errno
import json
from pathlib import Path
import re
import struct
import subprocess
import tempfile

import test_mbr_image as mbr
import test_mbr_prepare as metadata
import test_fat_format as fat

MIB = 1024 * 1024
PREFIX_BYTES = 8 * MIB  # Win32 ClearMBRGPT, without a new partition prefix.
TAIL_BYTES = MIB
DISK_BYTES = 65 * MIB
runs = 0


def invoke(args, image, size, operation="prepare", fault="none", error=0,
           started=True, syncs=1, sector=512):
    global runs
    command = [args.probe, image, str(size), "512", "63", str(sector), operation, fault, "32", "sfd"]
    result = subprocess.run(command, capture_output=True, timeout=10)
    runs += 1
    assert result.returncode == 0, (result.returncode, result.stdout, result.stderr)
    values = {key.decode(): int(value) for key, value in re.findall(rb"([A-Z_]+)=(-?\d+)", result.stdout)}
    assert values["ERROR"] == error and values["STARTED"] == started, (fault, values, result.stderr)
    assert values["WRITES"] == 0, "SFD must never create a partition table"
    assert values["SYNCS"] == syncs, values
    assert values["SYNC_ERROR"] == (errno.EIO if fault == "sync-fail" else 0), values
    assert values["FDS"] == 0 and values["FLAGS"] == 1, values
    assert values["POSITION"] == (-1 if fault == "pipe" else 17), values
    if error == 0:
        assert values["OFFSET"] == 0 and values["SIZE"] == size, values
    print(f"PASS SFD {operation} {fault}: {values}", flush=True)
    return values


def inspect_cleared(image, size):
    assert image.stat().st_size == size
    prefix = min(PREFIX_BYTES, size)
    tail = min(TAIL_BYTES, size - prefix)
    middle = size - prefix - tail
    with image.open("rb") as stream:
        assert stream.read(prefix) == bytes(prefix), "Old primary metadata remains"
        assert stream.read(middle) == b"S" * middle, "Data outside clearing ranges changed"
        assert stream.read() == bytes(tail), "Old backup metadata remains"


def inspect_fat(args, image, size, report, fat_bits=32, cluster=512):
    with image.open("rb") as stream:
        boot = stream.read(512)
        if fat_bits == 32:
            stream.seek(6 * 512)
            assert stream.read(512) == boot
        else:
            reserved = struct.unpack_from("<H", boot, 14)[0]
            fat_sectors = struct.unpack_from("<H", boot, 22)[0]
            root_entries = struct.unpack_from("<H", boot, 17)[0]
            assert reserved > 0 and fat_sectors > 0 and root_entries == 512
            stream.seek(reserved * 512)
            first_fat = stream.read(fat_sectors * 512)
            assert first_fat[:4] == b"\xf8\xff\xff\xff"
            assert stream.read(fat_sectors * 512) == first_fat
        stream.seek(size - TAIL_BYTES)
        assert stream.read() == bytes(TAIL_BYTES), "Backup GPT reappeared"
    assert boot[510:] == b"\x55\xaa" and boot[16] == 2
    assert boot[446:510] == bytes(64), "Unexpected partition entries in FAT boot sector"
    assert struct.unpack_from("<I", boot, 28)[0] == 0, "SFD has no hidden sectors"
    identity_offset = 39 if fat_bits == 16 else 67
    assert struct.unpack_from("<I", boot, identity_offset)[0] == 0x1234abcd
    assert boot[identity_offset + 4:identity_offset + 15] == b"RUFUS SFD  "
    assert boot[identity_offset + 15:identity_offset + 23] == f"FAT{fat_bits}   ".encode()
    assert struct.unpack_from("<H", boot, 11)[0] == 512 and boot[13] * 512 == cluster
    short_sectors = struct.unpack_from("<H", boot, 19)[0] if fat_bits == 16 else 0
    sectors = short_sectors or struct.unpack_from("<I", boot, 32)[0]
    assert report["BYTES"] == sectors * 512
    assert 0 <= size - report["BYTES"] < 63 * 512
    if fat_bits == 16:
        data_sectors = sectors - reserved - 2 * fat_sectors - root_entries * 32 // 512
        assert 4085 <= data_sectors // boot[13] <= 65524
    for command in ([args.fsck, "-n", image], [args.mdir, "-i", image, "::"]):
        checked = subprocess.run(command, capture_output=True, timeout=15)
        assert checked.returncode == 0, (command, checked.stdout, checked.stderr)
    table = subprocess.run([args.sfdisk, "--json", image], capture_output=True, timeout=10)
    if table.returncode == 0:
        # sfdisk can call the FAT 55-AA marker an empty DOS label. There must
        # be no actual entries, disk signature or surviving backup GPT.
        layout = json.loads(table.stdout)["partitiontable"]
        assert layout["label"] == "dos" and layout.get("id", "0x00000000") == "0x00000000"
        assert not layout.get("partitions"), layout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("probe", type=Path)
    parser.add_argument("gpt_probe", type=Path)
    parser.add_argument("fat_probe", type=Path)
    parser.add_argument("--mkfs", required=True)
    parser.add_argument("--fsck", required=True)
    parser.add_argument("--mdir", required=True)
    parser.add_argument("--sfdisk", required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="rufus-sfd-prepare-") as temporary:
        root = Path(temporary)
        # No MBR 2-TiB ceiling or track/cluster rounding in a whole-device layout.
        for size in (512, MIB, 2 * MIB, DISK_BYTES + 512, 2**41 + 512):
            invoke(args, root / "absent", size, "plan", started=False, syncs=0)
        for sector in (1024, 2048, 4096):
            invoke(args, root / "absent", DISK_BYTES, "plan", started=False, syncs=0, sector=sector)
        for size, sector in ((0, 512), (511, 512), (DISK_BYTES + 1, 512), (2**63, 512),
                             (DISK_BYTES, 0), (DISK_BYTES, 8192)):
            invoke(args, root / "absent", size, "plan", error=errno.EINVAL,
                   started=False, syncs=0, sector=sector)
        for size in (512, MIB, 2 * MIB, 8 * MIB, 8 * MIB + MIB // 2, 9 * MIB, DISK_BYTES):
            image = metadata.seed_target(root, args.gpt_probe, size)
            # seed_target handles whole MiB; include a partial-MiB boundary here.
            with image.open("ab") as stream:
                stream.write(b"S" * (size - image.stat().st_size))
            invoke(args, image, size)
            inspect_cleared(image, size)
            image.unlink()
        for fault, error, started, syncs in (
            ("readonly", errno.EINVAL, False, 0), ("append", errno.EINVAL, False, 0),
            ("pipe", errno.ENOTSUP, False, 0), ("block-request", errno.ENOTSUP, False, 0),
            ("cancel-before", errno.ECANCELED, False, 0),
            ("clear-write-fail", errno.EIO, True, 1), ("clear-partial-fail", errno.EIO, True, 1),
            ("clear-zero", errno.EIO, True, 1), ("clear-end-fail", errno.EIO, True, 1),
            ("clear-read-fail", errno.EIO, True, 1), ("clear-corrupt", errno.EBADMSG, True, 1),
            ("clear-tail-read-fail", errno.EIO, True, 1), ("clear-tail-corrupt", errno.EBADMSG, True, 1),
            ("cancel-during-clear", errno.ECANCELED, True, 1),
            ("cancel-after-clear", errno.ECANCELED, True, 1),
            ("resize-after-clear", errno.ESTALE, True, 1), ("sync-fail", errno.EIO, True, 1),
            ("sync-eintr", 0, True, 2), ("clear-eintr", 0, True, 1),
            ("clear-short", 0, True, 1), ("replace-path", 0, True, 1),
        ):
            image = metadata.seed_target(root, args.gpt_probe)
            before = mbr.digest(image)
            invoke(args, image, DISK_BYTES, fault=fault, error=error, started=started, syncs=syncs)
            if fault == "replace-path":
                assert image.read_bytes() == b"untouched"
                image.unlink()
                image = Path(str(image) + ".held")
            if not started:
                assert mbr.digest(image) == before
            if error == 0:
                inspect_cleared(image, DISK_BYTES)
            image.unlink()
        image = metadata.seed_target(root, args.gpt_probe)
        before = mbr.digest(image)
        invoke(args, image, DISK_BYTES + 512, error=errno.ESTALE, started=False, syncs=0)
        assert mbr.digest(image) == before
        # Follow native metadata preparation with the existing native formatter.
        fat_args = argparse.Namespace(probe=args.fat_probe, mkfs=args.mkfs)
        formats = ((32, DISK_BYTES, 512), (32, DISK_BYTES + 512, 512),
                   (16, 16 * MIB, 512), (16, DISK_BYTES, 2048), (16, DISK_BYTES + 512, 2048))
        for fat_bits, size, cluster in formats:
            image = metadata.seed_target(root, args.gpt_probe, size)
            with image.open("ab") as stream:
                stream.write(b"S" * (size - image.stat().st_size))
            invoke(args, image, size)
            fat_args.fat_bits = fat_bits
            report = fat.invoke(fat_args, image, offset=0, size=size, cluster=cluster, label="RUFUS SFD")
            inspect_fat(args, image, size, report, fat_bits, cluster)
            image.unlink()
    print(f"{runs} SFD planning/preparation cases and {fat.runs} native FAT16/FAT32 formats passed; fixtures removed.")


if __name__ == "__main__":
    main()
