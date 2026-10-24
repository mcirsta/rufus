#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Independent Zstd CLI vectors for the retained-source decoder."""
import errno
import hashlib
import pathlib
import random
import struct
import subprocess
import sys
import tempfile


def compress(data, *options):
    command = ["zstd", "-q", "-c", *options]
    packed = subprocess.run(command, input=data, capture_output=True, check=True).stdout
    decoder_options = ["--long=28"] if "--long=28" in options else []
    decoded = subprocess.run(["zstd", "-q", "-d", "-c", *decoder_options], input=packed,
                             capture_output=True, check=True).stdout
    assert decoded == data
    return packed


def probe(program, path, packed, maximum, mode="hash", timeout=30):
    path.write_bytes(packed)
    before = path.stat()
    completed = subprocess.run([program, str(maximum), mode, str(path)], text=True,
                               capture_output=True, timeout=timeout, check=True)
    after = path.stat()
    assert (before.st_ino, before.st_size, before.st_mtime_ns, before.st_ctime_ns) == (
        after.st_ino, after.st_size, after.st_mtime_ns, after.st_ctime_ns)
    return dict(line.split(" ", 1) for line in completed.stdout.splitlines())


def check(program, path, packed, data, members=1):
    result = probe(program, path, packed, max(1, len(data)))
    assert int(result["error"]) == 0, result
    assert int(result["type"]) == 9 and int(result["source"]) == len(packed), result
    assert int(result["decoded"]) == len(data) and int(result["members"]) == members, result
    assert int(result["trailing"]) == 0, result
    marker = len(data) >= 512 and data[510:512] == b"\x55\xaa"
    assert int(result["marker"]) == marker, result
    assert int(result["forced"]) == (1 if marker else 2 if len(data) >= 512 else 0), result
    assert result["prefix"] == data[:512].hex(), result
    assert result["sha256"] == hashlib.sha256(data).hexdigest(), result


def reject(program, path, packed, expected, maximum=1024 * 1024):
    result = probe(program, path, packed, maximum)
    assert int(result["error"]) == expected, result
    for field in ("type", "source", "decoded", "members", "trailing", "marker", "forced"):
        assert int(result[field]) == 0, result
    assert result["prefix"] == result["sha256"] == "", result


def check_faults(program, path, packed, decoded_size):
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
        if expected == 0:
            assert int(result["result_empty"]) == 0, result
            assert int(result["result_size"]) == decoded_size
            assert int(result["received"]) == decoded_size
        else:
            assert int(result["result_empty"]) == 1, result
    completed = subprocess.run([program, "0", str(decoded_size - 1), str(path)],
                               text=True, capture_output=True, timeout=30, check=True)
    result = dict(line.split(" ", 1) for line in completed.stdout.splitlines())
    assert int(result["error"]) == errno.EFBIG and int(result["result_empty"]) == 1, result


def main():
    program = str(pathlib.Path(sys.argv[1]).resolve())
    fault_program = str(pathlib.Path(sys.argv[2]).resolve()) if len(sys.argv) > 2 else None
    cases = 0
    with tempfile.TemporaryDirectory(prefix="rufus-zstd-vectors-") as directory:
        path = pathlib.Path(directory) / "image Ü.ZST"
        random_bytes = random.Random(30).randbytes(262163)
        for size in (0, 1, 511, 512, 513, 65535, 65536, 65537, 131075, 262163):
            data = bytearray(random_bytes[:size])
            if size >= 512:
                data[510:512] = b"\x55\xaa"
            check(program, path, compress(data), data)
            cases += 1
        data = random_bytes[:131075]
        for options in (("--no-check",), ("-19",), ("--fast=3",)):
            check(program, path, compress(data, *options), data)
            cases += 1
        known_source = pathlib.Path(directory) / "known-size.img"
        known_source.write_bytes(data)
        known = subprocess.run(["zstd", "-q", "-c", str(known_source)],
                               capture_output=True, check=True).stdout
        check(program, path, known, data)
        cases += 1
        frame_a, frame_b = compress(data[:512]), compress(data[512:])
        empty = compress(b"")
        check(program, path, frame_a + frame_b, data, 2)
        check(program, path, empty + frame_a + frame_b + empty, data, 4)
        cases += 2
        packed = compress(random_bytes[:8192])
        reject(program, path, b"", errno.EBADMSG)
        reject(program, path, random_bytes[:512], errno.EBADMSG)
        reject(program, path, packed + b"garbage", errno.EBADMSG)
        reject(program, path, packed + b"\x28\xb5\x2f\xfd", errno.EBADMSG)
        reject(program, path, packed[:3], errno.EBADMSG)
        reject(program, path, packed[:-1], errno.EBADMSG)
        cases += 6
        boundary = compress(random_bytes)
        for cut in (131071, 131075, len(boundary) - 1):
            reject(program, path, boundary[:cut], errno.EBADMSG)
            cases += 1
        damaged = bytearray(packed)
        damaged[-1] ^= 0x80
        reject(program, path, damaged, errno.EBADMSG)
        reject(program, path, bytes(damaged) + packed, errno.EBADMSG)
        cases += 2
        reject(program, path, packed, errno.EFBIG, len(random_bytes[:8192]) - 1)
        reject(program, path, packed, errno.EINVAL, 0)
        reject(program, path, packed, errno.EINVAL, 2**63)
        reject(program, pathlib.Path(directory) / "image.img", packed, errno.ENOTSUP)
        cases += 4
        # Valid skippable frames are part of the Zstd frame format. The
        # retained reader consumes them rather than treating them as a tail.
        skippable = struct.pack("<II", 0x184D2A50, 3) + b"tag"
        result = probe(program, path, skippable + packed, 8192)
        assert int(result["error"]) == 0 and int(result["decoded"]) == 8192, result
        assert int(result["members"]) == 2 and int(result["trailing"]) == 0, result
        cases += 1
        large_window = compress(b"A" * 8192, "--long=28")
        reject(program, path, large_window, errno.EFBIG)
        cases += 1
        if fault_program is not None:
            check_faults(fault_program, path, compress(random_bytes), len(random_bytes))
            cases += 11
        block = bytearray(1024 * 1024)
        block[510:512] = b"\x55\xaa"
        large = compress(block) * 4096 + compress(b"Z" * 512)
        expected = 2**32 + 512
        result = probe(program, path, large, expected, "count", timeout=120)
        assert int(result["error"]) == 0 and int(result["decoded"]) == expected, result
        assert int(result["members"]) == 4097 and int(result["marker"]) == 1, result
        assert result["prefix"] == bytes(block[:512]).hex(), result
        result = probe(program, path, large, 2**32, "count", timeout=120)
        assert int(result["error"]) == errno.EFBIG and int(result["decoded"]) == 0, result
        cases += 2
    print(f"Zstd retained source: {cases} independent vectors passed")


if __name__ == "__main__":
    main()
