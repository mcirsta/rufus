# SPDX-License-Identifier: GPL-3.0-or-later
"""Qualify native NTFS formatting on partition-sized regular files only."""
import argparse
import errno
import os
from pathlib import Path
import re
import signal
import struct
import subprocess
import tempfile

from test_fat_format import fake_formatter, copy_volume

MIB = 1024 * 1024
SECTOR = 512
DEFAULT_SIZE = 64 * MIB
ENVIRONMENT = dict(os.environ, LC_ALL="C.UTF-8")
runs = 0


def readonly_checker(program):
    if not program or program.endswith('-NOTFOUND'):
        return None
    help_result = subprocess.run([program, '--help'], env=ENVIRONMENT, capture_output=True, timeout=15)
    help_text = help_result.stdout + help_result.stderr
    # NTFS-3G's older ntfsck accepts only a device argument and silently
    # returns a usage error for -n. Require a documented no-repair option.
    readonly_option = re.search(rb'(?m)^\s*-n(?:,\s*--[^\s,]+)?[ \t]+[^\r\n]*(?:no fix|no repair|read.only|no.action)', help_text, re.IGNORECASE)
    if readonly_option is None:
        print('Optional ntfsck has no supported -n read-only mode; retaining independent NTFS readbacks.', flush=True)
        return None
    return program


def make_target(root, name, size=DEFAULT_SIZE):
    path = root / name
    with path.open("wb") as volume:
        volume.truncate(size)
    return path


def invoke(args, image, size=DEFAULT_SIZE, offset=MIB, cluster=4096, label="Rufus test",
           fault="none", error=0, program=None, started=True):
    global runs
    command = [str(args.probe), program or args.mkfs, str(image), str(offset), str(size), str(cluster), label, fault]
    # The runner must supply UTF-8 to mkntfs even when its caller has a C locale.
    process = subprocess.run(command, env=dict(os.environ, LC_ALL="C"), capture_output=True, timeout=15)
    runs += 1
    assert process.returncode == 0, (process.returncode, process.stdout, process.stderr)
    values = {key.decode(): int(value) for key, value in re.findall(rb"([A-Z_]+)=(-?\d+)", process.stdout)}
    assert values["ERROR"] == error and values["STARTED"] == started, (fault, values, process.stderr)
    assert values["FDS"] == 0 and values["FLAGS"] == 1, values
    assert values["LOCALE"] == 1, values
    assert values["OFFSET"] == (-1 if fault == "pipe" else 17), values
    assert values["CHILD"] == -1 and values["CHILD_ERRNO"] == errno.ECHILD, values
    assert values["DIAG"] <= 4095 and values["SYNCS"] >= int(started), values
    if error == 0:
        assert values["EXIT"] == 0 and values["BYTES"] == size, values
    return values


def inspect(args, image, size, offset, cluster, label, report):
    assert image.stat().st_size == size
    with image.open("rb") as volume:
        boot = volume.read(SECTOR)
        assert boot[3:11] == b"NTFS    " and boot[510:] == b"\x55\xaa"
        if offset == 0:
            assert boot[446:510] == bytes(64), "SFD must have no partition entries"
        assert struct.unpack_from("<H", boot, 11)[0] == SECTOR
        assert boot[13] * SECTOR == cluster
        assert struct.unpack_from("<HHI", boot, 24) == (63, 255, offset // SECTOR)
        assert struct.unpack_from("<Q", boot, 40)[0] == size // SECTOR - 1
        assert struct.unpack_from("<Q", boot, 72)[0] == report["VOLUME_ID"]
        volume.seek(-SECTOR, 2)
        assert volume.read(SECTOR) == boot
    info = subprocess.run([args.info, "-m", str(image)], env=ENVIRONMENT, capture_output=True, check=True, timeout=30)
    assert b"Volume Version: 3.1" in info.stdout and b"Volume Flags: 0x0000" in info.stdout, info.stdout
    assert re.search(rb"Cluster Size:\s+" + str(cluster).encode() + rb"\b", info.stdout), info.stdout
    actual = subprocess.run([args.label, str(image)], env=ENVIRONMENT, capture_output=True, check=True, timeout=15)
    assert actual.stdout.decode().rstrip("\n") == label, actual.stdout
    if args.fsck and not args.fsck.endswith("-NOTFOUND"):
        checked = subprocess.run([args.fsck, "-n", str(image)], env=ENVIRONMENT, capture_output=True, timeout=45)
        assert checked.returncode == 0, checked.stdout + checked.stderr


def check_failures(args, root):
    image = make_target(root, "invalid.img")
    original = image.read_bytes()
    invalid = [("readonly", {}, errno.EINVAL), ("append", {}, errno.EINVAL), ("pipe", {}, errno.ENOTSUP),
               ("block-request", {}, errno.ENOTSUP), ("missing-device", {}, errno.EINVAL),
               ("cancel-before", {}, errno.ECANCELED), ("label-nul", {}, errno.EINVAL),
               ("label-surrogate", {}, errno.EINVAL), ("none", {"size": 0}, errno.EINVAL),
               ("none", {"size": 8*MIB}, errno.EINVAL), ("none", {"size": DEFAULT_SIZE+1}, errno.EINVAL),
               ("none", {"size": DEFAULT_SIZE+SECTOR}, errno.ESTALE),
               ("none", {"offset": MIB+1}, errno.EINVAL), ("none", {"offset": 1 << 41}, errno.EINVAL),
               ("none", {"cluster": 0}, errno.EINVAL), ("none", {"cluster": 512}, errno.EINVAL),
               ("none", {"cluster": 12288}, errno.EINVAL), ("none", {"cluster": 131072}, errno.EINVAL),
               ("none", {"label": "x"*33}, errno.EINVAL), ("none", {"label": "😀"*17}, errno.EINVAL),
               ("none", {"program": "mkntfs"}, errno.EINVAL),
               ("none", {"program": str(root / "missing")}, errno.ENOENT)]
    for fault, settings, error in invalid:
        invoke(args, image, fault=fault, error=error, started=False, **settings)
        assert image.read_bytes() == original
    failures = [("false-success", "sys.exit(0)", "none", errno.EBADMSG),
                ("exit7", "print('format failed'); sys.exit(7)", "none", errno.EIO),
                ("signal", "os.kill(os.getpid(), signal.SIGUSR1)", "none", errno.EIO),
                ("shrink", "os.ftruncate(3, os.fstat(3).st_size - 512)", "none", errno.ESTALE),
                ("noisy", "os.write(1, b'x' * 1048576)", "none", errno.EBADMSG),
                ("cancel", "signal.signal(signal.SIGTERM, signal.SIG_IGN); time.sleep(10)", "cancel-running", errno.ECANCELED),
                ("closed-output", "os.close(1); os.close(2); time.sleep(10)", "cancel-running", errno.ECANCELED),
                ("timeout", "time.sleep(10)", "timeout", errno.ETIMEDOUT)]
    for name, body, fault, error in failures:
        image = make_target(root, name + ".img")
        report = invoke(args, image, program=fake_formatter(root, name, body), fault=fault, error=error)
        if name == "exit7": assert report["EXIT"] == 7
        if name == "signal": assert report["SIGNAL"] == signal.SIGUSR1
        if name == "noisy": assert report["DIAG"] == 4095
        if name == "cancel": assert report["SIGNAL"] == signal.SIGKILL
        image.unlink()


def check_readback(args, root):
    original = make_target(root, "original.img")
    invoke(args, original)
    with original.open("rb") as volume:
        boot = volume.read(SECTOR)
    mirror_offset = struct.unpack_from("<Q", boot, 56)[0] * 4096
    cases = [("bad-cluster", "os.pwrite(3, b'\\x01', 13)"),
             ("bad-serial-copy", "os.pwrite(3, b'\\0' * 8, 72)"),
             ("bad-backup", "os.pwrite(3, b'bad', os.fstat(3).st_size - 512)"),
             ("bad-mirror", f"os.pwrite(3, b'bad', {mirror_offset})")]
    for name, body in cases:
        image = root / (name + ".img")
        copy_volume(original, image, 0, DEFAULT_SIZE)
        invoke(args, image, program=fake_formatter(root, name, body), error=errno.EBADMSG)
        image.unlink()
    original.unlink()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("probe", type=Path)
    for name in ("mkfs", "info", "label"):
        parser.add_argument("--" + name, required=True)
    parser.add_argument("--fsck")
    args = parser.parse_args()
    args.fsck = readonly_checker(args.fsck)
    with tempfile.TemporaryDirectory(prefix="rufus-ntfs-format-") as temporary:
        root = Path(temporary)
        cases = [(16*MIB, 0, 4096, ""), (DEFAULT_SIZE, 0, 4096, "Rufus ș😀"), (DEFAULT_SIZE, MIB, 8192, "Rufus ș 😀"),
                 (DEFAULT_SIZE, MIB, 16384, 'Qt $(id); "label"'), (DEFAULT_SIZE, MIB, 32768, "Windows 11"),
                 (DEFAULT_SIZE, MIB, 65536, "X"*32), (DEFAULT_SIZE+SECTOR, MIB+SECTOR, 4096, "Odd sectors"),
                 (6*1024*MIB, MIB, 4096, "Large volume")]
        for number, (size, offset, cluster, label) in enumerate(cases):
            image = make_target(root, f"native{number}.img", size)
            report = invoke(args, image, size, offset, cluster, label)
            inspect(args, image, size, offset, cluster, label, report)
            image.unlink()
        image = make_target(root, "retained.img")
        report = invoke(args, image, fault="replace-path")
        assert image.read_bytes() == b"retained"
        inspect(args, Path(str(image) + ".held"), DEFAULT_SIZE, MIB, 4096, "Rufus test", report)
        check_failures(args, root)
        check_readback(args, root)
        for fault, error in (("sync-fail", errno.EIO), ("sync-eintr", 0), ("none", 0)):
            image = make_target(root, fault + ".img")
            report = invoke(args, image, fault=fault, error=error)
            assert report["SYNCS"] == (2 if fault == "sync-eintr" else 1)
            if error == 0:
                inspect(args, image, DEFAULT_SIZE, MIB, 4096, "Rufus test", report)
            image.unlink()
    print(f"{runs} native NTFS formatting cases passed; disposable partition files only.")


if __name__ == "__main__":
    main()
