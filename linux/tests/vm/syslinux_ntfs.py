#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Investigate unmodified extlinux on NTFS in an offline, disposable USB guest."""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys

import prepare
from fat_extract import add_executable, digest, run
from test_fat_format import copy_volume
from test_mbr_image import invoke as partition_mbr, inspect_mbr, ROOT
from test_ntfs_format import invoke as format_ntfs
from test_iso_extract import make_image
from test_syslinux_scan import scan

sys.path.insert(0, str(ROOT / "linux/probes/syslinux"))
from test_standalone import boot_prompt
from verify import boot as boot_configured

MIB = 1024 * 1024
DISK_BYTES = 64 * MIB
CLUSTER_BYTES = 4096
SECTOR_BYTES = 512
GUEST_TIMEOUT_SECONDS = 600
DRIVERS = ("ntfs", "lowntfs-3g")


def make_iso(args, base):
    fixture = base / "fixture.bs"
    run(["nasm", "-f", "bin", ROOT / "linux/tests/vm/boot.asm", "-o", fixture])
    prefix = "boot/x86_64/loader/"
    config = (b"SERIAL 0 115200\nDEFAULT checkpoint\nPROMPT 0\nTIMEOUT 1\n"
              b"LABEL checkpoint\n  BOOT fixture.bs\n  APPEND root=live:CDLABEL=RUFUS_SOURCE\n")
    module = (args.upstream / "bios/com32/elflink/ldlinux/ldlinux.c32").read_bytes()
    entries = {prefix + "isolinux.cfg": config,
               prefix + "isolinux.bin": (args.upstream / "bios/core/isolinux.bin").read_bytes(),
               prefix + "ldlinux.c32": module, prefix + "fixture.bs": fixture.read_bytes(),
               "ldlinux.c32": module, "ldlinux.sys": b"Source loader must not be copied.\n",
               "syslinux.cfg": b"DEFAULT wrong-root-config\n",
               "keep.txt": b"Ordinary ISO contents must survive installation.\n" * 257}
    image = make_image(base, "RUFUS_SOURCE", entries, args.mkisofs)
    scan(args.build / "syslinux_scan_probe", image, "/" + prefix + "isolinux.cfg",
         0x0604, "/pre1", selected=0x0604, overwrite=True)
    # Independent expectations for Rufus's shared skip/patch/backup/redirect.
    del entries["ldlinux.sys"]
    entries[prefix + "isolinux.cfg"] = config.replace(b"RUFUS_SOURCE", b"RUFUS_EXT")
    entries["syslinux.org"] = entries["syslinux.cfg"]
    entries["syslinux.cfg"] = (b"DEFAULT loadconfig\n\nLABEL loadconfig\n"
                               b"  CONFIG /boot/x86_64/loader/isolinux.cfg\n"
                               b"  APPEND /boot/x86_64/loader/\n")
    expected = base / "iso-expected"
    expected.mkdir()
    for name, data in entries.items():
        path = expected / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    return image


def make_disk(args, base, driver):
    disk = base / (driver + ".raw")
    with disk.open("xb") as output:
        output.truncate(DISK_BYTES)
    layout = partition_mbr(args.build / "mbr_image_probe", disk, DISK_BYTES,
                           CLUSTER_BYTES, file_system="ntfs", non_bootable=True)
    inspect_mbr(disk, DISK_BYTES, CLUSTER_BYTES, file_system="ntfs", non_bootable=True)
    # This adapter-only fixture seeds the original MBR before installation.
    # CP20l separately qualifies production layout creation through the helper.
    header = (ROOT / "src/ms-sys/inc/mbr_syslinux.h").read_text().split("{", 1)[1].split("}", 1)[0]
    bootstrap = bytes(int(value, 16) for value in re.findall(r"0x([0-9a-fA-F]{2})\b", header))
    assert len(bootstrap) == 440
    with disk.open("r+b") as output:
        output.write(bootstrap)
        output.write(struct.pack("<I", 0x78563412))
        output.seek(446)
        output.write(b"\x80")
    inspect_mbr(disk, DISK_BYTES, CLUSTER_BYTES, file_system="ntfs")
    volume = base / "volume.raw"
    with volume.open("xb") as output:
        output.truncate(layout["SIZE"])
    formatter = argparse.Namespace(probe=args.build / "ntfs_format_probe", mkfs=shutil.which("mkntfs"))
    formatted = format_ntfs(formatter, volume, layout["SIZE"], layout["OFFSET"], CLUSTER_BYTES, "RUFUS_EXT")
    with disk.open("r+b") as output, volume.open("rb") as source:
        output.seek(layout["OFFSET"])
        shutil.copyfileobj(source, output, length=MIB)
    volume.unlink()
    with disk.open("rb") as source:
        prefix = source.read(layout["OFFSET"])
        boot = source.read(SECTOR_BYTES)
        source.seek(layout["OFFSET"] + layout["SIZE"])
        tail = source.read()
    return {"layout": layout, "formatter": formatted, "prefix": prefix, "boot": boot, "tail": tail}


def make_guest(args, base):
    root = base / "root"
    root.mkdir()
    (root / "lib").symlink_to("usr/lib")
    prepare.bootstrap(base, root, args.kernel)
    init = base / "initramfs"
    (init / "target").mkdir()
    (init / "tmp").mkdir(mode=0o1777)
    add_executable(init, args.installer)
    add_executable(init, args.probe_build / "syslinux_extlinux_facts")
    if args.adapter:
        add_executable(init, args.build / "syslinux_ntfs_probe")
        prepare.write(init, "etc/rufus-extlinux-adapter", "checked\n")
        prepare.write(init, "tmp/fail-installer", "#!/bin/bash\nexit 19\n", 0o700)
        prepare.write(init, "tmp/wait-installer",
                      "#!/bin/bash\ntrap '' TERM\n: > /tmp/extlinux-ready\nwhile :; do sleep 1; done\n", 0o700)
    if args.iso:
        add_executable(init, args.build / "iso_extract_probe")
        shutil.copy2(base / "RUFUS_SOURCE.iso", init / "source.iso")
        prepare.write(init, "etc/rufus-extlinux-iso", "extract-before-install\n")
    for name in ("umount", "sync", "stat", "sha256sum", "cp", "readlink"):
        add_executable(init, Path("/usr/bin") / name)
    for package in (args.ntfs_3g_package, args.ntfs_library_package):
        run(["bsdtar", "-xpf", str(package), "--no-same-owner", "--no-xattrs",
             "--exclude=.PKGINFO", "--exclude=.BUILDINFO", "--exclude=.MTREE", "--exclude=.INSTALL", "-C", str(init)])
    shutil.copy2("/usr/lib/libfuse.so.2", init / "usr/lib/libfuse.so.2")
    shutil.copytree("/usr/lib/locale/C.utf8", init / "usr/lib/locale/C.utf8")
    shutil.copy2(args.upstream / "bios/core/ldlinux.sys", init / "loader.bin")
    kernel = Path("/usr/lib/modules") / args.kernel
    for name in ("ntfs", "fuse", "nls_utf8"):
        modules = run(["modprobe", "--show-depends", "-S", args.kernel, name], capture_output=True, text=True).stdout
        for line in modules.splitlines():
            if line.startswith("insmod "):
                module = Path(line.split()[1]).resolve()
                destination = init / "usr/lib/modules" / args.kernel / module.relative_to(kernel)
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(module, destination)
    run(["depmod", "-b", str(init), args.kernel])
    prepare.write(init, "etc/rufus-extlinux-fixture", "extlinux-ntfs\n")
    prepare.write(init, "init", (prepare.HERE / "guest-syslinux-ntfs").read_text(), 0o755)
    names = run(["find", ".", "-print0"], cwd=init, capture_output=True).stdout
    with (base / "initramfs.cpio").open("wb") as output:
        run(["cpio", "--null", "-o", "-H", "newc", "--owner=0:0"], cwd=init, input=names, stdout=output)


def readback(args, base, driver, expected, result, disk=None, uefi_payload=0):
    disk = disk or base / (driver + ".raw")
    offset, size = expected["layout"]["OFFSET"], expected["layout"]["SIZE"]
    signature = struct.unpack_from("<I", expected["prefix"], 440)[0]
    inspect_mbr(disk, DISK_BYTES, CLUSTER_BYTES, signature=signature, file_system="ntfs",
                uefi_payload=uefi_payload, boot_code="syslinux")
    with disk.open("rb") as source:
        assert source.read(offset) == expected["prefix"]
        boot = source.read(SECTOR_BYTES)
        assert boot[3:84] == expected["boot"][3:84], "NTFS BPB changed"
        source.seek(offset + size - SECTOR_BYTES)
        assert source.read(SECTOR_BYTES) == expected["boot"], "Backup boot sector changed"
        source.seek(offset + size)
        assert source.read() == expected["tail"]
    volume = base / "volume.raw"
    copy_volume(disk, volume, offset, size)
    checked = subprocess.run(["ntfsinfo", "-m", str(volume)], capture_output=True, text=True, timeout=20)
    (base / (driver + "-ntfsinfo-original.log")).write_text(checked.stdout + checked.stderr)
    if checked.returncode != 0:
        assert "Boot sector: Bad jump." in checked.stderr + checked.stdout, checked
        # Only the disposable readback copy: isolate this checker's rejection
        # of the original Syslinux jump. BIOS boots use the untouched disk.
        with volume.open("r+b") as output:
            output.write(expected["boot"][:3])
    info = run(["ntfsinfo", "-m", str(volume)], capture_output=True, text=True).stdout
    (base / (driver + "-ntfsinfo-metadata.log")).write_text(info)
    assert "Volume Flags: 0x0000" in info, info
    # 7-Zip reads the actual Syslinux boot sector and NTFS file records without
    # that checker's jump restriction. Restore the copy before independent reads.
    with volume.open("r+b") as output:
        output.write(boot[:3])
    (base / (driver + "-boot.bin")).write_bytes(boot)
    listing = run(["7z", "l", "-slt", str(volume)], capture_output=True, text=True).stdout
    (base / (driver + "-files.log")).write_text(listing)
    attributes = {}
    for record in listing.split("\n\n"):
        fields = dict(line.split(" = ", 1) for line in record.splitlines() if " = " in line)
        if fields.get("Path") in ("ldlinux.sys", "ldlinux.c32"):
            attributes[fields["Path"]] = fields["Attributes"]
    if args.adapter:
        assert set(attributes) == {"ldlinux.sys", "ldlinux.c32"}, attributes
        assert all(flag in attributes["ldlinux.sys"] for flag in "RHS"), attributes
        assert all(flag not in attributes["ldlinux.c32"] for flag in "RHS"), attributes
    files = {}
    for name in ("ldlinux.sys", "ldlinux.c32"):
        extracted = subprocess.run(["7z", "x", "-so", str(volume), name], capture_output=True, timeout=20)
        if extracted.returncode == 0:
            target = base / (driver + "-" + name)
            target.write_bytes(extracted.stdout)
            files[name] = {"size": target.stat().st_size, "sha256": digest(target),
                           "attributes": attributes[name]}
    iso_files = {}
    if getattr(args, "iso", False):
        for source in sorted((base / "iso-expected").rglob("*")):
            if not source.is_file():
                continue
            name = source.relative_to(base / "iso-expected").as_posix()
            extracted = run(["7z", "x", "-so", str(volume), name], capture_output=True).stdout
            assert extracted == source.read_bytes(), (driver, name)
            iso_files[name] = {"size": source.stat().st_size, "sha256": digest(source)}
    volume.unlink()
    if result == 0:
        assert set(files) == {"ldlinux.sys", "ldlinux.c32"}, files
        assert files["ldlinux.sys"]["size"] == 69632
        assert files["ldlinux.c32"]["sha256"] == digest(args.upstream / "bios/com32/elflink/ldlinux/ldlinux.c32")
        bios = base / (driver + "-bios")
        bios.mkdir()
        environment = dict(os.environ, LC_ALL="C", TMPDIR=str(bios))
        if iso_files:
            boot_configured(disk, environment)
        else:
            boot_prompt(disk, bios, environment)
    return {"installer_exit": result, "files": files, "disk_sha256": digest(disk), "bare_bios_boot": not iso_files and result == 0,
            "iso_files": iso_files, "configured_bios_boot": bool(iso_files) and result == 0,
            "ntfsinfo_original_exit": checked.returncode, "readback_copy_jump_normalized": checked.returncode != 0}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("build", "probe-build", "upstream", "ntfs-3g-package", "ntfs-library-package"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--kernel", required=True)
    parser.add_argument("--installer", type=Path, help="Packaged rufus-extlinux (defaults to the main build)")
    parser.add_argument("--adapter", action="store_true", help="Qualify the checked adapter and its fault matrix")
    parser.add_argument("--iso", action="store_true", help="Extract an ISO before installation; combine with --adapter to check image jobs")
    parser.add_argument("--mkisofs", type=Path, help="ISO fixture builder, required for --iso")
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    for name in ("build", "probe_build", "upstream", "ntfs_3g_package", "ntfs_library_package"):
        setattr(args, name, getattr(args, name).resolve(strict=True))
    args.installer = (args.installer or args.build / "syslinux/rufus-extlinux").resolve(strict=True)
    if args.installer.name != "rufus-extlinux":
        parser.error("Installer must be named rufus-extlinux")
    if args.iso and not args.mkisofs:
        parser.error("--iso requires --mkisofs")
    base = args.directory.resolve()
    if any(character in str(base) for character in ",\n\r"):
        parser.error("Output path must not contain QEMU option separators")
    base.mkdir()
    if args.iso:
        make_iso(args, base)
    expected = {driver: make_disk(args, base, driver) for driver in DRIVERS}
    make_guest(args, base)
    inputs = [args.installer, args.probe_build / "syslinux_extlinux_facts",
              base / "vmlinuz", base / "initramfs.cpio", args.ntfs_3g_package, args.ntfs_library_package]
    if args.adapter:
        inputs.append(args.build / "syslinux_ntfs_probe")
    if args.iso:
        inputs.extend([args.build / "iso_extract_probe", args.build / "syslinux_scan_probe", base / "RUFUS_SOURCE.iso"])
    manifest = {"kernel": args.kernel, "accel": "tcg", "disk_bytes": DISK_BYTES,
                "inputs": {str(path): digest(path) for path in inputs},
                "layouts": {driver: data["layout"] for driver, data in expected.items()}}
    (base / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    serial = base / "serial.log"
    command = ["qemu-system-x86_64", "-accel", "tcg", "-cpu", "max", "-m", "512", "-nic", "none",
               "-display", "none", "-monitor", "none", "-serial", "file:" + str(serial), "-no-reboot",
               "-kernel", str(base / "vmlinuz"), "-initrd", str(base / "initramfs.cpio"),
               "-append", "console=ttyS0 panic=-1", "-smbios", "type=1,product=RufusExtlinuxFixture",
               "-device", "qemu-xhci,id=xhci"]
    for index, driver in enumerate(DRIVERS):
        command.extend(["-drive", f"if=none,id=stick{index},format=raw,file={base / (driver + '.raw')}",
                        "-device", f"usb-storage,drive=stick{index},removable=on,serial=RUFUS-EXTLINUX-{index}"])
    (base / "qemu-command.json").write_text(json.dumps(command, indent=2) + "\n")
    with (base / "qemu.log").open("wb") as output:
        subprocess.run(command, check=True, timeout=GUEST_TIMEOUT_SECONDS, stdout=output, stderr=subprocess.STDOUT)
    text = serial.read_text(errors="replace")
    assert "RUFUS_EXTLINUX_DONE" in text and "RUFUS_EXTLINUX_EXIT 0" in text, serial
    if args.adapter:
        cases = re.findall(r"RUFUS_NTFS_ADAPTER (\S+) (PASS|FAIL)", text)
        expected_cases = 166 if args.iso else 118
        assert len(cases) == expected_cases and all(status == "PASS" for _, status in cases), cases
        attributes = re.findall(r"RUFUS_NTFS_ATTRIBUTES (\S+) (PASS|FAIL) value=([a-f0-9]+)", text)
        assert len(attributes) == 8 and all(status == "PASS" for _, status, _ in attributes), attributes
    exits = dict(re.findall(r"RUFUS_EXTLINUX_INSTALL (\S+) RESULT=(\d+)", text))
    assert set(exits) == set(DRIVERS), exits
    assert all(int(result) == 0 for result in exits.values()), exits
    if args.iso:
        extracts = re.findall(r"RUFUS_EXTLINUX_ISO (\S+) PASS", text)
        assert extracts == list(DRIVERS), extracts
        source_hashes = re.findall(r"([a-f0-9]{64})  /source.iso", text)
        assert source_hashes == [digest(base / "RUFUS_SOURCE.iso")] * 4, source_hashes
    remounts = dict(re.findall(r"RUFUS_EXTLINUX_KERNEL_REMOUNT (\S+) RESULT=(\d+)", text))
    assert remounts == dict.fromkeys(DRIVERS, "0"), remounts
    results = {driver: readback(args, base, driver, expected[driver], int(exits[driver])) for driver in DRIVERS}
    for driver in DRIVERS:
        section = re.search(r"RUFUS_EXTLINUX_BEGIN " + driver + r"\s+(.*?)RUFUS_EXTLINUX_END " + driver, text, re.S)
        hashes = {name: digest for digest, name in re.findall(r"([a-f0-9]{64})  /target/(ldlinux\.(?:sys|c32))", section[1])}
        assert hashes == {name: data["sha256"] for name, data in results[driver]["files"].items()}, driver
    for path in inputs:
        assert digest(path) == manifest["inputs"][str(path)]
    (base / "results.json").write_text(json.dumps(results, indent=2) + "\n")
    # Retain compact evidence; source packages and the user's ISOs are untouched.
    for driver in DRIVERS:
        (base / (driver + ".raw")).unlink()
    for name in ("root", "initramfs"):
        shutil.rmtree(base / name)
    for name in ("vmlinuz", "initramfs.cpio"):
        (base / name).unlink()
    for path in base.glob("*-bios/*.bin"):
        path.unlink()
    if args.iso:
        shutil.rmtree(base / "RUFUS_SOURCE")
        shutil.rmtree(base / "iso-expected")
        (base / "RUFUS_SOURCE.iso").unlink()
        (base / "fixture.bs").unlink()
    print(json.dumps(results, indent=2), flush=True)


if __name__ == "__main__":
    main()
