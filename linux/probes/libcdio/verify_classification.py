# SPDX-License-Identifier: GPL-3.0-or-later
"""Test shared file rules using real ISO directory records, without mounts."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def make_image(directory, name, entries, bsdtar):
    tree = directory / name
    tree.mkdir()
    for path, size in entries.items():
        file = tree / path.lstrip("/")
        file.parent.mkdir(parents=True, exist_ok=True)
        if isinstance(size, str):
            file.symlink_to(size)
        else:
            file.write_bytes(bytes(size))
    image = directory / f"{name}-ș.iso"
    subprocess.run([bsdtar, "--format", "iso9660", "-cf", str(image),
                    "-C", str(tree), "."], check=True, capture_output=True, timeout=10)
    return image


def classify(executable, image, paths, expected_code=0):
    result = subprocess.run([str(executable), str(image), *paths],
                            capture_output=True, text=True, timeout=10)
    if result.returncode != expected_code:
        raise RuntimeError(f"{image.name}: {result.returncode}\n{result.stdout}\n{result.stderr}")
    values, files, boot = {}, {}, []
    for line in result.stdout.splitlines():
        key, value = line.split("=", 1)
        if key == "FILE":
            path, size, flags = value.split("\t")
            files[path] = (int(size), int(flags))
        elif key == "BOOT":
            boot.append(value)
        else:
            values[key] = value
    return values, files, boot


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--bsdtar", default="bsdtar")
    args = parser.parse_args()
    executable = args.executable.resolve()
    with tempfile.TemporaryDirectory(prefix="rufus-file-rules-") as temporary:
        directory = Path(temporary)
        entries = {
            "/EFI/BOOT/BoOtX64.EfI": 512, "/EFI/Fedora/grubx64.efi": 512,
            "/BootMgr.Efi": 512, "/bootmgr": 512,
            "/isolinux/isolinux.cfg": 24, "/isolinux/menu.c32": 53500,
            "/isolinux/vesamenu.c32": 148001, "/isolinux/ldlinux.c32": 512,
            "/isolinux/isolinux.bin": 512, "/Sources/Install.WIM": 4096,
            "/casper-pop-os/vmlinuz": 512, "/ReactOS/FreeLdr.SYS": 512,
            "/loader/entries/arch.conf": 24,
            "/firmware/blob": 512, "/firmware/link": "blob",
        }
        image = make_image(directory, "mixed", entries, args.bsdtar)
        values, files, boot = classify(executable, image, entries)
        expected_flags = {
            "/isolinux/isolinux.cfg": 9, "/isolinux/menu.c32": 32,
            "/isolinux/ldlinux.c32": 4, "/isolinux/isolinux.bin": 2,
            "/loader/entries/arch.conf": 16,
        }
        assert files == {path: (0 if isinstance(size, str) else size, expected_flags.get(path, 0))
                         for path, size in entries.items()}, files
        assert values == {
            "EFI": "9", "BOOTMGR": "1", "BOOTMGR_EFI": "1", "GRUB": "0",
            "CASPER": "1", "DISABLE_ISO": "1", "WININST": "1",
            "WIM0": "?:/Sources/Install.WIM", "REACTOS": "/ReactOS/FreeLdr.SYS", "EFI_IMG": "",
            "LINK": "blob",
        }, values
        assert boot == ["0\t/EFI/BOOT/BoOtX64.EfI", "1\t/EFI/Fedora/grubx64.efi", "3\t/BootMgr.Efi"], boot
        classify(executable, image, ["/missing"], 3)
        classify(executable, image, ["/EFI"], 4)

        entries = {"/boot/grub2/i386-pc/normal.mod": 512, "/EFI/BOOT/bootx64.efi": 255}
        small = make_image(directory, "small-efi", entries, args.bsdtar)
        values, files, boot = classify(executable, small, entries)
        assert values["GRUB"] == "2" and values["EFI"] == str(0x4008), values
        assert values["EFI_IMG"] == "[BOOT]/1-Boot-NoEmul.img", values

        for name, content in (("empty", b""), ("plain", b"Not an ISO"),
                              ("truncated", image.read_bytes()[:32768])):
            invalid = directory / f"{name}.iso"
            invalid.write_bytes(content)
            classify(executable, invalid, ["/bootmgr"], 1)
        print("ISO fixtures: 17 file entries; case, size, properties, report, symlink and ordinary errors pass.")
        print("Fixture lookup only; full traversal, UDF and boot validation are not covered.")


if __name__ == "__main__":
    main()
