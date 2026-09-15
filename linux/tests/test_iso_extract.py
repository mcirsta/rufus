# SPDX-License-Identifier: GPL-3.0-or-later
"""Extract generated ISO/UDF trees and inspect disposable output independently."""
import argparse
import errno
import os
from pathlib import Path
import re
import shutil
import stat
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
        assert values["FLAGS"] & os.O_NOFOLLOW, values
        assert values["FLAGS"] & os.O_CLOEXEC, values
        assert not values["FLAGS"] & os.O_TRUNC, values
        if values["FLAGS"] & os.O_CREAT:
            assert values["FLAGS"] & os.O_EXCL, values
        else:
            assert values["FLAGS"] & os.O_NONBLOCK, values
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
    report = extract(args.probe, original_image, target, fault="collision")
    assert report["REPLACEMENTS"] == report["TRUNCATIONS"] == 1, report
    check_tree(target, entries)


def make_named_image(root, name, mkisofs, udf=False):
    data = bytes(range(256)) * (MIB // 128) + b"tail"
    entries = {"*root?<>:|.txt": b"all six characters", "MiX:dir/sub?dir/ș-é.txt": data,
               "MiX:dir/sub?dir/ldlinux.sys": b"nested loader keeps its role"}
    expected = {"_root_____.txt": b"all six characters", "MiX_dir/sub_dir/ș-é.txt": data,
                "MiX_dir/sub_dir/ldlinux.sys": b"nested loader keeps its role"}
    empty_dirs = ["empty_dir/nested_empty"]
    if udf:
        # mkisofs rewrites these characters in UDF names itself. Restore the
        # original names in valid FIDs, retaining their CRCs and placement.
        image = make_image(root, name, expected, mkisofs, True, empty_dirs)
        for old, new in [("_root_____.txt", "*root?<>:|.txt"), ("MiX_dir", "MiX:dir"),
                         ("sub_dir", "sub?dir"), ("empty_dir", "empty:dir"),
                         ("nested_empty", "nested?empty")]:
            rename_udf_entry(image, old, new)
    else:
        image = make_image(root, name, entries, mkisofs, directories=["empty:dir/nested?empty"])
    return image, expected, empty_dirs


def check_sanitized_names(args, root, udf):
    image, expected, empty_dirs = make_named_image(root, "NAMES", args.mkisofs, udf)
    original = image.read_bytes()
    target = root / "renamed-output"
    target.mkdir()
    report = extract(args.probe, image, target)
    check_tree(target, expected, empty_dirs)
    assert report["FILES"] == len(expected) and report["DIRECTORIES"] == 4, report
    assert image.read_bytes() == original

    # Fail after creating the sanitized name, then verify that exact file was
    # removed. A caller-owned output path containing ':' must stay untouched.
    data = bytes(range(256)) * (MIB // 128) + b"tail"
    source_name = "bad_name.txt" if udf else "bad:name.txt"
    image = make_image(root, "RENAMEDFAULT", {source_name: data}, args.mkisofs, udf)
    if udf:
        rename_udf_entry(image, source_name, "bad:name.txt")
    original = image.read_bytes()
    cases = [("write-fail", errno.ENOSPC), ("sync-fail", errno.EIO),
             ("close-fail", errno.EIO), ("write-cancel", errno.ECANCELED),
             ("collision", 0)]
    for fault, error in cases:
        target = root / ("output:" + fault)
        target.mkdir()
        report = extract(args.probe, image, target, fault=fault, error=error)
        if fault == "write-cancel":
            assert report["RETRY"] == report["RETRY_FDS"] == 0, report
            check_tree(target, {"bad_name.txt": data})
        elif fault == "collision":
            check_tree(target, {"bad_name.txt": data})
        else:
            check_tree(target, {})
        assert image.read_bytes() == original

    # Distinct image names can map to one output name even on a case-sensitive
    # host filesystem. The later source entry replaces the earlier one.
    if udf:
        collision_entries = {"first000.txt": b"first", "second00.txt": b"second"}
    else:
        collision_entries = {"bad:name.txt": b"first", "bad?name.txt": b"second"}
    image = make_image(root, "NAMECLASH", collision_entries, args.mkisofs, udf)
    if udf:
        rename_udf_entry(image, "first000.txt", "bad:name.txt")
        rename_udf_entry(image, "second00.txt", "bad?name.txt")
    target = root / "name-clash"
    target.mkdir()
    report = extract(args.probe, image, target)
    assert report["FILES"] == 2 and report["WRITTEN"] == len(b"firstsecond"), report
    assert report["REPLACEMENTS"] == report["TRUNCATIONS"] == 1, report
    check_tree(target, {"bad_name.txt": b"second"})


def make_merged_image(root, name, mkisofs, udf=False):
    entries = {"part:dir/only-a.txt": b"keep a", "part:dir/same.txt": b"old content with a tail",
               "part?dir/only-b.txt": b"keep b", "part?dir/same.txt": b"new"}
    directories = ["part:dir/empty", "part?dir/empty"]
    if udf:
        # Avoid mkisofs merging the UDF names before the test sees them.
        entries = {path.replace("part:dir", "part0dir").replace("part?dir", "part1dir"): data
                   for path, data in entries.items()}
        directories = ["part0dir/empty", "part1dir/empty"]
    image = make_image(root, name, entries, mkisofs, udf, directories)
    if udf:
        rename_udf_entry(image, "part0dir", "part:dir")
        rename_udf_entry(image, "part1dir", "part?dir")
    expected = {"part_dir/only-a.txt": b"keep a", "part_dir/only-b.txt": b"keep b",
                "part_dir/same.txt": b"new"}
    return image, expected


def check_replacements(args, root, udf):
    for name, content in (("SHORT", b"x"), ("ZERO", b"")):
        image = make_image(root, name, {"data.bin": content}, args.mkisofs, udf)
        target = root / (name + "-output")
        target.mkdir()
        report = extract(args.probe, image, target, fault="collision")
        assert report["REPLACEMENTS"] == report["TRUNCATIONS"] == 1, report
        check_tree(target, {"data.bin": content})
    image, expected = make_merged_image(root, "MERGED", args.mkisofs, udf)
    original = image.read_bytes()
    target = root / "merged-output"
    target.mkdir()
    report = extract(args.probe, image, target)
    assert report["FILES"] == 4 and report["DIRECTORIES"] == 4, report
    assert report["REPLACEMENTS"] == report["TRUNCATIONS"] == 1, report
    check_tree(target, expected, ["part_dir/empty"])
    assert image.read_bytes() == original

    image = make_image(root, "REPLACEFAIL", {"data.bin": bytes(MIB)}, args.mkisofs, udf)
    cases = [("replace-truncate-fail", errno.EIO, True), ("replace-cancel", errno.ECANCELED, True),
             ("replace-write-fail", errno.ENOSPC, False), ("replace-sync-fail", errno.EIO, False)]
    for fault, error, preserved in cases:
        target = root / fault
        target.mkdir()
        report = extract(args.probe, image, target, fault=fault, error=error)
        assert report["FILES"] == 0, report
        assert report["TRUNCATIONS"] == (0 if fault == "replace-cancel" else 1), report
        check_tree(target, {"data.bin": b"existing"} if preserved else {})


def check_target_types(args, root):
    outside = root / "outside"
    outside.mkdir()
    sentinel = outside / "sentinel"
    sentinel.write_bytes(b"outside output tree")
    image = make_image(root, "TARGETTYPE", {"data.bin": b"new"}, args.mkisofs)
    cases = [("target-symlink", errno.ELOOP), ("target-hardlink", errno.EMLINK),
             ("target-fifo", errno.ENXIO), ("target-directory", errno.EISDIR)]
    for fault, error in cases:
        target = root / fault
        target.mkdir()
        report = extract(args.probe, image, target, fault=fault, error=error)
        assert report["TRUNCATIONS"] == report["WRITTEN"] == 0, report
        assert sentinel.read_bytes() == b"outside output tree"
        output = target / "data.bin"
        if fault == "target-symlink":
            assert output.is_symlink()
        elif fault == "target-hardlink":
            assert output.samefile(sentinel)
        elif fault == "target-fifo":
            assert stat.S_ISFIFO(output.lstat().st_mode)
        else:
            assert output.is_dir() and not list(output.iterdir())
    image = make_image(root, "DIRECTORYTYPE", {}, args.mkisofs, directories=["node"])
    for fault in ("directory-file", "directory-link"):
        target = root / fault
        target.mkdir()
        report = extract(args.probe, image, target, fault=fault, error=errno.ENOTDIR)
        assert report["DIRECTORIES"] == report["WRITTEN"] == 0, report
        if fault == "directory-file":
            check_tree(target, {"node": b"existing"})
        else:
            assert (target / "node").is_symlink()
        check_tree(outside, {"sentinel": b"outside output tree"})


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
            names = root / ("names-udf" if udf else "names-iso")
            names.mkdir()
            check_sanitized_names(args, names, udf)
            check_replacements(args, names, udf)
        types = root / "target-types"
        types.mkdir()
        check_target_types(args, types)
        check_preflight(args, root)
    print(f"{runs} native ISO/UDF extraction cases passed; disposable directories only.")


if __name__ == "__main__":
    main()
