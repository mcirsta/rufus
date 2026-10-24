#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""ZIP frozen plans and bounded writes on disposable regular files."""
import errno
import hashlib
import json
from pathlib import Path
import random
import subprocess
import sys
import tempfile

from test_compressed_zip import archive, record


def invoke(program, source, target, sector=512, force=False, fault="none"):
    command = [program, str(source), str(target), str(sector), str(int(force)),
               str(2**63 - 1), fault]
    result = subprocess.run(command, capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, (command, result.returncode, result.stderr)
    return json.loads(result.stdout)


def payload(size, marker=True):
    data = bytearray(random.Random(size).randbytes(size))
    data[510:512] = b"\x55\xaa" if marker else b"\0\0"
    return bytes(data)


def check(program, root, data, method, sector, fault="none", force=False, first_directory=False):
    source, target = root / "image.ZIP", root / "target.img"
    row = record(data, method)
    content = archive([record(b"", 0, name=b"directory/"), row], order=[1, 0]) if first_directory else archive([row])
    source.write_bytes(content)
    padded = data + bytes(-len(data) % sector)
    original = b"\xa5" * (len(padded) + 29)
    target.write_bytes(original)
    before = source.stat()
    result = invoke(program, source, target, sector, force, fault)
    assert result["error"] == 0 and result["phase"] == 4, (method, fault, result)
    assert result["compression"] == 1 and result["decoded"] == len(data), result
    assert result["padded"] == result["written"] == result["verified"] == len(padded), result
    assert result["plan_sha256"] == hashlib.sha256(data).hexdigest(), result
    assert result["members"] > 0 and result["trailing"] > 0, result
    assert target.read_bytes() == padded + original[len(padded):]
    after = source.stat()
    assert source.read_bytes() == content
    assert (before.st_ino, before.st_size, before.st_mtime_ns, before.st_ctime_ns) == (
        after.st_ino, after.st_size, after.st_mtime_ns, after.st_ctime_ns)
    if fault == "parallel":
        assert result["parallel_sha256"] == hashlib.sha256(content).hexdigest(), result


def refusals(program, root):
    source, target = root / "image.ZIP", root / "target.img"
    data = payload(3 * 65536 + 37)
    packed = archive([record(data)])
    padded = data + bytes(-len(data) % 512)
    original = b"\xa5" * (len(padded) + 29)
    for fault, expected in (("plan-hash", errno.ESTALE), ("plan-size-small", errno.ESTALE),
                            ("plan-size-large", errno.ESTALE), ("plan-members", errno.ESTALE),
                            ("plan-tail", errno.ESTALE), ("plan-prefix", errno.ESTALE),
                            ("source-before-change", errno.ESTALE), ("preflight-read", errno.EIO),
                            ("cancel-before", errno.ECANCELED), ("readonly", errno.EINVAL),
                            ("append", errno.EINVAL)):
        source.write_bytes(packed)
        target.write_bytes(original)
        result = invoke(program, source, target, fault=fault)
        assert result["error"] == expected and result["written"] == result["syncs"] == 0, (fault, result)
        assert target.read_bytes() == original, fault
    for fault, expected in (("write-fail", errno.ENOSPC), ("sync-fail", errno.EIO),
                            ("verify-fail", errno.EIO), ("corrupt", errno.EBADMSG)):
        source.write_bytes(packed)
        target.write_bytes(original)
        result = invoke(program, source, target, fault=fault)
        assert result["error"] == expected and result["phase"] != 4, (fault, result)
        assert result["syncs"] == int(result["written"] > 0), (fault, result)
    source.write_bytes(packed)
    target.write_bytes(original)
    result = invoke(program, source, target, force=False)
    assert result["error"] == 0 and target.read_bytes() == padded + original[len(padded):]
    target.write_bytes(original)
    result = invoke(program, source, source)
    assert result["error"] == errno.EINVAL and result["written"] == 0, result
    assert source.read_bytes() == packed and target.read_bytes() == original
    damaged = bytearray(packed)
    assert damaged[:4] == b"PK\x03\x04"
    damaged[14] ^= 1
    source.write_bytes(damaged)
    result = invoke(program, source, target)
    assert result["error"] == errno.EBADMSG and result["written"] == 0, result
    assert target.read_bytes() == original
    source.write_bytes(archive([record(payload(8193, marker=False))]))
    result = invoke(program, source, target)
    assert result["error"] == errno.ENOTSUP and result["written"] == 0, result
    assert target.read_bytes() == original


def decoded_hash_substitution(program, root):
    source, target = root / "image.ZIP", root / "target.img"
    data = payload(3 * 65536 + 37)
    changed = bytearray(data)
    changed[65550] ^= 1
    packed = archive([record(data, 0)])
    alternate = archive([record(bytes(changed), 0)])
    assert len(packed) == len(alternate)
    replacement = Path(str(source) + ".replacement.zip")
    replacement.write_bytes(alternate)
    padded = data + bytes(-len(data) % 512)
    original = b"\xa5" * (len(padded) + 29)
    for fault, expected, phase, written in (("preflight-substitute", errno.ESTALE, 0, 0),
                                            ("copy-substitute", errno.ESTALE, 1, 3 * 65536),
                                            ("verify-substitute", errno.EBADMSG, 3, len(padded))):
        source.write_bytes(packed)
        target.write_bytes(original)
        result = invoke(program, source, target, fault=fault)
        assert (result["error"], result["phase"], result["written"]) == (expected, phase, written), (fault, result)
        assert result["syncs"] == int(written > 0), (fault, result)
        assert source.read_bytes() == packed and replacement.read_bytes() == alternate


def main():
    program = str(Path(sys.argv[1]).resolve())
    cases = 0
    with tempfile.TemporaryDirectory(prefix="rufus-zip-write-") as temporary:
        root = Path(temporary)
        for method in (0, 8, 12, 14, 95):
            for sector in (512, 4096, 65536):
                for size in (512, 513, 65537, 3 * 65536 + 37):
                    check(program, root, payload(size), method, sector)
                    cases += 1
            check(program, root, payload(8193, marker=False), method, 4096, force=True)
            check(program, root, payload(65537), method, 512, first_directory=True)
            check(program, root, payload(3 * 65536 + 37), method, 512, fault="parallel")
            cases += 3
        refusals(program, root)
        decoded_hash_substitution(program, root)
    print(f"ZIP writer: {cases} stored/deflate/BZIP2/LZMA/XZ success cases and frozen-plan/I/O refusals passed")


if __name__ == "__main__":
    main()
