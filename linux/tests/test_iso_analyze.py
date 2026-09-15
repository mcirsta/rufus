# SPDX-License-Identifier: GPL-3.0-or-later
"""Run the complete shared ISO9660 metadata operation on disposable file images."""
import argparse
from pathlib import Path
import subprocess
import tempfile

from test_grub_image import payload as grub
from test_syslinux_scan import banner as syslinux
from test_efi_image import banner as efi


def make_image(directory, name, entries, bsdtar, label="Demo-Live-x86_64"):
    tree = directory / name
    tree.mkdir()
    for path, data in entries.items():
        file = tree / path.lstrip("/")
        file.parent.mkdir(parents=True, exist_ok=True)
        if isinstance(data, str):
            file.symlink_to(data)
        else:
            file.write_bytes(data)
    image = directory / f"{name}-ș.iso"
    subprocess.run([bsdtar, "--format", "iso9660", "--options", f"iso9660:volume-id={label}",
                    "-cf", str(image), "-C", str(tree), "."],
                   check=True, capture_output=True, timeout=10)
    # libarchive normalizes the PVD label. Supply a controlled mixed-case label
    # here so finalization and GRUB's label-derived build suffix are exercised.
    data = bytearray(image.read_bytes())
    data[16 * 2048 + 40:16 * 2048 + 72] = label.encode("ascii").ljust(32, b" ")
    image.write_bytes(data)
    return image


def analyze(executable, image, status=0, pending=0, mode="rr", fault="none", **expected):
    result = subprocess.run([str(executable), str(image), mode, fault],
                            capture_output=True, text=True, timeout=10)
    assert result.returncode == status, (image.name, fault, result.returncode, result.stdout, result.stderr)
    values = dict(line.split("=", 1) for line in result.stdout.splitlines())
    expected.update(STATUS=status, PENDING=pending)
    if status in (1, 2):
        expected.update(CLEARED=1, TREE=0)
        assert set(values) == {"STATUS", "PENDING", "TREE", "READS", "CLEARED"}, values
    for key, value in expected.items():
        assert values[key] == str(value), (image.name, fault, key, values, result.stderr)
    return values, result.stderr


def check_combined(executable, directory, bsdtar):
    entries = {
        "/isolinux/isolinux.cfg": b"default linux\n",
        "/isolinux/isolinux.bin": syslinux("6.04 pre1"),
        "/isolinux/ldlinux.c32": bytes(512),
        "/boot/grub/i386-pc/normal.mod": grub("2.06"),
        "/EFI/BOOT/grubx64.efi": grub("2.12", filesystems=("fat", "ntfs", "fat")),
        "/EFI/vendor/grubaa64.efi": grub("2.14", filesystems=("exfat", "ext2")),
        "/EFI/BOOT/bootx64.efi": efi(b"#### LoaderInfo: systemd-boot 261.3 ####\0"),
        "/firmware/blob": bytes(4097), "/firmware/link": "blob", "/empty": b"",
    }
    image = make_image(directory, "combined", entries, bsdtar)
    blocks = sum((len(data) + 2047) // 2048 for data in entries.values() if not isinstance(data, str))
    values, logs = analyze(executable, image, status=3, pending=32, TREE=1, ISO=1,
                          LABEL="Demo-Live-x86_64", FILES=len(entries), BLOCKS=blocks,
                          EXTRA=3, PROJECTED=int(blocks * 2048 * 1.0099999904632568),
                          SIZE=image.stat().st_size, MISMATCH=0, PRESENT=1,
                          CONFIG="/isolinux/isolinux.cfg", DETECTED=0x0604,
                          DETECTED_EXT="/pre1", SELECTED=0x0604, INFERRED=0,
                          GRUB=129, GRUB_VERSION="2.06-demo", GRUB_FS=7, READS=5)
    assert "Detected systemd-boot version: 261.3" in logs, logs
    # Abort with path arrays, reader buffers and GRUB names live. The first
    # failed read remains a partial diagnostic; later stages still run.
    for point in (1, 2, 5, 15):
        analyze(executable, image, status=2, fault=f"cancel:{point}")
    for point in range(1, int(values["READS"]) + 1):
        analyze(executable, image, status=2, fault=f"cancel-read:{point}", READS=point)
        analyze(executable, image, status=3, pending=36, fault=f"fail-read:{point}", READS=5)
    for point in (1, 2, 3):
        analyze(executable, image, status=1, fault=f"fail-array:{point}")
    analyze(executable, image, status=1, fault="fail-fs")
    analyze(executable, image, status=3, pending=32, GRUB_FS=7)
    return image


def check_variants(executable, directory, bsdtar):
    for name, loader, selected, detected, inferred in (
        ("known", syslinux("4.07 2013-07-25"), 0x0407, 0x0407, 0),
        ("unknown", bytes(512), 0x0407, 0, 1),
        ("unreadable", b"", 0x0407, 0, 1),
    ):
        image = make_image(directory, name, {
            "/MiXeD/IsOlInUx.CfG": b"default linux\n", "/MiXeD/IsOlInUx.BiN": loader,
        }, bsdtar)
        partial = name == "unreadable"
        for mode, config in (("rr", "/MiXeD/IsOlInUx.CfG"), ("joliet", "/MiXeD/IsOlInUx.CfG"),
                             ("plain", "/mixed/isolinux.cfg")):
            analyze(executable, image, status=3 if partial else 0, pending=4 if partial else 0,
                    mode=mode, TREE=1, ISO=1, CONFIG=config, PRESENT=1,
                    DETECTED=detected, SELECTED=selected, INFERRED=inferred)
        analyze(executable, image, status=3 if partial else 0, pending=4 if partial else 0,
                mode="no-fallback", DETECTED=detected, SELECTED=detected, INFERRED=0)
    for name, label, entries, pending, expected in (
        ("empty-tree", "EMPTY", {}, 0, {"FILES": 0, "PROJECTED": 0}),
        ("spaces", "Trailing spaces  ", {"/data": b"data"}, 0, {"LABEL": "Trailing spaces"}),
        ("grub2", "Demo-Live-x86_64", {"/boot/grub2/i386-pc/normal.mod": grub("2.06", debug=True)},
         0, {"GRUB": 2, "GRUB_VERSION": "2.06-nonstandard-gdie-demo"}),
        ("unknown-grub", "DEMO", {"/boot/grub/i386-pc/normal.mod": bytes(512)},
         0, {"GRUB": 0, "GRUB_VERSION": ""}),
        ("efi-grub", "DEMO", {"/EFI/BOOT/grubx64.efi": grub("2.12", filesystems=("fat",))},
         32, {"GRUB": 128, "GRUB_VERSION": "2.12", "GRUB_FS": 1, "READS": 1}),
        ("empty-grub", "DEMO", {"/boot/grub/i386-pc/normal.mod": b""},
         4, {"GRUB": 0, "GRUB_VERSION": ""}),
        ("config-only", "DEMO", {"/syslinux.cfg": b"default linux"},
         0, {"PRESENT": 1, "DETECTED": 0, "SELECTED": 0x0407, "INFERRED": 1}),
        ("embedded-efi", "DEMO", {"/boot/efi.img": bytes(512)},
         8, {"EFI": 0}),
        ("windows", "DEMO", {"/bootmgr": bytes(512), "/sources/install.wim": bytes(512)},
         16, {"GRUB": 0}),
        ("empty-efi", "DEMO", {"/EFI/BOOT/bootx64.efi": b""},
         44, {"EFI": 0x4008}),
    ):
        image = make_image(directory, name, entries, bsdtar, label)
        analyze(executable, image, status=3 if pending else 0, pending=pending, TREE=1, **expected)


def check_errors(executable, directory, bsdtar, valid_image):
    image = make_image(directory, "deep", {
        "/" + "/".join(f"level{i}" for i in range(10)) + "/syslinux.cfg": b"default linux",
    }, bsdtar)
    analyze(executable, image, status=3, pending=2, TREE=0, ISO=0, PROJECTED=0, READS=0, SELECTED=0)
    for name, path in (("long-path", "/" + "a" * 140 + "/" + "b" * 140 + "/file"),
                       ("long-config", "/" + "a" * 120 + "/syslinux.cfg")):
        image = make_image(directory, name, {path: b"data"}, bsdtar)
        analyze(executable, image, status=1, READS=0)
    for name, data in (("empty", b""), ("text", b"Not an ISO"),
                       ("truncated-pvd", valid_image.read_bytes()[:32768])):
        image = directory / f"{name}.iso"
        image.write_bytes(data)
        analyze(executable, image, status=1, READS=0)
    # Change only the claimed volume size: the valid tree and bootloader bytes
    # remain readable, but the operation must not call this a complete image.
    data = bytearray(valid_image.read_bytes())
    blocks = int.from_bytes(data[16 * 2048 + 80:16 * 2048 + 84], "little") + 100
    data[16 * 2048 + 80:16 * 2048 + 88] = blocks.to_bytes(4, "little") + blocks.to_bytes(4, "big")
    image = directory / "size-mismatch.iso"
    image.write_bytes(data)
    analyze(executable, image, status=3, pending=96, MISMATCH=204800)
    image = directory / "trailing-data.iso"
    image.write_bytes(valid_image.read_bytes() + b"trailing data")
    analyze(executable, image, status=3, pending=32, MISMATCH=-13)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--bsdtar", default="bsdtar")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="rufus-iso-analysis-") as temporary:
        directory = Path(temporary)
        image = check_combined(args.executable, directory, args.bsdtar)
        check_variants(args.executable, directory, args.bsdtar)
        check_errors(args.executable, directory, args.bsdtar, image)
    print("Shared ISO9660 operation: metadata/content, filesystem views, partial reasons,")
    print("cancellation, failure cleanup and ordinary-file fixtures pass.")


if __name__ == "__main__":
    main()
