#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""VTSI sparse writer on owned regular files, with independent target readback."""
import errno
import hashlib
import pathlib
import random
import sys
import tempfile

from test_compressed_vtsi import SECTOR, image
from test_compressed_write import run


def boot_data(size):
    data = bytearray(random.Random(size + 31).randbytes(size))
    data[510:512] = b"\x55\xaa"
    return bytes(data)


def check_write(program, source, target, sector, disk_sectors, segments):
    packed = image(disk_sectors, segments)
    source.write_bytes(packed)
    target_bytes = disk_sectors * SECTOR
    tail = b"preserved suffix" * 11
    initial = b"\xa5" * target_bytes + tail
    target.write_bytes(initial)
    logical = bytearray(target_bytes)
    expected = bytearray(initial)
    data_bytes = 0
    for start, data, _ in segments:
        position = start * SECTOR
        logical[position:position + len(data)] = data
        expected[position:position + len(data)] = data
        data_bytes += len(data)
    report = run(program, source, target, sector)
    assert report["error"] == report["sync_error"] == 0, report
    assert report["compression"] == 8 and report["members"] == 1, report
    assert report["decoded"] == target_bytes and report["padded"] == target_bytes, report
    assert report["written"] == report["verified"] == data_bytes, report
    assert report["plan_sha256"] == hashlib.sha256(logical).hexdigest(), report
    assert target.read_bytes() == expected
    assert source.read_bytes() == packed
    return report


def main():
    program = str(pathlib.Path(sys.argv[1]).resolve())
    cases = 0
    with tempfile.TemporaryDirectory(prefix="rufus-vtsi-write-") as directory:
        root = pathlib.Path(directory)
        source, target = root / "image Ü.VTSI", root / "target.img"
        boot = boot_data(SECTOR)
        middle = random.Random(32).randbytes(2 * SECTOR)
        small = [(0, boot, 0xdeadbeef), (4, middle, 0)]
        check_write(program, source, target, SECTOR, 8, small)
        cases += 1
        for sector in (4096, 65536):
            sectors = sector // SECTOR
            first = boot_data(sector)
            last = random.Random(sector).randbytes(sector)
            check_write(program, source, target, sector, 3 * sectors,
                        [(0, first, 777), (2 * sectors, last, 0)])
            cases += 1
        packed = image(8, small)
        source.write_bytes(packed)
        target.write_bytes(b"\xa5" * 8192)
        report = run(program, source, target, 4096)
        assert report["error"] == errno.ENOTSUP and report["written"] == 0, report
        assert target.read_bytes() == b"\xa5" * 8192
        cases += 1
        damaged = bytearray(packed)
        damaged[-SECTOR + 100] ^= 1
        source.write_bytes(damaged)
        report = run(program, source, target)
        assert report["error"] == errno.EBADMSG and report["written"] == 0, report
        assert target.read_bytes() == b"\xa5" * 8192
        source.write_bytes(image(8, []))
        report = run(program, source, target, force=True)
        assert report["error"] == errno.ENOTSUP and report["written"] == 0, report
        cases += 2
        source.write_bytes(packed)
        target.write_bytes(b"\xa5" * 4095)
        report = run(program, source, target)
        assert report["error"] == errno.ENOSPC and report["written"] == 0, report
        assert target.read_bytes() == b"\xa5" * 4095
        cases += 1
        large = [(0, boot_data(2 * 65536), 0), (384, random.Random(33).randbytes(65536), 7)]
        packed = image(512, large)
        for fault in ("short", "eintr"):
            source.write_bytes(packed)
            target.write_bytes(b"\xa5" * (512 * SECTOR))
            report = run(program, source, target, fault=fault)
            assert report["error"] == 0 and report["written"] == 3 * 65536, (fault, report)
            assert target.read_bytes()[2 * 65536:384 * SECTOR] == b"\xa5" * (128 * SECTOR)
            cases += 1
        failure = (("read-fail", errno.EIO, True), ("early-eof", errno.EIO, True),
                   ("write-fail", errno.ENOSPC, True), ("sync-fail", errno.EIO, True),
                   ("verify-fail", errno.EIO, True), ("corrupt", errno.EBADMSG, True),
                   ("target-sync-resize", errno.ESTALE, True),
                   ("cancel-last-write", errno.ECANCELED, True),
                   ("source-copy-change", errno.ESTALE, False),
                   ("source-verify-change", errno.ESTALE, True))
        for fault, expected, wrote in failure:
            source.write_bytes(packed)
            target.write_bytes(b"\xa5" * (512 * SECTOR))
            report = run(program, source, target, fault=fault)
            assert report["error"] == expected, (fault, report)
            assert (report["written"] > 0) == wrote, (fault, report)
            assert target.read_bytes()[2 * 65536:384 * SECTOR] == b"\xa5" * (128 * SECTOR)
            cases += 1
        changed = bytearray(large[1][1])
        changed[123] ^= 1
        replacement = pathlib.Path(str(source) + ".replacement.vtsi")
        replacement.write_bytes(image(512, [large[0], (384, bytes(changed), 7)]))
        for fault, expected, wrote in (("preflight-substitute", errno.ESTALE, False),
                                       ("copy-substitute", errno.ESTALE, True),
                                       ("verify-substitute", errno.EBADMSG, True)):
            source.write_bytes(packed)
            target.write_bytes(b"\xa5" * (512 * SECTOR))
            report = run(program, source, target, fault=fault)
            assert report["error"] == expected and (report["written"] > 0) == wrote, (fault, report)
            assert target.read_bytes()[2 * 65536:384 * SECTOR] == b"\xa5" * (128 * SECTOR)
            cases += 1
        replacement.write_bytes(image(512, [(0, large[0][1][:65536], 0),
                                            (128, large[0][1][65536:], 0), large[1]]))
        assert len(replacement.read_bytes()) == len(packed)
        for fault, wrote in (("preflight-layout-substitute", False),
                             ("copy-layout-substitute", True),
                             ("verify-layout-substitute", True),
                             ("plan-vtsi-layout", False)):
            source.write_bytes(packed)
            target.write_bytes(b"\xa5" * (512 * SECTOR))
            report = run(program, source, target, fault=fault)
            assert report["error"] == errno.ESTALE and (report["written"] > 0) == wrote, (fault, report)
            assert target.read_bytes()[2 * 65536:384 * SECTOR] == b"\xa5" * (128 * SECTOR)
            cases += 1
    print(f"VTSI sparse writer: {cases} regular-file/geometry/fault cases passed")


if __name__ == "__main__":
    main()
