# SPDX-License-Identifier: GPL-3.0-or-later
"""Read Syslinux banners from generated images; no installer or boot execution."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def detect(executable, image, path, expected):
    result = subprocess.run([str(executable), str(image), path],
                            capture_output=True, text=True, timeout=10)
    code = 1 if expected is None else 0
    if result.returncode != code:
        raise AssertionError(f"{path}: exit={result.returncode}\n{result.stdout}\n{result.stderr}")
    if expected is None:
        assert result.stdout == "", result.stdout
        return
    values = dict(line.split("=", 1) for line in result.stdout.splitlines())
    assert values == {"VERSION": str(expected[0]), "EXT": expected[1]}, (path, values)


def banner(text):
    data = bytearray(512)
    data[128:128 + len(text) + 1] = text.encode("ascii") + b"\0"
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("resources", type=Path)
    parser.add_argument("--bsdtar", default="bsdtar")
    args = parser.parse_args()
    # Bundled binaries are read as fixture data under their existing notices.
    # Other payloads are synthetic bytes with banners, not executable loaders.
    files = {
        "/isolinux/isolinux.bin": ((args.resources / "ldlinux_v4.sys").read_bytes(), (0x0407, "/2013-07-25")),
        "/Boot/IsOlInUx.BiN": ((args.resources / "ldlinux_v6.sys").read_bytes(), (0x0604, "/pre1")),
        "/arch/boot.bin": (banner("ISOLINUX 6.03 2014-10-06* "), (0x0603, "/2014-10-06")),
        "/distro/boot.bin": (banner("ISOLINUX 6.04 6.04-pre1 build"), (0x0604, "/pre1 build")),
        "/bare/boot.bin": (banner("SYSLINUX 4.07"), (0x0407, "")),
        "/unknown/isolinux.bin": (bytes(512), (0, "")),
        "/small/boot.bin": (b"not a loader", (0, "")),
        "/empty/boot.bin": (b"", None),
    }
    # Here the content reader supplies the banner's terminating byte at EOF.
    suffix = "build-" + "a" * 130
    files["/eof/boot.bin"] = (bytes(128) + ("ISOLINUX 6.03 " + suffix).encode("ascii"), (0x0603, "/" + suffix))
    with tempfile.TemporaryDirectory(prefix="rufus-syslinux-image-") as temporary:
        directory = Path(temporary)
        tree = directory / "tree"
        for name, (data, expected) in files.items():
            file = tree / name.lstrip("/")
            file.parent.mkdir(parents=True, exist_ok=True)
            file.write_bytes(data)
        image = directory / "syslinux-ș.iso"
        subprocess.run([args.bsdtar, "--format", "iso9660", "-cf", str(image),
                        "-C", str(tree), "."], check=True, capture_output=True, timeout=10)
        for name, (data, expected) in files.items():
            detect(args.executable, image, name, expected)
        detect(args.executable, image, "/missing", None)
        detect(args.executable, image, "/isolinux", None)
        invalid = directory / "plain.iso"
        invalid.write_text("Not an image")
        detect(args.executable, invalid, "/isolinux/isolinux.bin", None)
    print("Syslinux image reads: bundled v4/v6, suffixes, EOF termination, unknown and read failures pass.")
    print("Recognition only; no version fallback, native installer selection, writes or boot validation.")


if __name__ == "__main__":
    main()
