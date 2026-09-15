# SPDX-License-Identifier: GPL-3.0-or-later
"""Extract generated ISO/UDF trees and inspect disposable output independently."""
import argparse
import errno
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from test_udf_images import rename_udf_entry
from test_udf_embedded_efi import fragment_file

MIB = 1024 * 1024
runs = 0


def make_image(root, name, entries, mkisofs, udf=False, directories=(), special=None):
    tree = root / name
    tree.mkdir()
    for path, content in entries.items():
        file = tree / path
        file.parent.mkdir(parents=True, exist_ok=True)
        file.write_bytes(content)
    for path in directories:
        (tree / path).mkdir(parents=True, exist_ok=True)
    if special == "symlink":
        (tree / "link").symlink_to("data.bin")
    if special == "fifo":
        os.mkfifo(tree / "fifo")
    image = root / (name + ".iso")
    command = [mkisofs, "-quiet", "-R", "-J", "-input-charset", "utf-8", "-V", name]
    if udf:
        command.append("-udf")
    subprocess.run(command + ["-o", str(image), str(tree)], check=True, capture_output=True, timeout=15)
    return image


def extract(probe, image, target, view="rr", fault="none", error=0):
    global runs
    result = subprocess.run([str(probe), str(image), str(target), view, fault],
                            capture_output=True, timeout=20)
    runs += 1
    assert result.returncode == 0, (result.returncode, result.stderr)
    values = {key.decode(): int(value) for key, value in re.findall(rb"([A-Z_]+)=(-?\d+)", result.stdout)}
    assert values["ERROR"] == error, (fault, values, result.stderr)
    assert values["FDS"] == 0, (fault, values)
    assert values["WRITTEN"] == values["ACTUAL"], (fault, values)
    assert values["MAX_READ"] <= MIB and values["MAX_WRITE"] <= MIB, values
    if values["FLAGS"]:
        assert values["FLAGS"] & os.O_EXCL and values["FLAGS"] & os.O_NOFOLLOW, values
        assert values["FLAGS"] & os.O_CLOEXEC, values
    return values


def check_tree(target, entries, empty=()):
    expected_dirs = set(empty)
    for path in tuple(entries) + tuple(empty):
        expected_dirs.update(str(parent) for parent in Path(path).parents if str(parent) != ".")
    actual_dirs = set()
    actual_files = {}
    for path in target.rglob("*"):
        assert not path.is_symlink(), path
        relative = str(path.relative_to(target))
        if path.is_dir():
            actual_dirs.add(relative)
        else:
            actual_files[relative] = path.read_bytes()
    assert actual_files == entries, (list(actual_files), list(entries))
    assert actual_dirs == expected_dirs, (actual_dirs, expected_dirs)


def check_success(args, root):
    entries = {"MiXeD/ș-é.txt": b"UTF-8 names", "empty": b"", "one": b"x",
               "boot/data.bin": bytes(range(256)) * (MIB // 128) + b"tail",
               "ldlinux.sys": b"installer owns root loader", "boot/ldlinux.sys": b"keep nested loader",
               "syslinux.cfg": b"default linux\n", "space dir/name with spaces.txt": b"spaces"}
    expected = dict(entries)
    del expected["ldlinux.sys"]
    for udf in (False, True):
        image = make_image(root, "TREE" + str(int(udf)), entries, args.mkisofs, udf, ["empty-dir/nested"])
        original = image.read_bytes()
        for view in ("rr", "joliet"):
            target = root / f"output-{udf}-{view}"
            target.mkdir()
            report = extract(args.probe, image, target, view)
            check_tree(target, expected, ["empty-dir/nested"])
            assert report["FILES"] == len(expected) and report["SKIPPED"] == 1, report
            assert report["WRITTEN"] == sum(map(len, expected.values())), report
            assert report["DIRECTORIES"] == 5, report
            assert image.read_bytes() == original
        # UDF tree and content must stay on the same view, never silently fall back.
        if udf:
            rename_udf_entry(image, "data.bin", "diff.bin")
            target = root / "udf-different"
            target.mkdir()
            altered = dict(expected)
            altered["boot/diff.bin"] = altered.pop("boot/data.bin")
            extract(args.probe, image, target)
            check_tree(target, altered, ["empty-dir/nested"])
    image = make_image(root, "PLAIN", {"readme.txt": b"plain"}, args.mkisofs)
    target = root / "plain-output"
    target.mkdir()
    extract(args.probe, image, target, "plain")
    check_tree(target, {"readme.txt": b"plain"})
    image = make_image(root, "EMPTY", {}, args.mkisofs, directories=["empty"])
    target = root / "empty-output"
    target.mkdir()
    extract(args.probe, image, target)
    check_tree(target, {}, ["empty"])
    data = bytes((value * 31 + 7) % 256 for value in range(256)) * 320
    for long_ad in (False, True):
        image = make_image(root, "FRAGMENT" + str(int(long_ad)), {"data.bin": data}, args.mkisofs, True)
        fragment_file(image, data, long_ad)
        target = root / f"fragment-output-{long_ad}"
        target.mkdir()
        extract(args.probe, image, target)
        check_tree(target, {"data.bin": data})


def check_failures(args, root, udf):
    entries = {"data.bin": bytes(range(256)) * (MIB // 128) + b"tail"}
    original_image = make_image(root, "FAULTS", entries, args.mkisofs, udf)
    original = original_image.read_bytes()
    cases = [("cancel-before", errno.ECANCELED, 0), ("cancel-after-scan", errno.ECANCELED, 0),
             ("change-after-scan", errno.ESTALE, 0), ("replace-after-scan", errno.ESTALE, 0),
             ("open-fail", errno.EACCES, 0), ("zero-write", errno.EIO, 0),
             ("write-fail", errno.ENOSPC, 127), ("write-cancel", errno.ECANCELED, 127),
             ("read-fail", errno.EIO, MIB), ("read-cancel", errno.ECANCELED, 0),
             ("source-change", errno.ESTALE, MIB),
             ("sync-fail", errno.EIO, len(entries["data.bin"])),
             ("close-fail", errno.EIO, len(entries["data.bin"])),
             ("sync-cancel", errno.ECANCELED, len(entries["data.bin"]))]
    for fault, error, written in cases:
        image = root / "fault.iso"
        image.write_bytes(original)
        replacement = Path(str(image) + ".replacement")
        if fault == "replace-after-scan":
            replacement.write_bytes(original)
            shutil.copystat(image, replacement)
        target = root / fault
        target.mkdir()
        report = extract(args.probe, image, target, fault=fault, error=error)
        assert report["WRITTEN"] == written and report["FILES"] == 0, (fault, report)
        if fault == "write-cancel":
            assert report["RETRY"] == report["RETRY_FDS"] == 0, report
            check_tree(target, entries)
        else:
            assert not list(target.iterdir()), (fault, list(target.iterdir()))
    for fault in ("short", "eintr", "sync-eintr"):
        target = root / fault
        target.mkdir()
        extract(args.probe, original_image, target, fault=fault)
        check_tree(target, entries)
    target = root / "directory-sync-fail"
    target.mkdir()
    report = extract(args.probe, original_image, target, fault="directory-sync-fail", error=errno.EIO)
    assert report["FILES"] == 1, report
    check_tree(target, entries)  # Completed files remain, but the operation failed.
    target = root / "collision"
    target.mkdir()
    extract(args.probe, original_image, target, fault="collision", error=errno.EEXIST)
    check_tree(target, {"data.bin": b"existing"})


def check_preflight(args, root):
    image = make_image(root, "DIRECTORY", {"nested/file": b"data"}, args.mkisofs)
    target = root / "mkdir-fail"
    target.mkdir()
    extract(args.probe, image, target, fault="mkdir-fail", error=errno.EACCES)
    assert not list(target.iterdir())
    (target / "keep").write_bytes(b"keep me")
    extract(args.probe, image, target, error=errno.ENOTEMPTY)
    check_tree(target, {"keep": b"keep me"})
    link = root / "target-link"
    link.symlink_to(target, target_is_directory=True)
    extract(args.probe, image, link, error=errno.ENOTDIR)
    check_tree(target, {"keep": b"keep me"})
    outside = root / "outside"
    outside.mkdir()
    (outside / "sentinel").write_bytes(b"unchanged")
    target = root / "parent-link"
    target.mkdir()
    extract(args.probe, image, target, fault="parent-link", error=errno.ENOTDIR)
    check_tree(outside, {"sentinel": b"unchanged"})
    target = root / "nested-sync-fail"
    target.mkdir()
    extract(args.probe, image, target, fault="directory-sync-fail", error=errno.EIO)
    check_tree(target, {"nested/file": b"data"})
    for special in ("symlink", "fifo"):
        image = make_image(root, special.upper(), {"data.bin": b"data"}, args.mkisofs, special=special)
        target = root / (special + "-output")
        target.mkdir()
        extract(args.probe, image, target, error=errno.ENOTSUP)
        assert not list(target.iterdir())
    image = root / "invalid.iso"
    image.write_bytes(bytes(65536))
    target = root / "invalid-output"
    target.mkdir()
    extract(args.probe, image, target, error=errno.EIO)
    assert not list(target.iterdir())
    image = make_image(root, "TRUNCATED", {"data.bin": bytes(MIB)}, args.mkisofs)
    image.write_bytes(image.read_bytes()[:-2048])
    target = root / "truncated-output"
    target.mkdir()
    extract(args.probe, image, target, error=errno.ENOTSUP)
    assert not list(target.iterdir())
    deep = "/".join(f"level{i}" for i in range(10))
    image = make_image(root, "DEEP", {deep + "/data": b"data"}, args.mkisofs)
    target = root / "deep-output"
    target.mkdir()
    extract(args.probe, image, target, error=errno.ENOTSUP)
    assert not list(target.iterdir())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("probe", type=Path)
    parser.add_argument("--mkisofs", required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="rufus-iso-extract-") as temporary:
        root = Path(temporary)
        check_success(args, root)
        for udf in (False, True):
            faults = root / ("faults-udf" if udf else "faults-iso")
            faults.mkdir()
            check_failures(args, faults, udf)
        check_preflight(args, root)
    print(f"{runs} native ISO/UDF extraction cases passed; disposable directories only.")


if __name__ == "__main__":
    main()
