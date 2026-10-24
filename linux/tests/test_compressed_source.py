#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Independent gzip corpus, stream/prefix checks and real >4-GiB output counting."""
import errno
import gzip
import hashlib
import pathlib
import random
import struct
import subprocess
import sys
import tempfile
import zlib


def probe(program, path, maximum, mode="hash", timeout=30):
    before = path.stat()
    completed = subprocess.run([program, str(maximum), mode, str(path)], text=True,
                               capture_output=True, timeout=timeout, check=True)
    result = dict(line.split(" ", 1) for line in completed.stdout.splitlines())
    after = path.stat()
    assert (before.st_ino, before.st_size, before.st_mtime_ns, before.st_ctime_ns) == (
        after.st_ino, after.st_size, after.st_mtime_ns, after.st_ctime_ns)
    return result


def check(program, path, packed, unpacked, members=1, trailing=0):
    path.write_bytes(packed)
    result = probe(program, path, max(1, len(unpacked)))
    assert int(result["error"]) == 0, result
    assert int(result["type"]) == 3 and int(result["source"]) == len(packed)
    assert int(result["decoded"]) == len(unpacked) and int(result["members"]) == members
    assert int(result["trailing"]) == trailing, result
    marker = len(unpacked) >= 512 and unpacked[510:512] == b"\x55\xaa"
    assert int(result["marker"]) == marker
    assert int(result["forced"]) == (1 if marker else 2 if len(unpacked) >= 512 else 0)
    assert result["prefix"] == unpacked[:512].hex()
    assert result["sha256"] == hashlib.sha256(unpacked).hexdigest()


def reject(program, path, packed, expected, maximum=8 * 1024**3):
    path.write_bytes(packed)
    result = probe(program, path, maximum)
    assert int(result["error"]) == expected, result
    for name in ("type", "source", "decoded", "members", "trailing", "marker", "forced"):
        assert int(result[name]) == 0, result
    assert result["prefix"] == result["sha256"] == ""


def with_name(packed, length):
    header = bytearray(packed[:10])
    header[3] |= 8
    return bytes(header) + b"n" * length + b"\0" + packed[10:]


def main():
    program = str(pathlib.Path(sys.argv[1]).resolve())
    cases = 0
    with tempfile.TemporaryDirectory(prefix="rufus-compressed-vectors-") as directory:
        path = pathlib.Path(directory) / "image ü.GZ"
        random_bytes = random.Random(17).randbytes(262163)
        for size in (0, 1, 511, 512, 513, 4096, 65535, 65536, 65537, 262163):
            data = bytearray(random_bytes[:size])
            if size >= 512:
                data[510:512] = b"\x55\xaa"
            packed = gzip.compress(data, mtime=0)
            assert gzip.decompress(packed) == data
            check(program, path, packed, data)
            cases += 1
        data = random_bytes[:511] + b"\x00" + random_bytes[512:8193]
        check(program, path, gzip.compress(data, mtime=0), data)
        cases += 1
        data = bytearray(random_bytes[:2048])
        data[510:512] = b"\x55\xaa"
        packed = gzip.compress(data, mtime=0)
        for members in ([bytes(data[:128]), bytes(data[128:])],
                        [b"", bytes(data), b""], [bytes(data), bytes(data)]):
            stream = b"".join(gzip.compress(member, mtime=0) for member in members)
            check(program, path, stream, b"".join(members), len(members))
            cases += 1
        for alignment in (0, 65535):
            first = gzip.compress(bytes(data[:512]), mtime=0)
            first = with_name(first, (alignment - len(first) - 1) % 65536)
            assert len(first) % 65536 == alignment
            stream = first + gzip.compress(bytes(data[512:]), mtime=0)
            check(program, path, stream, data, 2)
            cases += 1
        # All optional gzip header fields, including CRC16 of that header.
        header = bytearray(packed[:10])
        header[3] = 0x1e
        header += struct.pack("<H", 5) + b"extra" + b"disk.img\0comment\0"
        header += struct.pack("<H", zlib.crc32(header) & 0xffff)
        optional = bytes(header) + packed[10:]
        assert gzip.decompress(optional) == data
        check(program, path, optional, data)
        cases += 1
        for tail in (b"x", b"\x1f", b"unrelated trailing data", b"junk" + packed):
            check(program, path, packed + tail, data, trailing=len(tail))
            cases += 1
        for offset in (-8, -4, 2, 3):
            damaged = bytearray(packed)
            damaged[offset] ^= 0x20
            reject(program, path, damaged, errno.EBADMSG)
            cases += 1
        # A later valid member must never conceal a corrupt first trailer.
        for offset in (-8, -4):
            damaged = bytearray(packed)
            damaged[offset] ^= 1
            reject(program, path, bytes(damaged) + packed, errno.EBADMSG)
            cases += 1
        damaged = bytearray(optional)
        damaged[len(header) - 1] ^= 1
        reject(program, path, damaged, errno.EBADMSG)
        cases += 1
        for cut in (*range(0, 19), len(packed) - 8, len(packed) - 1):
            reject(program, path, packed[:cut], errno.EBADMSG)
            cases += 1
        reject(program, path, packed + b"\x1f\x8b", errno.EBADMSG)
        reject(program, path, zlib.compress(data), errno.EBADMSG)
        cases += 2
        # Dispatch only the initial Unix-compress signature. The same signature
        # after an actual gzip member remains an ignored tail.
        check(program, path, b"\x1f\x9d\x90", b"")
        check(program, path, packed + b"\x1f\x9d\x90", data, trailing=3)
        cases += 2
        for maximum in (1, 511, 512, len(data) - 1):
            reject(program, path, packed, errno.EFBIG, maximum)
            cases += 1
        for maximum in (0, 2**63):
            reject(program, path, packed, errno.EINVAL, maximum)
            cases += 1
        reject(program, pathlib.Path(directory) / "wrong.zip", packed, errno.EBADMSG)
        reject(program, pathlib.Path(directory) / "wrong.xz", packed, errno.EBADMSG)
        reject(program, pathlib.Path(directory) / "wrong.bz2", packed, errno.EBADMSG)
        reject(program, pathlib.Path(directory) / "wrong.lzma", packed, errno.EBADMSG)
        reject(program, pathlib.Path(directory) / "wrong.Z", packed, errno.EBADMSG)
        reject(program, pathlib.Path(directory) / "wrong.zst", packed, errno.EBADMSG)
        reject(program, pathlib.Path(directory) / "wrong.vtsi", packed, errno.EBADMSG)
        cases += 7
        reject(program, pathlib.Path(directory) / "wrong.vhdx", packed, errno.EBADMSG)
        cases += 1
        reject(program, pathlib.Path(directory) / "wrong.ffu", packed, errno.EBADMSG)
        cases += 1
        for suffix in (".img", ".7z"):
            reject(program, pathlib.Path(directory) / ("other" + suffix), packed, errno.ENOTSUP)
            cases += 1
        print(f"Compressed source: {cases} small corpus cases passed; generating >4-GiB stream", flush=True)
        # No expanded image is stored. ISIZE is only 512 although the real
        # decoded stream is 2**32 + 512 bytes, exposing the footer-size trap.
        prefix = bytearray(512)
        prefix[510:512] = b"\x55\xaa"
        zeros = bytes(1024 * 1024)
        compressor = zlib.compressobj(level=1, wbits=31)
        with path.open("wb") as file:
            file.write(compressor.compress(prefix))
            for _ in range(4096):
                file.write(compressor.compress(zeros))
            file.write(compressor.flush())
        with path.open("rb") as file:
            file.seek(-4, 2)
            assert struct.unpack("<I", file.read(4))[0] == 512
        result = probe(program, path, 2**32 + 512, "count")
        assert int(result["error"]) == 0 and int(result["decoded"]) == 2**32 + 512, result
        assert result["prefix"] == prefix.hex() and int(result["marker"]) == 1
        result = probe(program, path, 2**32, "count")
        assert int(result["error"]) == errno.EFBIG and int(result["decoded"]) == 0, result
        cases += 2
    print(f"Compressed source: {cases} corpus/large-stream cases passed")


if __name__ == "__main__":
    main()
