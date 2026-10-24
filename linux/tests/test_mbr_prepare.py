# SPDX-License-Identifier: GPL-3.0-or-later
"""Replace stale GPT/FAT metadata using Rufus's bounded clearing policy."""
import argparse
import errno
from functools import partial
from pathlib import Path
import subprocess
import tempfile

import test_mbr_image as mbr

MIB = 1024 * 1024
DISK_BYTES = 65 * MIB
PREFIX_BYTES = 9 * MIB  # 8 MiB at disk start, plus 8 MiB at the 1 MiB partition start.
TAIL_BYTES = MIB


def seed_target(directory, probe, size=DISK_BYTES):
    image = directory / "usb.raw"
    with image.open("wb") as stream:
        for unused in range(size // MIB):
            stream.write(b"S" * MIB)
    if size >= DISK_BYTES:
        subprocess.run([probe, image, str(size), "512", "63", "none", str(MIB)], check=True, capture_output=True)
    return image


def inspect_cleared(image, size=DISK_BYTES, file_system=32, non_bootable=False, uefi_payload=0, boot_code=None):
    mbr.inspect_mbr(image, size, file_system=file_system, non_bootable=non_bootable,
                    uefi_payload=uefi_payload, boot_code=boot_code)
    ranges = [(512, min(PREFIX_BYTES, size)), (max(min(PREFIX_BYTES, size), size - TAIL_BYTES), size)]
    if uefi_payload:
        offset, length = mbr.trailing_extent(size, uefi_payload)
        ranges.append((offset, offset + min(length, PREFIX_BYTES - MIB)))
    with image.open("rb") as stream:
        stream.seek(512)
        position = 512
        for start, end in sorted(ranges):
            if start > position:
                assert stream.read(start - position) == b"S" * (start - position), "Clearing exceeded its ranges"
                position = start
            end = max(position, end)
            assert stream.read(end - position) == bytes(end - position), "Old partition metadata was not cleared"
            position = end
        assert stream.read() == b"S" * (size - position)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("probe", type=Path)
    parser.add_argument("gpt_probe", type=Path)
    parser.add_argument("--fat-bits", dest="file_system", type=int, choices=(16, 32), default=32)
    parser.add_argument("--non-bootable", action="store_true")
    parser.add_argument("--filesystem", choices=("ntfs", "exfat"))
    parser.add_argument("--uefi-ntfs", action="store_true")
    parser.add_argument("--boot-code", choices=("rufus", "rufus-masquerade", "win7", "grub2", "grub4dos"))
    args = parser.parse_args()
    uefi_payload = MIB if args.uefi_ntfs else 0
    assert not (args.uefi_ntfs and args.non_bootable)
    assert not (args.boot_code and args.non_bootable)
    assert not (args.boot_code and args.uefi_ntfs and args.filesystem != "ntfs")
    assert not (args.boot_code and args.filesystem and args.filesystem != "ntfs")
    if args.filesystem:
        assert args.non_bootable or args.uefi_ntfs or args.boot_code
        args.file_system = args.filesystem
    call = partial(mbr.invoke, args.probe, file_system=args.file_system, non_bootable=args.non_bootable,
                   uefi_payload=uefi_payload, boot_code=args.boot_code)
    with tempfile.TemporaryDirectory(prefix="rufus-mbr-prepare-") as temporary:
        directory = Path(temporary)
        for size in ((3 if uefi_payload else 2) * MIB, 9 * MIB, DISK_BYTES):
            image = seed_target(directory, args.gpt_probe, size)
            result = call(image, size, operation="prepare")
            assert result["WRITES"] == 1 and result["SYNCS"] == 2, result
            inspect_cleared(image, size, args.file_system, args.non_bootable, uefi_payload, args.boot_code)
            call(image, size, operation="check")
            image.unlink()
        if args.boot_code:
            image = seed_target(directory, args.gpt_probe)
            before = mbr.digest(image)
            overrides_list = [{"boot_code": "invalid-code"}, {"file_system": "exfat"},
                              {"file_system": 12}]
            if args.file_system != "ntfs":
                overrides_list.append({"uefi_payload": MIB})
            for overrides in overrides_list:
                result = call(image, DISK_BYTES, operation="prepare", error=errno.ENOTSUP,
                              started=False, **overrides)
                assert result["WRITES"] == result["BOOT_WRITES"] == result["SYNCS"] == 0
                assert mbr.digest(image) == before, "Invalid bootstrap request cleared the target"
            image.unlink()
        # The GPT table creator stays non-destructive outside metadata. Only
        # explicit preparation authorizes these larger, bounded clear ranges.
        for fault, error, started, syncs, tables in (
            ("readonly", errno.EINVAL, False, 0, 0),
            ("append", errno.EINVAL, False, 0, 0),
            ("cancel-before", errno.ECANCELED, False, 0, 0),
            ("clear-write-fail", errno.EIO, True, 1, 0),
            ("clear-partial-fail", errno.EIO, True, 1, 0),
            ("clear-zero", errno.EIO, True, 1, 0),
            ("clear-end-fail", errno.EIO, True, 1, 0),
            ("clear-read-fail", errno.EIO, True, 1, 0),
            ("clear-corrupt", errno.EBADMSG, True, 1, 0),
            ("cancel-during-clear", errno.ECANCELED, True, 1, 0),
            ("cancel-after-clear", errno.ECANCELED, True, 1, 0),
            ("resize-after-clear", errno.ESTALE, True, 1, 0),
            ("sync-fail", errno.EIO, True, 1, 0),
            ("sync-eintr", 0, True, 3, 1),
            ("clear-eintr", 0, True, 2, 1),
            ("clear-short", 0, True, 2, 1),
            ("add-fail", errno.ENOMEM, True, 1, 0),
            ("cancel-before-write", errno.ECANCELED, True, 1, 0),
            ("write-fail", errno.EIO, True, 2, 1),
            ("replace-path", 0, True, 2, 1),
        ):
            image = seed_target(directory, args.gpt_probe)
            before = mbr.digest(image)
            result = call(image, DISK_BYTES, operation="prepare", fault=fault,
                                error=error, started=started)
            assert result["SYNCS"] == syncs and result["WRITES"] == tables, result
            if fault == "replace-path":
                assert image.read_bytes() == b"untouched"
                image.unlink()
                image = Path(str(image) + ".held")
            if not started:
                assert mbr.digest(image) == before, "Rejected preparation changed the target"
            if error == 0:
                inspect_cleared(image, file_system=args.file_system, non_bootable=args.non_bootable,
                                uefi_payload=uefi_payload, boot_code=args.boot_code)
            image.unlink()
        if uefi_payload:
            for fault, error in (("clear-boot-write-fail", errno.EIO), ("clear-boot-read-fail", errno.EIO),
                                 ("clear-boot-corrupt", errno.EBADMSG)):
                image = seed_target(directory, args.gpt_probe)
                result = call(image, DISK_BYTES, operation="prepare", fault=fault, error=error)
                assert result["SYNCS"] == 1 and result["WRITES"] == 0
                image.unlink()
    mode = "UEFI:NTFS" if uefi_payload else "non-bootable" if args.non_bootable else args.boot_code or "Syslinux"
    print(f"{mbr.runs} {mode}/{args.file_system} preparation/readback calls passed; temporary media removed.", flush=True)


if __name__ == "__main__":
    main()
