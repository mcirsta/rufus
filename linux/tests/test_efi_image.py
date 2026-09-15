# SPDX-License-Identifier: GPL-3.0-or-later
"""Read EFI loader banners through ISO files without executing bootloader code."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def banner(text):
    data = bytearray(512)
    data[64:64 + len(text)] = text
    return data


def inspect(executable, image, path, expected, code=0):
    result = subprocess.run([str(executable), str(image), path],
                            capture_output=True, text=True, timeout=10)
    if result.returncode != code:
        raise AssertionError(f"{path}: exit={result.returncode}\n{result.stdout}\n{result.stderr}")
    detected = [line for line in result.stderr.splitlines() if line.startswith("  Detected ")]
    wanted = [] if expected is None else [f"  Detected {expected[0]} version: {expected[1]} (from '{path}')"]
    assert detected == wanted, (path, detected, wanted)
    assert result.stdout == "", result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--bsdtar", default="bsdtar")
    parser.add_argument("--systemd-boot", action="append", type=Path, default=[])
    parser.add_argument("--systemd-version", help="Independently verified expected version for optional real files")
    args = parser.parse_args()
    if bool(args.systemd_boot) != bool(args.systemd_version):
        parser.error("Supply --systemd-boot and --systemd-version together")
    files = {
        "/EFI/BOOT/bootx64.efi": (banner(b"UEFI SHIM\n$Version: 15.8 \0"), ("Shim", "15.8")),
        "/EFI/systemd/systemd-bootx64.efi": (banner(b"#### LoaderInfo: systemd-boot 261.3 ####\0"), ("systemd-boot", "261.3")),
        "/nul/boot.efi": (banner(b"UEFI SHIM\n$Version: 15.8\0extra bytes"), ("Shim", "15.8")),
        "/limit/boot.efi": (banner(b"UEFI SHIM\n$Version: " + b"A" * 80), ("Shim", "A" * 32)),
        "/footer/boot.efi": (banner(b"ID=systemd-boot\nVERSION=261.3\0"), None),
        "/unknown/boot.efi": (bytes(512), None),
        "/small/boot.efi": (b"UEFI SHIM\n$Version: 15.8\0", None),
        "/empty/boot.efi": (b"", None),
    }
    first = banner(b"#### LoaderInfo: systemd-boot 261.3 ####\0")
    second = b"UEFI SHIM\n$Version: 15.8\0"
    first[256:256 + len(second)] = second
    files["/first/boot.efi"] = (first, ("systemd-boot", "261.3"))
    for index, path in enumerate(args.systemd_boot):
        files[f"/real/{index}/{path.name}"] = (path.read_bytes(), ("systemd-boot", args.systemd_version))
    with tempfile.TemporaryDirectory(prefix="rufus-efi-image-") as temporary:
        directory = Path(temporary)
        tree = directory / "tree"
        for name, (data, expected) in files.items():
            file = tree / name.lstrip("/")
            file.parent.mkdir(parents=True, exist_ok=True)
            file.write_bytes(data)
        image = directory / "efi-ș.iso"
        subprocess.run([args.bsdtar, "--format", "iso9660", "-cf", str(image),
                        "-C", str(tree), "."], check=True, capture_output=True, timeout=10)
        for name, (data, expected) in files.items():
            inspect(args.executable, image, name, expected, 0 if data else 1)
        for name in ("/missing", "/EFI/BOOT"):
            inspect(args.executable, image, name, None, 1)
        invalid = directory / "plain.iso"
        invalid.write_text("Not an image")
        inspect(args.executable, invalid, "/EFI/BOOT/bootx64.efi", None, 1)
    print(f"EFI banner image reads pass: 9 synthetic files, {len(args.systemd_boot)} optional real files, ordinary read failures.")
    print("Banner diagnostics only; no PE structure, signature, revocation or boot validation.")


if __name__ == "__main__":
    main()
