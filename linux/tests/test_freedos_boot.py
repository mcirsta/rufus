# SPDX-License-Identifier: GPL-3.0-or-later
"""Original Rufus boot records on real FAT16/FAT32 regular-file fixtures; no devices."""
import argparse
import errno
import hashlib
from pathlib import Path
import re
import struct
import subprocess
import tempfile
from types import SimpleNamespace

import test_fat_format as fat

SECTOR = 512
WINDOW = 19 * SECTOR
FSINFO_HINT_OFFSET = SECTOR + 488
FSINFO_HINT_BYTES = 8
runs = 0


def read_window(image, offset):
    with image.open("rb") as source:
        source.seek(offset)
        return source.read(WINDOW)


def restore_window(image, offset, contents):
    with image.open("r+b") as target:
        target.seek(offset)
        target.write(contents)


def outside_digest(image, offset):
    digest = hashlib.sha256()
    with image.open("rb") as source:
        digest.update(source.read(offset))
        source.seek(offset + WINDOW)
        while chunk := source.read(fat.MIB):
            digest.update(chunk)
    return digest.digest()


def original_array(name):
    path = Path(__file__).resolve().parents[2] / "src/ms-sys/inc" / name
    # Expected boot bytes come from Rufus's original arrays. Offsets and all
    # preserved bytes below are independent of the native adapter's constants.
    initializer = path.read_text().split("{", 1)[1].split("}", 1)[0]
    initializer = re.sub(r"/\*.*?\*/|//[^\n]*", "", initializer, flags=re.S)
    return bytes(int(value, 16) for value in re.findall(r"0x([0-9a-fA-F]{2})\b", initializer))


def expected_records(before, bits, standard=False, windows=False, winpe=False,
                     winpe_bootmgr=False, masquerade_disk=False):
    after = bytearray(before)
    regions = [(0, "br_fat16_0x0.h"), (0x3e, "br_fat16fd_0x3e.h")] if bits == 16 else [
        (0, "br_fat32_0x0.h"), (0x52, "br_fat32fd_0x52.h"), (0x3f0, "br_fat32fd_0x3f0.h")]
    if standard or ((windows or winpe) and bits == 16):
        regions = [(position, name.replace("fat16fd_", "fat16_").replace("fat32fd_", "fat32_"))
                   for position, name in regions]
    if (windows or winpe_bootmgr) and bits == 32:
        regions = [(0, "br_fat32_0x0.h"), (0x52, "br_fat32pe_0x52.h"),
                   (0x3f0, "br_fat32pe_0x3f0.h"), (0x1800, "br_fat32pe_0x1800.h")]
    if winpe and bits == 32:
        regions = [(0, "br_fat32_0x0.h"), (0x52, "br_fat32nt_0x52.h"),
                   (0x3f0, "br_fat32nt_0x3f0.h"), (0x1800, "br_fat32nt_0x1800.h")]
    for base in ([0] if bits == 16 else [0, 6 * SECTOR]):
        for position, name in regions:
            data = original_array(name)
            after[base + position:base + position + len(data)] = data
        label = base + (43 if bits == 16 else 71)
        after[label:label + 11] = b"NO NAME    "
        after[base + (36 if bits == 16 else 64)] = 0x80
        if winpe_bootmgr:
            assert bits == 32
            hint = base + FSINFO_HINT_OFFSET
            after[hint:hint + FSINFO_HINT_BYTES] = before[hint:hint + FSINFO_HINT_BYTES]
            after[base + 64] = 0x81 if masquerade_disk else 0x80
    assert len(after) == len(before)
    return bytes(after)


def invoke(args, image, bits, offset, size, cluster, label, fault="none", error=0, written=0):
    global runs
    command = [str(args.probe), str(image), str(offset), str(size), str(cluster), label, str(bits), fault]
    if args.standard:
        command.append("standard")
    elif args.windows:
        command.append("windows")
    elif args.winpe:
        command.append("winpe")
    elif args.winpe_bootmgr:
        command.append("winpe-bootmgr-minint" if args.minint else "winpe-bootmgr-i386")
    process = subprocess.run(command, capture_output=True, timeout=10)
    assert process.returncode == 0, (command, process.returncode, process.stdout, process.stderr)
    values = {key.decode(): int(value) for key, value in re.findall(rb"([A-Z_]+)=(-?\d+)", process.stdout)}
    assert values["ERROR"] == error and values["BYTES"] == written, (command, values, process.stderr)
    assert values["FDS"] == 0 and values["OFFSET"] == 17 and values["FLAGS"] == 1, values
    assert values["SYNCS"] == (2 if fault == "sync-eintr" else int(written != 0)), values
    expected_sync = errno.ENOSPC if fault in ("sync-fail", "partial-sync-fail") else 0
    assert values["SYNC"] == expected_sync, values
    if fault.startswith("patch-"):
        assert values["PATCHES"] == int(fault[6:]) and values["WRITES"] == 0, values
    runs += 1
    return values


def check_directory(args, image, bits, offset, label):
    with image.open("rb") as source:
        source.seek(offset)
        boot = source.read(SECTOR)
        reserved = struct.unpack_from("<H", boot, 14)[0]
        fat_sectors = struct.unpack_from("<H" if bits == 16 else "<I", boot, 22 if bits == 16 else 36)[0]
        root_sector = reserved + boot[16] * fat_sectors
        if bits == 32:
            root_cluster = struct.unpack_from("<I", boot, 44)[0]
            root_sector += (root_cluster - 2) * boot[13]
        source.seek(offset + root_sector * SECTOR)
        entry = source.read(32)
        if label:
            assert entry[:11] == label.encode().ljust(11, b" ") and entry[11] == 8, entry
        else:
            assert entry[0] == 0, entry
    listing = subprocess.run([args.mdir, "-i", f"{image}@@{offset}", "::"], capture_output=True, timeout=10)
    assert listing.returncode == 0 and b"No files" in listing.stdout, listing.stdout + listing.stderr
    if label:
        assert label.encode() in listing.stdout, listing.stdout


def check_case(args, root, bits, offset, size, cluster, label, failures, unknown_hints=False):
    image = fat.make_target(root, "freedos.img", offset, size)
    formatter = SimpleNamespace(probe=args.formatter, mkfs=args.mkfs, fat_bits=bits)
    fat.invoke(formatter, image, offset, size, cluster, label)
    before = read_window(image, offset)
    if bits == 32:
        # Give the backup different allocation hints. Standard/FreeDOS keep
        # them; the PE payload resets some hint bytes, exactly as WritePBR does.
        changed = bytearray(before)
        changed[6 * SECTOR + 1000:6 * SECTOR + 1008] = b"\xff" * 8
        if unknown_hints:
            changed[FSINFO_HINT_OFFSET:FSINFO_HINT_OFFSET + FSINFO_HINT_BYTES] = b"\xff" * FSINFO_HINT_BYTES
        # Nonzero reserved gaps expose accidental whole-span writes/overlaps.
        for sector in [3, 4, 5, *range(9, 19)]:
            changed[sector * SECTOR:(sector + 1) * SECTOR] = bytes([0xa0 + sector]) * SECTOR
        before = bytes(changed)
        restore_window(image, offset, before)
    expected = expected_records(before, bits, args.standard, args.windows, args.winpe,
                                args.winpe_bootmgr, not args.minint)
    untouched = outside_digest(image, offset)
    prefix = SECTOR if bits == 16 else 3 * SECTOR
    total = prefix if bits == 16 else 2 * prefix
    regions = [(0, prefix)] if bits == 16 else [(0, prefix), (6 * SECTOR, prefix)]
    windows_pe = (args.windows or args.winpe or args.winpe_bootmgr) and bits == 32
    if windows_pe:
        regions += [(12 * SECTOR, SECTOR), (18 * SECTOR, SECTOR)]
        total += 2 * SECTOR
    invoke(args, image, bits, offset, size, cluster, label, written=total)
    assert read_window(image, offset) == expected
    assert outside_digest(image, offset) == untouched
    fat.check_guards(image, offset, size)
    check_directory(args, image, bits, offset, label)
    if not failures:
        image.unlink()
        return

    faults = [("short-write", 0, total), ("write-eintr", 0, total), ("sync-eintr", 0, total), ("read-eintr", 0, total),
              ("read-fail", errno.EIO, 0), ("write-fail", errno.EIO, 0), ("write-zero", errno.EIO, 0),
              ("partial-fail", errno.EIO, 17), ("partial-sync-fail", errno.EIO, 17),
              ("sync-fail", errno.ENOSPC, total), ("verify-fail", errno.EIO, total),
              ("verify-eof", errno.EIO, total), ("verify-corrupt", errno.EBADMSG, total),
              ("cancel-before", errno.ECANCELED, 0), ("cancel-partial", errno.ECANCELED, 17),
              ("cancel-record", errno.ECANCELED, prefix), ("cancel-sync", errno.ECANCELED, total),
              ("cancel-staged", errno.ECANCELED, 0), ("resize", errno.ESTALE, total)]
    if bits == 32:
        faults += [("short-read", 0, total), ("backup-read-fail", errno.EIO, 0), ("backup-fail", errno.EIO, prefix),
                   ("verify-backup-fail", errno.EIO, total), ("verify-backup-corrupt", errno.EBADMSG, total)]
    if windows_pe:
        faults += [("tail-read-fail", errno.EIO, 0), ("backup-tail-read-fail", errno.EIO, 0),
                   ("tail-fail", errno.EIO, 2 * prefix), ("backup-tail-fail", errno.EIO, 2 * prefix + SECTOR),
                   ("partial-tail-fail", errno.EIO, 2 * prefix + 17),
                   ("cancel-tail", errno.ECANCELED, 2 * prefix + SECTOR),
                   ("verify-tail-fail", errno.EIO, total), ("verify-backup-tail-fail", errno.EIO, total),
                   ("verify-tail-corrupt", errno.EBADMSG, total), ("verify-backup-tail-corrupt", errno.EBADMSG, total)]
    if args.winpe_bootmgr:
        faults += [(name, errno.EBADMSG, total) for name in ("verify-drive-corrupt",
                   "verify-backup-drive-corrupt", "verify-hints-corrupt", "verify-backup-hints-corrupt")]
    patch_calls = 4 if bits == 16 else 12 if windows_pe else 10
    faults += [(f"patch-{position}", errno.EIO, 0) for position in range(1, patch_calls + 1)]
    for fault, error, written in faults:
        restore_window(image, offset, before)
        old_size = image.stat().st_size
        invoke(args, image, bits, offset, size, cluster, label, fault, error, written)
        if fault == "resize":
            assert image.stat().st_size == old_size + SECTOR
            with image.open("r+b") as target:
                target.truncate(old_size)
        expected_partial = bytearray(before)
        remaining = written
        for position, length in regions:
            count = min(remaining, length)
            expected_partial[position:position + count] = expected[position:position + count]
            remaining -= count
        assert remaining == 0, fault
        assert read_window(image, offset) == expected_partial, fault
        assert outside_digest(image, offset) == untouched, fault

    restore_window(image, offset, before)
    invalid = [("readonly", {}, errno.EINVAL), ("append", {}, errno.EINVAL), ("direct", {}, errno.EINVAL),
               ("invalid-code", {}, errno.EINVAL), ("null-options", {}, errno.EINVAL), ("null-result", {}, errno.EINVAL), ("serial", {}, errno.EBADMSG),
               ("none", {"bits": 12}, errno.EINVAL), ("none", {"offset": offset + 1}, errno.EINVAL),
               ("none", {"offset": 4 * size}, errno.ENOSPC), ("none", {"size": 2 * size}, errno.ENOSPC),
               ("none", {"size": size + 1}, errno.EINVAL), ("none", {"cluster": 0}, errno.EINVAL),
               ("none", {"label": "WRONG"}, errno.EBADMSG)]
    if args.winpe_bootmgr:
        # The dedicated FAT32 API has neither a boot-code nor filesystem enum.
        invalid = [item for item in invalid if item[0] != "invalid-code" and "bits" not in item[1]]
    for fault, overrides, error in invalid:
        request = dict(bits=bits, offset=offset, size=size, cluster=cluster, label=label)
        request.update(overrides)
        invoke(args, image, **request, fault=fault, error=error)
        assert read_window(image, offset) == before and outside_digest(image, offset) == untouched

    corrupt = [(510, b"\0\0", errno.EBADMSG), (11, b"\0\4", errno.EBADMSG),
               (54 if bits == 16 else 82, b"UNKNOWN ", errno.EBADMSG)]
    if bits == 32:
        corrupt += [(14, struct.pack("<H", 8), errno.ENOTSUP), (48, b"\2\0", errno.ENOTSUP),
                    (50, b"\0\0", errno.ENOTSUP), (50, b"\5\0", errno.ENOTSUP),
                    (6 * SECTOR + 510, b"\0\0", errno.EBADMSG), (6 * SECTOR + 71, b"WRONG", errno.EBADMSG),
                    (6 * SECTOR + 11, b"\0\4", errno.EBADMSG)]
        if windows_pe:
            corrupt += [(14, struct.pack("<H", 18), errno.ENOTSUP)]
    for position, data, error in corrupt:
        damaged = bytearray(before)
        damaged[position:position + len(data)] = data
        restore_window(image, offset, damaged)
        invoke(args, image, bits, offset, size, cluster, label, error=error)
        assert read_window(image, offset) == damaged and outside_digest(image, offset) == untouched
    image.unlink()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("probe", type=Path)
    parser.add_argument("formatter", type=Path)
    parser.add_argument("--mkfs", required=True)
    parser.add_argument("--mdir", required=True)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--standard", action="store_true")
    mode.add_argument("--windows", action="store_true")
    mode.add_argument("--winpe", action="store_true")
    mode.add_argument("--winpe-bootmgr", action="store_true")
    parser.add_argument("--minint", action="store_true")
    args = parser.parse_args()
    if args.minint and not args.winpe_bootmgr:
        parser.error("--minint requires --winpe-bootmgr")
    subprocess.run([args.probe, "--memory-bounds"], check=True, timeout=10)
    with tempfile.TemporaryDirectory(prefix="rufus-freedos-boot-") as temporary:
        root = Path(temporary)
        if not args.winpe_bootmgr:
            check_case(args, root, 16, fat.MIB, 16 * fat.MIB, 512, "RUFUS TEST", True)
        check_case(args, root, 32, fat.MIB, 64 * fat.MIB, 512, "RUFUS TEST", True)
        if not args.winpe_bootmgr:
            check_case(args, root, 16, 0, 32 * fat.MIB, 1024, "", False)
        check_case(args, root, 32, 0, 128 * fat.MIB, 1024, "", False, unknown_hints=args.winpe_bootmgr)
        if args.winpe_bootmgr:
            image = fat.make_target(root, "fat16-refused.img", fat.MIB, 16 * fat.MIB)
            formatter = SimpleNamespace(probe=args.formatter, mkfs=args.mkfs, fat_bits=16)
            fat.invoke(formatter, image, fat.MIB, 16 * fat.MIB, 512, "RUFUS TEST")
            original = hashlib.sha256(image.read_bytes()).digest()
            invoke(args, image, 16, fat.MIB, 16 * fat.MIB, 512, "RUFUS TEST", error=errno.ENOTSUP)
            assert hashlib.sha256(image.read_bytes()).digest() == original
            image.unlink()
    # Kernel descriptor-kind validation occurs before any target writes.
    # /dev/null is a character device, not a disk or partition.
    command = [args.probe, "/dev/null", "0", str(16 * fat.MIB), "512", "", "16", "none"]
    if args.winpe_bootmgr:
        command.append("winpe-bootmgr-minint" if args.minint else "winpe-bootmgr-i386")
    process = subprocess.run(command, capture_output=True, check=True, timeout=10)
    assert f"ERROR={errno.ENOTSUP} BYTES=0".encode() in process.stdout, process.stdout
    name = "WinPE BOOTMGR" if args.winpe_bootmgr else "WinPE" if args.winpe else "Windows" if args.windows else "standard" if args.standard else "FreeDOS"
    print(f"{runs} {name} install/refusal/fault cases passed, "
          "plus memory bounds and non-file rejection; fixtures removed.")


if __name__ == "__main__":
    main()
