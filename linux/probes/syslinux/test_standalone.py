# SPDX-License-Identifier: GPL-3.0-or-later
"""Boot empty MBR/FAT media through the checked v4/v6 native adapter."""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time

from verify import MIB, ROOT, SECTOR_BYTES, check_files, check_volume, check_unsupported, digest, run

sys.path.insert(0, str(ROOT / "linux/tests"))
sys.path.insert(0, str(ROOT / "linux/tests/vm"))
from test_mbr_image import invoke as partition_mbr, inspect_mbr
from test_fat_format import invoke as format_fat
from boot_linux import firmware_options
from syslinux_alpine import qmp_read, qmp_command, read_screen
from test_command import tools_directory
from test_install import install

BOOT_TIMEOUT_SECONDS = 30


def boot_prompt(image, output, env, version="6.04"):
    before = digest(image)
    command = ["qemu-system-x86_64", "-accel", "tcg", "-m", "64", "-vga", "std",
               "-display", "none", "-monitor", "none", "-serial", "none", "-nic", "none",
               "-no-reboot", "-qmp", "stdio", "-snapshot", "-device", "qemu-xhci,id=xhci",
               "-drive", "if=none,id=stick,format=raw,file=" + str(image),
               "-device", "usb-storage,drive=stick,removable=on,bootindex=1"]
    command.extend(firmware_options(output, "bios", "syslinux"))
    (output / "qemu-command.json").write_text(json.dumps(command, indent=2) + "\n")
    with (output / "qemu.log").open("wb") as log:
        process = subprocess.Popen(command, env=env, stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, stderr=log, bufsize=0)
        try:
            assert "QMP" in qmp_read(process)
            qmp_command(process, "qmp_capabilities")
            deadline = time.monotonic() + BOOT_TIMEOUT_SECONDS
            screen = ""
            while process.poll() is None and time.monotonic() < deadline:
                screen = read_screen(process, output)
                if re.search(r"^boot:\s*$", screen, re.MULTILINE):
                    break
                time.sleep(.1)
            (output / "prompt.txt").write_text(screen + "\n")
            qmp_command(process, "screendump", {"filename": str(output / "prompt.png"), "format": "png"})
            assert f"SYSLINUX {version}" in screen, screen
            missing_config = "ERROR" if version == "4.07" else "WARNING"
            assert f"{missing_config}: No configuration file found" in screen, screen
            assert re.search(r"^boot:\s*$", screen, re.MULTILINE), screen
            qmp_command(process, "quit")
            assert process.wait(timeout=5) == 0
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
            process.stdin.close()
            process.stdout.close()
    assert digest(image) == before, "Firmware boot changed the verified backing image"
    return before


def qualify(args, root, fat_bits):
    directory = root / f"fat{fat_bits}"
    directory.mkdir()
    env = dict(os.environ, LC_ALL="C", MTOOLSRC="/dev/null", TMPDIR=str(directory))
    image = directory / "standalone.img"
    disk_bytes = 64 * MIB
    cluster = 2048 if fat_bits == 16 else 512
    with image.open("wb") as stream:
        stream.truncate(disk_bytes)
    layout = partition_mbr(args.main_build / "mbr_image_probe", image, disk_bytes, cluster, file_system=fat_bits)
    offset, size = layout["OFFSET"], layout["SIZE"]
    formatter = argparse.Namespace(probe=args.main_build / "fat_format_probe",
                                  mkfs=shutil.which("mkfs.fat"), fat_bits=fat_bits)
    format_fat(formatter, image, offset, size, cluster, label="RUFUS_TEST")
    with image.open("rb") as stream:
        prefix = stream.read(offset)
        boot = stream.read(SECTOR_BYTES)
        stream.seek(offset + size)
        tail = stream.read()
    spec = f"{image}@@{offset}"
    listing_command = ["mdir", "-a", "-b", "-/", "-i", spec, "::/"]
    assert not run(listing_command, env).strip(), "Probe requires an empty filesystem"
    tools = directory / "tools"
    tools_directory(tools)
    probe = args.probe or args.main_build / "syslinux_install_probe"
    adapter = install(probe, args.installer, None, image, tools, directory, env,
                      offset=offset, size=size, cluster=cluster, fat_bits=fat_bits, major=args.major)
    listing = run(listing_command, env).decode().splitlines()
    names = {name.removeprefix("::/").lower() for name in listing}
    expected = {"ldlinux.sys"} if args.major == 4 else {"ldlinux.sys", "ldlinux.c32"}
    assert names == expected, names
    module = args.upstream / "bios/com32/elflink/ldlinux/ldlinux.c32"
    if args.major == 6:
        check_files(spec, directory, env, {"/ldlinux.c32": module})
    loader = directory / "installed.sys"
    run(["mcopy", "-i", spec, "::/ldlinux.sys", loader], env)
    original = args.upstream / ("core/ldlinux.sys" if args.major == 4 else "bios/core/ldlinux.sys")
    padded = (original.stat().st_size + SECTOR_BYTES - 1) // SECTOR_BYTES * SECTOR_BYTES
    assert loader.stat().st_size == padded + 2 * SECTOR_BYTES
    attributes = run(["mattrib", "-i", spec] + [f"::/{name}" for name in sorted(expected)], env).decode()
    for line in attributes.splitlines():
        flags, name = line.split("::/", 1)
        assert {"R", "H", "S"} <= set(flags), (name, flags)
    assert len(attributes.splitlines()) == len(expected)
    check_volume(image, directory, offset, size, boot, env, fat_bits)
    inspect_mbr(image, disk_bytes, cluster, file_system=fat_bits)
    with image.open("rb") as stream:
        assert stream.read(offset) == prefix, "Installer changed MBR/alignment gap"
        stream.seek(offset + size)
        assert stream.read() == tail, "Installer changed the trailing gap"
    assert not list(directory.rglob("syslinux-mtools-*")), "Installer left temporary configuration"
    output = directory / "bios"
    output.mkdir()
    version = "4.07" if args.major == 4 else "6.04"
    image_hash = boot_prompt(image, output, env, version)
    result = {"version": version, "fat_bits": fat_bits, "disk_bytes": disk_bytes, "disk_sha256": image_hash,
              "loader_bytes": loader.stat().st_size, "loader_sha256": digest(loader),
              "module_sha256": digest(module) if args.major == 6 else None, "files": sorted(names), "attributes": attributes,
              "adapter": adapter, "passed": True}
    (output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    if args.evidence:
        shutil.copytree(output, args.evidence / directory.name, ignore=shutil.ignore_patterns("*.bin"))
    shutil.rmtree(directory)
    print(f"PASS Syslinux {version} FAT{fat_bits}: {len(expected)} boot files, no configuration/kernel, BIOS boot prompt.", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("installer", type=Path)
    parser.add_argument("upstream", type=Path)
    parser.add_argument("main_build", type=Path)
    parser.add_argument("--major", type=int, choices=(4, 6), default=6)
    parser.add_argument("--probe", type=Path, help="Override the checked adapter probe")
    parser.add_argument("--evidence", type=Path, help="Retain compact logs/screenshots, never media")
    args = parser.parse_args()
    if args.evidence:
        args.evidence.mkdir(parents=True, exist_ok=False)
    with tempfile.TemporaryDirectory(prefix="rufus-syslinux-standalone-") as temporary:
        for fat_bits in (16, 32):
            qualify(args, Path(temporary), fat_bits)
        if args.major == 4:
            env = dict(os.environ, LC_ALL="C", TMPDIR=temporary, MTOOLSRC="/dev/null")
            check_unsupported(args.installer, Path(temporary), env)
    print("Two standalone boot probes passed; disposable media removed.", flush=True)


if __name__ == "__main__":
    main()
