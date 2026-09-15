# SPDX-License-Identifier: GPL-3.0-or-later
"""Bounded architecture probe. Generates regular ISO files, never bootable code."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile


def run_probe(executable, image, entry, expected_code):
    result = subprocess.run([str(executable), str(image), entry],
                            capture_output=True, text=True, timeout=10)
    if result.returncode != expected_code:
        raise RuntimeError(f"{executable.name}: {result.returncode}\n{result.stdout}\n{result.stderr}")
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build_directory", type=Path)
    args = parser.parse_args()
    build = args.build_directory.resolve()
    with tempfile.TemporaryDirectory(prefix="rufus-cp3-images-") as temporary:
        directory = Path(temporary)
        tree = directory / "tree"
        (tree / "EFI/BOOT").mkdir(parents=True)
        payload = b"RUFUS-CP3-PROBE!" + bytes(4096 - 16)
        assert len(payload) == 4096
        (tree / "boot.img").write_bytes(payload)
        (tree / "EFI/BOOT/BOOTX64.EFI").write_bytes(payload)
        (tree / "readme.txt").write_text("Synthetic parser fixture; no executable bootloader.\n")
        image = directory / "imagine-ș.iso"
        subprocess.run(["bsdtar", "--format", "iso9660", "--options",
                        "iso9660:volume-id=RUFUS_CP3,iso9660:boot=boot.img,iso9660:boot-type=no-emulation",
                        "-cf", str(image), "-C", str(tree), "."], check=True,
                       capture_output=True, text=True, timeout=10)
        prefix = f"PREFIX={payload[:16].hex()}"
        regular = {}
        for name in ("probe_bundled", "probe_system"):
            output = run_probe(build / name, image, "/EFI/BOOT/BOOTX64.EFI", 0)
            assert "LABEL=RUFUS_CP3" in output and "SIZE=4096" in output and prefix in output, (name, output)
            regular[name] = "label, size and payload prefix match"
        virtual = "/[BOOT]/0-Boot-NoEmul.img"
        bundled = run_probe(build / "probe_bundled", image, virtual, 0)
        assert prefix in bundled
        system = run_probe(build / "probe_system", image, virtual, 3)
        assert "ENTRY_MISSING" in system
        # Ordinary invalid inputs, not a vulnerability reproduction or fuzz corpus.
        for filename, content in (("empty.iso", b""), ("not-an-iso.iso", b"Plain text"),
                                  ("truncated.iso", image.read_bytes()[:32768])):
            invalid = directory / filename
            invalid.write_bytes(content)
            for name in ("probe_bundled", "probe_system"):
                output = run_probe(build / name, invalid, "/EFI/BOOT/BOOTX64.EFI", 1)
                assert "ISO_OPEN_FAILED" in output
        print(json.dumps({"ordinary_iso": regular,
                          "rufus_virtual_boot_image": "bundled reads expected bytes; system entry missing",
                          "invalid_inputs": "empty, plain text and descriptor-truncated ISO rejected by both",
                          "boot_validation": "not performed; payload is non-executable test data"}, indent=2))


if __name__ == "__main__":
    main()
