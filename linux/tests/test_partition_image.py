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
ESP_TYPE = uuid.UUID("C12A7328-F81F-11D2-BA4B-00A0C93EC93B")
MSR_TYPE = uuid.UUID("E3C9E316-0B5C-4DB8-817D-F92DF00215AE")
runs = 0


def trailing_extent(size, payload, track=63):
    if payload == 0:
        return 0, 0
    track_bytes = track * SECTOR_BYTES
    length = (payload + track_bytes - 1) // track_bytes * track_bytes
    end = size - GPT_TAIL_SECTORS * SECTOR_BYTES
    return (end - length) // track_bytes * track_bytes, length


def extent(size, cluster, track=63, payload=0):
    # Original Rufus order, independently calculated in sectors/bytes.
    end = trailing_extent(size, payload, track)[0] if payload else size - GPT_TAIL_SECTORS * SECTOR_BYTES
    usable_bytes = end - MIB
    track_bytes = track * SECTOR_BYTES
    length = usable_bytes // track_bytes * track_bytes
    return MIB, length // cluster * cluster


def make_image(root, name, size, cluster=512, track=63, payload=0):
    image = root / name
    offset, length = extent(size, cluster, track, payload)
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


def windows_extents(size, cluster, track):
    # drive.c: 260-MiB ESP, then 128-MiB MSR, then the data partition.
    track_bytes = track * 512
    after_esp = ((MIB + 260*MIB + track_bytes-1) // track_bytes * track_bytes) // cluster * cluster
    main = ((after_esp + 128*MIB + track_bytes-1) // track_bytes * track_bytes) // cluster * cluster
    length = ((size - 33*512 - main) // track_bytes * track_bytes) // cluster * cluster
    return [(MIB, 260*MIB), (after_esp, 128*MIB), (main, length)]


def invoke(args, image, size, cluster=512, track=63, fault="none", error=0, started=True, payload=0, togo=False, persistence=0):
    global runs
    command = [str(args.probe), str(image), str(size), str(cluster), str(track), fault, str(payload)]
    if togo:
        command.extend(["togo", str(persistence)])
    process = subprocess.run(command,
                             capture_output=True, timeout=TOOL_TIMEOUT_SECONDS)
    runs += 1
    assert process.returncode == 0, (process.returncode, process.stdout, process.stderr)
    values = {key.decode(): int(value) for key, value in re.findall(rb"([A-Z_]+)=(-?\d+)", process.stdout)}
    assert values["ERROR"] == error and values["STARTED"] == started, (fault, values, process.stderr)
    assert values["FDS"] == 0 and values["FLAGS"] == 1, values
    assert values["POSITION"] == (-1 if fault == "pipe" else 17), values
    assert values["WRITES"] == int(started) and values["SYNCS"] >= int(started), values
    if error == 0:
        expected = windows_extents(size, cluster, track)[2] if togo else extent(size, cluster, track, payload)
        assert (values["OFFSET"], values["SIZE"]) == expected, values
        assert (values["UEFI_OFFSET"], values["UEFI_SIZE"]) == trailing_extent(size, payload, track), values
    return values


def inspect_gpt(args, image, size, cluster, track=63, payload=0, togo=False):
    total = size // 512
    expected_start, expected_size = extent(size, cluster, track, payload)
    expected = [(expected_start, expected_size, "Main Data Partition", 0)]
    if payload:
        boot_start, boot_size = trailing_extent(size, payload, track)
        expected.append((boot_start, boot_size, "UEFI:NTFS", 1 << 63))
        assert expected_start + expected_size <= boot_start
    types = [DATA_TYPE] * len(expected)
    if togo:
        names = ["EFI System Partition", "Microsoft Reserved Partition", "Main Data Partition"]
        expected = [(start, length, name, 0) for (start, length), name in zip(windows_extents(size, cluster, track), names)]
        types = [ESP_TYPE, MSR_TYPE, DATA_TYPE]
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
        partition_ids = set(identifiers)
        for number, (start, length, name, attrs) in enumerate(expected):
            entry = table[number*128:(number+1)*128]
            assert uuid.UUID(bytes_le=entry[:16]) == types[number]
            assert uuid.UUID(bytes_le=entry[16:32]).int != 0 and entry[16:32] not in partition_ids
            partition_ids.add(entry[16:32])
            assert struct.unpack_from("<QQQ", entry, 32) == (start//512, (start+length)//512-1, attrs)
            assert entry[56:128].decode("utf-16le").rstrip("\0") == name
        used = len(expected) * 128
        assert table[used:] == bytes(len(table)-used)
    listing = subprocess.run([args.sfdisk, "--json", str(image)], check=True, capture_output=True, timeout=TOOL_TIMEOUT_SECONDS)
    parsed = json.loads(listing.stdout)["partitiontable"]
    assert parsed["label"] == "gpt" and parsed["firstlba"] == 34 and parsed["lastlba"] == total-34
    assert len(parsed["partitions"]) == len(expected)
    for partition, (start, length, name, attrs), expected_type in zip(parsed["partitions"], expected, types):
        assert (partition["start"]*512, partition["size"]*512) == (start, length)
        assert uuid.UUID(partition["type"]) == expected_type and partition["name"] == name
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


def check_uefi_layouts(args, root):
    payload = args.uefi_ntfs.stat().st_size
    assert payload > 0
    for number, (size, cluster, track) in enumerate([
            (64*MIB, 512, 63), (64*MIB+512, 4096, 63), (128*MIB, 32768, 32)]):
        image, segments = make_image(root, f"uefi{number}.img", size, cluster, track, payload)
        invoke(args, image, size, cluster, track, payload=payload)
        inspect_gpt(args, image, size, cluster, track, payload)
        check_preserved(image, size, segments)
        # Replacing either layout must remove old table entries without touching
        # the filesystem regions. The small partition still contains sentinels.
        invoke(args, image, size, cluster, track)
        inspect_gpt(args, image, size, cluster, track)
        invoke(args, image, size, cluster, track, payload=payload)
        inspect_gpt(args, image, size, cluster, track, payload)
        check_preserved(image, size, segments)

    image, segments = make_image(root, "uefi-64k.img", 64*MIB, 65536, payload=payload)
    invoke(args, image, 64*MIB, 65536, payload=payload)
    inspect_gpt(args, image, 64*MIB, 65536, payload=payload)
    check_preserved(image, 64*MIB, segments)

    image, segments = make_image(root, "uefi-invalid.img", 64*MIB, payload=payload)
    before = digest(image)
    invalid = [("extra-add-fail", payload, errno.ENOMEM),
               ("extra-attrs-fail", payload, errno.EIO),
               ("extra-attrs-mismatch", payload, errno.EBADMSG),
               ("extra-offset-mismatch", payload, errno.EBADMSG),
               ("cancel-before-write", payload, errno.ECANCELED),
               ("block-request", payload, errno.ENOTSUP),
               ("none", 63*MIB, errno.ENOSPC), ("none", 64*MIB, errno.ENOSPC),
               ("none", (1 << 64)-1, errno.ENOSPC)]
    for fault, requested, error in invalid:
        invoke(args, image, 64*MIB, payload=requested, fault=fault, error=error, started=False)
        assert digest(image) == before
    for fault, error in [("damaged-primary", errno.EBADMSG), ("damaged-backup", errno.EBADMSG),
                         ("sync-fail", errno.EIO), ("cancel-after-write", errno.ECANCELED)]:
        image, segments = make_image(root, "uefi-" + fault + ".img", 64*MIB, payload=payload)
        invoke(args, image, 64*MIB, payload=payload, fault=fault, error=error)
        check_preserved(image, 64*MIB, segments)
        if fault in ("sync-fail", "cancel-after-write"):
            inspect_gpt(args, image, 64*MIB, 512, payload=payload)


def check_windows_layouts(args, root):
    for cluster, track in [(512, 63), (4096, 63), (65536, 32)]:
        size = 512*MIB + 512
        image, segments = make_image(root, "windows.img", size, cluster, track)
        report = invoke(args, image, size, cluster, track, togo=True)
        esp, msr, main = windows_extents(size, cluster, track)
        assert (report["ESP_OFFSET"], report["ESP_SIZE"]) == esp
        assert (report["MSR_OFFSET"], report["MSR_SIZE"]) == msr
        assert esp[0] + esp[1] <= msr[0] and msr[0] + msr[1] <= main[0]
        inspect_gpt(args, image, size, cluster, track, togo=True)
        check_preserved(image, size, segments)
        # Replacing the layout in either direction must remove stale entries.
        invoke(args, image, size, cluster, track)
        inspect_gpt(args, image, size, cluster, track)
        invoke(args, image, size, cluster, track, togo=True)
        inspect_gpt(args, image, size, cluster, track, togo=True)
    size = 512*MIB
    image, segments = make_image(root, "windows-invalid.img", size)
    before = digest(image)
    for options, error in [({"size": 128*MIB}, errno.ENOSPC),
                           ({"size": size, "payload": MIB}, errno.EINVAL),
                           ({"size": size, "persistence": 256*MIB}, errno.EINVAL),
                           ({"size": size, "fault": "extra-add-fail"}, errno.ENOMEM),
                           ({"size": size, "fault": "extra-offset-mismatch"}, errno.EBADMSG)]:
        invoke(args, image, togo=True, started=False, error=error, **options)
        assert digest(image) == before


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("probe", type=Path)
    parser.add_argument("format_probe", type=Path)
    for name in ("sfdisk", "mkfs", "fsck", "mdir"):
        parser.add_argument("--" + name, required=True)
    parser.add_argument("--uefi-ntfs", type=Path, required=True)
    parser.add_argument("--windows-to-go-only", action="store_true")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="rufus-gpt-image-") as temporary:
        root = Path(temporary)
        check_windows_layouts(args, root)
        if args.windows_to_go_only:
            print(f"{runs} Windows To Go GPT layout cases passed; disposable files only.")
            return
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
                   ("none", {"cluster": 64*MIB}, errno.EINVAL),
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
        for cluster in (128*1024, 32*MIB):
            image, segments = make_image(root, f"exfat-{cluster}.img", 256*MIB, cluster)
            invoke(args, image, 256*MIB, cluster)
            inspect_gpt(args, image, 256*MIB, cluster)
            check_preserved(image, 256*MIB, segments)
        check_uefi_layouts(args, root)
    print(f"{runs} native GPT image cases passed, including FAT32 integration and UEFI:NTFS layouts; disposable files only.")


if __name__ == "__main__":
    main()
