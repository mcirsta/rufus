#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fixed/dynamic VHD through the strict helper on owned regular-file devices."""
import errno
import hashlib
import os
import pathlib
import resource
import sys
import tempfile

from test_compressed_device_helper import DEVICE, run, timestamp
from test_compressed_vhd import SECTOR, dynamic, footer


def packet(root, packed, logical, capacity):
    source = root / "source.vhd"
    source.write_bytes(packed)
    info = source.stat()
    return {"operation": "compressed-write-device-v1", "path": str(root / "disk.bin"),
            "device": str(DEVICE), "size": str(capacity), "sequence": "19",
            "object": "/org/freedesktop/UDisks2/block_devices/fixture", "owner": ":1.42",
            "identity": ["fixture"],
            "source": {"path": str(source), "device": str(info.st_dev), "inode": str(info.st_ino),
                       "size": str(info.st_size), "modified": timestamp(info.st_mtime_ns),
                       "changed": timestamp(info.st_ctime_ns),
                       "compression": {"type": 11, "decoded": str(len(logical)), "sector": SECTOR,
                                       "sha256": hashlib.sha256(logical).hexdigest(),
                                       "ignoreMarker": False}}}


def main():
    assert os.getuid() != 0, "Run the owned fixture as an ordinary user"
    _, hard = resource.getrlimit(resource.RLIMIT_CORE)
    resource.setrlimit(resource.RLIMIT_CORE, (0, hard))
    program = str(pathlib.Path(sys.argv[1]).resolve())
    logical = bytearray(16 * SECTOR)
    logical[510:512] = b"\x55\xaa"
    logical[3 * SECTOR:4 * SECTOR] = b"V" * SECTOR
    fixed = bytes(logical) + footer(len(logical), 2, 0xffffffffffffffff)
    cases = 0
    with tempfile.TemporaryDirectory(prefix="rufus-compressed-device-") as directory:
        root = pathlib.Path(directory)
        capacity = 64 * 1024
        initial = b"\xa5" * capacity
        expected = bytes(logical) + initial[len(logical):]
        modes = ("success", "protected", "bad-footer", "bad-sha", "wrong-inode",
                 "source-owner", "claim-error", "mount-mismatch", "boundary-sector",
                 "write-error", "sync-error", "verify-error", "corrupt", "cancel",
                 "cancel-sync", "final-snapshot-error", "target-close-fail")
        for kind, packed in (("fixed", fixed), ("dynamic", dynamic(bytes(logical)))):
            for mode in modes:
                current = bytearray(packed)
                if mode == "bad-footer":
                    current[-SECTOR + 10] ^= 1
                request = packet(root, bytes(current), logical, capacity)
                if mode == "bad-sha":
                    request["source"]["compression"]["sha256"] = "00" * 32
                if mode == "wrong-inode":
                    request["source"]["inode"] = str(int(request["source"]["inode"]) + 1)
                target = root / "disk.bin"
                target.write_bytes(initial)
                result, diagnostics = run(program, root, request, mode, SECTOR, len(logical))
                error, written, verified = result["error"], int(result["written"]), int(result["verified"])
                stages = [line.removeprefix("COMPRESSED_FIXTURE ") for line in diagnostics
                          if line.startswith("COMPRESSED_FIXTURE ")]
                assert (root / "source.vhd").read_bytes() == current, (kind, mode)
                if mode in ("success", "protected"):
                    assert error == result["syncError"] == 0 and result["phase"] == 4, (kind, mode, result, diagnostics)
                    assert written == verified == len(logical) and target.read_bytes() == expected, (kind, mode, result)
                elif mode in ("bad-footer", "bad-sha", "wrong-inode", "source-owner"):
                    wanted = {"bad-footer": errno.EBADMSG, "bad-sha": errno.ESTALE,
                              "wrong-inode": errno.ESTALE, "source-owner": errno.EACCES}[mode]
                    assert error == wanted and written == verified == 0 and "claim" not in stages, (kind, mode, result, diagnostics)
                    assert target.read_bytes() == initial, (kind, mode)
                elif mode in ("claim-error", "mount-mismatch", "boundary-sector"):
                    wanted = {"claim-error": errno.EBUSY, "mount-mismatch": errno.ESTALE,
                              "boundary-sector": errno.ESTALE}[mode]
                    assert error == wanted and written == verified == 0, (kind, mode, result, diagnostics)
                    assert target.read_bytes() == initial, (kind, mode)
                elif mode in ("write-error", "cancel"):
                    assert error == (errno.ENOSPC if mode == "write-error" else errno.ECANCELED), (kind, mode, result, diagnostics)
                    assert written == 17 and verified == 0 and stages.count("sync") == 1, (kind, mode, result, diagnostics)
                    assert target.read_bytes()[:17] == logical[:17], (kind, mode)
                else:
                    wanted = {"sync-error": errno.EIO, "verify-error": errno.EIO,
                              "corrupt": errno.EBADMSG, "cancel-sync": errno.ECANCELED,
                              "final-snapshot-error": errno.ESTALE, "target-close-fail": errno.EIO}[mode]
                    assert error == wanted and written == len(logical), (kind, mode, result, diagnostics)
                    assert stages.count("sync") == 1, (kind, mode, result, diagnostics)
                    actual = target.read_bytes()
                    if mode == "corrupt":
                        assert actual == expected[:23] + bytes([expected[23] ^ 1]) + expected[24:], (kind, mode)
                    else:
                        assert actual == expected, (kind, mode)
                if "claim" in stages:
                    assert stages.count("claim") == stages.count("close-target") == 1, (kind, mode, stages)
                    assert stages.index("claim") < stages.index("close-target") < stages.index("close-source"), (kind, mode, stages)
                cases += 1
    print(f"VHD device helper: {cases} fixed/dynamic wire/claim/fault/cleanup cases passed")


if __name__ == "__main__":
    main()
