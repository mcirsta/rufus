# SPDX-License-Identifier: GPL-3.0-or-later
"""Qualify the pinned native installer on disposable, unmounted FAT32 files."""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


SECTOR_BYTES = 512
MIB = 1024 * 1024
BPB_START = 11
FAT16_BPB_END = 62
FAT32_CODE_START = 90
BACKUP_SECTOR_FIELD = 50
QEMU_SUCCESS = (0x10 << 1) | 1
BOOT_MARKER = b"RUFUS_RAW_BOOT_OK"
SYSLINUX_BANNER = b"SYSLINUX 6.04 6.04-pre1"
ROOT = Path(__file__).resolve().parents[3]


def run(command, env, expected=0, timeout=20):
    print("+", " ".join(str(part) for part in command), flush=True)
    result = subprocess.run(command, env=env, capture_output=True, timeout=timeout)
    print(result.stdout.decode(errors="replace"), end="", flush=True)
    print(result.stderr.decode(errors="replace"), end="", flush=True)
    assert result.returncode == expected, (command, result.returncode, expected)
    return result.stdout


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def check_files(image_spec, directory, env, files):
    for target, source in files.items():
        output = directory / "readback"
        run(["mcopy", "-o", "-i", image_spec, f"::{target}", output], env)
        assert output.read_bytes() == source.read_bytes(), target
        print(f"READBACK {target} {digest(output)}", flush=True)


def check_volume(image, directory, offset, size, before, env, fat_bits=32):
    # Work on a copy: upstream updates the primary VBR only, as Rufus does.
    # Normalize that one known backup mismatch to let fsck detect other damage.
    volume = directory / "check.img"
    with image.open("rb") as source, volume.open("wb") as target:
        source.seek(offset)
        remaining = size
        while remaining:
            chunk = source.read(min(MIB, remaining))
            assert chunk, "Short FAT volume"
            target.write(chunk)
            remaining -= len(chunk)
    with volume.open("r+b") as stream:
        after = stream.read(SECTOR_BYTES)
        bpb_end = FAT16_BPB_END if fat_bits == 16 else FAT32_CODE_START
        assert after[BPB_START:bpb_end] == before[BPB_START:bpb_end]
        assert after[-2:] == b"\x55\xaa"
        assert after != before, "Installer did not change the VBR"
        if fat_bits == 32:
            backup_sector = int.from_bytes(before[BACKUP_SECTOR_FIELD:BACKUP_SECTOR_FIELD + 2], "little")
            stream.seek(backup_sector * SECTOR_BYTES)
            assert stream.read(SECTOR_BYTES) == before, "Unexpected backup VBR change"
            stream.seek(backup_sector * SECTOR_BYTES)
            stream.write(after)
    run(["fsck.fat", "-n", volume], env)
    volume.unlink()
    print(f"FAT{fat_bits} BPB preserved; fsck clean (FAT32 backup normalized on copy only)", flush=True)


def boot(image, env):
    before = digest(image)
    output = run([
        "qemu-system-x86_64", "-machine", "pc,accel=tcg", "-m", "64",
        "-nodefaults", "-display", "none", "-serial", "stdio", "-monitor", "none",
        "-nic", "none", "-no-reboot", "-boot", "c", "-snapshot",
        "-drive", f"file={image},format=raw,if=ide",
        "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
    ], env, expected=QEMU_SUCCESS, timeout=30)
    loader_seen = SYSLINUX_BANNER in output
    fixture_seen = BOOT_MARKER in output
    assert loader_seen and fixture_seen, "Syslinux did not hand off to the configured boot fixture"
    assert output.index(SYSLINUX_BANNER) < output.index(BOOT_MARKER)
    assert digest(image) == before, "VM changed its backing FAT image"
    print(f"BOOT verified; backing SHA256 unchanged: {before}", flush=True)


def qualify(installer, upstream, directory, env, cluster_sectors, subdir, offset):
    size = 64 * MIB * cluster_sectors
    guard = b"RUFUS_GUARD_" * SECTOR_BYTES
    image = directory / "FAT volume ș.img"
    with image.open("wb") as stream:
        stream.truncate(offset + size)
        if offset:
            stream.seek(offset - len(guard))
            stream.write(guard)
    run(["mkfs.fat", "-F", "32", "-S", str(SECTOR_BYTES), "-s", str(cluster_sectors),
         "-n", "RUFUS_TEST", "-i", "1234abcd", "-h", str(offset // SECTOR_BYTES),
         f"--offset={offset // SECTOR_BYTES}", image, str(size // 1024)], env)
    with image.open("r+b") as stream:
        prefix = stream.read(offset)
        before = stream.read(SECTOR_BYTES)
        stream.seek(offset + size)
        stream.write(guard)
    image_spec = f"{image}@@{offset}"
    target_dir = subdir
    if subdir:
        run(["mmd", "-i", image_spec, f"::{subdir}"], env)
    config = directory / "syslinux.cfg"
    config.write_text("SERIAL 0 115200\n"
                      "DEFAULT checkpoint\nPROMPT 0\nTIMEOUT 1\n"
                      "LABEL checkpoint\n  BOOT fixture.bs\n", encoding="ascii")
    fixture = directory / "fixture.bs"
    run(["nasm", "-f", "bin", ROOT / "linux/tests/vm/boot.asm", "-o", fixture], env)
    sentinel = directory / "keep.txt"
    sentinel.write_bytes(b"Existing ordinary file must survive.\n" * 257)
    files = {f"{target_dir}/syslinux.cfg": config, f"{target_dir}/fixture.bs": fixture,
             "/keep.txt": sentinel}
    for target, source in files.items():
        run(["mcopy", "-i", image_spec, source, f"::{target}"], env)
    command = [installer, "--install", "--offset", str(offset)]
    if subdir:
        command.extend(["--directory", subdir])
    command.append(image)
    run(command, env, timeout=40)
    module = upstream / "bios/com32/elflink/ldlinux/ldlinux.c32"
    files[f"{target_dir}/ldlinux.c32"] = module
    check_files(image_spec, directory, env, files)
    loader = directory / "installed.sys"
    run(["mcopy", "-i", image_spec, f"::{target_dir}/ldlinux.sys", loader], env)
    payload = upstream / "bios/core/ldlinux.sys"
    padded_size = (payload.stat().st_size + SECTOR_BYTES - 1) // SECTOR_BYTES * SECTOR_BYTES
    assert loader.stat().st_size == padded_size + 2 * SECTOR_BYTES
    check_volume(image, directory, offset, size, before, env)
    with image.open("rb") as stream:
        assert stream.read(offset) == prefix, "Writes before the selected FAT volume"
        stream.seek(offset + size)
        assert stream.read() == guard, "Writes after the selected FAT volume"
    assert not list(directory.glob("syslinux-mtools-*")), "Installer left its temporary config"
    if not offset:
        boot(image, env)
    print(f"PASS cluster={cluster_sectors * SECTOR_BYTES} directory={target_dir or '/'} offset={offset}", flush=True)


def check_unsupported(installer, directory, env):
    # A valid 4K-sector FAT volume must be rejected before the native installer
    # writes anything. Rufus supports variable sectors; this native build does not.
    image = directory / "sector4k.img"
    with image.open("wb") as stream:
        stream.truncate(64 * MIB)
    run(["mkfs.fat", "-F", "16", "-S", "4096", image], env)
    before = digest(image)
    run([installer, "--install", image], env, expected=1)
    assert digest(image) == before, "Unsupported sector size was modified"
    print("PASS unsupported 4096-byte sectors rejected without writes", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("installer", type=Path)
    parser.add_argument("upstream", type=Path)
    args = parser.parse_args()
    for tool in ("mkfs.fat", "fsck.fat", "mcopy", "mmd", "mattrib", "mmove", "nasm", "qemu-system-x86_64"):
        assert shutil.which(tool), f"Missing test dependency: {tool}"
    with tempfile.TemporaryDirectory(prefix="rufus-syslinux-native-") as temporary:
        root = Path(temporary)
        env = os.environ.copy()
        env.update({"LC_ALL": "C", "MTOOLSRC": "/dev/null"})
        run([args.installer, "--version"], env)
        for name, clusters, subdir, offset in (
            ("root", 1, "", 0), ("subdir", 2, "/boot", 0), ("offset", 1, "", MIB),
        ):
            directory = root / name
            directory.mkdir()
            env["TMPDIR"] = str(directory)
            qualify(args.installer, args.upstream, directory, env, clusters, subdir, offset)
            shutil.rmtree(directory)
        env["TMPDIR"] = str(root)
        check_unsupported(args.installer, root, env)
    print("All native Syslinux qualification checks passed; temporary images removed.", flush=True)


if __name__ == "__main__":
    main()
