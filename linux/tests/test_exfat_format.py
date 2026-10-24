# SPDX-License-Identifier: GPL-3.0-or-later
"""Qualify native exFAT quick formatting on disposable partition-sized files."""
import argparse
import errno
import hashlib
import os
from pathlib import Path
import re
import signal
import struct
import subprocess
import tempfile

from test_fat_format import fake_formatter

MIB = 1024 * 1024
SECTOR = 512
SIZE = 16 * MIB
BOOT_REGION_BYTES = 12 * SECTOR
GUARD = b"quick format keeps free data\0" * 256
runs = 0


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def make_target(root, name, size=SIZE):
    path = root / name
    with path.open("wb") as stream:
        stream.truncate(size)
        stream.seek(size - len(GUARD))
        stream.write(GUARD)
    return path


def invoke(args, image, size=SIZE, cluster=4096, label="Rufus test", fault="none",
           program=None, error=0, started=True):
    global runs
    result = subprocess.run([args.probe, program or args.mkfs, image, str(size), str(cluster), label, fault],
                            capture_output=True, timeout=15, env=dict(os.environ, LC_ALL="C"))
    runs += 1
    assert result.returncode == 0, (result.returncode, result.stdout, result.stderr)
    values = {key.decode(): int(value) for key, value in re.findall(rb"([A-Z_]+)=(-?\d+)", result.stdout)}
    assert values["ERROR"] == error and values["STARTED"] == started, (fault, values, result.stderr)
    assert values["FDS"] == 0 and values["FLAGS"] == 1 and values["LOCALE"] == 1, values
    assert values["OFFSET"] == (-1 if fault == "pipe" else 17), values
    assert values["CHILD"] == -1 and values["CHILD_ERRNO"] == errno.ECHILD, values
    assert values["DIAG"] <= 4095 and values["SYNCS"] >= int(started), values
    if error == 0:
        assert values["BYTES"] == size and values["EXIT"] == 0, values
    print(f"PASS {fault} cluster={cluster} error={error}", flush=True)
    return values


def inspect(args, image, size, cluster, label, free_tail=True, partition_offset=0):
    assert image.stat().st_size == size
    with image.open("rb") as stream:
        regions = stream.read(2 * BOOT_REGION_BYTES)
        main, backup = regions[:BOOT_REGION_BYTES], regions[BOOT_REGION_BYTES:]
        assert main == backup and main[3:11] == b"EXFAT   "
        assert main[510:512] == b"\x55\xaa"
        assert main[108] == 9 and SECTOR << main[109] == cluster
        assert struct.unpack_from("<QQ", main, 64) == (partition_offset // SECTOR, size // SECTOR)
        assert main[446:510] == bytes(64), "Formatter added an MBR partition table"
        checksum = 0
        for index, byte in enumerate(main[:11 * SECTOR]):
            if index not in (106, 107, 112):
                checksum = (((checksum << 31) | (checksum >> 1)) + byte) & 0xffffffff
        assert main[11 * SECTOR:] == struct.pack("<I", checksum) * (SECTOR // 4)
        heap, count, root = struct.unpack_from("<III", main, 88)
        assert count > 0 and heap * SECTOR + count * cluster <= size
        assert struct.unpack_from("<I", main, 80)[0] * SECTOR == MIB
        assert heap * SECTOR % MIB == 0
        stream.seek(heap * SECTOR + (root - 2) * cluster)
        entry = stream.read(32)
        assert entry[0] == 0x83
        label_bytes = label.encode("utf-16-le")
        assert entry[1] == len(label_bytes) // 2 and entry[2:2 + len(label_bytes)] == label_bytes
        if free_tail:
            stream.seek(size - len(GUARD))
            assert stream.read() == GUARD, "Quick format changed free tail data"
    checked = subprocess.run([args.fsck, "-n", image], capture_output=True, timeout=30)
    assert checked.returncode == 0, checked.stdout + checked.stderr
    dumped = subprocess.run([args.dump, image], capture_output=True, timeout=30, env=dict(os.environ, LC_ALL="C.UTF-8"))
    assert dumped.returncode == 0, dumped.stdout + dumped.stderr
    assert re.search(rb"Cluster size:\s+" + str(cluster).encode() + rb"\b", dumped.stdout), dumped.stdout


def native_cases(args, root):
    for shift in range(9, 26):  # Rufus's full 512-byte through 32-MiB range.
        cluster = 1 << shift
        size = max(SIZE, cluster * 8)
        image = make_target(root, "cluster.img", size)
        invoke(args, image, size=size, cluster=cluster)
        inspect(args, image, size, cluster, "Rufus test")
        image.unlink()
    for label in ("", "Mixed.case", "Rufus ș😀", "ș" * 9 + "😀", "x" * 11, "trail ", "$(id)", "-LABEL"):
        image = make_target(root, "label.img")
        invoke(args, image, label=label)
        inspect(args, image, SIZE, 4096, label)
        image.unlink()
    image = make_target(root, "odd-sector.img", SIZE + SECTOR)
    invoke(args, image, size=SIZE + SECTOR)
    inspect(args, image, SIZE + SECTOR, 4096, "Rufus test")
    image.unlink()
    image = make_target(root, "retained.img")
    invoke(args, image, fault="replace-path")
    assert image.read_bytes() == b"retained"
    held = Path(str(image) + ".held")
    inspect(args, held, SIZE, 4096, "Rufus test")
    image.unlink()
    held.unlink()


def rejected_cases(args, root):
    image = make_target(root, "rejected.img")
    original = digest(image)
    cases = [("file-offset", {}, errno.EINVAL), ("unaligned-offset", {}, errno.EINVAL),
             ("overflow-offset", {}, errno.EINVAL), ("old-library", {}, errno.ENOTSUP), ("readonly", {}, errno.EINVAL),
             ("append", {}, errno.EINVAL), ("pipe", {}, errno.ENOTSUP),
             ("cancel-before", {}, errno.ECANCELED), ("label-nul", {}, errno.EINVAL),
             ("label-surrogate", {}, errno.EINVAL), ("none", {"size": 0}, errno.EINVAL),
             ("none", {"size": SIZE - 1}, errno.EINVAL), ("none", {"size": 8 * MIB}, errno.EINVAL),
             ("none", {"size": SIZE + SECTOR}, errno.ESTALE), ("none", {"size": 256 * 1024**4}, errno.EINVAL),
             ("none", {"cluster": 0}, errno.EINVAL), ("none", {"cluster": 256}, errno.EINVAL),
             ("none", {"cluster": 1536}, errno.EINVAL), ("none", {"cluster": 64 * MIB}, errno.EINVAL),
             ("none", {"cluster": 8 * MIB}, errno.ENOSPC),
             ("none", {"cluster": 32 * MIB}, errno.ENOSPC),
             ("none", {"size": 3 * 1024**4, "cluster": SECTOR}, errno.EINVAL),
             ("none", {"size": 256 * 1024**4 - SECTOR, "cluster": SECTOR}, errno.EINVAL),
             ("none", {"label": "x" * 12}, errno.EINVAL), ("none", {"label": "😀" * 6}, errno.EINVAL),
             ("none", {"label": "bad/name"}, errno.EINVAL), ("none", {"label": "bad\nname"}, errno.EINVAL),
             ("none", {"program": "mkfs.exfat"}, errno.EINVAL),
             ("none", {"program": str(root / "missing")}, errno.ENOENT)]
    for fault, settings, error in cases:
        invoke(args, image, fault=fault, error=error, started=False, **settings)
        assert digest(image) == original, "Rejected request wrote to its target"
    image.unlink()


def geometry_cases(args, root):
    # Whole-disk defaults remain independent of the partition's actual extent.
    gib, tib = 1024 * MIB, 1024**2 * MIB
    cases = [(16 * MIB, 16 * MIB, 0x007ffe00, 4096),
             (26 * MIB - SECTOR, 26 * MIB - SECTOR, 0x007ffe00, 4096),
             (26 * MIB, 26 * MIB, 0x00fffe00, 4096),
             (50 * MIB - SECTOR, 50 * MIB - SECTOR, 0x00fffe00, 4096),
             (50 * MIB, 50 * MIB, 0x01fffe00, 4096),
             (98 * MIB - SECTOR, 98 * MIB - SECTOR, 0x01fffe00, 4096),
             (98 * MIB, 98 * MIB, 0x03fffe00, 4096),
             (256 * MIB, 255 * MIB, 0x03fffe00, 32768),
             (32 * gib, 32 * gib - MIB, 0x03fffe00, 131072),
             (3 * tib, 3 * tib - MIB, 0x03fffc00, 131072),
             (256 * tib - SECTOR, 256 * tib - SECTOR, 0x03ff0000, 131072),
             (256 * tib, 128 * tib, 0, 0), (2**64 - 1, 2**64 - 1, 0, 0),
             (16 * MIB, 16 * MIB - SECTOR, 0, 0),
             (16 * MIB, 16 * MIB + SECTOR, 0, 0),
             (32 * MIB, 16 * MIB + 1, 0, 0), (16 * MIB, 0, 0, 0)]
    for disk, volume, allowed, default in cases:
        result = subprocess.run([args.probe, "--clusters", str(disk), str(volume)],
                                capture_output=True, timeout=15)
        assert result.returncode == 0, result.stderr
        assert tuple(map(int, result.stdout.split())) == (allowed, default), (disk, volume, result.stdout)
    # At each large-cluster lower boundary, all three metadata allocations just
    # fit. The final cluster is occupied by the root, not free tail data.
    for cluster in (8 * MIB, 16 * MIB, 32 * MIB):
        minimum = 2 * MIB + 3 * cluster
        for adjustment in (-SECTOR, 0, SECTOR):
            size = minimum + adjustment
            image = make_target(root, "metadata-boundary.img", size)
            if adjustment < 0:
                original = digest(image)
                invoke(args, image, size=size, cluster=cluster, error=errno.ENOSPC, started=False)
                assert digest(image) == original
            else:
                invoke(args, image, size=size, cluster=cluster)
                inspect(args, image, size, cluster, "Rufus test", free_tail=False)
            image.unlink()
    # Native readback on both sides of Rufus's default transitions. Large files
    # are sparse and removed immediately; only formatter metadata is allocated.
    for size, cluster in ((256 * MIB - SECTOR, 4096), (256 * MIB, 32768),
                          (32 * gib - SECTOR, 32768), (32 * gib, 131072)):
        image = make_target(root, "default-boundary.img", size)
        invoke(args, image, size=size, cluster=cluster)
        inspect(args, image, size, cluster, "Rufus test")
        image.unlink()
    print(f"{len(cases)} native exFAT cluster-policy cases passed.", flush=True)


def failure_cases(args, root):
    for fault, error in (("sync-eintr", 0), ("sync-fail", errno.EIO), ("read-fail", errno.EIO),
                         ("corrupt-primary", errno.EBADMSG), ("corrupt-backup", errno.EBADMSG),
                         ("corrupt-both", errno.EBADMSG)):
        image = make_target(root, fault + ".img")
        report = invoke(args, image, fault=fault, error=error)
        assert report["SYNC"] == (errno.EIO if fault == "sync-fail" else 0), report
        if fault == "sync-eintr":
            assert report["SYNCS"] == 2
        image.unlink()
    cases = [("false-success", "sys.exit(0)", "none", errno.EBADMSG),
             ("exit7", "sys.exit(7)", "none", errno.EIO),
             ("signal", "os.kill(os.getpid(), signal.SIGUSR1)", "none", errno.EIO),
             ("shrink", "os.ftruncate(3, os.fstat(3).st_size - 512)", "none", errno.ESTALE),
             ("noisy", "os.write(1, b'x' * 1048576)", "none", errno.EBADMSG),
             ("cancel", "signal.signal(signal.SIGTERM, signal.SIG_IGN); time.sleep(10)", "cancel-running", errno.ECANCELED),
             ("timeout", "time.sleep(10)", "timeout", errno.ETIMEDOUT)]
    for name, body, fault, error in cases:
        image = make_target(root, name + ".img")
        report = invoke(args, image, program=fake_formatter(root, name, body), fault=fault, error=error)
        if name == "noisy":
            assert report["DIAG"] == 4095
        if name == "cancel":
            assert report["SIGNAL"] == signal.SIGKILL
        image.unlink()
    # Valid filesystems with the wrong requested settings must also fail.
    for name, replacements in (("wrong-cluster", "args[args.index('-c') + 1] = '8192'"),
                               ("wrong-label", "args[args.index('-L') + 1] = 'WRONG'")):
        body = f"import subprocess\nargs = sys.argv[1:]\n{replacements}\nsubprocess.run([{str(args.mkfs)!r}] + args, pass_fds=(3,), check=True)"
        image = make_target(root, name + ".img")
        invoke(args, image, program=fake_formatter(root, name, body), error=errno.EBADMSG)
        image.unlink()


def provider_cases(args, root, help_text):
    cases = [(args.mkfs, "none", 0), (root / "missing", "none", errno.ENOENT),
             (args.mkfs, "old-library", errno.ENOTSUP), (args.mkfs, "cancel-before", errno.ECANCELED)]
    for name, body, fault, error in (
            ("old-help", "print('Usage: mkfs.exfat --cluster-size'); sys.exit(1)", "none", errno.ENOTSUP),
            ("help-exit7", f"os.write(1, {help_text!r}); sys.exit(7)", "none", errno.ENOTSUP),
            ("help-large", "os.write(1, b'x' * 65536)", "none", errno.EOVERFLOW),
            ("help-timeout", "time.sleep(10)", "none", errno.ETIMEDOUT),
            ("help-cancel", "time.sleep(10)", "cancel-running", errno.ECANCELED)):
        cases.append((fake_formatter(root, name, body), fault, error))
    for option in (b"--check-written", b"--no-discard", b"--partition-table", b"--boundary-align",
                   b"--sector-size", b"--cluster-size", b"--volume-label"):
        help_without_option = help_text.replace(option, b"--missing")
        program = fake_formatter(root, option.decode(), f"os.write(1, {help_without_option!r}); sys.exit(1)")
        cases.append((program, "none", errno.ENOTSUP))
    for program, fault, error in cases:
        result = subprocess.run([args.probe, "--tools", program, fault], capture_output=True, timeout=10)
        assert result.returncode == 0, (result.returncode, result.stdout, result.stderr)
        fields = {key.decode(): int(value) for key, value in re.findall(rb"([A-Z_]+)=(-?\d+)", result.stdout)}
        assert fields == {"ERROR": error, "FDS": 0, "CHILD": -1, "CHILD_ERRNO": errno.ECHILD,
                          "PROBLEM": int(error != 0)}, (program, fields, result.stderr)
    print(f"{len(cases)} exFAT provider preflight cases passed.", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("probe", type=Path)
    parser.add_argument("--mkfs", type=Path, required=True)
    parser.add_argument("--fsck", type=Path, required=True)
    parser.add_argument("--dump", type=Path, required=True)
    args = parser.parse_args()
    supported = subprocess.run([args.probe, "--check-library"], timeout=15)
    if supported.returncode == 77:
        print("SKIP: libblkid 2.39+ required for exFAT boot-checksum validation", flush=True)
        return 77
    assert supported.returncode == 0, supported.returncode
    help_result = subprocess.run([args.mkfs, "--help"], capture_output=True)
    help_text = help_result.stdout + help_result.stderr
    required = (b"--check-written", b"--no-discard", b"--partition-table")
    if not all(option in help_text for option in required):
        print("SKIP: exfatprogs with metadata readback, no-discard and partition-table control required", flush=True)
        return 77
    with tempfile.TemporaryDirectory(prefix="rufus-exfat-format-") as temporary:
        root = Path(temporary)
        neighbour = root / "untouched-file"
        neighbour.write_bytes(GUARD)
        provider_cases(args, root, help_text)
        native_cases(args, root)
        rejected_cases(args, root)
        geometry_cases(args, root)
        failure_cases(args, root)
        assert neighbour.read_bytes() == GUARD
    print(f"{runs} exFAT formatting cases passed; all temporary volumes removed.", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
