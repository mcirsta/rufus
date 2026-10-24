# SPDX-License-Identifier: GPL-3.0-or-later
"""Check native-pair eligibility after Rufus's shared version/config selection."""
import argparse
import errno
import json
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "linux/tests"))
from test_syslinux_scan import banner

FAT16 = 0
FAT32 = 1
NTFS = 2
SECTOR_BYTES = 512
MIB = 1024 * 1024


def select(probe, loader, module, config="/isolinux/isolinux.cfg", root_exists=False,
           filesystem=FAT32, sector=SECTOR_BYTES, offset=MIB, error=0):
    command = [str(probe), str(loader), str(module), config, str(int(root_exists)),
               str(filesystem), str(sector), str(offset)]
    result = subprocess.run(command, capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, (command, result.stdout, result.stderr)
    report = json.loads(result.stdout)
    assert report["error"] == error, report
    assert bool(report["problem"]) == bool(error), report
    print(json.dumps(report), flush=True)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("probe", type=Path)
    parser.add_argument("upstream", type=Path)
    args = parser.parse_args()
    loader = args.upstream / "bios/core/isolinux.bin"
    module = args.upstream / "bios/com32/elflink/ldlinux/ldlinux.c32"
    with tempfile.TemporaryDirectory(prefix="rufus-syslinux-selection-") as temporary:
        root = Path(temporary)
        known = select(args.probe, loader, module)
        assert known["detected"] == known["selected"] == 0x0604
        assert known["detected_suffix"] == known["selected_suffix"] == "/pre1"
        assert not known["inferred"]
        unknown = root / "unknown.bin"
        unknown.write_bytes(bytes(SECTOR_BYTES))
        inferred = select(args.probe, unknown, module)
        assert inferred["detected"] == 0 and inferred["detected_suffix"] == ""
        assert inferred["selected"] == 0x0604 and inferred["selected_suffix"] == ""
        assert inferred["inferred"]
        for version in ("6.04 6.04-pre3", "6.04", "6.03", "4.07"):
            alternate = root / "alternate.bin"
            alternate.write_bytes(banner(version))
            select(args.probe, alternate, module)
        module_bytes = module.read_bytes()
        changed = bytearray(module_bytes)
        changed[-1] ^= 1
        for contents in (b"", module_bytes[:-1], changed):
            unmatched = root / "unmatched.c32"
            unmatched.write_bytes(contents)
            select(args.probe, loader, unmatched, error=errno.ENOTSUP)
            select(args.probe, unknown, unmatched, error=errno.ENOTSUP)
        for filesystem in (-1, 3):  # exFAT has no qualified Syslinux installer.
            select(args.probe, loader, module, filesystem=filesystem, error=errno.ENOTSUP)
        select(args.probe, loader, module, filesystem=FAT16)
        select(args.probe, unknown, module, filesystem=FAT16)
        select(args.probe, loader, module, filesystem=NTFS)
        select(args.probe, unknown, module, filesystem=NTFS)
        for offset in (0, 4 * 1024 * MIB, ((1 << 32) - 1) * SECTOR_BYTES):
            select(args.probe, loader, module, filesystem=NTFS, offset=offset)
        for offset in (1, (1 << 32) * SECTOR_BYTES):
            select(args.probe, loader, module, filesystem=NTFS, offset=offset, error=errno.ENOTSUP)
        select(args.probe, loader, module, filesystem=NTFS, sector=4096, error=errno.ENOTSUP)
        for sector in (0, 4096):
            select(args.probe, loader, module, sector=sector, error=errno.ENOTSUP)
        for offset in (1, 4 * 1024 * MIB):
            select(args.probe, loader, module, offset=offset, error=errno.ENOTSUP)
        for offset in (0, (1 << 32) - SECTOR_BYTES):
            select(args.probe, loader, module, offset=offset)
        select(args.probe, loader, module, config="", error=errno.ENOTSUP)
        existing = select(args.probe, loader, module, root_exists=True)
        assert existing["config"] == "/syslinux.cfg" and not existing["create"]
        overridden = select(args.probe, loader, module, root_exists=True,
                            config="/boot/x86_64/loader/isolinux.cfg")
        assert overridden["create"] and overridden["backup"]
        assert "APPEND /boot/x86_64/loader/" in overridden["text"]
    print("Native-pair/config selection cases passed for FAT and NTFS; input reports preserved.")


if __name__ == "__main__":
    main()
