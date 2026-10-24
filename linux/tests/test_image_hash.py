#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare the shared Rufus core and native file reader with independent hashlib."""
import hashlib
import pathlib
import subprocess
import sys
import tempfile


NAMES = ("md5", "sha1", "sha256", "sha512")
ABC_DIGESTS = (
    "900150983cd24fb0d6963f7d28e17f72",
    "a9993e364706816aba3e25717850c26c9cd0d89d",
    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
    "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
    "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f",
)


def check(runner, path, data, mode, chunk):
    before = path.stat()
    result = subprocess.run([*runner, mode, str(chunk), str(path)],
                            text=True, capture_output=True, timeout=15, check=True)
    values = dict(line.split(" ", 1) for line in result.stdout.splitlines())
    count = 3 if mode == "default" else 4
    assert int(values["count"]) == count, result.stdout
    assert int(values["size"]) == len(data), result.stdout
    for index, name in enumerate(NAMES):
        expected = hashlib.new(name, data).hexdigest() if index < count else ""
        if data == b"abc" and index < count:
            assert expected == ABC_DIGESTS[index]
        assert values[name] == expected, (mode, chunk, len(data), name, values[name], expected)
    if mode == "software":
        assert values["sha1_acceleration"] == values["sha256_acceleration"] == "0"
    after = path.stat()
    assert (before.st_ino, before.st_size, before.st_mtime_ns, before.st_ctime_ns) == (
        after.st_ino, after.st_size, after.st_mtime_ns, after.st_ctime_ns)
    return values["sha1_acceleration"], values["sha256_acceleration"]


def main():
    probe = str(pathlib.Path(sys.argv[1]).resolve())
    runner = [probe]
    emulated = len(sys.argv) == 3
    if emulated:
        runner = [sys.argv[2], "-cpu", "max", probe]
    checks = 0
    acceleration = None
    with tempfile.TemporaryDirectory(prefix="rufus-hash-vectors-") as scratch:
        path = pathlib.Path(scratch) / "image with spaces & ü.img"
        # Empty/abc, padding boundaries for both block sizes, multi-block binary,
        # known million-'a' vector, and reads crossing the native 64 KiB buffer.
        vectors = [b"", b"abc", b"a" * 1_000_000]
        for size in (55, 56, 63, 64, 65, 111, 112, 127, 128, 129, 65535, 65536, 65537, 131109, 1048593):
            vectors.append(bytes((i * 37 + i // 251) % 256 for i in range(size)))
        for data in vectors:
            path.write_bytes(data)
            for mode in ("software", "auto", "default", "extra"):
                acceleration = check(runner, path, data, mode, 65536)
                checks += 1
        data = bytes((i * 17) % 256 for i in range(8193))
        path.write_bytes(data)
        for chunk in (1, 3, 55, 56, 63, 64, 65, 111, 112, 127, 128, 129, 511):
            for mode in ("software", "auto"):
                acceleration = check(runner, path, data, mode, chunk)
                checks += 1
        # Follow a source symlink with ordinary spaces/non-ASCII in its path.
        link = pathlib.Path(scratch) / "source link.img"
        link.symlink_to(path)
        check(runner, link, data, "extra", 65536)
        checks += 1
        # Four first-use readers share one descriptor, exercising once-only CPU
        # initialization and independent digest contexts without shared offsets.
        check(runner, path, data, "parallel", 65536)
        checks += 1
    if emulated:
        assert acceleration == ("1", "1"), "CPU emulation did not exercise both SHA instruction paths"
    print(f"Image hashes: {checks} vector/stream cases; auto SHA-1/SHA-256 acceleration {acceleration}")


if __name__ == "__main__":
    main()
