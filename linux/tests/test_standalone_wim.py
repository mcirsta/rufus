# SPDX-License-Identifier: GPL-3.0-or-later
"""Qualify standalone WIM candidates and retained native files without devices."""
import argparse
import errno
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from test_udf_images import make_image
from test_wim_extract import pe_bytes

runs = 0
SIGNATURE = b"MSWIM\0\0\0"


def probe(args, path, mode="none", replacement=None):
    global runs
    command = [args.probe, path, mode]
    if replacement is not None:
        command.append(replacement)
    result = subprocess.run([str(value) for value in command], capture_output=True, timeout=15)
    runs += 1
    assert result.returncode == 0, (command, result.returncode, result.stderr)
    report = json.loads(result.stdout)
    assert report["opened"] <= 1, report
    if not report["source_current"]:
        assert report["current_arch"] == 0, report
    return report


def current_candidate(report, candidate=True):
    # Current source does not imply a valid archive, known version or bootability.
    assert report["source_error"] == report["check_error"] == 0, report
    assert report["source_current"] == 1, report
    assert report["is_wim"] == report["opened"] == int(candidate), report


def capture(args, root, output, mode="none", second=None):
    command = [args.capture, root, output, mode]
    if second is not None:
        command.append(second)
    subprocess.run([str(value) for value in command], check=True, capture_output=True, timeout=30)


def check_classification(args, root):
    wim = root / "metadata.wim"
    for kind, major, valid, invalid in (("win7", 7, 15, 0), ("win11", 11, 15, 0),
                                       ("unknown", 0, 0, 0), ("invalid", 0, 14, 1),
                                       ("no-revision", 11, 7, 0), ("multi", 7, 15, 0)):
        subprocess.run([str(args.fixture), str(wim), kind], check=True, capture_output=True)
        report = probe(args, wim)
        current_candidate(report)
        assert report["wim_error"] == report["metadata_error"] == 0, report
        assert report["major"] == major and report["valid_fields"] == valid, report
        assert report["invalid_fields"] == invalid, report
        assert report["efi_error"] != 0 and report["observed_arch"] == 0, report
        assert report["image_count"] == (2 if kind == "multi" else 1), report
        assert report["boot_index"] == (2 if kind == "multi" else 0), report
    data = wim.read_bytes()
    for name in ("image.wim", "image.esd", "image.bin", "image", "ș-é image.ISO"):
        path = root / name
        path.write_bytes(data)
        report = probe(args, path)
        current_candidate(report)
        assert report["major"] == 7, report
    for size in range(9):
        wim.write_bytes(SIGNATURE[:size])
        report = probe(args, wim)
        current_candidate(report, size == 8)
        if size == 8:
            assert report["wim_error"] != 0 and report["archive_version"] == 0, report
    for data in (b"a file called install.wim", b"MSWIM\0\0x", SIGNATURE + bytes(200)):
        wim.write_bytes(data)
        report = probe(args, wim)
        current_candidate(report, data.startswith(SIGNATURE))
        assert report["major"] == report["observed_arch"] == 0, report
        if report["is_wim"]:
            assert report["wim_error"] != 0, report
    for mode in ("pipable", "solid", "integrity"):
        subprocess.run([str(args.fixture), str(wim), "win11", mode],
                       check=True, capture_output=True)
        report = probe(args, wim)
        current_candidate(report, mode != "pipable")
        assert wim.read_bytes().startswith(SIGNATURE) == (mode != "pipable")
        if mode != "pipable":
            assert report["wim_error"] == 0 and report["major"] == 11, report
    image = make_image(root, "OUTER_ISO", {"/Sources/install.wim": wim.read_bytes()}, args.mkisofs)
    current_candidate(probe(args, image), False)


def check_content(args, root):
    first, second = root / "first", root / "second"
    # Lookup uses Windows casing; the archive's spelling controls output naming.
    for tree, machine in ((first, 0x8664), (second, 0xaa64)):
        efi = tree / "windows/boot/efi/BOOTMGR.EFI"
        efi.parent.mkdir(parents=True)
        efi.write_bytes(pe_bytes(machine))
    for mode in ("none", "lzx", "xpress", "solid", "integrity"):
        path = root / (mode + ".esd")
        capture(args, first, path, mode, second)
        report = probe(args, path)
        current_candidate(report)
        assert report["wim_error"] == report["metadata_error"] == report["efi_error"] == 0, report
        assert report["image_count"] == report["boot_index"] == 2, report
        assert report["observed_arch"] == report["current_arch"] == 2, report
        assert report["major"] == report["valid_fields"] == report["invalid_fields"] == 0, report
    alternate = root / "alternate.wim"
    capture(args, second, alternate, second=first)
    report = probe(args, alternate)
    assert report["current_arch"] == 4 and report["boot_index"] == 2, report
    for name, contents in (("missing", None), ("unknown-pe", b"MZ")):
        tree = root / name
        efi = tree / "Windows/Boot/EFI/bootmgr.efi"
        efi.parent.mkdir(parents=True)
        if contents is not None:
            efi.write_bytes(contents)
        path = root / (name + ".wim")
        capture(args, tree, path)
        report = probe(args, path)
        current_candidate(report)
        assert report["wim_error"] == report["metadata_error"] == 0, report
        assert (report["efi_error"] != 0) == (contents is None), report
        assert report["observed_arch"] == report["current_arch"] == 0, report
    return root / "none.esd", alternate


def check_ownership(args, root, original, alternate):
    assert original.stat().st_size == alternate.stat().st_size
    path, replacement = root / "selected.wim", root / "replacement.wim"
    modes = ("replace-before-open", "replace-after-open", "replace-after-read",
             "unlink-before-open", "change-after-read", "truncate-after-read",
             "cancel-before", "cancel-after-read")
    for mode in modes:
        shutil.copyfile(original, path)
        shutil.copyfile(alternate, replacement)
        report = probe(args, path, mode, replacement if mode.startswith("replace-") else None)
        assert report["source_current"] == report["current_arch"] == 0, (mode, report)
        if mode == "cancel-before":
            assert report["source_error"] == errno.ECANCELED and report["opened"] == 0, report
        else:
            assert report["source_error"] == 0 and report["opened"] == 1, (mode, report)
            if mode in ("replace-before-open", "unlink-before-open"):
                # Upstream realpath() rejects an already-unlinked /proc/self/fd target.
                assert report["wim_error"] == 45 and report["observed_arch"] == 0, (mode, report)
                assert report["archive_version"] == report["valid_fields"] == 0, report
            else:
                assert report["wim_error"] == report["efi_error"] == 0, (mode, report)
                assert report["observed_arch"] == 2, (mode, report)
            assert report["check_error"] != 0, (mode, report)
            if mode == "cancel-after-read":
                assert report["check_error"] == errno.ECANCELED, report
    alias = root / "alias.esd"
    alias.symlink_to(original)
    report = probe(args, alias)
    current_candidate(report)
    assert report["current_arch"] == 2, report
    fifo = root / "fifo.wim"
    os.mkfifo(fifo)
    for path, error in ((fifo, errno.EINVAL), (root, errno.EINVAL), (root / "absent", errno.ENOENT)):
        report = probe(args, path)
        assert report["source_error"] == error and report["opened"] == 0, report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("probe", type=Path)
    parser.add_argument("fixture", type=Path)
    parser.add_argument("capture", type=Path)
    parser.add_argument("--mkisofs", required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="rufus-standalone-wim-") as temporary:
        root = Path(temporary)
        check_classification(args, root)
        original, alternate = check_content(args, root)
        check_ownership(args, root, original, alternate)
    print(f"{runs} standalone WIM classification, content and ownership probes passed.")


if __name__ == "__main__":
    main()
