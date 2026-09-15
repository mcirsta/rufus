# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise shared tree scanning, content reads and Syslinux selection on file images."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def banner(version):
    data = bytearray(512)
    text = b"ISOLINUX " + version.encode("ascii") + b"\0"
    data[128:128 + len(text)] = text
    return data


def make_image(directory, name, entries, bsdtar):
    tree = directory / name
    tree.mkdir()
    for path, data in entries.items():
        file = tree / path.lstrip("/")
        file.parent.mkdir(parents=True, exist_ok=True)
        file.write_bytes(data)
    image = directory / f"{name}-ș.iso"
    subprocess.run([bsdtar, "--format", "iso9660", "-cf", str(image),
                    "-C", str(tree), "."], check=True, capture_output=True, timeout=10)
    return image


def scan(executable, image, config="", detected=0, suffix="", selected=0,
         inferred=False, overwrite=False, fallback=("0x0407", "0x0604"), error=False):
    result = subprocess.run([str(executable), str(image), *fallback],
                            capture_output=True, text=True, timeout=10)
    if result.returncode != int(error):
        raise AssertionError(f"{image.name}: {result.returncode}\n{result.stdout}\n{result.stderr}")
    if error:
        assert result.stdout == "", result.stdout
        return
    values = dict(line.split("=", 1) for line in result.stdout.splitlines())
    expected = {
        "PRESENT": str(int(bool(config))), "CONFIG": config, "OVERWRITE": str(int(overwrite)),
        "DETECTED": str(detected), "DETECTED_EXT": suffix,
        "SELECTED": str(selected), "SELECTED_TEXT": f"{selected >> 8}.{selected & 255:02d}" if selected else "",
        "SELECTED_EXT": suffix if detected else "", "INFERRED": str(int(inferred)),
    }
    assert values == expected, (image.name, values, expected, result.stderr)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("resources", type=Path)
    parser.add_argument("--bsdtar", default="bsdtar")
    args = parser.parse_args()
    cfg = b"default linux\n"
    zero = bytes(512)
    old = (args.resources / "ldlinux_v4.sys").read_bytes()
    modern = (args.resources / "ldlinux_v6.sys").read_bytes()
    with tempfile.TemporaryDirectory(prefix="rufus-syslinux-scan-") as temporary:
        directory = Path(temporary)
        for name, entries in (
            ("empty", {}), ("loader-only", {"/isolinux.bin": old}),
            ("module-only", {"/ldlinux.c32": zero}),
        ):
            image = make_image(directory, name, entries, args.bsdtar)
            scan(args.executable, image)
        for name, loader, detected, suffix, has_c32 in (
            ("known-v4", old, 0x0407, "/2013-07-25", False),
            ("known-v6", modern, 0x0604, "/pre1", True),
            ("unknown-v4", zero, 0, "", False),
            ("unknown-v6", zero, 0, "", True),
        ):
            entries = {"/isolinux/isolinux.cfg": cfg, "/isolinux/isolinux.bin": loader}
            if has_c32:
                entries["/isolinux/ldlinux.c32"] = zero
            image = make_image(directory, name, entries, args.bsdtar)
            scan(args.executable, image, "/isolinux/isolinux.cfg", detected, suffix,
                 detected or (0x0604 if has_c32 else 0x0407), not bool(detected))
            # Unavailable fallback must neither erase presence nor override a detection.
            scan(args.executable, image, "/isolinux/isolinux.cfg", detected, suffix,
                 detected, fallback=("0", "0"))
        image = make_image(directory, "config-only", {"/syslinux.cfg": cfg}, args.bsdtar)
        scan(args.executable, image, "/syslinux.cfg", selected=0x0407, inferred=True)
        image = make_image(directory, "suse", {
            "/syslinux.cfg": cfg, "/boot/x86_64/loader/isolinux.cfg": cfg,
            "/boot/x86_64/loader/isolinux.bin": old,
        }, args.bsdtar)
        scan(args.executable, image, "/boot/x86_64/loader/isolinux.cfg",
             0x0407, "/2013-07-25", 0x0407, overwrite=True)
        image = make_image(directory, "ties", {
            "/EFI/BOOT/isolinux.cfg": cfg, "/isolinux/isolinux.cfg": cfg,
        }, args.bsdtar)
        scan(args.executable, image, "/isolinux/isolinux.cfg", selected=0x0407, inferred=True)
        image = make_image(directory, "mixed-case", {
            "/IsOlInUx/IsOlInUx.CfG": cfg, "/IsOlInUx/IsOlInUx.BiN": modern,
        }, args.bsdtar)
        scan(args.executable, image, "/IsOlInUx/IsOlInUx.CfG", 0x0604, "/pre1", 0x0604)
        # The a-directory is encountered before the selected isolinux directory.
        for name, second, detected, suffix, selected, inferred in (
            ("conflict", modern, 0x0604, "/pre1", 0x0604, False),
            ("unknown-preferred", zero, 0, "", 0x0407, True),
            ("unreadable-preferred", b"", 0x0407, "/2013-07-25", 0x0407, False),
        ):
            image = make_image(directory, name, {
                "/a/isolinux.bin": old, "/isolinux/isolinux.cfg": cfg,
                "/isolinux/isolinux.bin": second,
            }, args.bsdtar)
            scan(args.executable, image, "/isolinux/isolinux.cfg", detected, suffix, selected, inferred)
        image = make_image(directory, "suffix-conflict", {
            "/a/isolinux.bin": modern, "/isolinux/isolinux.cfg": cfg,
            "/isolinux/isolinux.bin": banner("6.04 6.04-distro"),
        }, args.bsdtar)
        scan(args.executable, image, "/isolinux/isolinux.cfg", 0x0604, "/distro", 0x0604)
        for length in (127, 128):
            path = "/" + "p" * (length - len("/isolinux.cfg") - 1) + "/isolinux.cfg"
            assert len(path) == length
            image = make_image(directory, f"config-length-{length}", {path: cfg}, args.bsdtar)
            if length == 127:
                scan(args.executable, image, path, selected=0x0407, inferred=True)
            else:
                scan(args.executable, image, error=True)
        invalid = directory / "plain.iso"
        invalid.write_text("Not an image")
        scan(args.executable, invalid, error=True)
    print("Syslinux scan: 17 generated images pass with shared traversal, reads, detection and selection.")
    print("Policy only: no native installer, media writes or boot validation.")


if __name__ == "__main__":
    main()
