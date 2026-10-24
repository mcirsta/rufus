# SPDX-License-Identifier: GPL-3.0-or-later
"""FreeDOS startup publication on disposable directories; no host locale inference."""
import argparse
import errno
import hashlib
from pathlib import Path
import subprocess
import tempfile

import test_dos_locale as dos
from test_freedos_copy import PAYLOADS, values

SENTINEL = b"preserve existing payload"


def invoke(args, directory, fault="none", keyboard="ro", codepage=852, error=0, payloads=False):
    command = [args.probe, str(directory), fault, keyboard, str(codepage)]
    if payloads:
        command.append("--with-payloads")
    process = subprocess.run(command, capture_output=True, text=True, timeout=10)
    assert process.returncode == 0, (command, process.returncode, process.stdout, process.stderr)
    report = values(process.stdout)
    assert report["ERROR"] == error, (command, report, process.stderr)
    assert report["FDS"] == 0 and report["OFFSET"] == 1, report
    assert report["FLAGS"] == (0 if fault == "bad-fd" else 1), report
    assert report["PROGRESS"] == (5 if payloads else 0), report
    assert report["SETS"] == (2 if payloads else 0), report
    if error == 0:
        assert process.stdout.endswith(" PATH=\n"), process.stdout
        assert report["SYNC"] == 0, report
    return report


def reference(args, root, keyboard, codepage):
    path = root / "oracle"
    for name in dos.FILES:
        Path(str(path) + "\\" + name).unlink(missing_ok=True)
    dos.invoke(args.reference, path, keyboard, codepage, True)
    files = dos.read_files(path, reference=True)
    if codepage == 30003:
        # CP19c1's documented correction, already qualified against the oracle.
        files["AUTOEXEC.BAT"] = files["AUTOEXEC.BAT"].replace(
            b"\\locale\\ega12) > NUL\r\n", b"\\locale\\ega12.cpx) > NUL\r\n")
    return files


def prepared_directory(root, name):
    directory = root / name
    directory.mkdir()
    (directory / "LOCALE").mkdir()
    (directory / "KERNEL.SYS").write_bytes(SENTINEL)
    return directory


def check_bytes(directory, expected, report):
    actual = dos.read_files(directory)
    assert set(actual) <= set(expected), actual
    for name, data in actual.items():
        assert expected[name].startswith(data), (name, data)
    assert sum(len(data) for data in actual.values()) == report["BYTES"], report
    assert (directory / "KERNEL.SYS").read_bytes() == SENTINEL
    assert list((directory / "LOCALE").iterdir()) == []
    assert set(path.name for path in directory.iterdir()) == set(actual) | {"KERNEL.SYS", "LOCALE"}
    return actual


def check_selections(args, root, digest):
    # Expected fallbacks are explicit; the original oracle only receives known
    # DOS inputs. No Linux language/XKB name is treated as a detected keyboard.
    cases = [("us", 437, "us", 437), ("ro", 852, "ro", 852), ("fr", 850, "fr", 850),
             ("ro", 65001, "ro", 0), ("ur", 0, "ur", 0), ("ar", 0, "ar", 0),
             ("unknown", 850, "us", 850), ("-", 0, "us", 437), ("", 437, "us", 437),
             ("$(touch bad)", 437, "us", 437), ("dv", 437, "dv", 437),
             ("ro", 30003, "ro", 30003), ("gk", 737, "gk", 737)]
    for number, (keyboard, codepage, expected_kb, expected_cp) in enumerate(cases):
        directory = prepared_directory(root, f"selection-{number}")
        expected = reference(args, root, expected_kb, expected_cp)
        report = invoke(args, directory, keyboard=keyboard, codepage=codepage)
        assert check_bytes(directory, expected, report) == expected
        assert report["FILES"] == len(expected) and report["SYNCS"] == len(expected) + 1, report
        for name, data in sorted(expected.items()):
            digest.update(name.encode() + b"\0" + data)
    return len(cases)


def check_faults(args, root):
    expected = reference(args, root, "ro", 852)
    cases = [("short-io", 0, 2), ("write-eintr", 0, 2), ("read-eintr", 0, 2), ("sync-eintr", 0, 2),
             ("wrong-fs", errno.ENOTSUP, 0), ("opath", errno.EINVAL, 0), ("bad-fd", errno.EBADF, 0),
             ("stat-fail", errno.EACCES, 0), ("cancel-before", errno.ECANCELED, 0),
             ("cancel-partial", errno.ECANCELED, 0), ("cancel-file", errno.ECANCELED, 1),
             ("cancel-sync", errno.ECANCELED, 0), ("cancel-final", errno.ECANCELED, 2),
             ("open-fail", errno.EACCES, 0), ("write-fail", errno.ENOSPC, 0), ("write-zero", errno.EIO, 0),
             ("partial-fail", errno.ENOSPC, 0), ("partial-sync-fail", errno.ENOSPC, 0),
             ("read-fail", errno.EIO, 0), ("read-eof", errno.EIO, 0), ("read-corrupt", errno.EBADMSG, 0),
             ("close-fail", errno.EIO, 0), ("sync-1", errno.EIO, 0), ("sync-2", errno.EIO, 1),
             ("sync-3", errno.EIO, 2), ("stream-open-fail", errno.ENOMEM, 0),
             ("stream-close-fail", errno.ENOMEM, 0)]
    refusals = {"wrong-fs", "opath", "bad-fd", "stat-fail", "cancel-before"}
    for fault, error, files in cases:
        directory = prepared_directory(root, fault)
        report = invoke(args, directory, fault=fault, error=error)
        actual = check_bytes(directory, expected, report)
        assert report["FILES"] == files, (fault, report)
        if error == 0:
            assert actual == expected
            assert report["SYNCS"] == (4 if fault == "sync-eintr" else 3), report
        if fault in refusals:
            assert not actual and report["SYNCS"] == 0, (fault, report)
        else:
            assert report["SYNCS"] >= 1  # Root metadata is flushed after a started operation.
        if fault in ("partial-fail", "partial-sync-fail", "cancel-partial"):
            assert report["BYTES"] == 17, report
        if fault in ("partial-sync-fail", "sync-1", "sync-2", "sync-3"):
            assert report["SYNC"] == errno.EIO, report
        if files == 0:
            assert not (directory / "AUTOEXEC.BAT").exists()
    regular = root / "regular-file"
    regular.write_bytes(SENTINEL)
    report = invoke(args, regular, fault="not-directory", error=errno.ENOTDIR)
    assert regular.read_bytes() == SENTINEL and report["SYNCS"] == report["FILES"] == 0
    return len(cases) + 1


def check_existing(args, root):
    outside = root / "outside"
    outside.write_bytes(SENTINEL)
    count = 0
    for keyboard, codepage in (("ro", 852), ("us", 437)):
        for name in dos.FILES:
            for kind in ("file", "directory", "symlink", "dangling", "hardlink"):
                directory = prepared_directory(root, f"existing-{count}")
                path = directory / name
                if kind == "directory":
                    path.mkdir()
                elif kind == "symlink":
                    path.symlink_to(outside)
                elif kind == "dangling":
                    path.symlink_to(root / "missing")
                elif kind == "hardlink":
                    path.hardlink_to(outside)
                else:
                    path.write_bytes(SENTINEL)
                report = invoke(args, directory, keyboard=keyboard, codepage=codepage, error=errno.EEXIST)
                assert report["FILES"] == report["BYTES"] == report["SYNCS"] == 0, report
                assert set(p.name for p in directory.iterdir()) == {name, "KERNEL.SYS", "LOCALE"}
                if kind in ("file", "symlink", "hardlink"):
                    assert path.read_bytes() == SENTINEL
                elif kind == "directory":
                    assert not list(path.iterdir())
                else:
                    assert path.is_symlink() and not path.exists()
                count += 1
    assert outside.read_bytes() == SENTINEL and not (root / "missing").exists()
    directory = prepared_directory(root, "raced-file")
    report = invoke(args, directory, fault="race-file", error=errno.EEXIST)
    assert report["FILES"] == report["BYTES"] == 0 and report["SYNCS"] == 1, report
    assert (directory / "CONFIG.SYS").read_bytes() == b"preserve raced file"
    assert not (directory / "AUTOEXEC.BAT").exists()
    return count + 1


def check_combined(args, root):
    for keyboard, codepage in (("ro", 852), ("us", 437)):
        directory = root / ("combined-" + keyboard)
        directory.mkdir()
        expected = reference(args, root, keyboard, codepage)
        report = invoke(args, directory, keyboard=keyboard, codepage=codepage, payloads=True)
        actual = {str(p.relative_to(directory)): p.read_bytes() for p in directory.rglob("*") if p.is_file()}
        assert actual == PAYLOADS | expected
        assert report["PAYLOADS"] == 27 and report["FILES"] == len(expected), report
        assert report["BYTES"] == sum(len(data) for data in expected.values()), report
        assert report["SYNCS"] == 29 + len(expected) + 1, report
        assert [report[f"P{i}"] for i in range(5)] == [5, 11, 17, 23, 27], report
    return 2


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("probe")
    parser.add_argument("reference")
    args = parser.parse_args()
    digest = hashlib.sha256()
    with tempfile.TemporaryDirectory(prefix="rufus-freedos-startup-") as temporary:
        root = Path(temporary)
        count = check_selections(args, root, digest)
        count += check_faults(args, root)
        count += check_existing(args, root)
        count += check_combined(args, root)
    print(f"{count} FreeDOS startup cases passed: 13 frozen-oracle selections, "
          "faults/refusals and two payload-plus-startup workflows. "
          f"Output SHA256={digest.hexdigest()}. Temporary outputs removed.")


if __name__ == "__main__":
    main()
