#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""FreeDOS files/attributes on real kernel FAT16/FAT32 in a disposable USB guest."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys

import prepare
from fat_extract import add_executable, digest, run

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from test_freedos_copy import PAYLOADS, TOTAL, values


def make_guest(args, base):
    root = base / "root"
    root.mkdir()
    (root / "lib").symlink_to("usr/lib")
    prepare.bootstrap(base, root, args.kernel)
    init = base / "initramfs"
    (init / "target").mkdir()
    for binary in (args.build / "freedos_copy_probe", Path("/usr/bin/umount")):
        add_executable(init, binary)
    prepare.write(init, "init", '''#!/bin/bash
set -eu
[[ $$ == 1 ]] || exit 77
export PATH=/usr/bin LC_ALL=C
mount -t devtmpfs devtmpfs /dev
mount -t proc proc /proc
mount -t sysfs sysfs /sys
finish() {
    result=$?
    trap - EXIT
    echo "RUFUS_FREEDOS_COPY_EXIT $result"
    echo o > /proc/sysrq-trigger
    while :; do sleep 1; done
}
trap finish EXIT
for module in xhci_pci usb_storage sd_mod vfat nls_cp437 nls_iso8859_1; do modprobe "$module"; done
for attempt in {1..100}; do [[ -b /dev/sdb ]] && break; sleep .1; done
[[ -b /dev/sda && -b /dev/sdb ]]
for device in sda sdb; do
    mount -t vfat -o utf8=1,shortname=mixed,umask=0077,nodev,nosuid,noexec /dev/$device /target
    echo "RUFUS_FREEDOS_COPY_DEVICE $device"
    freedos_copy_probe /target native
    umount /target
done
echo RUFUS_FREEDOS_COPY_DONE
''', 0o755)
    names = run(["find", ".", "-print0"], cwd=init, capture_output=True).stdout
    with (base / "initramfs.cpio").open("wb") as output:
        run(["cpio", "--null", "-o", "-H", "newc", "--owner=0:0"], cwd=init, input=names, stdout=output)


def inspect_volume(base, disk, bits):
    fsck = run(["fsck.fat", "-n", str(disk)], capture_output=True)
    (base / f"fsck-{bits}.log").write_bytes(fsck.stdout + fsck.stderr)
    listing = run(["mdir", "-a", "-i", str(disk), "::/", "::/LOCALE"], capture_output=True)
    (base / f"mdir-{bits}.log").write_bytes(listing.stdout + listing.stderr)
    attributes = run(["mattrib", "-i", str(disk), "::/*", "::/LOCALE/*"], capture_output=True, text=True).stdout
    (base / f"attributes-{bits}.log").write_text(attributes)
    hashes = {}
    for name, expected in PAYLOADS.items():
        data = run(["mtype", "-i", str(disk), "::/" + name], capture_output=True).stdout
        assert data == expected, (bits, name)
        line = next(line for line in attributes.splitlines() if line.endswith("::/" + name))
        flags = line.split("::", 1)[0]
        root = "/" not in name
        assert ("H" in flags) == root and ("S" in flags) == root, line
        hashes[name] = {"bytes": len(data), "attributes": flags.strip()}
    return {"bits": bits, "sha256": digest(disk), "files": hashes}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--kernel", required=True)
    parser.add_argument("--accel", choices=("kvm", "tcg"), default="tcg")
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    args.build = args.build.resolve()
    base = args.directory.resolve()
    if any(ch in str(base) for ch in ",\r\n"):
        parser.error("Output path contains QEMU option separators")
    base.mkdir()
    disks = []
    for bits, mib in ((16, 16), (32, 64)):
        disk = base / f"fat{bits}.raw"
        with disk.open("xb") as output:
            output.truncate(mib * 1024 * 1024)
        command = [str(args.build / "fat_format_probe"), "/usr/bin/mkfs.fat", str(disk), "0",
                   str(disk.stat().st_size), "512", "FREEDOS", "none"]
        if bits == 16:
            command.append("16")
        formatted = run(command, capture_output=True, text=True)
        assert values(formatted.stdout)["ERROR"] == 0, formatted
        (base / f"format-{bits}.log").write_text(formatted.stdout + formatted.stderr)
        disks.append(disk)
    make_guest(args, base)
    serial = base / "serial.log"
    command = ["qemu-system-x86_64", "-accel", args.accel, "-cpu", "host" if args.accel == "kvm" else "max",
               "-m", "512", "-display", "none", "-monitor", "none", "-serial", "file:" + str(serial),
               "-no-reboot", "-nic", "none", "-kernel", str(base / "vmlinuz"), "-initrd", str(base / "initramfs.cpio"),
               "-append", "console=ttyS0 panic=-1 rufus.freedos_copy=1",
               "-smbios", "type=1,manufacturer=RufusTest,product=RufusFreeDosCopyFixture", "-device", "qemu-xhci,id=xhci"]
    for index, disk in enumerate(disks):
        command += ["-drive", f"if=none,id=stick{index},format=raw,file={disk}", "-device",
                    f"usb-storage,drive=stick{index},removable=on,serial=RUFUS-FD-COPY-{index}"]
    (base / "qemu-command.json").write_text(json.dumps(command, indent=2) + "\n")
    input_hashes = {p.name: digest(p) for p in (base / "vmlinuz", base / "initramfs.cpio", args.build / "freedos_copy_probe")}
    with (base / "qemu.log").open("wb") as log:
        subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=180)
    text = serial.read_text(errors="replace")
    assert "RUFUS_FREEDOS_COPY_DONE" in text and "RUFUS_FREEDOS_COPY_EXIT 0" in text, serial
    reports = [values(line) for line in text.splitlines() if line.startswith("ERROR=")]
    assert len(reports) == 2, reports
    for report in reports:
        assert report["ERROR"] == report["SYNC"] == report["FDS"] == 0, report
        assert report["FILES"] == 27 and report["BYTES"] == TOTAL and report["SETS"] == 2, report
        assert report["PROGRESS"] == 5 and report["SYNCS"] == 29 and report["OFFSET"] == report["FLAGS"] == 1
    volumes = [inspect_volume(base, disk, bits) for disk, bits in zip(disks, (16, 32))]
    (base / "manifest.json").write_text(json.dumps({"kernel": args.kernel, "accel": args.accel,
        "inputs": input_hashes, "reports": reports, "volumes": volumes, "media_removed": True}, indent=2) + "\n")
    # The next checkpoint does not need these payload-only, nonbootable volumes.
    for disk in disks:
        disk.unlink()
    for name in ("root", "initramfs"):
        shutil.rmtree(base / name)
    (base / "vmlinuz").unlink()
    (base / "initramfs.cpio").unlink()
    print("Kernel FAT16/FAT32 copying and hidden/system attributes passed; independent readback/fsck passed; media removed.")


if __name__ == "__main__":
    main()
