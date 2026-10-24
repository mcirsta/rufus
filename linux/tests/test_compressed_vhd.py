#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Independent fixed/dynamic VHD layouts, corruptions and retained-source faults."""
import errno
import hashlib
import json
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile


SECTOR = 512


def checksum(data, offset):
    return ~sum(data[:offset] + data[offset + 4:]) & 0xffffffff


def footer(size, kind, data_offset):
    data = bytearray(SECTOR)
    data[:8] = b"conectix"
    struct.pack_into(">IIQ", data, 8, 2, 0x10000, data_offset)
    struct.pack_into(">QQ", data, 40, size, size)
    struct.pack_into(">I", data, 60, kind)
    struct.pack_into(">I", data, 64, checksum(data, 64))
    return bytes(data)


def dynamic(raw):
    assert len(raw) == 16 * SECTOR
    block_size = 8 * SECTOR
    header = bytearray(1024)
    header[:8] = b"cxsparse"
    struct.pack_into(">QQIII", header, 8, 0xffffffffffffffff, 1536, 0x10000, 2, block_size)
    struct.pack_into(">I", header, 36, checksum(header, 36))
    bat = struct.pack(">II", 4, 0xffffffff) + bytes(SECTOR - 8)
    bitmap = b"\x90" + bytes(SECTOR - 1)  # only sectors 0 and 3 are present
    payload = bytearray(raw[:block_size])
    payload[SECTOR:2 * SECTOR] = b"P" * SECTOR  # must be ignored by the reader
    mark = footer(len(raw), 3, SECTOR)
    return mark + header + bat + bitmap + payload + mark


def invoke(probe, path, packed, maximum, mode="hash"):
    path.write_bytes(packed)
    before = path.stat()
    run = subprocess.run([probe, str(maximum), mode, str(path)],
                         text=True, capture_output=True, check=True, timeout=30)
    after = path.stat()
    assert (before.st_ino, before.st_size, before.st_mtime_ns, before.st_ctime_ns) == (
        after.st_ino, after.st_size, after.st_mtime_ns, after.st_ctime_ns)
    return dict(line.split(" ", 1) for line in run.stdout.splitlines())


def accept(probe, path, packed, raw):
    result = invoke(probe, path, packed, len(raw))
    assert int(result["error"]) == 0 and int(result["type"]) == 11, result
    assert int(result["source"]) == len(packed), result
    assert int(result["decoded"]) == len(raw) and int(result["members"]) == 1, result
    assert int(result["trailing"]) == 0, result
    assert result["prefix"] == raw[:SECTOR].hex(), result
    assert result["sha256"] == hashlib.sha256(raw).hexdigest(), result


def reject(probe, path, packed, expected=errno.EBADMSG, maximum=1024 * 1024):
    result = invoke(probe, path, packed, maximum)
    assert int(result["error"]) == expected, result
    for key in ("type", "source", "decoded", "members", "trailing", "marker", "forced"):
        assert int(result[key]) == 0, result
    assert result["prefix"] == result["sha256"] == "", result


def change_dynamic(packed, offset, fmt, value, header=False):
    data = bytearray(packed)
    start = SECTOR if header else len(data) - SECTOR
    struct.pack_into(fmt, data, start + offset, value)
    if header:
        struct.pack_into(">I", data, start + 36, 0)
        struct.pack_into(">I", data, start + 36, checksum(data[start:start + 1024], 36))
    else:
        struct.pack_into(">I", data, start + 64, 0)
        struct.pack_into(">I", data, start + 64, checksum(data[start:start + SECTOR], 64))
        data[:SECTOR] = data[-SECTOR:]
    return bytes(data)


def fault_matrix(fault_probe, path, packed, size):
    path.write_bytes(packed)
    for mode, expected in ((0, 0), (1, 0), (2, 0), (3, errno.EIO),
                           (4, errno.EIO), (5, errno.EIO), (6, errno.ECANCELED),
                           (7, errno.ECANCELED), (8, errno.ENOSPC), (9, errno.EIO)):
        run = subprocess.run([fault_probe, str(mode), str(size), str(path)],
                             text=True, capture_output=True, check=True, timeout=30)
        result = dict(line.split(" ", 1) for line in run.stdout.splitlines())
        assert int(result["error"]) == expected, (mode, result)
        assert int(result["offset"]) == 7 and int(result["readonly"]) == 1, result
        assert int(result["result_empty"]) == (expected != 0), result
        if expected == 0:
            assert int(result["received"]) == size, result


def write_matrix(write_probe, root, packed, raw):
    source = root / "writer.VHD"
    target = root / "writer-target.img"
    source.write_bytes(packed)
    for sector_size in (512, 4096):
        for fault, expected in (("none", 0), ("short", 0), ("eintr", 0),
                                ("parallel", 0), ("plan-hash", errno.ESTALE),
                                ("preflight-cancel", errno.ECANCELED),
                                ("plan-marker", errno.EINVAL)):
            target.write_bytes(b"\xa5" * len(raw) + b"TAIL")
            run = subprocess.run([write_probe, str(source), str(target), str(sector_size),
                                  "0", str(2**63 - 1), fault], text=True, capture_output=True,
                                 check=True, timeout=30)
            report = json.loads(run.stdout)
            assert report["error"] == expected, (fault, report)
            assert report["compression"] == 11 and report["decoded"] == len(raw), report
            if fault != "plan-hash":
                assert report["plan_sha256"] == hashlib.sha256(raw).hexdigest(), report
            if expected == 0:
                assert target.read_bytes() == raw + b"TAIL", report
                assert report["written"] == report["verified"] == len(raw), report
            else:
                assert target.read_bytes() == b"\xa5" * len(raw) + b"TAIL", report
    return 14


def qemu_crosscheck(probe, write_probe, directory):
    if shutil.which("qemu-img") is None:
        return 0
    raw = bytearray(5 * 1024 * 1024)
    raw[510:512] = b"\x55\xaa"
    raw[123 * SECTOR:124 * SECTOR] = b"Q" * SECTOR
    raw[3 * 1024 * 1024:3 * 1024 * 1024 + SECTOR] = b"R" * SECTOR
    last = 4 * 1024 * 1024 + 512 * 1024
    raw[last:last + SECTOR] = b"S" * SECTOR
    source = directory / "qemu-source.raw"
    source.write_bytes(raw)
    for kind in ("fixed", "dynamic"):
        output = directory / ("qemu-" + kind + ".vhd")
        subprocess.run(["qemu-img", "convert", "-f", "raw", "-O", "vpc",
                        "-o", "subformat=" + kind, str(source), str(output)],
                       check=True, capture_output=True, timeout=30)
        actual_size = struct.unpack_from(">Q", output.read_bytes()[-SECTOR:], 48)[0]
        expected = bytes(raw).ljust(actual_size, b"\0")
        accept(probe, output, output.read_bytes(), expected)
        target = directory / ("qemu-" + kind + "-written.img")
        target.write_bytes(b"\xa5" * actual_size)
        run = subprocess.run([write_probe, str(output), str(target), "512", "0",
                              str(2**63 - 1), "none"], text=True, capture_output=True,
                             check=True, timeout=30)
        report = json.loads(run.stdout)
        assert report["error"] == 0 and report["written"] == report["verified"] == actual_size, report
        assert target.read_bytes() == expected, report
    return 4


def main():
    probe = str(pathlib.Path(sys.argv[1]).resolve())
    fault_probe = str(pathlib.Path(sys.argv[2]).resolve())
    write_probe = str(pathlib.Path(sys.argv[3]).resolve())
    count = 0
    with tempfile.TemporaryDirectory(prefix="rufus-vhd-vectors-") as name:
        root = pathlib.Path(name)
        path = root / "VHD Ü.VHD"
        raw = bytearray(16 * SECTOR)
        raw[510:512] = b"\x55\xaa"
        raw[3 * SECTOR:4 * SECTOR] = b"D" * SECTOR
        logical = bytes(raw)
        packed = dynamic(logical)
        accept(probe, path, packed, logical)
        accept(probe, path, logical + footer(len(logical), 2, 0xffffffffffffffff), logical)
        count += 2
        reject(probe, path, packed, errno.EFBIG, len(logical) - 1)
        reject(probe, path, packed[:-1])
        reject(probe, path, packed + b"x")
        reject(probe, path, bytes(len(packed)))
        count += 4
        damaged = bytearray(packed)
        damaged[-SECTOR + 10] ^= 1
        reject(probe, path, bytes(damaged))
        damaged = bytearray(packed)
        damaged[SECTOR + 20] ^= 1
        reject(probe, path, bytes(damaged))
        count += 2
        for changed, code in ((change_dynamic(packed, 60, ">I", 4), errno.EBADMSG),
                              (change_dynamic(packed, 48, ">Q", 0), errno.EBADMSG),
                              (change_dynamic(packed, 16, ">Q", len(packed)), errno.EBADMSG),
                              (change_dynamic(packed, 32, ">I", 1000, True), errno.EBADMSG),
                              (change_dynamic(packed, 28, ">I", 1, True), errno.EBADMSG),
                              (change_dynamic(packed, 16, ">Q", 512, True), errno.EBADMSG)):
            reject(probe, path, changed, code)
            count += 1
        for sector in (0, 1, 3, (len(packed) - SECTOR) // SECTOR):
            damaged = bytearray(packed)
            struct.pack_into(">I", damaged, 1536, sector)
            reject(probe, path, bytes(damaged))
            count += 1
        damaged = bytearray(packed)
        struct.pack_into(">I", damaged, 1540, 4)  # two logical blocks alias one physical block
        reject(probe, path, bytes(damaged))
        count += 1
        fault_matrix(fault_probe, path, packed, len(logical))
        count += 10
        count += write_matrix(write_probe, root, packed, logical)
        count += qemu_crosscheck(probe, write_probe, root)
    print(f"VHD fixed/dynamic source: {count} layout/fault/QEMU cases passed")


if __name__ == "__main__":
    main()
