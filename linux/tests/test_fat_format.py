# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise native FAT32 formatting on guarded, disposable sparse files only."""
import argparse
import errno
import os
from pathlib import Path
import re
import signal
import struct
import subprocess
import tempfile

MIB = 1024 * 1024
GUARD = b"guard outside filesystem\x00" * 4096
runs = 0


def make_target(root, name, offset, size):
    image = root / name
    with image.open("wb") as output:
        output.truncate(offset + size + len(GUARD))
        if offset:
            output.write((GUARD * ((offset + len(GUARD) - 1) // len(GUARD)))[:offset])
        output.seek(offset + size)
        output.write(GUARD)
    return image


def check_guards(image, offset, size):
    assert image.stat().st_size == offset + size + len(GUARD)
    with image.open("rb") as source:
        if offset:
            assert source.read(offset) == (GUARD * ((offset + len(GUARD) - 1) // len(GUARD)))[:offset]
        source.seek(offset + size)
        assert source.read() == GUARD


def invoke(args, image, offset=MIB, size=64*MIB, cluster=512, label="RUFUS TEST",
           fault="none", error=0, program=None, started=True):
    global runs
    result = subprocess.run([str(args.probe), program or args.mkfs, str(image), str(offset),
                             str(size), str(cluster), label, fault], capture_output=True, timeout=8)
    runs += 1
    assert result.returncode == 0, (result.returncode, result.stdout, result.stderr)
    values = {key.decode(): int(value) for key, value in re.findall(rb"([A-Z_]+)=(-?\d+)", result.stdout)}
    assert values["ERROR"] == error, (fault, values, result.stderr)
    assert values["STARTED"] == started, values
    assert values["FDS"] == 0 and values["OFFSET"] == (-1 if fault == "pipe" else 17) and values["FLAGS"] == 1, values
    assert values["CHILD"] == -1 and values["CHILD_ERRNO"] == errno.ECHILD, values
    assert values["DIAG"] <= 4095 and values["MS"] < 6000, values
    assert values["SYNCS"] >= int(started), values
    if error == 0:
        assert values["EXIT"] == 0 and values["SYNC"] == 0 and values["BYTES"] > 0, values
    return values


def copy_volume(image, destination, offset, size):
    # Preserve every allocated extent in the requested range without filling a
    # multi-GiB sparse fixture. SEEK_DATA/HOLE are available on Linux test storage.
    with image.open("rb") as source, destination.open("wb") as output:
        output.truncate(size)
        position = offset
        while position < offset + size:
            try:
                data = os.lseek(source.fileno(), position, os.SEEK_DATA)
            except OSError as error:
                if error.errno == errno.ENXIO:
                    break
                raise
            end = min(os.lseek(source.fileno(), data, os.SEEK_HOLE), offset + size)
            if data >= end:
                break
            source.seek(data)
            output.seek(data - offset)
            while data < end:
                chunk = source.read(min(MIB, end - data))
                assert chunk
                output.write(chunk)
                data += len(chunk)
            position = end


def inspect(args, root, image, offset, size, cluster, label, report):
    check_guards(image, offset, size)
    with image.open("rb") as source:
        source.seek(offset)
        boot = source.read(512)
        sector, spc, reserved, fats = struct.unpack_from("<HBHB", boot, 11)
        total, fat_size, root_cluster = (struct.unpack_from("<I", boot, pos)[0] for pos in (32, 36, 44))
        assert sector == 512 and spc * sector == cluster and fats == 2
        assert boot[510:512] == b"\x55\xaa" and boot[82:90] == b"FAT32   "
        assert struct.unpack_from("<I", boot, 28)[0] == offset // 512
        assert struct.unpack_from("<I", boot, 67)[0] == 0x1234abcd
        assert boot[71:82] == (label or "NO NAME").encode().ljust(11, b" ")
        assert total * sector == report["BYTES"] and 0 <= size - total * sector < 63 * sector
        clusters = (total - reserved - fats * fat_size) // spc
        assert 65525 <= clusters <= 0x0ffffff4 and root_cluster == 2
        source.seek(offset + 6 * sector)
        assert source.read(512) == boot  # Backup boot sector.
        source.seek(offset + sector)
        info = source.read(512)
        assert info[:4] == b"RRaA" and info[484:488] == b"rrAa" and info[508:] == b"\0\0\x55\xaa"
        source.seek(offset + reserved * sector)
        fat = source.read(fat_size * sector)
        source.seek(offset + (reserved + fat_size) * sector)
        assert source.read(len(fat)) == fat
    volume = root / "volume.img"
    copy_volume(image, volume, offset, size)
    checked = subprocess.run([args.fsck, "-n", str(volume)], capture_output=True, timeout=15)
    assert checked.returncode == 0, checked.stdout + checked.stderr
    listing = subprocess.run([args.mdir, "-i", f"{image}@@{offset}", "::"], capture_output=True, timeout=10)
    assert listing.returncode == 0, listing.stdout + listing.stderr
    assert b"No files" in listing.stdout, listing.stdout
    volume.unlink()


def fake_formatter(root, name, body):
    program = root / name
    program.write_text("#!/usr/bin/python3\nimport os, signal, sys, time\n" + body + "\n")
    program.chmod(0o700)
    return str(program)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("probe", type=Path)
    parser.add_argument("--mkfs", required=True)
    parser.add_argument("--fsck", required=True)
    parser.add_argument("--mdir", required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="rufus-fat-format-") as temporary:
        root = Path(temporary)
        cases = [(MIB, 64*MIB, 512, "RUFUS TEST"), (0, 128*MIB, 1024, ""),
                 (MIB, 64*MIB+512, 512, "ODD SECTORS"),
                 (MIB+512, 64*MIB, 512, "ODD OFFSET"),
                 (MIB, 512*MIB, 4096, "LINUX_12345"), (MIB, 2304*MIB, 32768, "QT$(ID)")]
        for number, (offset, size, cluster, label) in enumerate(cases):
            image = make_target(root, f"native{number}.img", offset, size)
            report = invoke(args, image, offset, size, cluster, label)
            inspect(args, root, image, offset, size, cluster, label, report)
            image.unlink()
        image = make_target(root, "retained.img", MIB, 64*MIB)
        report = invoke(args, image, fault="replace-path")
        assert image.read_bytes() == b"retained"
        inspect(args, root, Path(str(image) + ".held"), MIB, 64*MIB, 512, "RUFUS TEST", report)
        image = make_target(root, "invalid.img", MIB, 64*MIB)
        invalid = [("readonly", {}, errno.EINVAL), ("append", {}, errno.EINVAL), ("pipe", {}, errno.ENOTSUP),
                   ("partition-file", {}, errno.ENOTSUP), ("partition-zero", {}, errno.EINVAL),
                   ("cancel-before", {}, errno.ECANCELED), ("none", {"offset": 513}, errno.EINVAL),
                   ("none", {"size": 64*MIB+1}, errno.EINVAL), ("none", {"size": 0}, errno.ENOTSUP),
                   ("none", {"size": 128*MIB}, errno.ENOSPC), ("none", {"cluster": 0}, errno.EINVAL),
                   ("none", {"cluster": 1536}, errno.EINVAL), ("none", {"cluster": 65536}, errno.EINVAL),
                   ("none", {"cluster": 32768}, errno.ENOTSUP), ("none", {"label": "TOO_LONG_LBL" + "X"}, errno.EINVAL),
                   ("none", {"label": "lower"}, errno.EINVAL), ("none", {"label": "BAD/LABEL"}, errno.EINVAL),
                   ("none", {"label": "É"}, errno.EINVAL), ("none", {"label": " SPACE"}, errno.EINVAL),
                   ("none", {"program": "mkfs.fat"}, errno.EINVAL),
                   ("none", {"program": str(root / "missing")}, errno.ENOENT)]
        original = image.read_bytes()
        for fault, options, error in invalid:
            invoke(args, image, fault=fault, error=error, started=False, **options)
            assert image.read_bytes() == original
        for fault in ("sync-eintr", "sync-fail"):
            image = make_target(root, fault + ".img", MIB, 64*MIB)
            report = invoke(args, image, fault=fault, error=errno.EIO if fault == "sync-fail" else 0)
            check_guards(image, MIB, 64*MIB)
            assert report["SYNC"] == (errno.EIO if fault == "sync-fail" else 0)
            assert report["SYNCS"] == (1 if fault == "sync-fail" else 2)
        failures = [("exit7", "print('formatter failed'); sys.exit(7)", "none", errno.EIO),
                    ("false-success", "sys.exit(0)", "none", errno.EBADMSG),
                    ("signal", "os.kill(os.getpid(), signal.SIGUSR1)", "none", errno.EIO),
                    ("shrink", "os.ftruncate(3, os.fstat(3).st_size - 1024)", "none", errno.ESTALE),
                    ("noisy", "os.write(1, b'x' * 1048576)", "none", errno.EBADMSG),
                    ("cancel", "signal.signal(signal.SIGTERM, signal.SIG_IGN); time.sleep(10)", "cancel-running", errno.ECANCELED),
                    ("closed-output", "os.close(1); os.close(2); time.sleep(10)", "cancel-running", errno.ECANCELED),
                    ("timeout", "time.sleep(10)", "timeout", errno.ETIMEDOUT)]
        for name, body, fault, error in failures:
            image = make_target(root, name + ".img", MIB, 64*MIB)
            program = fake_formatter(root, name, body)
            report = invoke(args, image, program=program, fault=fault, error=error)
            if name != "shrink": check_guards(image, MIB, 64*MIB)
            if name == "exit7": assert report["EXIT"] == 7 and report["DIAG"] > 0
            if name == "signal": assert report["SIGNAL"] == signal.SIGUSR1
            if name == "cancel": assert report["SIGNAL"] == signal.SIGKILL and report["MS"] >= 650
            if name == "noisy": assert report["DIAG"] == 4095
        # A failed/cancelled process cannot leave a formatter alive for the next operation.
        image = make_target(root, "recovery.img", MIB, 64*MIB)
        report = invoke(args, image)
        inspect(args, root, image, MIB, 64*MIB, 512, "RUFUS TEST", report)
    print(f"{runs} native FAT32 formatting cases passed; disposable files only.")


if __name__ == "__main__":
    main()
