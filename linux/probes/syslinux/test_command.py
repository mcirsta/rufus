# SPDX-License-Identifier: GPL-3.0-or-later
"""Supervise native Syslinux and faulty installers using disposable files only."""
import argparse
import errno
import os
from pathlib import Path
import shutil
import signal
import sys
import tempfile

from verify import check_files, check_volume, run, MIB, SECTOR_BYTES


def tools_directory(directory):
    directory.mkdir()
    for name in ("mcopy", "mattrib"):
        source = shutil.which(name)
        assert source, f"Missing {name}"
        (directory / name).symlink_to(source)


def invoke(probe, installer, image, tools, directory, mode="normal", offset=0, error=0, started=True):
    directory.mkdir(mode=0o700)
    env = os.environ.copy()
    env.update({"RUFUS_POISON": "must not reach tool", "PYTHONPATH": "/nonexistent/rufus-poison",
                "MTOOLSRC": "/nonexistent/rufus-poison", "PATH": "/nonexistent/rufus-poison"})
    command = [probe, installer, image, tools, directory, mode, str(offset)]
    output = run(command, env, timeout=20)
    fields = dict(item.split("=", 1) for item in output.decode().split())
    result = {name: int(value) for name, value in fields.items()}
    assert result["ERROR"] == error, result
    assert result["STARTED"] == started, result
    assert result["SYNCS"] == (2 if mode == "sync-eintr" else int(started)), result
    assert result["SYNC_CHILDREN"] == 0, "Sync ran before all writers were reaped"
    assert result["FDS"] == 0 and result["FLAGS"] == 1 and result["OFFSET"] == 17, result
    assert result["CHILD"] == -1 and result["CHILD_ERRNO"] == errno.ECHILD, result
    assert result["UNRELATED"] == 1, "Runner consumed another child's exit status"
    assert result["SYNC"] == (errno.EIO if mode == "sync-fail" else 0), result
    pid_file = directory / "pids"
    if pid_file.exists():
        for pid in pid_file.read_text().split():
            assert not Path(f"/proc/{pid}").exists(), f"Surviving writer {pid}"
    print(f"PASS supervised {mode}: error={error}; children gone before sync", flush=True)
    return result


def check_faults(args, root, tools):
    fake = root / "fault installer"
    source = Path(__file__).with_name("child.py").read_text().split("\n", 1)[1]
    fake.write_text(f"#!{sys.executable}\n" + source)
    fake.chmod(0o700)
    for mode, error, started in (
        ("normal", 0, True), ("replace-path", 0, True), ("unrelated", 0, True),
        ("failure", errno.EIO, True), ("signal", errno.EIO, True),
        ("orphan", errno.EIO, True), ("cancel-tree", errno.ECANCELED, True),
        ("cancel-closed", errno.ECANCELED, True), ("cancel-noisy", errno.ECANCELED, True),
        ("timeout", errno.ETIMEDOUT, True), ("clock-fail", errno.EIO, True),
        ("cancel-before", errno.ECANCELED, False), ("no-subreaper", errno.EINVAL, False),
        ("ignore-children", errno.EINVAL, False), ("auto-reap", errno.EINVAL, False),
        ("no-environment", errno.EINVAL, False), ("no-directory", errno.EINVAL, False),
        ("relative-program", errno.EINVAL, False),
        ("missing-executable", errno.ENOENT, False), ("sync-fail", errno.EIO, True),
        ("sync-eintr", 0, True),
    ):
        image = root / "fault.img"
        image.write_bytes(bytes(2048))
        program = root / "absent" if mode == "missing-executable" else fake
        directory = root / mode
        result = invoke(args.command, program, image, tools, directory, mode, error=error, started=started)
        if not started:
            assert image.read_bytes() == bytes(2048), "Rejected call wrote target"
        if mode == "replace-path":
            assert image.read_bytes() == b"untouched"
            assert image.with_suffix(".img.held").read_bytes().startswith(b"installed\n")
            image.with_suffix(".img.held").unlink()
        if mode in ("cancel-tree", "cancel-closed"):
            assert result["SIGNAL"] == signal.SIGKILL and result["MS"] >= 650, result
        if mode == "cancel-noisy":
            assert result["DIAG"] == 4095, result
        if mode == "failure":
            assert result["EXIT"] == 19, result
        if mode == "signal":
            assert result["SIGNAL"] == signal.SIGUSR1, result
        if mode == "orphan":
            assert result["EXIT"] == 0, "Fault must exercise successful parent with a surviving writer"
        image.unlink()
        shutil.rmtree(directory)
    return fake


def check_native(args, root, tools, fake):
    env = os.environ.copy()
    env.update({"LC_ALL": "C", "MTOOLSRC": "/dev/null"})
    for mode, offset in (("normal", 0), ("replace-path", MIB), ("cancel-native", 0)):
        case = root / f"native-{mode}"
        case.mkdir()
        image = case / "FAT ș '$ image.img"
        size = 64 * MIB
        guard = b"guard" * 64
        with image.open("wb") as stream:
            stream.truncate(offset + size)
            stream.write(guard if offset else b"")
        run(["mkfs.fat", "-F", "32", "-S", str(SECTOR_BYTES), "-s", "1",
             "-h", str(offset // SECTOR_BYTES), f"--offset={offset // SECTOR_BYTES}",
             image, str(size // 1024)], env)
        with image.open("r+b") as stream:
            prefix = stream.read(offset)
            before = stream.read(SECTOR_BYTES)
            stream.seek(offset + size)
            stream.write(guard)
        selected_tools = tools
        if mode == "cancel-native":
            selected_tools = case / "tools"
            tools_directory(selected_tools)
            (selected_tools / "mcopy").unlink()
            (selected_tools / "mcopy").symlink_to(fake)
        temporary = case / "private temp ș '$"
        error = errno.ECANCELED if mode == "cancel-native" else 0
        invoke(args.command, args.installer, image, selected_tools, temporary, mode, offset, error)
        if mode == "replace-path":
            assert image.read_bytes() == b"untouched"
            image = Path(str(image) + ".held")
        if mode != "cancel-native":
            if args.major == 6:
                module = args.upstream / "bios/com32/elflink/ldlinux/ldlinux.c32"
                check_files(f"{image}@@{offset}", case, env, {"/ldlinux.c32": module})
            check_volume(image, case, offset, size, before, env)
            assert not list(temporary.glob("syslinux-mtools-*"))
        else:
            # The killed upstream process leaves MTOOLSRC in its private directory.
            # The worker owner removes it only after the complete group has exited.
            assert list(temporary.glob("syslinux-mtools-*")), "Fault did not reach native mtools"
        with image.open("rb") as stream:
            assert stream.read(offset) == prefix
            stream.seek(offset + size)
            assert stream.read() == guard
        shutil.rmtree(case)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", type=Path)
    parser.add_argument("installer", type=Path)
    parser.add_argument("upstream", type=Path)
    parser.add_argument("--major", type=int, choices=(4, 6), default=6)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="rufus-syslinux-command-") as temporary:
        root = Path(temporary)
        tools = root / "tools"
        tools_directory(tools)
        fake = check_faults(args, root, tools)
        check_native(args, root, tools, fake)
    print("21 process cases and 3 native cases passed; all disposable files removed.", flush=True)


if __name__ == "__main__":
    main()
