# SPDX-License-Identifier: GPL-3.0-or-later
"""Rufus's original NTFS boot record on disposable native-formatted files."""
import argparse
import errno
import os
from pathlib import Path
import re
import subprocess
import tempfile
from types import SimpleNamespace

from test_ntfs_format import inspect as inspect_ntfs, invoke as format_ntfs, make_target

MIB = 1024 * 1024
SIZE = 64 * MIB
OFFSET = MIB
CLUSTER = 4096
RECORD_OFFSET = 0x54
LABEL = "Rufus test"
SOURCE = Path(__file__).resolve().parents[2] / "src/ms-sys/inc"


def original_code(name):
    text = (SOURCE / name).read_text()
    contents = text.split("{", 1)[1].split("}", 1)[0]
    return bytes(int(value, 16) for value in re.findall(r"0x([0-9a-fA-F]{2})\b", contents))


def invoke(probe, image, fault, error, written=0, sync_error=0):
    process = subprocess.run([probe, image, str(OFFSET), str(SIZE), str(CLUSTER), fault],
                             capture_output=True, timeout=20)
    assert process.returncode == 0, (fault, process.stdout, process.stderr)
    values = {name.decode(): int(value) for name, value in re.findall(rb"([A-Z_]+)=(-?\d+)", process.stdout)}
    assert values["ERROR"] == error and values["BYTES"] == written, (fault, values, process.stderr)
    assert values["SYNC"] == sync_error, (fault, values)
    assert values["SYNCS"] == (2 if fault == "sync-eintr" else int(written != 0)), (fault, values)
    assert values["FDS"] == 0 and values["OFFSET"] == 17 and values["FLAGS"] == 1, (fault, values)
    return values


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("probe", type=Path)
    parser.add_argument("format_probe", type=Path)
    parser.add_argument("--mkfs", required=True)
    parser.add_argument("--info", required=True)
    parser.add_argument("--label", required=True)
    parser.add_argument("--fsck")
    args = parser.parse_args()
    first = original_code("br_ntfs_0x0.h")
    code = original_code("br_ntfs_0x54.h")
    record_bytes = RECORD_OFFSET + len(code)
    assert len(first) == 11 and record_bytes == 4136
    with tempfile.TemporaryDirectory(prefix="rufus-ntfs-boot-") as temporary:
        image = make_target(Path(temporary), "ntfs.raw", SIZE)
        formatter = SimpleNamespace(probe=args.format_probe, mkfs=args.mkfs)
        formatted = format_ntfs(formatter, image, SIZE, OFFSET, CLUSTER, LABEL)
        inspect_ntfs(args, image, SIZE, OFFSET, CLUSTER, LABEL, formatted)
        before = image.read_bytes()
        expected = bytearray(before)
        expected[:len(first)] = first
        expected[RECORD_OFFSET:record_bytes] = code
        assert expected[11:RECORD_OFFSET] == before[11:RECORD_OFFSET]
        assert expected[record_bytes:] == before[record_bytes:]
        assert expected[-512:] == before[-512:]

        invalid = [("readonly", errno.EINVAL), ("append", errno.EINVAL),
                   ("direct", errno.EINVAL), ("block-request", errno.ENOTSUP),
                   ("wrong-offset", errno.EBADMSG), ("wrong-cluster", errno.EBADMSG),
                   ("read-fail", errno.EIO), ("patch-fail", errno.EBADMSG),
                   ("cancel-before", errno.ECANCELED)]
        for fault, error in invalid:
            image.write_bytes(before)
            invoke(args.probe, image, fault, error)
            assert image.read_bytes() == before, fault

        success = ["none", "short-write", "short-read", "read-eintr", "write-eintr", "sync-eintr"]
        for fault in success:
            image.write_bytes(before)
            invoke(args.probe, image, fault, 0, record_bytes)
            assert image.read_bytes() == expected, fault

        failures = [("write-fail", errno.EIO, 0), ("write-zero", errno.EIO, 0),
                    ("partial-fail", errno.EIO, 17), ("cancel-partial", errno.ECANCELED, 17),
                    ("cancel-after-write", errno.ECANCELED, record_bytes),
                    ("sync-fail", errno.EIO, record_bytes),
                    ("verify-fail", errno.EIO, record_bytes),
                    ("verify-eof", errno.EIO, record_bytes),
                    ("verify-corrupt", errno.EBADMSG, record_bytes),
                    ("backup-corrupt", errno.EBADMSG, record_bytes),
                    ("resize", errno.ESTALE, record_bytes)]
        for fault, error, written in failures:
            image.write_bytes(before)
            invoke(args.probe, image, fault, error, written, errno.EIO if fault == "sync-fail" else 0)
            result = image.read_bytes()
            changed = bytearray(before)
            changed[:written] = expected[:written]
            if fault == "resize":
                changed.extend(bytes(512))
            assert result == changed, fault

        image.write_bytes(before)
        with image.open("r+b") as target:
            target.write(b"X")
        damaged = image.read_bytes()
        invoke(args.probe, image, "none", errno.EBADMSG)
        assert image.read_bytes() == damaged
        image.write_bytes(before)
        with image.open("r+b") as target:
            target.seek(-512, os.SEEK_END)
            target.write(b"X")
        damaged = image.read_bytes()
        invoke(args.probe, image, "none", errno.EBADMSG)
        assert image.read_bytes() == damaged

        image.write_bytes(before)
        invoke(args.probe, image, "none", 0, record_bytes)
        label = subprocess.run([args.label, image], capture_output=True, timeout=15, check=True)
        assert label.stdout.decode().strip() == LABEL
        info = subprocess.run([args.info, "-m", image], capture_output=True, timeout=15, check=True)
        assert b"Volume Version: 3.1" in info.stdout
    print("29 NTFS PBR success/refusal/fault cases passed; formatted file removed.")


if __name__ == "__main__":
    main()
