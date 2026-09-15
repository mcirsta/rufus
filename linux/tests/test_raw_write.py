# SPDX-License-Identifier: GPL-3.0-or-later
"""Write disposable files through the native operation and inspect output independently."""
import errno
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

runs = 0
MIB = 1024 * 1024
PREPARE, COPY, SYNC, VERIFY, DONE = range(5)


def run(probe, source, target, fault="none", replacement=None):
    global runs
    command = [str(probe), str(source), str(target), fault]
    if replacement is not None:
        command.append(str(replacement))
    process = subprocess.run(command, capture_output=True, timeout=10)
    runs += 1
    assert process.returncode == 0, (command, process.returncode, process.stderr)
    report = json.loads(process.stdout)
    assert (report["phase"] == DONE) == (report["error"] == 0), report
    return report


def payload(size):
    pattern = bytes((value * 31 + 7) % 256 for value in range(256))
    return (pattern * ((size + 255) // 256))[:size]


def check_success(probe, root):
    source, target = root / "source.img", root / "target.img"
    tail = b"unchanged target tail" * 127
    for size in (1, 511, 512, MIB - 1, MIB, MIB + 1, 2 * MIB + 37):
        data = payload(size)
        source.write_bytes(data)
        for extra in (b"", tail):
            target.write_bytes(b"\xa5" * size + extra)
            report = run(probe, source, target)
            assert report["error"] == report["sync_error"] == 0, report
            assert report["written"] == report["verified"] == size and report["syncs"] == 1, report
            assert target.read_bytes() == data + extra
            assert source.read_bytes() == data
    for fault in ("short", "eintr"):
        target.write_bytes(b"\xa5" * len(data) + tail)
        report = run(probe, source, target, fault)
        assert report["error"] == 0 and report["written"] == report["verified"] == len(data), report
        assert report["syncs"] == (2 if fault == "eintr" else 1), report
        assert target.read_bytes() == data + tail and source.read_bytes() == data


def check_failures(probe, root):
    source, target = root / "source.img", root / "target.img"
    data = payload(2 * MIB + 37)
    original_target = b"\xa5" * (len(data) + 123)
    cases = (("cancel-before", errno.ECANCELED, PREPARE, 0),
             ("read-fail", errno.EIO, COPY, MIB),
             ("early-eof", errno.EIO, COPY, MIB),
             ("zero-write", errno.EIO, COPY, 0),
             ("write-fail", errno.ENOSPC, COPY, 4096),
             ("write-cancel", errno.ECANCELED, COPY, 4096),
             ("sync-fail", errno.EIO, SYNC, len(data)),
             ("double-fail", errno.ENOSPC, COPY, 4096),
             ("sync-cancel", errno.ECANCELED, SYNC, len(data)),
             ("verify-fail", errno.EIO, VERIFY, len(data)),
             ("verify-cancel", errno.ECANCELED, VERIFY, len(data)),
             ("corrupt", errno.EBADMSG, VERIFY, len(data)),
             ("source-change", errno.ESTALE, SYNC, len(data)))
    for fault, error, phase, written in cases:
        source.write_bytes(data)
        target.write_bytes(original_target)
        report = run(probe, source, target, fault)
        assert report["error"] == error and report["phase"] == phase, (fault, report)
        assert report["written"] == written, (fault, report)
        assert report["syncs"] == int(written != 0), (fault, report)
        assert report["sync_error"] == (errno.EIO if fault in ("sync-fail", "double-fail") else 0), report
        assert report["verified"] == (MIB if fault == "verify-cancel" else 0), report
        expected = bytearray(data[:written] + original_target[written:])
        if fault == "corrupt":
            expected[23] ^= 1
        assert target.read_bytes() == expected, fault
        if fault != "source-change":
            assert source.read_bytes() == data
    # A new operation can recover on the same ordinary files after a partial write.
    source.write_bytes(data)
    report = run(probe, source, target)
    assert report["error"] == 0 and target.read_bytes() == data + original_target[len(data):], report


def check_preflight(probe, root):
    source, target = root / "source.img", root / "target.img"
    data = payload(4096)
    source.write_bytes(data)
    for fault in ("readonly", "writeonly", "append"):
        target.write_bytes(b"\xa5" * len(data))
        report = run(probe, source, target, fault)
        assert report["error"] == errno.EINVAL and report["written"] == report["syncs"] == 0, report
        assert target.read_bytes() == b"\xa5" * len(data)
    target.write_bytes(b"\xa5" * (len(data) - 1))
    report = run(probe, source, target)
    assert report["error"] == errno.ENOSPC and report["written"] == 0, report
    assert target.read_bytes() == b"\xa5" * (len(data) - 1)
    report = run(probe, source, source)
    assert report["error"] == errno.EINVAL and source.read_bytes() == data, report
    target.unlink()
    os.link(source, target)
    report = run(probe, source, target)
    assert report["error"] == errno.EINVAL and source.read_bytes() == data, report
    target.unlink()
    target.write_bytes(b"\xa5" * len(data))
    replacement = root / "replacement.img"
    replacement.write_bytes(b"\x17" * len(data))
    report = run(probe, source, target, "source-replace", replacement)
    assert report["error"] == errno.ESTALE and report["written"] == report["syncs"] == 0, report
    assert target.read_bytes() == b"\xa5" * len(data)
    source.write_bytes(b"")
    report = run(probe, source, target)
    assert report["error"] == errno.EINVAL and target.read_bytes() == b"\xa5" * len(data), report
    source.write_bytes(data)
    fifo = root / "fifo"
    os.mkfifo(fifo)
    report = run(probe, source, fifo, "fifo")
    assert report["error"] == errno.ENOTSUP and report["written"] == report["syncs"] == 0, report


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_raw_write.py PROBE")
    with tempfile.TemporaryDirectory(prefix="rufus-raw-write-") as temporary:
        root = Path(temporary)
        check_success(sys.argv[1], root)
        check_failures(sys.argv[1], root)
        check_preflight(sys.argv[1], root)
    print(f"{runs} native raw write/flush/verify cases passed; only disposable files used.")


if __name__ == "__main__":
    main()
