# SPDX-License-Identifier: GPL-3.0-or-later
"""Native FreeDOS copy faults on disposable directories; FAT ioctls simulated here."""
import argparse
import errno
import hashlib
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
NAMES = ["COMMAND.COM", "KERNEL.SYS", "DISPLAY.EXE", "KEYB.EXE", "MODE.COM",
         "KEYBOARD.SYS", "KEYBRD2.SYS", "KEYBRD3.SYS", "KEYBRD4.SYS", "EGA.CPX"]
NAMES += [f"EGA{number}.CPX" for number in range(2, 19)]
PAYLOADS = {name if index < 2 else "LOCALE/" + name: (ROOT / "res/freedos" / name).read_bytes()
            for index, name in enumerate(NAMES)}
TOTAL = sum(len(data) for data in PAYLOADS.values())


def values(output):
    return {key: int(value) for key, value in re.findall(r"([A-Z_0-9]+)=(-?\d+)", output)}


def check_manifest():
    manifest = (ROOT / "src/freedos_files.inc").read_text()
    entries = re.findall(r'FREEDOS_FILE_ENTRY\("([A-Z0-9.]+)", (IDR_FD_\w+), (true|false), (true|false)\)', manifest)
    assert [entry[0] for entry in entries] == NAMES
    resources = (ROOT / "src/rufus.rc").read_text(encoding="latin1")
    active = dict(re.findall(r'^\s*(IDR_FD_\w+)\s+RCDATA\s+"../res/freedos/([A-Z0-9.]+)"', resources, re.M))
    assert len(active) == len(entries) == 27 and TOTAL == 400604
    for i, (name, resource, root, progress) in enumerate(entries):
        assert active[resource] == name
        assert root == str(i < 2).lower() and progress == str(i in (4, 10, 16, 22, 26)).lower()
    assert PAYLOADS["KERNEL.SYS"][13] == 1


def invoke(probe, directory, fault, error, files=None):
    process = subprocess.run([probe, directory, fault], capture_output=True, text=True, timeout=10)
    assert process.returncode == 0, (fault, process.returncode, process.stdout, process.stderr)
    report = values(process.stdout)
    assert report["ERROR"] == error, (fault, report, process.stderr)
    assert report["FDS"] == 0 and report["FLAGS"] == 1 and report["OFFSET"] == 1, report
    if files is not None:
        assert report["FILES"] == files, (fault, report)
    if error == 0:
        assert report["FILES"] == 27 and report["BYTES"] == TOTAL and report["SETS"] == 2, report
        assert report["PROGRESS"] == 5 and [report[f"P{i}"] for i in range(5)] == [5, 11, 17, 23, 27]
        assert report["SYNCS"] == (30 if fault == "sync-eintr" else 29) and report["SYNC"] == 0
        assert process.stdout.endswith(" PATH=\n")
    return report


def check_output(directory, complete):
    files = {str(path.relative_to(directory)): path.read_bytes() for path in directory.rglob("*") if path.is_file()}
    if complete:
        assert files == PAYLOADS
    else:
        assert set(files) <= set(PAYLOADS)
        for name, data in files.items():
            assert PAYLOADS[name].startswith(data), name
    return sum(len(data) for data in files.values())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("probe")
    args = parser.parse_args()
    check_manifest()
    cases = [("none", 0, 27), ("short-io", 0, 27), ("write-eintr", 0, 27), ("read-eintr", 0, 27),
             ("sync-eintr", 0, 27), ("wrong-fs", errno.ENOTSUP, 0), ("opath", errno.EINVAL, 0),
             ("cancel-before", errno.ECANCELED, 0), ("cancel-partial", errno.ECANCELED, 0),
             ("cancel-file", errno.ECANCELED, 1), ("cancel-sync", errno.ECANCELED, 0),
             ("open-fail", errno.EACCES, 0), ("write-fail", errno.ENOSPC, 0), ("write-zero", errno.EIO, 0),
             ("partial-fail", errno.ENOSPC, 0), ("partial-sync-fail", errno.ENOSPC, 0),
             ("read-fail", errno.EIO, 0), ("read-eof", errno.EIO, 0), ("read-corrupt", errno.EBADMSG, 0),
             ("attribute-get-fail", errno.EIO, 0), ("attribute-set-fail", errno.EACCES, 0),
             ("attribute-mismatch", errno.EBADMSG, 0), ("close-fail", errno.EIO, 0),
             ("sync-1", errno.EIO, 0), ("sync-2", errno.EIO, 1), ("sync-28", errno.EIO, 27), ("sync-29", errno.EIO, 27)]
    with tempfile.TemporaryDirectory(prefix="rufus-freedos-copy-") as temporary:
        root = Path(temporary)
        for fault, error, files in cases:
            directory = root / fault
            directory.mkdir()
            report = invoke(args.probe, directory, fault, error, files)
            assert check_output(directory, error == 0) == report["BYTES"], (fault, report)
            if fault in ("cancel-before", "wrong-fs", "opath"):
                assert not list(directory.iterdir()) and report["SYNCS"] == 0
            else:
                assert report["SYNCS"] >= 2  # Both directories flushed after any started copy.
            if fault in ("partial-fail", "partial-sync-fail", "cancel-partial"):
                assert report["BYTES"] == 17
            if fault in ("partial-sync-fail", "sync-1", "sync-2", "sync-28", "sync-29"):
                assert report["SYNC"] == errno.EIO
        outside = root / "outside"
        outside.write_bytes(b"preserve outside content")
        for kind in ("file", "directory", "symlink", "hidden"):
            directory = root / ("nonempty-" + kind)
            directory.mkdir()
            entry = directory / (".hidden" if kind == "hidden" else "LOCALE")
            if kind == "directory":
                entry.mkdir()
            elif kind == "symlink":
                entry.symlink_to(outside)
            else:
                entry.write_bytes(b"preserve existing content")
            report = invoke(args.probe, directory, "none", errno.ENOTEMPTY, 0)
            assert report["SYNCS"] == report["BYTES"] == 0 and len(list(directory.iterdir())) == 1
        assert outside.read_bytes() == b"preserve outside content"
    digest = hashlib.sha256(b"".join(PAYLOADS.values())).hexdigest()
    print(f"{len(cases) + 4} FreeDOS copy cases passed; 27 shared payloads/{TOTAL} bytes, "
          f"five original progress slots. Payload SHA256={digest}. Temporary outputs removed.")


if __name__ == "__main__":
    main()
