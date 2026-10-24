#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""VTSI sparse segments through the real helper on a simulated owned device."""
import errno
import hashlib
import os
import pathlib
import random
import resource
import struct
import sys
import tempfile

from test_compressed_device_helper import DEVICE, run, timestamp
from test_compressed_vtsi import SECTOR, image


def packet(root, packed, logical, segments, capacity, sector=SECTOR):
    source = root / "source.vtsi"
    source.write_bytes(packed)
    info = source.stat()
    layout = hashlib.sha256()
    source_offset = 0
    for start, data, _ in segments:
        layout.update(struct.pack("<QQQ", start * SECTOR, source_offset, len(data)))
        source_offset += len(data)
    layout.update(struct.pack("<QQ", len(logical), source_offset))
    return {"operation": "compressed-write-device-v1", "path": str(root / "disk.bin"),
            "device": str(DEVICE), "size": str(capacity), "sequence": "19",
            "object": "/org/freedesktop/UDisks2/block_devices/fixture", "owner": ":1.42",
            "identity": ["fixture"],
            "source": {"path": str(source), "device": str(info.st_dev), "inode": str(info.st_ino),
                       "size": str(info.st_size), "modified": timestamp(info.st_mtime_ns),
                       "changed": timestamp(info.st_ctime_ns),
                       "compression": {"type": 8, "decoded": str(len(logical)), "sector": sector,
                                       "sha256": hashlib.sha256(logical).hexdigest(),
                                       "layoutSha256": layout.hexdigest(),
                                       "ignoreMarker": False}}}


def main():
    assert os.getuid() != 0, "Run the owned fixture as an ordinary user"
    _, hard = resource.getrlimit(resource.RLIMIT_CORE)
    resource.setrlimit(resource.RLIMIT_CORE, (0, hard))
    program = str(pathlib.Path(sys.argv[1]).resolve())
    cases = 0
    with tempfile.TemporaryDirectory(prefix="rufus-compressed-device-") as directory:
        root = pathlib.Path(directory)
        boot = bytearray(random.Random(31).randbytes(2 * 65536))
        boot[510:512] = b"\x55\xaa"
        second = random.Random(32).randbytes(65536)
        segments = [(0, bytes(boot), 0xdeadbeef), (384, second, 0)]
        packed = image(512, segments)
        logical = bytearray(512 * SECTOR)
        logical[:len(boot)] = boot
        logical[384 * SECTOR:384 * SECTOR + len(second)] = second
        capacity = 1024 * 1024
        initial = b"\xa5" * capacity
        expected = bytearray(initial)
        expected[:len(boot)] = boot
        expected[384 * SECTOR:384 * SECTOR + len(second)] = second
        payload_bytes = len(boot) + len(second)
        modes = ("success", "protected", "bad-footer", "bad-sha", "bad-layout", "changed-layout", "wrong-inode",
                 "source-owner", "claim-error", "mount-mismatch", "boundary-sector",
                 "write-error", "sync-error", "verify-error", "corrupt", "cancel",
                 "cancel-sync", "final-snapshot-error", "target-close-fail")
        for mode in modes:
            this_packed = bytearray(packed)
            if mode == "bad-footer":
                this_packed[-SECTOR + 100] ^= 1
            if mode == "changed-layout":
                this_packed = bytearray(image(512, [(0, bytes(boot[:65536]), 0),
                                                    (128, bytes(boot[65536:]), 0), segments[1]]))
                assert len(this_packed) == len(packed)
            request = packet(root, bytes(this_packed), logical, segments, capacity)
            if mode == "bad-sha":
                request["source"]["compression"]["sha256"] = "00" * 32
            if mode == "bad-layout":
                request["source"]["compression"]["layoutSha256"] = "00" * 32
            if mode == "wrong-inode":
                request["source"]["inode"] = str(int(request["source"]["inode"]) + 1)
            target = root / "disk.bin"
            target.write_bytes(initial)
            result, diagnostics = run(program, root, request, mode, SECTOR, len(logical))
            error, written, verified = result["error"], int(result["written"]), int(result["verified"])
            stages = [line.removeprefix("COMPRESSED_FIXTURE ") for line in diagnostics
                      if line.startswith("COMPRESSED_FIXTURE ")]
            assert target.read_bytes()[len(boot):384 * SECTOR] == initial[len(boot):384 * SECTOR], (mode, result)
            assert target.read_bytes()[384 * SECTOR + len(second):] == initial[384 * SECTOR + len(second):], (mode, result)
            assert (root / "source.vtsi").read_bytes() == this_packed
            if mode in ("success", "protected"):
                assert error == result["syncError"] == 0 and result["phase"] == 4, (mode, result, diagnostics)
                assert written == verified == payload_bytes and target.read_bytes() == expected, (mode, result)
            elif mode in ("bad-footer", "bad-sha", "bad-layout", "changed-layout", "wrong-inode", "source-owner"):
                wanted = {"bad-footer": errno.EBADMSG, "bad-sha": errno.ESTALE,
                          "bad-layout": errno.ESTALE, "changed-layout": errno.ESTALE,
                          "wrong-inode": errno.ESTALE,
                          "source-owner": errno.EACCES}[mode]
                assert error == wanted and written == verified == 0 and "claim" not in stages, (mode, result, diagnostics)
                assert target.read_bytes() == initial
            elif mode in ("claim-error", "mount-mismatch", "boundary-sector"):
                wanted = {"claim-error": errno.EBUSY, "mount-mismatch": errno.ESTALE,
                          "boundary-sector": errno.ESTALE}[mode]
                assert error == wanted and written == verified == 0, (mode, result, diagnostics)
                assert target.read_bytes() == initial
            elif mode in ("write-error", "cancel"):
                assert error == (errno.ENOSPC if mode == "write-error" else errno.ECANCELED), (mode, result, diagnostics)
                assert written == 17 and verified == 0 and stages.count("sync") == 1, (mode, result, diagnostics)
                assert target.read_bytes()[:17] == boot[:17]
            else:
                wanted = {"sync-error": errno.EIO, "verify-error": errno.EIO,
                          "corrupt": errno.EBADMSG, "cancel-sync": errno.ECANCELED,
                          "final-snapshot-error": errno.ESTALE, "target-close-fail": errno.EIO}[mode]
                assert error == wanted and written == payload_bytes, (mode, result, diagnostics)
                assert stages.count("sync") == 1, (mode, result, diagnostics)
                assert target.read_bytes() == (expected if mode != "corrupt" else
                    expected[:23] + bytes([expected[23] ^ 1]) + expected[24:]), (mode, result)
            if "claim" in stages:
                assert stages.count("claim") == stages.count("close-target") == 1, (mode, stages)
                assert stages.index("claim") < stages.index("close-target") < stages.index("close-source"), (mode, stages)
            cases += 1
    print(f"VTSI device helper: {cases} sparse wire/claim/cleanup cases passed; owned regular files only")


if __name__ == "__main__":
    main()
