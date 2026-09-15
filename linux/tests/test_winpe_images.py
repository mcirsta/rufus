# SPDX-License-Identifier: GPL-3.0-or-later
"""WinPE options through generated ISO/UDF images, without Windows executables."""
import argparse
from pathlib import Path
import tempfile

from test_iso_analyze import analyze, make_image as make_iso
from test_udf_images import make_image as make_udf, rename_udf_entry


def pe_files(text, directory="i386"):
    # Rufus's AMD64 mask combines the two i386 loaders with amd64/txtsetup.sif.
    loaders = "i386" if directory == "amd64" else directory
    return {f"/{loaders}/ntdetect.com": b"filename fixture",
            f"/{loaders}/setupldr.bin": b"filename fixture",
            f"/{directory}/txtsetup.sif": text}


def check_formats(directory, args):
    for udf in (False, True):
        for name, text, pending, minint in (
            ("MININT", b"[SetupData]\r\nOsLoadOptions=\"/MININT /fastdetect\"\r\n", 0, 1),
            ("NORMAL", b"OsLoadOptions=/fastdetect\n", 0, 0),
            ("ABSENT", b"[SetupData]\nOther=1\n", 0, 0),
            ("EMPTY", b"", 16, 0),
            ("UTF16", "OsLoadOptions=/minint".encode("utf-16"), 16, 0),
            ("TOO_BIG", b"OsLoadOptions=/minint\n" + bytes(65536), 16, 0),
            ("LIMIT", b"OsLoadOptions=/minint\n".ljust(65536, b" "), 0, 1),
        ):
            label = name + ("_UDF" if udf else "_ISO")
            entries = pe_files(text)
            image = (make_udf(directory, label, entries, args.mkisofs) if udf else
                     make_iso(directory, label, entries, args.bsdtar))
            analyze(args.scanner, image, status=3 if pending else 0, pending=pending,
                    WINPE=7, MININT=minint, WINPE_KNOWN=int(pending == 0), READS=1,
                    FILESYSTEM=2 if udf else 1)
            if name == "MININT":
                for fault in ("cancel:1", "cancel-read:1"):
                    analyze(args.scanner, image, status=2, fault=fault)
                analyze(args.scanner, image, status=3, pending=16, fault="fail-read:1",
                        MININT=0, WINPE_KNOWN=0, READS=1)
                analyze(args.scanner, image, MININT=1, WINPE_KNOWN=1)
    for name, entries, mask, minint, pending in (
        ("AMD64", pe_files(b"OsLoadOptions=/MININT", "amd64"), 0x23, 1, 0),
        ("MININT_DIR", pe_files(b"OsLoadOptions=/fastdetect", "minint"), 0x1c0, 0, 0),
        ("PRIORITY", dict(pe_files(b"OsLoadOptions=/fastdetect"),
                          **pe_files(b"OsLoadOptions=/minint", "minint")), 0x1c7, 0, 0),
        ("WIM_PENDING", dict(pe_files(b"OsLoadOptions=/minint"),
                             **{"/sources/install.wim": b"not a WIM", "/bootmgr": b"fixture"}), 7, 1, 16),
    ):
        image = make_udf(directory, name, entries, args.mkisofs)
        analyze(args.scanner, image, status=3 if pending else 0, pending=pending,
                WINPE=mask, MININT=minint, WINPE_KNOWN=1, READS=1)


def check_case_and_views(directory, args):
    entries = pe_files(b"OsLoadOptions=/minint")
    entries["/i386/othsetup.sif"] = b"OsLoadOptions=/fastdetect"
    image = make_udf(directory, "SELECTED_UDF", entries, args.mkisofs)
    rename_udf_entry(image, "txtsetup.sif", "tmpsetup.sif")
    rename_udf_entry(image, "othsetup.sif", "txtsetup.sif")
    rename_udf_entry(image, "tmpsetup.sif", "othsetup.sif")
    rename_udf_entry(image, "i386", "I386")
    rename_udf_entry(image, "txtsetup.sif", "TXTSETUP.SIF")
    for mode in ("rr", "joliet", "plain"):
        analyze(args.scanner, image, mode=mode, FILESYSTEM=2, WINPE=7,
                WINPE_KNOWN=1, MININT=0, READS=1)
    entries = {key.upper(): value for key, value in pe_files(b"OsLoadOptions=/MININT").items()}
    image = make_iso(directory, "UPPERCASE", entries, args.bsdtar)
    for mode in ("rr", "joliet", "plain"):
        analyze(args.scanner, image, mode=mode, WINPE=7, MININT=1, WINPE_KNOWN=1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("scanner", type=Path)
    parser.add_argument("--bsdtar", required=True)
    parser.add_argument("--mkisofs", required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="rufus-winpe-") as temporary:
        directory = Path(temporary)
        check_formats(directory, args)
        check_case_and_views(directory, args)
    print("WinPE: 20 generated ISO/UDF files, options, directory priority, case,")
    print("size limits, selected UDF view, pending WIM checks and cancellation pass.")


if __name__ == "__main__":
    main()
