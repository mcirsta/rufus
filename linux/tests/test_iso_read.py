# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare entire files read through Rufus, using generated ISO fixtures."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def read_file(executable, image, path, expected, mode="all"):
    result = subprocess.run([str(executable), str(image), path, mode],
                            capture_output=True, timeout=10)
    code = 1 if expected is None else 0
    if result.returncode != code or result.stdout != (expected or b""):
        raise AssertionError(f"{image.name}:{path}/{mode}: exit={result.returncode}, "
                             f"bytes={len(result.stdout)}\n{result.stderr.decode(errors='replace')}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--bsdtar", default="bsdtar")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="rufus-iso-read-") as temporary:
        directory = Path(temporary)
        tree = directory / "tree"
        tree.mkdir()
        files = {}
        for size in (1, 2047, 2048, 2049, 70001):
            files[f"/payload{size}.bin"] = bytes((i * 37 + 11) % 256 for i in range(size))
        files["/boot/grub/i386-pc/core.img"] = b"Synthetic GRUB content\x00with embedded NUL\xff\n"
        files["/MiXeD/IsOlInUx.CfG"] = b"DEFAULT test\n"
        files["/boot.img"] = bytes(range(256)) * 16
        files["/empty"] = b""
        for name, content in files.items():
            path = tree / name.lstrip("/")
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(content)
        image = directory / "imagine-ș.iso"
        subprocess.run([args.bsdtar, "--format", "iso9660", "--options",
                        "iso9660:boot=boot.img,iso9660:boot-type=no-emulation,iso9660:boot-load-size=8",
                        "-cf", str(image), "-C", str(tree), "."],
                       check=True, capture_output=True, timeout=10)
        for name, content in files.items():
            read_file(args.executable, image, name, content or None)
        # The scanner supplies RR paths. Content reads must use matching options.
        for mode in ("rr", "joliet"):
            read_file(args.executable, image, "/MiXeD/IsOlInUx.CfG", files["/MiXeD/IsOlInUx.CfG"], mode)
            read_file(args.executable, image, "/boot/grub/i386-pc/core.img", files["/boot/grub/i386-pc/core.img"], mode)
        read_file(args.executable, image, "/mixed/isolinux.cfg", files["/MiXeD/IsOlInUx.CfG"], "plain")
        read_file(args.executable, image, "/[BOOT]/0-Boot-NoEmul.img", files["/boot.img"])
        for path in ("/missing", "/boot", "/"):
            read_file(args.executable, image, path, None)
        # Preserve directory metadata but end partway through an ordinary file.
        image_bytes = image.read_bytes()
        offset = image_bytes.find(files["/payload70001.bin"])
        assert offset >= 0 and offset % 2048 == 0
        incomplete = directory / "incomplete-file.iso"
        incomplete.write_bytes(image_bytes[:offset + 3000])
        read_file(args.executable, incomplete, "/payload70001.bin", None)
        for name, content in (("empty", b""), ("text", b"Not an image"),
                              ("truncated", image.read_bytes()[:32768])):
            invalid = directory / f"{name}.iso"
            invalid.write_bytes(content)
            read_file(args.executable, invalid, "/payload1.bin", None)
        # Confirm the fixture remains readable after the ordinary error cases.
        read_file(args.executable, image, "/payload70001.bin", files["/payload70001.bin"])
    print("ISO content fixtures pass: exact bytes, block edges, embedded NUL, name options,")
    print("synthetic El Torito entry, empty files, directories and ordinary invalid images.")


if __name__ == "__main__":
    main()
