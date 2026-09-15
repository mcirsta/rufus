# SPDX-License-Identifier: GPL-3.0-or-later
"""Read synthetic GRUB content from generated images; no executable loaders."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def payload(version, marker="GRUB  version %s", filesystems=(), debug=False):
    data = bytearray(4096)
    banner = marker.encode("ascii") + b"\0" + version.encode("ascii") + b"\0"
    data[64:64 + len(banner)] = banner
    if debug:
        data[512:533] = b"grub_debug_is_enabled\0"
    for index, name in enumerate(filesystems):
        entry = b"fshelp\0" + name.encode("ascii") + b"\0"
        offset = 1024 + index * 64
        data[offset:offset + len(entry)] = entry
    return data


def inspect(executable, image, paths, expected, flags=1, label="Demo-Live-x86_64"):
    result = subprocess.run([str(executable), str(image), str(flags), label, *paths],
                            capture_output=True, text=True, timeout=10)
    code = 1 if expected is None else 0
    if result.returncode != code:
        raise AssertionError(f"{paths}: exit={result.returncode}\n{result.stdout}\n{result.stderr}")
    if expected is None:
        assert result.stdout == "", result.stdout
        return
    values = dict(line.split("=", 1) for line in result.stdout.splitlines())
    assert values == {"VERSION": expected[0], "FS_MASK": str(expected[1]),
                      "FILESYSTEMS": expected[2]}, (paths, values, expected)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--bsdtar", default="bsdtar")
    args = parser.parse_args()
    files = {
        "/boot/grub/i386-pc/normal.mod": payload("2.12"),
        "/boot/grub2/i386-pc/normal.mod": payload("2.06", debug=True),
        "/EFI/BOOT/grubx64.efi": payload("2.06", "GRUB version %s", ("fat", "ntfs", "fat", "ext2"), True),
        "/centos/normal.mod": payload("2.04", "GRUB  version %s\n"),
        "/extra/grub.efi": payload("2.14", filesystems=("exfat", "FAT", "ntfs")),
        "/zero/normal.mod": payload("0.97"),
        "/unknown/normal.mod": bytes(4096),
        "/small/normal.mod": b"no GRUB here",
        "/empty/normal.mod": b"",
    }
    # The second marker in one file wins; the first usable file then wins.
    two = payload("2.04")
    other = b"GRUB version %s\0" + b"2.12\0"
    two[2048:2048 + len(other)] = other
    files["/two/normal.mod"] = two
    # Exercise the reader-added NUL with a sufficiently long, ordinary suffix.
    eof_version = "2.12-" + "a" * 64
    files["/eof/normal.mod"] = bytes(64) + b"GRUB  version %s\0" + eof_version.encode("ascii")
    with tempfile.TemporaryDirectory(prefix="rufus-grub-image-") as temporary:
        directory = Path(temporary)
        tree = directory / "tree"
        for name, data in files.items():
            file = tree / name.lstrip("/")
            file.parent.mkdir(parents=True, exist_ok=True)
            file.write_bytes(data)
        image = directory / "grub-ș.iso"
        subprocess.run([args.bsdtar, "--format", "iso9660", "-cf", str(image),
                        "-C", str(tree), "."], check=True, capture_output=True, timeout=10)
        normal = "/boot/grub/i386-pc/normal.mod"
        efi = "/EFI/BOOT/grubx64.efi"
        inspect(args.executable, image, [normal], ("2.12", 0, ""))
        inspect(args.executable, image, ["/boot/grub2/i386-pc/normal.mod"],
                ("2.06-nonstandard-gdie-demo", 0, ""), flags=2)
        inspect(args.executable, image, [efi], ("2.06-gdie-demo", 5, "fat,ntfs,ext2"), flags=128)
        inspect(args.executable, image, ["/centos/normal.mod"], ("2.04", 0, ""))
        inspect(args.executable, image, [normal, efi, "/extra/grub.efi"],
                ("2.12", 7, "fat,ntfs,ext2,exfat,FAT"), flags=129)
        inspect(args.executable, image, ["/two/normal.mod"], ("2.12", 0, ""))
        inspect(args.executable, image, ["/eof/normal.mod"], (eof_version, 0, ""))
        for kind in ("zero", "unknown", "small"):
            inspect(args.executable, image, [f"/{kind}/normal.mod"], ("", 0, ""))
        inspect(args.executable, image, ["/zero/normal.mod", normal], ("2.12", 0, ""))
        for name in ("/empty/normal.mod", "/missing", "/boot/grub"):
            inspect(args.executable, image, [name], None)
        invalid = directory / "plain.iso"
        invalid.write_text("Not an image")
        inspect(args.executable, invalid, [normal], None)
    print("GRUB image reads: version/build rules, filesystem union, selection order and read failures pass.")
    print("Synthetic content only: no PE validation, installer, media writes or boot validation.")


if __name__ == "__main__":
    main()
