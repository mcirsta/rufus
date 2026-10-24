# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise the complete native scan plus Windows stage on generated ISO/UDF images."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile

from test_udf_images import make_image, rename_udf_entry

runs = 0


def probe(args, image, state, pending, major, mode=None):
    global runs
    command = [str(args.probe), str(image)]
    if mode:
        command.append(mode)
    result = subprocess.run(command, capture_output=True, timeout=15)
    assert result.returncode == 0, (command, result.stderr)
    report, metadata = [json.loads(line) for line in result.stdout.splitlines()]
    assert report["state"] == state and report["pending"] == pending, report
    assert report["major"] == major, (report, metadata)
    assert report["status"] == (2 if mode == "cancel" else 3 if pending else 0), report
    assert report["handles"] == 1, report
    runs += 1
    return report, metadata


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("probe", type=Path)
    parser.add_argument("fixture", type=Path)
    parser.add_argument("--mkisofs", required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="rufus-windows-images-") as temporary:
        directory = Path(temporary)
        wims = {}
        for kind in ("win11", "win7", "win8", "win8.1", "unknown", "invalid", "no-revision", "multi"):
            file = directory / (kind + ".wim")
            subprocess.run([str(args.fixture), str(file), kind], check=True, timeout=15)
            wims[kind] = file.read_bytes()
        for kind, state, major in (("win11", 1, 11), ("win7", 1, 7), ("win8", 1, 8), ("win8.1", 1, 8), ("unknown", 2, 0),
                                   ("invalid", 2, 0), ("no-revision", 2, 11), ("multi", 1, 7)):
            image = make_image(directory, kind.upper().replace("-", "_"),
                               {"/Sources/Install.wim": wims[kind]}, args.mkisofs)
            report, metadata = probe(args, image, state, 0 if state == 1 else 16, major)
            assert report["filesystem"] == 2 and report["format"] == metadata["format"] == 0x10d00
            if kind == "multi":
                assert metadata["images"] == 2 and metadata["boot"] == 2
            if kind == "no-revision":
                assert metadata["valid"] == 7 and metadata["invalid"] == 0
            if kind == "invalid":
                assert metadata["invalid"] == 1
            probe(args, image, 0, 0, 0, "cancel")

        # The first recorded path wins, including failure/unknown results.
        for kind, state in (("corrupt", 3), ("unknown", 2)):
            first = b"not a WIM" if kind == "corrupt" else wims[kind]
            image = make_image(directory, "FIRST_" + kind.upper(), {
                "/a/Sources/Install.wim": first, "/b/Sources/Install.wim": wims["win11"]}, args.mkisofs)
            report, _ = probe(args, image, state, 16, 0)
            assert report["paths"] == 2 and bool(report["error"]) == (state == 3)

        # The tree and subsequent WIM read must share the UDF namespace.
        image = make_image(directory, "VIEWS", {"/Sources/Install.wim": wims["win11"],
                           "/Sources/Otherxx.wim": wims["win7"]}, args.mkisofs)
        rename_udf_entry(image, "Install.wim", "Movedxx.wim")
        rename_udf_entry(image, "Otherxx.wim", "Install.wim")
        probe(args, image, 1, 0, 7)
        rename_udf_entry(image, "Install.wim", "Missing.wim")
        report, _ = probe(args, image, 0, 0, 0)
        assert report["paths"] == 0  # Companion ISO presence must not create a candidate.

        for suffix in ("esd", "swm"):
            image = make_image(directory, suffix.upper(), {"/Sources/Install." + suffix: wims["win11"]}, args.mkisofs)
            probe(args, image, 1, 0, 11)

        pe = {"/I386/NTDETECT.COM": b"fixture", "/I386/SETUPLDR.BIN": b"fixture",
              "/I386/TXTSETUP.SIF": b""}
        image = make_image(directory, "WINPE_PENDING", dict(pe, **{"/Sources/Install.wim": wims["win11"]}), args.mkisofs)
        probe(args, image, 1, 16, 11)
        pe["/I386/TXTSETUP.SIF"] = b"OsLoadOptions=/minint\n"
        image = make_image(directory, "WINPE_KNOWN", dict(pe, **{"/Sources/Install.wim": wims["win11"]}), args.mkisofs)
        probe(args, image, 1, 0, 11)
        image = make_image(directory, "SECURE_PENDING", {"/Sources/Install.wim": wims["win11"],
                           "/EFI/BOOT/bootx64.efi": bytes(512)}, args.mkisofs)
        probe(args, image, 1, 32, 11)
        image = make_image(directory, "BOOTMGR_ONLY", {"/BootMgr": b"fixture"}, args.mkisofs)
        probe(args, image, 0, 16, 0)
    print(f"Windows image analysis: {runs} probes pass; first path/image, selected UDF, masks,")
    print("pending WinPE/Secure Boot, failure/unknown state and cancellation verified.")


if __name__ == "__main__":
    main()
