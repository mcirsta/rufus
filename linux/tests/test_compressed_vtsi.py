#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Independent Ventoy sparse-image fixtures and retained-source faults."""
import errno
import hashlib
import pathlib
import random
import struct
import subprocess
import sys
import tempfile


SECTOR = 512
MAGIC = 0x0000594F544E4556


def checksum(data):
    return ~sum(data) & 0xffffffff


def image(disk_sectors, segments):
    payload = b"".join(data for _, data, _ in segments)
    assert all(len(data) % SECTOR == 0 for _, data, _ in segments)
    padding = bytes(-len(payload) % SECTOR)
    offset = len(payload) + len(padding)
    table = b"".join(struct.pack("<QQQ", start, len(data) // SECTOR, data_offset)
                     for start, data, data_offset in segments)
    footer = bytearray(SECTOR)
    struct.pack_into("<QHHQIIIIQ", footer, 0, MAGIC, 1, 0, disk_sectors * SECTOR,
                     0x12345678, 0, len(segments), checksum(table), offset)
    struct.pack_into("<I", footer, 24, checksum(footer))
    return payload + padding + table + bytes(-len(table) % SECTOR) + footer


def change_footer(packed, offset, format_code, value):
    changed = bytearray(packed)
    struct.pack_into(format_code, changed, len(changed) - SECTOR + offset, value)
    struct.pack_into("<I", changed, len(changed) - SECTOR + 24, 0)
    struct.pack_into("<I", changed, len(changed) - SECTOR + 24,
                     checksum(changed[-SECTOR:]))
    return bytes(changed)


def run(program, path, packed, maximum, mode="hash", timeout=30):
    path.write_bytes(packed)
    before = path.stat()
    completed = subprocess.run([program, str(maximum), mode, str(path)],
                               text=True, capture_output=True, timeout=timeout, check=True)
    after = path.stat()
    assert (before.st_ino, before.st_size, before.st_mtime_ns, before.st_ctime_ns) == (
        after.st_ino, after.st_size, after.st_mtime_ns, after.st_ctime_ns)
    return dict(line.split(" ", 1) for line in completed.stdout.splitlines())


def check(program, path, packed, decoded):
    result = run(program, path, packed, len(decoded))
    assert int(result["error"]) == 0, result
    assert int(result["type"]) == 8 and int(result["source"]) == len(packed), result
    assert int(result["decoded"]) == len(decoded) and int(result["members"]) == 1, result
    assert int(result["trailing"]) == 0, result
    marker = decoded[510:512] == b"\x55\xaa"
    assert int(result["marker"]) == marker and int(result["forced"]) == (1 if marker else 2), result
    assert result["prefix"] == decoded[:SECTOR].hex(), result
    assert result["sha256"] == hashlib.sha256(decoded).hexdigest(), result


def reject(program, path, packed, expected=errno.EBADMSG, maximum=1024 * 1024):
    result = run(program, path, packed, maximum)
    assert int(result["error"]) == expected, result
    for field in ("type", "source", "decoded", "members", "trailing", "marker", "forced"):
        assert int(result[field]) == 0, result
    assert result["prefix"] == result["sha256"] == "", result


def faults(program, path, packed, decoded_size):
    path.write_bytes(packed)
    modes = ((0, 0), (1, 0), (2, 0), (3, errno.EIO), (4, errno.EIO),
             (5, errno.EIO), (6, errno.ECANCELED), (7, errno.ECANCELED),
             (8, errno.ENOSPC), (9, errno.EIO))
    for mode, expected in modes:
        completed = subprocess.run([program, str(mode), str(decoded_size), str(path)],
                                   text=True, capture_output=True, timeout=30, check=True)
        result = dict(line.split(" ", 1) for line in completed.stdout.splitlines())
        assert int(result["error"]) == expected, (mode, result)
        assert int(result["offset"]) == 7 and int(result["readonly"]) == 1, result
        assert int(result["result_empty"]) == (expected != 0), result
        if expected == 0:
            assert int(result["received"]) == decoded_size, result
            assert int(result["result_size"]) == decoded_size, result


def main():
    program = str(pathlib.Path(sys.argv[1]).resolve())
    fault_program = str(pathlib.Path(sys.argv[2]).resolve())
    cases = 0
    with tempfile.TemporaryDirectory(prefix="rufus-vtsi-vectors-") as directory:
        path = pathlib.Path(directory) / "sparse Ü.VTSI"
        boot = bytearray(random.Random(31).randbytes(SECTOR))
        boot[510:512] = b"\x55\xaa"
        second = random.Random(32).randbytes(2 * SECTOR)
        packed = image(8, [(0, boot, 0xdeadbeef), (4, second, 0)])
        decoded = bytes(boot) + bytes(3 * SECTOR) + second + bytes(2 * SECTOR)
        check(program, path, packed, decoded)
        cases += 1
        check(program, path, change_footer(packed, 10, "<H", 7), decoded)
        cases += 1
        check(program, path, image(4, [(2, second, 123)]), bytes(2 * SECTOR) + second)
        check(program, path, image(1, []), bytes(SECTOR))
        cases += 2
        check(program, path, image(1, [(0, b"", 0)] * 1025), bytes(SECTOR))
        cases += 1
        reject(program, path, packed, errno.EFBIG, len(decoded) - 1)
        cases += 1
        for changed in (packed[:-1], packed[:-SECTOR], packed + b"x", bytes(SECTOR),
                        b"x" * len(packed)):
            reject(program, path, changed)
            cases += 1
        damaged = bytearray(packed)
        damaged[-SECTOR] ^= 1
        reject(program, path, bytes(damaged))
        damaged = bytearray(packed)
        damaged[-SECTOR + 100] ^= 1
        reject(program, path, bytes(damaged))
        damaged = bytearray(packed)
        damaged[3 * SECTOR + 1] ^= 1
        reject(program, path, bytes(damaged))
        cases += 3
        for changed in (change_footer(packed, 8, "<H", 2),
                        change_footer(packed, 12, "<Q", 0),
                        change_footer(packed, 12, "<Q", 513),
                        change_footer(packed, 28, "<I", 1000),
                        change_footer(packed, 36, "<Q", len(packed)),
                        change_footer(packed, 36, "<Q", 0),
                        change_footer(packed, 32, "<I", 0)):
            reject(program, path, changed)
            cases += 1
        for invalid in ([(0, boot, 0), (0, second, 0)],
                        [(4, second, 0), (1, boot, 0)],
                        [(7, second, 0)]):
            reject(program, path, image(8, invalid))
            cases += 1
        # The table checksum is valid, but its declared data runs into the table.
        overlap = bytearray(image(2, [(0, bytes(SECTOR), 0)]))
        struct.pack_into("<Q", overlap, SECTOR + 8, 2)
        struct.pack_into("<I", overlap, len(overlap) - SECTOR + 32,
                         checksum(overlap[SECTOR:SECTOR + 24]))
        struct.pack_into("<I", overlap, len(overlap) - SECTOR + 24, 0)
        struct.pack_into("<I", overlap, len(overlap) - SECTOR + 24,
                         checksum(overlap[-SECTOR:]))
        reject(program, path, bytes(overlap))
        cases += 1
        faults(fault_program, path, packed, len(decoded))
        cases += 10
        large = image(2**23 + 1, [])  # 4 GiB + one sector without stored data.
        result = run(program, path, large, 2**32 + SECTOR, "count", timeout=90)
        assert int(result["error"]) == 0 and int(result["decoded"]) == 2**32 + SECTOR, result
        assert int(result["members"]) == 1 and result["prefix"] == bytes(SECTOR).hex(), result
        reject(program, path, large, errno.EFBIG, 2**32)
        cases += 2
    print(f"VTSI retained source: {cases} independent layout/fault/large-output vectors passed")


if __name__ == "__main__":
    main()
