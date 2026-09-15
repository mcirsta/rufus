# SPDX-License-Identifier: GPL-3.0-or-later
"""Inspect native GPT bytes and format only the resulting partition extent."""
import argparse
import errno
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import uuid
import zlib

from test_fat_format import copy_volume, invoke as format_image

MIB = 1024 * 1024
SECTOR_BYTES = 512
GPT_FIRST_USABLE_SECTOR = 34
GPT_TAIL_SECTORS = 33
TOOL_TIMEOUT_SECONDS = 10
DATA_TYPE = uuid.UUID("EBD0A0A2-B9E5-4433-87C0-68B6B72699C7")
runs = 0


def extent(size, cluster, track=63):
    # Original Rufus order, independently calculated in sectors/bytes.
    usable_bytes = size - GPT_TAIL_SECTORS * SECTOR_BYTES - MIB
    track_bytes = track * SECTOR_BYTES
    length = usable_bytes // track_bytes * track_bytes
    return MIB, length // cluster * cluster


def make_image(root, name, size, cluster=512, track=63):
    image = root / name
    offset, length = extent(size, cluster, track)
    segments = [(GPT_FIRST_USABLE_SECTOR * SECTOR_BYTES, b"P" * (MIB - GPT_FIRST_USABLE_SECTOR * SECTOR_BYTES)), (offset, b"D" * 65536),
                (size//2, b"M" * 65536), (offset + length, b"T" * (size - GPT_TAIL_SECTORS * SECTOR_BYTES - offset - length))]
    with image.open("wb") as target:
        target.truncate(size)
        for start, data in segments:
            target.seek(start)
            target.write(data)
    return image, segments


def check_preserved(image, size, segments, formatted=None):
    assert image.stat().st_size == size
    ranges = [(GPT_FIRST_USABLE_SECTOR * SECTOR_BYTES, size - GPT_TAIL_SECTORS * SECTOR_BYTES)]
    if formatted:
        start, length = formatted
        ranges = [(GPT_FIRST_USABLE_SECTOR * SECTOR_BYTES, start), (start + length, size - GPT_TAIL_SECTORS * SECTOR_BYTES)]
    with image.open("rb") as source:
        for start, end in ranges:
            source.seek(start)
            while start < end:
                count = min(MIB, end - start)
                expected = bytearray(count)
                for offset, data in segments:
                    low, high = max(start, offset), min(start + count, offset + len(data))
                    if low < high:
                        expected[low-start:high-start] = data[low-offset:high-offset]
                assert source.read(count) == expected, (start, count)
                start += count


def digest(image):
    with image.open("rb") as source:
        result = hashlib.sha256()
        while data := source.read(MIB):
            result.update(data)
        return result.digest()


def invoke(args, image, size, cluster=512, track=63, fault="none", error=0, started=True):
    global runs
    process = subprocess.run([str(args.probe), str(image), str(size), str(cluster), str(track), fault],
                             capture_output=True, timeout=TOOL_TIMEOUT_SECONDS)
    runs += 1
    assert process.returncode == 0, (process.returncode, process.stdout, process.stderr)
    values = {key.decode(): int(value) for key, value in re.findall(rb"([A-Z_]+)=(-?\d+)", process.stdout)}
    assert values["ERROR"] == error and values["STARTED"] == started, (fault, values, process.stderr)
    assert values["FDS"] == 0 and values["FLAGS"] == 1, values
    assert values["POSITION"] == (-1 if fault == "pipe" else 17), values
    assert values["WRITES"] == int(started) and values["SYNCS"] >= int(started), values
    if error == 0:
        assert (values["OFFSET"], values["SIZE"]) == extent(size, cluster, track), values
    return values


def inspect_gpt(args, image, size, cluster, track=63):
    total = size // 512
    expected_start, expected_size = extent(size, cluster, track)
    tables, identifiers, raw = [], [], []
    with image.open("rb") as source:
        mbr = source.read(512)
        assert mbr[:440] == bytes(440) and mbr[510:] == b"\x55\xaa"
        assert mbr[446] == 0 and mbr[450] == 0xee and mbr[462:510] == bytes(48)
        assert struct.unpack_from("<II", mbr, 454) == (1, min(total-1, 0xffffffff))
        raw.append(mbr)
        for location, other, entries_lba in [(1, total-1, 2), (total-1, 1, total-33)]:
            source.seek(location * 512)
            header = source.read(512)
            fields = struct.unpack_from("<8sIIIIQQQQ16sQIII", header)
            signature, revision, header_size, crc, reserved, here, alternate, first, last, disk_id, entries, count, width, table_crc = fields
            valid_signature = signature == b"EFI PART" and revision == 0x10000
            valid_header = header_size == 92 and reserved == 0
            assert valid_signature and valid_header, fields
            checked = bytearray(header[:header_size])
            checked[16:20] = bytes(4)
            assert zlib.crc32(checked) == crc
            assert (here, alternate, first, last) == (location, other, 34, total-34)
            assert (entries, count, width) == (entries_lba, 128, 128)
            assert uuid.UUID(bytes_le=disk_id).int != 0
            identifiers.append(disk_id)
            source.seek(entries_lba * 512)
            table = source.read(count * width)
            assert zlib.crc32(table) == table_crc
            tables.append(table)
            raw.extend([header, table])
        assert identifiers[0] == identifiers[1] and tables[0] == tables[1]
        table = tables[0]
        assert uuid.UUID(bytes_le=table[:16]) == DATA_TYPE
        assert uuid.UUID(bytes_le=table[16:32]).int != 0 and table[16:32] != identifiers[0]
        assert struct.unpack_from("<QQQ", table, 32) == (expected_start//512, (expected_start+expected_size)//512-1, 0)
        assert table[56:128].decode("utf-16le").rstrip("\0") == "Main Data Partition"
        assert table[128:] == bytes(len(table)-128)
    listing = subprocess.run([args.sfdisk, "--json", str(image)], check=True, capture_output=True, timeout=TOOL_TIMEOUT_SECONDS)
    parsed = json.loads(listing.stdout)["partitiontable"]
    assert parsed["label"] == "gpt" and parsed["firstlba"] == 34 and parsed["lastlba"] == total-34
    assert len(parsed["partitions"]) == 1
    partition = parsed["partitions"][0]
    assert (partition["start"]*512, partition["size"]*512) == (expected_start, expected_size)
    assert uuid.UUID(partition["type"]) == DATA_TYPE and partition["name"] == "Main Data Partition"
    subprocess.run([args.sfdisk, "--verify", str(image)], check=True, capture_output=True, timeout=TOOL_TIMEOUT_SECONDS)
    return raw


def check_format(args, root, image, size, cluster, segments):
    metadata = inspect_gpt(args, image, size, cluster)
    offset, length = extent(size, cluster)
    formatter = argparse.Namespace(probe=args.format_probe, mkfs=args.mkfs)
    report = format_image(formatter, image, offset, length, cluster)
    assert 0 <= length - report["BYTES"] < 63*512
    assert metadata == inspect_gpt(args, image, size, cluster)
    check_preserved(image, size, segments, (offset, length))
    with image.open("rb") as source:
        source.seek(offset)
        boot = source.read(512)
        assert struct.unpack_from("<H", boot, 11)[0] == 512 and boot[13]*512 == cluster
        assert struct.unpack_from("<I", boot, 28)[0] == offset//512
    volume = root / "volume.img"
    copy_volume(image, volume, offset, length)
    subprocess.run([args.fsck, "-n", str(volume)], check=True, capture_output=True, timeout=15)
    listing = subprocess.run([args.mdir, "-i", f"{image}@@{offset}", "::"], check=True, capture_output=True, timeout=TOOL_TIMEOUT_SECONDS)
    assert b"No files" in listing.stdout
    volume.unlink()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("probe", type=Path)
    parser.add_argument("format_probe", type=Path)
    for name in ("sfdisk", "mkfs", "fsck", "mdir"):
        parser.add_argument("--" + name, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="rufus-gpt-image-") as temporary:
        root = Path(temporary)
        for number, (size, cluster) in enumerate([(64*MIB, 512), (128*MIB, 1024), (512*MIB, 4096)]):
            image, segments = make_image(root, f"native{number}.img", size, cluster)
            invoke(args, image, size, cluster)
            inspect_gpt(args, image, size, cluster)
            check_preserved(image, size, segments)
            check_format(args, root, image, size, cluster, segments)
            # Recreating the GPT must replace its metadata without touching the volume.
            with image.open("rb") as source:
                source.seek(MIB)
                boot = source.read(512)
            invoke(args, image, size, cluster)
            inspect_gpt(args, image, size, cluster)
            with image.open("rb") as source:
                source.seek(MIB)
                assert source.read(512) == boot
        image, segments = make_image(root, "retained.img", 64*MIB)
        invoke(args, image, 64*MIB, fault="replace-path")
        assert image.read_bytes() == b"retained"
        retained = Path(str(image) + ".held")
        inspect_gpt(args, retained, 64*MIB, 512)
        check_preserved(retained, 64*MIB, segments)
        invalid = [("readonly", {}, errno.EINVAL), ("append", {}, errno.EINVAL), ("pipe", {}, errno.ENOTSUP),
                   ("cancel-before", {}, errno.ECANCELED), ("sector-4k", {}, errno.EINVAL),
                   ("block-request", {}, errno.ENOTSUP),
                   ("add-fail", {}, errno.ENOMEM), ("cancel-before-write", {}, errno.ECANCELED),
                   ("none", {"size": 65*MIB}, errno.ESTALE), ("none", {"size": 64*MIB+1}, errno.EINVAL),
                   ("none", {"cluster": 0}, errno.EINVAL), ("none", {"cluster": 1536}, errno.EINVAL),
                   ("none", {"track": 0}, errno.EINVAL), ("none", {"track": 64}, errno.EINVAL)]
        image, segments = make_image(root, "invalid.img", 64*MIB)
        before = digest(image)
        for fault, options, error in invalid:
            request = {"size": 64*MIB, **options}
            invoke(args, image, fault=fault, error=error, started=False, **request)
            assert digest(image) == before
        for fault, error in [("write-fail", errno.EIO), ("sync-fail", errno.EIO),
                             ("sync-eintr", 0), ("cancel-after-write", errno.ECANCELED),
                             ("damaged-primary", errno.EBADMSG), ("damaged-backup", errno.EBADMSG),
                             ("resize-before-write", errno.ESTALE)]:
            image, segments = make_image(root, fault + ".img", 64*MIB)
            report = invoke(args, image, 64*MIB, fault=fault, error=error, started=fault != "resize-before-write")
            if fault != "resize-before-write": check_preserved(image, 64*MIB, segments)
            if fault in ("sync-fail", "sync-eintr", "cancel-after-write"):
                inspect_gpt(args, image, 64*MIB, 512)
            if fault == "sync-fail": assert report["SYNC_ERROR"] == errno.EIO
            if fault == "sync-eintr": assert report["SYNCS"] == 2
    print(f"{runs} native GPT image cases passed, including 3 FAT32 integration runs; disposable files only.")


if __name__ == "__main__":
    main()
