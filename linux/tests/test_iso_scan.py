# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise Rufus's shared ISO tree scan using generated regular files only."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def make_image(directory, name, entries, bsdtar):
    tree = directory / name
    tree.mkdir()
    for path, content in entries.items():
        file = tree / path.lstrip("/")
        file.parent.mkdir(parents=True, exist_ok=True)
        if isinstance(content, str):
            file.symlink_to(content)
        else:
            file.write_bytes(bytes(content))
    image = directory / f"{name}-ș.iso"
    subprocess.run([bsdtar, "--format", "iso9660", "-cf", str(image),
                    "-C", str(tree), "."], check=True, capture_output=True, timeout=10)
    return image


def scan(executable, image, mode="rr", cancel_after=0, expected_code=0):
    result = subprocess.run([str(executable), str(image), mode, str(cancel_after)],
                            capture_output=True, text=True, timeout=10)
    if result.returncode != expected_code:
        raise AssertionError(f"{image.name}/{mode}/{cancel_after}: {result.returncode}"
                             f"\n{result.stdout}\n{result.stderr}")
    values = {"CONFIG": [], "ISOLINUX": [], "BOOT": []}
    for line in result.stdout.splitlines():
        key, value = line.split("=", 1)
        if key in ("CONFIG", "ISOLINUX", "BOOT"):
            values[key].append(value)
        else:
            assert key not in values, line
            values[key] = value
    if expected_code != 0:
        assert "BLOCKS" not in values and "FILES" not in values, values
    return values


def check_mixed_tree(executable, directory, bsdtar):
    entries = {
        "/EFI/BOOT/BoOtX64.EfI": 512, "/EFI/Fedora/grubx64.efi": 512,
        "/BootMgr.Efi": 512, "/bootmgr": 512,
        "/isolinux/isolinux.cfg": 24, "/isolinux/menu.c32": 53500,
        "/isolinux/vesamenu.c32": 148001, "/isolinux/ldlinux.c32": 512,
        "/isolinux/isolinux.bin": 512, "/Sources/Install.WIM": 4096,
        "/casper/vmlinuz": 512, "/ReactOS/FreeLdr.SYS": 512,
        "/loader/entries/arch.conf": 24, "/boot/grub2/i386-pc/normal.mod": 512,
        "/firmware/blob": 4097, "/firmware/link": "blob", "/live": "casper",
        "/empty": 0, "/" + "long-name-" * 10 + ".txt": 1,
    }
    # Grow both original StrArrays past their initial capacity of eight.
    configs = ["/isolinux/isolinux.cfg"]
    isolinux = ["/isolinux/isolinux.bin"]
    for index in range(10):
        config = f"/variant{index}/syslinux.cfg"
        bootloader = f"/variant{index}/isolinux.bin"
        entries[config] = 23
        entries[bootloader] = 512
        configs.append(config)
        isolinux.append(bootloader)
    image = make_image(directory, "mixed", entries, bsdtar)
    values = scan(executable, image)
    assert sorted(values.pop("CONFIG")) == sorted(configs), values
    assert sorted(values.pop("ISOLINUX")) == sorted(isolinux), values
    assert sorted(values.pop("BOOT")) == sorted([
        "0\t/EFI/BOOT/BoOtX64.EfI", "1\t/EFI/Fedora/grubx64.efi", "3\t/BootMgr.Efi"]), values
    blocks = sum((size + 2047) // 2048 for size in entries.values() if isinstance(size, int))
    assert values == {
        "STATUS": "tree-complete", "ANALYSIS": "partial",
        "FILES": str(len(entries)), "BLOCKS": str(blocks), "EXTRA_BLOCKS": "3",
        "JOLIET": "0", "LDLINUX": "1", "LONG_NAME": "1", "SYMLINKS": "1",
        "NEEDS_NTFS": "1", "EFI": "9", "BOOTMGR": "1", "BOOTMGR_EFI": "1",
        "GRUB": "2", "CASPER": "1", "DISABLE_ISO": "0", "OLD_MENU": "1",
        "OLD_VESAMENU": "0", "WININST": "1", "WIM0": "?:/Sources/Install.WIM",
        "REACTOS": "/ReactOS/FreeLdr.SYS",
    }, values
    # Cancellation before the first directory read and while directory lists
    # and dynamically grown path arrays are live; sanitizers check their cleanup.
    for point in (1, 5, 30, 70):
        values = scan(executable, image, cancel_after=point, expected_code=2)
        assert values["STATUS"] == "cancelled", values
    assert scan(executable, image)["STATUS"] == "tree-complete"
    return image


def check_names(executable, directory, bsdtar):
    image = make_image(directory, "names", {
        "/MiXeD/IsOlInUx.CfG": 2049, "/bootmgr": 0,
        "/firmware/data": 2048, "/firmware/alias": "data",
    }, bsdtar)
    rr = scan(executable, image)
    joliet = scan(executable, image, "joliet")
    plain = scan(executable, image, "plain")
    assert rr["CONFIG"] == ["/MiXeD/IsOlInUx.CfG"], rr
    assert joliet["CONFIG"] == rr["CONFIG"], joliet
    assert plain["CONFIG"] == ["/mixed/isolinux.cfg"], plain
    assert rr["JOLIET"] == plain["JOLIET"] == "0" and joliet["JOLIET"] != "0"
    assert rr["SYMLINKS"] == "1" and rr["EXTRA_BLOCKS"] == "1", rr
    for values in (joliet, plain):
        assert values["SYMLINKS"] == "0" and values["EXTRA_BLOCKS"] == "0", values
    for values in (rr, joliet, plain):
        assert values["BOOTMGR"] == "1" and values["BLOCKS"] == "3", values


def check_limits(executable, directory, bsdtar, valid_image):
    image = make_image(directory, "empty-tree", {}, bsdtar)
    values = scan(executable, image)
    assert values["FILES"] == values["BLOCKS"] == "0", values

    image = make_image(directory, "long-path", {
        "/" + "a" * 140 + "/" + "b" * 140 + "/file.txt": 1,
    }, bsdtar)
    scan(executable, image, expected_code=1)

    image = make_image(directory, "deep", {
        "/" + "/".join(f"level{i}" for i in range(10)) + "/bootmgr": 1,
    }, bsdtar)
    values = scan(executable, image, expected_code=3)
    assert values["STATUS"] == "tree-partial" and values["DEEP_DIRECTORIES"] == "1", values

    for name, content in (("empty", b""), ("text", b"Not an ISO"),
                          ("truncated", valid_image.read_bytes()[:32768])):
        invalid = directory / f"{name}.iso"
        invalid.write_bytes(content)
        scan(executable, invalid, expected_code=1)
    scan(executable, directory / "missing.iso", expected_code=1)
    scan(executable, directory, expected_code=1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--bsdtar", default="bsdtar")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="rufus-iso-tree-") as temporary:
        directory = Path(temporary)
        image = check_mixed_tree(args.executable, directory, args.bsdtar)
        check_names(args.executable, directory, args.bsdtar)
        check_limits(args.executable, directory, args.bsdtar, image)
    print("ISO tree fixtures pass: file rules, path-array growth, block rounding, symlinks,")
    print("Rock Ridge/Joliet/plain names, cancellation, partial scans and ordinary errors.")
    print("Read-only tree scan; UDF, content analysis, GUI integration and boot tests remain separate.")


if __name__ == "__main__":
    main()
