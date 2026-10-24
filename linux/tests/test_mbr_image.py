# SPDX-License-Identifier: GPL-3.0-or-later
"""Check Rufus MBR bootstrap choices using disposable regular files only."""
import argparse
import errno
from functools import partial
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import tempfile

MIB = 1024 * 1024
SECTOR_BYTES = 512
ROOT = Path(__file__).resolve().parents[2]
runs = 0


def trailing_extent(size, payload, track=63):
    track_bytes = track * SECTOR_BYTES
    length = (payload + track_bytes - 1) // track_bytes * track_bytes
    return (size - length) // track_bytes * track_bytes, length


def extent(size, cluster, track=63, uefi_payload=0):
    end = trailing_extent(size, uefi_payload, track)[0] if uefi_payload else size
    available = end - MIB
    track_bytes = track * SECTOR_BYTES
    rounded = available // track_bytes * track_bytes
    if cluster % SECTOR_BYTES == 0:
        rounded = rounded // cluster * cluster
    return MIB, rounded


def invoke(probe, image, size, cluster=512, track=63, operation="create", fault="none",
           error=0, started=None, sector=512, file_system=32, non_bootable=False, uefi_payload=0,
           boot_code=None):
    global runs
    if started is None:
        started = operation in ("create", "prepare")
    command = [probe, image, str(size), str(cluster), str(track), str(sector), operation, fault, str(file_system)]
    if non_bootable:
        command.append("non-bootable")
    elif boot_code:
        command.append(boot_code)
        if uefi_payload:
            command.append(str(uefi_payload))
    elif uefi_payload:
        command.extend(["uefi-ntfs", str(uefi_payload)])
    result = subprocess.run(command, capture_output=True, timeout=10)
    runs += 1
    assert result.returncode == 0, (result.returncode, result.stdout, result.stderr)
    values = {key.decode(): int(value) for key, value in re.findall(rb"([A-Z_]+)=(-?\d+)", result.stdout)}
    assert values["ERROR"] == error and values["STARTED"] == started, (operation, fault, values, result.stderr)
    if operation != "prepare":
        assert values["WRITES"] == int(started), values
        assert values["SYNCS"] == (2 if fault == "sync-eintr" else int(started)), values
    assert values["SYNC_ERROR"] == (errno.EIO if fault == "sync-fail" else 0), values
    assert values["FDS"] == 0 and values["FLAGS"] == 1, values
    assert values["POSITION"] == (-1 if fault == "pipe" else 17), values
    if error == 0:
        assert (values["OFFSET"], values["SIZE"]) == extent(size, cluster, track, uefi_payload), values
        if uefi_payload:
            assert (values["BOOT_OFFSET"], values["BOOT_SIZE"]) == trailing_extent(size, uefi_payload, track), values
    print(f"PASS MBR {operation} {fault}: {values}", flush=True)
    return values


def inspect_mbr(image, size, cluster=512, track=63, sfdisk="sfdisk", signature=0x78563412, file_system=32, non_bootable=False, uefi_payload=0,
                boot_code=None):
    zero_code = non_bootable or (uefi_payload != 0 and boot_code is None)
    masqueraded = boot_code == "rufus-masquerade"
    payload_code = "rufus" if masqueraded else "grub" if boot_code == "grub4dos" else boot_code or "syslinux"
    name = "mbr_zero.h" if zero_code else f"mbr_{payload_code}.h"
    # Only the first initializer is boot code. Windows 7's header also has a
    # commented-out disk-signature array, which its original writer skips.
    payload = (ROOT / "src/ms-sys/inc" / name).read_text().split("{", 1)[1].split("}", 1)[0]
    expected = bytes(int(value, 16) for value in re.findall(r"0x([0-9a-fA-F]{2})\b", payload))
    assert len(expected) == (446 if zero_code else 432 if boot_code == "grub2" else 423 if boot_code == "grub4dos" else 440)
    if zero_code:
        signature = 0
    with image.open("rb") as stream:
        sector = stream.read(SECTOR_BYTES)
    assert sector[:len(expected)] == expected and sector[510:] == b"\x55\xaa"
    assert struct.unpack_from("<I", sector, 440)[0] == signature
    entries = 2 if uefi_payload else 1
    unused = 446 + entries * 16
    assert sector[444:446] == bytes(2) and sector[unused:510] == bytes(510 - unused)
    active, _, kind, _, start, length = struct.unpack_from("<B3sB3sII", sector, 446)
    expected_type = {16: 0x0e, 32: 0x0c, "ntfs": 0x07, "exfat": 0x07,
                     "ext2": 0x83, "ext3": 0x83}[file_system]
    expected_active = 0 if non_bootable else 0x81 if masqueraded else 0x80
    assert (active, kind) == (expected_active, expected_type)
    assert (start * 512, length * 512) == extent(size, cluster, track, uefi_payload)
    result = subprocess.run([sfdisk, "--json", image], check=True, capture_output=True, timeout=10)
    table = json.loads(result.stdout)["partitiontable"]
    assert table["label"] == "dos" and table["id"] == f"0x{signature:08x}"
    assert len(table["partitions"]) == entries
    entry = table["partitions"][0]
    # libfdisk/sfdisk reserve this property for 0x80. The raw flag above must
    # match the requested 0x81; absence here does not mean an inactive record.
    assert entry.get("bootable", False) == (not non_bootable and not masqueraded)
    assert entry["type"] == f"{expected_type:x}"
    assert (entry["start"], entry["size"]) == (start, length)
    if uefi_payload:
        active, _, kind, _, boot_start, boot_length = struct.unpack_from("<B3sB3sII", sector, 462)
        assert (active, kind) == (0, 0xef)
        assert (boot_start * 512, boot_length * 512) == trailing_extent(size, uefi_payload, track)
        boot = table["partitions"][1]
        assert not boot.get("bootable", False) and boot["type"] == "ef"
        assert (boot["start"], boot["size"]) == (boot_start, boot_length)
    subprocess.run([sfdisk, "--verify", image], check=True, capture_output=True, timeout=10)
    return sector


def digest(image, offset=0):
    with image.open("rb") as stream:
        stream.seek(offset)
        return hashlib.file_digest(stream, "sha256").digest()


def make_image(root, name, size):
    image = root / name
    with image.open("wb") as stream:
        stream.truncate(size)
        for position in (512, MIB, size // 2, size - 512):
            stream.seek(position)
            stream.write(b"S" * 512)
    return image


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("probe", type=Path)
    parser.add_argument("gpt_probe", type=Path)
    parser.add_argument("--sfdisk", default="sfdisk")
    parser.add_argument("--fat-bits", dest="file_system", type=int, choices=(16, 32), default=32)
    parser.add_argument("--non-bootable", action="store_true")
    parser.add_argument("--filesystem", choices=("ntfs", "exfat"))
    parser.add_argument("--uefi-ntfs", action="store_true")
    parser.add_argument("--boot-code", choices=("rufus", "rufus-masquerade", "win7", "syslinux", "grub2", "grub4dos"))
    args = parser.parse_args()
    uefi_payload = MIB if args.uefi_ntfs else 0
    assert not (args.uefi_ntfs and args.non_bootable)
    assert not (args.boot_code and args.non_bootable)
    if args.boot_code and args.boot_code != "syslinux":
        assert not args.uefi_ntfs or args.filesystem == "ntfs"
        assert not args.filesystem or args.filesystem == "ntfs"
    if args.filesystem:
        assert args.non_bootable or args.uefi_ntfs or args.filesystem == "ntfs"
        args.file_system = args.filesystem
    call = partial(invoke, args.probe, file_system=args.file_system, non_bootable=args.non_bootable,
                   uefi_payload=uefi_payload, boot_code=args.boot_code)
    inspect = partial(inspect_mbr, file_system=args.file_system, non_bootable=args.non_bootable,
                      uefi_payload=uefi_payload, boot_code=args.boot_code)
    other_file_system = 32 if args.file_system == 16 else 16
    max_cluster = {16: 65536, 32: 32768, "ntfs": 65536, "exfat": 32 * MIB}[args.file_system]
    with tempfile.TemporaryDirectory(prefix="rufus-mbr-image-") as temporary:
        root = Path(temporary)
        # The 2-TiB bound is tested without allocating or opening a large image.
        for size in ((3 if uefi_payload else 2) * MIB, 65 * MIB + 512, 2**41):
            call(root / "absent", size, operation="plan")
        for size, cluster, track, sector in ((2**41 + 512, 512, 63, 512), (MIB, 512, 63, 512),
                (65 * MIB + 1, 512, 63, 512), (65 * MIB, 513, 63, 512),
                (65 * MIB, max_cluster * 2, 63, 512), (65 * MIB, 512, 0, 512), (65 * MIB, 512, 63, 4096)):
            call(root / "absent", size, cluster, track, "plan", error=errno.EINVAL, sector=sector)
        call(root / "absent", 65 * MIB, operation="plan", file_system=12, error=errno.ENOTSUP)
        call(root / "absent", 512 * MIB, cluster=max_cluster, operation="plan")
        if args.boot_code and args.boot_code != "syslinux":
            for file_system in ("ntfs", "exfat"):
                supported = file_system == "ntfs"
                call(root / "absent", 65 * MIB, operation="plan", file_system=file_system,
                     error=0 if supported else errno.ENOTSUP)
            supports_payload = args.file_system == "ntfs"
            call(root / "absent", 65 * MIB, operation="plan", uefi_payload=MIB,
                 error=0 if supports_payload else errno.ENOTSUP)
            call(root / "absent", 65 * MIB, operation="plan", boot_code="invalid-code", error=errno.ENOTSUP)
        if args.non_bootable and args.file_system in ("ntfs", "exfat"):
            call(root / "absent", 65 * MIB, operation="plan", non_bootable=False,
                 error=0 if args.file_system == "ntfs" else errno.ENOTSUP)
        for size, cluster, track in ((65 * MIB, 512, 63), (129 * MIB + 512, 1024, 63), (65 * MIB, 4096, 32)):
            image = make_image(root, "normal.img", size)
            before = digest(image)
            call(image, size, cluster, track, file_system=12, error=errno.ENOTSUP, started=False)
            assert digest(image) == before, "Unsupported filesystem changed the target"
            if args.boot_code:
                call(image, size, cluster, track, boot_code="invalid-code", error=errno.ENOTSUP, started=False)
                assert digest(image) == before, "Unsupported bootstrap changed the target"
            body = digest(image, 512)
            call(image, size, cluster, track)
            initial = inspect(image, size, cluster, track, args.sfdisk)
            call(image, size, cluster, track, "check")
            if args.boot_code:
                for other_code in ("syslinux", "rufus", "rufus-masquerade", "win7", "grub2", "grub4dos"):
                    if other_code != args.boot_code:
                        call(image, size, cluster, track, "check", boot_code=other_code, error=errno.EBADMSG)
            # Boot-code/active policy is part of the contract, even with the same filesystem.
            if args.file_system in (16, 32):
                call(image, size, cluster, track, "check", non_bootable=not args.non_bootable, error=errno.EBADMSG)
            # A valid table for the other FAT type is not the requested table.
            call(image, size, cluster, track, "check", file_system=other_file_system, error=errno.ENOTSUP if uefi_payload else errno.EBADMSG)
            assert inspect(image, size, cluster, track, args.sfdisk) == initial
            # Replacing a DOS label must clear unused entries, not retain an
            # old second partition in the small trailing alignment gap.
            with image.open("r+b") as stream:
                stream.seek(478 if uefi_payload else 462)
                stream.write(struct.pack("<B3sB3sII", 0, bytes(3), 0x0c, bytes(3), size // 512 - 1, 1))
            call(image, size, cluster, track)
            assert inspect(image, size, cluster, track, args.sfdisk) == initial
            assert digest(image, 512) == body, "MBR writer changed bytes after sector zero"
            image.unlink()
        size = 65 * MIB
        for fault, error, started in (
            ("readonly", errno.EINVAL, False), ("append", errno.EINVAL, False), ("pipe", errno.ENOTSUP, False),
            ("cancel-before", errno.ECANCELED, False), ("cancel-before-write", errno.ECANCELED, False),
            ("add-fail", errno.ENOMEM, False), ("wrong-offset", errno.EBADMSG, False), ("active" if args.non_bootable else "inactive", errno.EBADMSG, False),
            ("resize-before-write", errno.ESTALE, False), ("write-fail", errno.EIO, True),
            ("boot-write-fail", errno.EIO, True), ("boot-partial-fail", errno.EIO, True),
            ("cancel-after-table", errno.ECANCELED, True), ("cancel-after-boot", errno.ECANCELED, True),
            ("sync-fail", errno.EIO, True), ("sync-eintr", 0, True), ("boot-eintr", 0, True), ("boot-short", 0, True),
            ("bad-code", errno.EBADMSG, True), ("bad-id", errno.EBADMSG, True), ("bad-reserved", errno.EBADMSG, True),
            ("bad-active", errno.EBADMSG, True), ("bad-type", errno.EBADMSG, True), ("bad-offset", errno.EBADMSG, True),
            ("bad-inactive", errno.EBADMSG, True), ("bad-other-drive", errno.EBADMSG, True),
            ("bad-size", errno.EBADMSG, True), ("extra-entry", errno.EBADMSG, True), ("bad-marker", errno.EBADMSG, True),
            ("replace-path", 0, True),
        ):
            # Writing zero is valid for an already inactive non-bootable table.
            # It is a corrupt flag only for the ordinary or masquerading BIOS modes.
            if fault == "bad-inactive" and args.non_bootable:
                error = 0
            image = make_image(root, fault + ".img", size)
            before, body = digest(image), digest(image, 512)
            call(image, size, fault=fault, error=error, started=started)
            if fault == "replace-path":
                assert image.read_bytes() == b"untouched"
                image.unlink()
                image = Path(str(image) + ".held")
            if fault != "resize-before-write":
                assert digest(image, 512) == body
                if not started:
                    assert digest(image) == before
            if error == 0:
                inspect(image, size, sfdisk=args.sfdisk)
            if error == errno.EBADMSG and (fault.startswith("bad-") or fault == "extra-entry"):
                damaged = digest(image)
                call(image, size, operation="check", error=errno.EBADMSG)
                assert digest(image) == damaged, "Readback repaired damaged metadata"
            image.unlink()
        if uefi_payload:
            for fault in ("bad-boot-active", "bad-boot-type", "bad-boot-offset", "bad-boot-size", "third-entry"):
                image = make_image(root, fault + ".img", size)
                call(image, size, fault=fault, error=errno.EBADMSG)
                damaged = digest(image)
                call(image, size, operation="check", error=errno.EBADMSG)
                assert digest(image) == damaged
                image.unlink()
            for payload_size in (size, 2**64 - 1):
                call(root / "absent", size, operation="plan", uefi_payload=payload_size, error=errno.ENOSPC)
            call(root / "absent", 2 * MIB, operation="plan", error=errno.ENOSPC)
        image = make_image(root, "gpt.img", size)
        subprocess.run([args.gpt_probe, image, str(size), "512", "63", "none"], check=True, capture_output=True)
        before = digest(image)
        call(image, size, error=errno.ENOTSUP, started=False)
        assert digest(image) == before, "Rejected GPT conversion modified the image"
    mode = "UEFI:NTFS" if uefi_payload else "non-bootable" if args.non_bootable else args.boot_code or "Syslinux"
    print(f"{runs} {mode}/{args.file_system} MBR calls passed; temporary images removed.", flush=True)


if __name__ == "__main__":
    main()
