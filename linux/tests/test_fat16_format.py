# SPDX-License-Identifier: GPL-3.0-or-later
"""Qualify native FAT16 formatting with guarded regular files, never devices."""
import argparse
import errno
from pathlib import Path
import signal
import struct
import subprocess
import tempfile

import test_fat_format as common

MIB = common.MIB


def invoke(args, image, offset=MIB, size=16*MIB, cluster=512, label="RUFUS TEST", **options):
    return common.invoke(args, image, offset, size, cluster, label, **options)


def inspect(args, root, image, offset, size, cluster, label, report):
    common.check_guards(image, offset, size)
    with image.open("rb") as source:
        source.seek(offset)
        boot = source.read(512)
        sector, spc, reserved, fats, entries, short_total = struct.unpack_from("<HBHBHH", boot, 11)
        fat_size = struct.unpack_from("<H", boot, 22)[0]
        total = short_total or struct.unpack_from("<I", boot, 32)[0]
        assert sector == 512 and spc * sector == cluster and fats == 2
        assert boot[510:512] == b"\x55\xaa" and boot[54:62] == b"FAT16   "
        assert entries >= 512 and entries * 32 % sector == 0
        assert struct.unpack_from("<I", boot, 28)[0] == offset // sector
        assert struct.unpack_from("<I", boot, 39)[0] == 0x1234abcd
        assert boot[43:54] == (label or "NO NAME").encode().ljust(11, b" ")
        assert total * sector == report["BYTES"] and 0 <= size - total * sector < 63 * sector
        clusters = (total - reserved - fats * fat_size - entries * 32 // sector) // spc
        assert 4085 <= clusters <= 65524
        source.seek(offset + reserved * sector)
        first_fat = source.read(fat_size * sector)
        assert first_fat[:4] == b"\xf8\xff\xff\xff"
        source.seek(offset + (reserved + fat_size) * sector)
        assert source.read(len(first_fat)) == first_fat
    volume = root / "volume.img"
    common.copy_volume(image, volume, offset, size)
    checked = subprocess.run([args.fsck, "-n", str(volume)], capture_output=True, timeout=15)
    assert checked.returncode == 0, checked.stdout + checked.stderr
    listing = subprocess.run([args.mdir, "-i", f"{image}@@{offset}", "::"], capture_output=True, timeout=10)
    assert listing.returncode == 0 and b"No files" in listing.stdout, listing.stdout + listing.stderr
    volume.unlink()
    invoke(args, image, offset, size, cluster, label, fault="check-only", started=False)


def check_native_formats(args, root):
    cases = [(MIB, 16*MIB, 512, "RUFUS TEST"), (0, 32*MIB, 512, ""),
             (MIB, 64*MIB, 1024, "LINUX_12345"), (MIB, 128*MIB, 2048, "QT$(ID)"),
             (MIB, 512*MIB, 8192, "LARGER"), (MIB, 2048*MIB, 65536, "CLUSTER64K"),
             (MIB, 2048*MIB+512, 65536, "ODD LARGE"),
             (MIB, 16*MIB+512, 512, "ODD SECTORS"), (MIB+512, 16*MIB, 512, "ODD OFFSET")]
    for number, (offset, size, cluster, label) in enumerate(cases):
        image = common.make_target(root, f"native{number}.img", offset, size)
        report = invoke(args, image, offset, size, cluster, label)
        inspect(args, root, image, offset, size, cluster, label, report)
        image.unlink()
    image = common.make_target(root, "retained.img", MIB, 16*MIB)
    report = invoke(args, image, fault="replace-path")
    assert image.read_bytes() == b"retained"
    inspect(args, root, Path(str(image) + ".held"), MIB, 16*MIB, 512, "RUFUS TEST", report)
    image.unlink()
    Path(str(image) + ".held").unlink()


def check_rejected_requests(args, root):
    image = common.make_target(root, "invalid.img", MIB, 16*MIB)
    original = image.read_bytes()
    invalid = [("readonly", {}, errno.EINVAL), ("append", {}, errno.EINVAL), ("pipe", {}, errno.ENOTSUP),
               ("partition-file", {}, errno.ENOTSUP), ("partition-zero", {}, errno.EINVAL),
               ("cancel-before", {}, errno.ECANCELED), ("none", {"offset": 513}, errno.EINVAL),
               ("none", {"size": 16*MIB+1}, errno.EINVAL), ("none", {"size": 0}, errno.ENOTSUP),
               ("none", {"size": MIB}, errno.ENOTSUP), ("none", {"size": 64*MIB}, errno.ENOTSUP),
               ("none", {"size": 32*MIB}, errno.ENOSPC), ("none", {"cluster": 0}, errno.EINVAL),
               ("none", {"cluster": 1536}, errno.EINVAL), ("none", {"cluster": 131072}, errno.EINVAL),
               ("none", {"cluster": 65536}, errno.ENOTSUP), ("none", {"label": "TOO_LONG_LABEL"}, errno.EINVAL),
               ("none", {"label": "lower"}, errno.EINVAL), ("none", {"label": "BAD/LABEL"}, errno.EINVAL),
               ("none", {"label": "É"}, errno.EINVAL), ("none", {"label": " SPACE"}, errno.EINVAL),
               ("none", {"program": "mkfs.fat"}, errno.EINVAL),
               ("none", {"program": str(root / "missing")}, errno.ENOENT)]
    for fault, options, error in invalid:
        invoke(args, image, fault=fault, error=error, started=False, **options)
        assert image.read_bytes() == original
    image.unlink()


def check_process_failures(args, root):
    for fault in ("sync-eintr", "sync-fail"):
        image = common.make_target(root, fault + ".img", MIB, 16*MIB)
        report = invoke(args, image, fault=fault, error=errno.EIO if fault == "sync-fail" else 0)
        common.check_guards(image, MIB, 16*MIB)
        assert report["SYNC"] == (errno.EIO if fault == "sync-fail" else 0)
        assert report["SYNCS"] == (1 if fault == "sync-fail" else 2)
        image.unlink()
    failures = [("exit7", "sys.exit(7)", "none", errno.EIO),
                ("false-success", "sys.exit(0)", "none", errno.EBADMSG),
                ("shrink", "os.ftruncate(3, os.fstat(3).st_size - 1024)", "none", errno.ESTALE),
                ("cancel", "signal.signal(signal.SIGTERM, signal.SIG_IGN); time.sleep(10)", "cancel-running", errno.ECANCELED),
                ("timeout", "time.sleep(10)", "timeout", errno.ETIMEDOUT)]
    for name, body, fault, error in failures:
        image = common.make_target(root, name + ".img", MIB, 16*MIB)
        program = common.fake_formatter(root, name, body)
        report = invoke(args, image, program=program, fault=fault, error=error)
        if name != "shrink":
            common.check_guards(image, MIB, 16*MIB)
        if name == "cancel":
            assert report["SIGNAL"] == signal.SIGKILL and report["MS"] >= 650
        image.unlink()


def check_corrupted_readback(args, root):
    image = common.make_target(root, "readback.img", MIB, 16*MIB)
    report = invoke(args, image)
    inspect(args, root, image, MIB, 16*MIB, 512, "RUFUS TEST", report)
    # Each corrupted field must fail read-only validation without launching a child.
    with image.open("r+b") as target:
        target.seek(MIB)
        boot = target.read(512)
        mutations = [(510, b"\0\0"), (11, struct.pack("<H", 1024)), (16, b"\x01"),
                     (13, b"\x02"), (14, b"\0\0"), (17, struct.pack("<H", 1024)), (22, b"\0\0"),
                     (19, struct.pack("<H", 2000)), (28, struct.pack("<I", 123)),
                     (39, b"\0\0\0\0"), (43, b"WRONG")]
        for position, data in mutations:
            target.seek(MIB)
            target.write(boot)
            target.seek(MIB + position)
            target.write(data)
            target.flush()
            invoke(args, image, fault="check-only", error=errno.EBADMSG, started=False)
        target.seek(MIB)
        target.write(boot)
    common.check_guards(image, MIB, 16*MIB)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("probe", type=Path)
    parser.add_argument("--mkfs", required=True)
    parser.add_argument("--fsck", required=True)
    parser.add_argument("--mdir", required=True)
    args = parser.parse_args()
    args.fat_bits = 16
    with tempfile.TemporaryDirectory(prefix="rufus-fat16-format-") as temporary:
        root = Path(temporary)
        check_native_formats(args, root)
        check_rejected_requests(args, root)
        check_process_failures(args, root)
        check_corrupted_readback(args, root)
    print(f"{common.runs} FAT16 format/readback cases passed; disposable files only.")


if __name__ == "__main__":
    main()
