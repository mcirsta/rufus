# SPDX-License-Identifier: GPL-3.0-or-later
"""Extract generated EFI bytes through native wimlib in ordinary and selected inputs."""
import argparse
from pathlib import Path
import random
import re
import struct
import subprocess
import tempfile

from test_udf_images import make_image, rename_udf_entry
from test_udf_embedded_efi import fragment_file

runs = 0
EFI_PATH = "/Windows/Boot/EFI/bootmgr.efi"
MAX_OUTPUT_BYTES = 16 * 1024 * 1024


def pe_bytes(machine):
    data = bytearray(random.Random(7).randbytes(128 * 1024))
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 60, 128)
    data[128:132] = b"PE\0\0"
    struct.pack_into("<H", data, 132, machine)
    return bytes(data)


def probe(args, image, view="file", wim_path="/Sources/install.wim", path=EFI_PATH,
          index=1, fault=None, point=None, expected=None, error=None):
    global runs
    command = [args.probe, image, view, wim_path, path, index]
    if fault:
        command.append(fault)
    if point is not None:
        command.append(point)
    result = subprocess.run([str(value) for value in command], capture_output=True, timeout=30)
    runs += 1
    assert result.returncode == 0, (command, result.returncode, result.stderr)
    metrics = dict((key.decode(), int(value)) for key, value in
                   re.findall(rb"(ERROR|SIZE|ARCH|READS|MAX_BLOCKS|ISO_READS|EXTRACTS)=(\d+)", result.stderr))
    assert "ERROR" in metrics, result.stderr
    if error is None:
        assert metrics["ERROR"] == 0 and result.stdout == expected, (command, metrics, result.stderr)
        assert metrics["SIZE"] == len(expected) and metrics["EXTRACTS"] == 1, metrics
    else:
        assert metrics["ERROR"] != 0 and not result.stdout and metrics["SIZE"] == 0, (command, metrics)
        if error != -1:
            assert metrics["ERROR"] == error, (command, metrics)
    assert metrics["MAX_BLOCKS"] <= 16, metrics
    if view == "udf":
        assert metrics["ISO_READS"] == 0, metrics
    return metrics


def capture(args, root, output, mode, second=None):
    command = [args.fixture, root, output, mode]
    if second:
        command.append(second)
    subprocess.run([str(value) for value in command], check=True, capture_output=True, timeout=45)


def check_selected_views(args, image, wims, x64, arm64):
    for view in ("rr", "joliet", "udf"):
        for mode in wims:
            probe(args, image, view, "/Sources/" + mode + ".wim", expected=x64)
        command = dict(view=view, wim_path="/Sources/none.wim")
        probe(args, image, **command, index=2, expected=arm64)
        for path, expected in (("empty.efi", b""), ("short.efi", b"MZ"), ("ș-é.efi", arm64)):
            probe(args, image, **command, path="/Windows/Boot/EFI/" + path, expected=expected)
        for path in ("missing.efi", "BOOTMGR.EFI", "directory.efi", "link.efi"):
            result = probe(args, image, **command, path="/Windows/Boot/EFI/" + path, error=-1)
            assert result["EXTRACTS"] == 0, result
        for index in (0, 3):
            result = probe(args, image, **command, index=index, error=-1)
            assert result["EXTRACTS"] == 0, result
        result = probe(args, image, **command, expected=x64)
        assert result["READS"] >= 3, result
        for point in range(1, result["READS"] + 1):
            for fault in ("cancel-io", "fail-io", "short-io"):
                probe(args, image, **command, fault=fault, point=point, error=-1)


def check_output_limits(args, root):
    # Exact output cap and one byte above; reject oversized streams before extraction.
    limits = root / "limits"
    limits.mkdir()
    for name, size in (("limit.efi", MAX_OUTPUT_BYTES), ("large.efi", MAX_OUTPUT_BYTES + 1)):
        with (limits / name).open("wb") as output:
            output.truncate(size)
    capture(args, limits, root / "limits.wim", "lzx")
    probe(args, root / "limits.wim", path="/limit.efi", expected=bytes(MAX_OUTPUT_BYTES))
    result = probe(args, root / "limits.wim", path="/large.efi", error=68)
    assert result["EXTRACTS"] == 0, result
    image = make_image(root, "LIMITS", {"/Sources/install.wim": (root / "limits.wim").read_bytes()},
                       args.mkisofs)
    for view in ("rr", "udf"):
        result = probe(args, image, view, path="/large.efi", error=68)
        assert result["EXTRACTS"] == 0, result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("probe", type=Path)
    parser.add_argument("fixture", type=Path)
    parser.add_argument("--mkisofs", required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="rufus-wim-extract-") as temporary:
        root = Path(temporary)
        tree = root / "input"
        efi = tree / "Windows/Boot/EFI"
        efi.mkdir(parents=True)
        x64, arm64 = pe_bytes(0x8664), pe_bytes(0xaa64)
        (efi / "bootmgr.efi").write_bytes(x64)
        (efi / "empty.efi").write_bytes(b"")
        (efi / "short.efi").write_bytes(b"MZ")
        (efi / "link.efi").symlink_to("bootmgr.efi")
        (efi / "directory.efi").mkdir()
        (efi / "ș-é.efi").write_bytes(arm64)
        second = root / "second"
        (second / "Windows/Boot/EFI").mkdir(parents=True)
        (second / EFI_PATH.lstrip("/")).write_bytes(arm64)

        wims = {}
        for mode in ("none", "lzx", "xpress", "solid", "pipable", "integrity"):
            wim = root / (mode + ".wim")
            capture(args, tree, wim, mode, second)
            wims[mode] = wim.read_bytes()
            result = probe(args, wim, expected=x64)
            assert result["ARCH"] == 2, result
            result = probe(args, wim, index=2, expected=arm64)
            assert result["ARCH"] == 4, result

        image = make_image(root, "WIM_EXTRACT", {"/Sources/" + mode + ".wim": data
                           for mode, data in wims.items()}, args.mkisofs)
        check_selected_views(args, image, wims, x64, arm64)

        ordinary = root / "lzx.wim"
        for fault in ("cancel-pre", "cancel-extract", "cancel-after", "cancel-read",
                      "mkdir", "write", "read", "short", "allocate", "size"):
            probe(args, ordinary, fault=fault, error=-1)
        for fault in ("cancel-extract", "cancel-after", "cancel-read", "write", "read", "short",
                      "allocate", "size"):
            probe(args, image, "udf", "/Sources/solid.wim", fault=fault, error=-1)

        # Corrupt the actual uncompressed file payload, leaving metadata intact.
        damaged = bytearray(wims["none"])
        position = damaged.index(x64)
        damaged[position + 300] ^= 1
        (root / "corrupt.wim").write_bytes(damaged)
        probe(args, root / "corrupt.wim", error=28)  # Upstream resource hash validation.
        damaged_image = make_image(root, "DAMAGED", {"/Sources/install.wim": bytes(damaged)}, args.mkisofs)
        probe(args, damaged_image, "udf", error=28)

        # Different WIMs at the same path in UDF and ISO must stay distinct.
        alternate = root / "alternate.wim"
        capture(args, second, alternate, "lzx")
        image = make_image(root, "VIEWS", {"/Sources/install.wim": wims["lzx"],
                           "/Sources/otherxx.wim": alternate.read_bytes()}, args.mkisofs)
        rename_udf_entry(image, "install.wim", "movedxx.wim")
        rename_udf_entry(image, "otherxx.wim", "install.wim")
        probe(args, image, "udf", expected=arm64)
        probe(args, image, "rr", expected=x64)
        rename_udf_entry(image, "install.wim", "missing.wim")
        probe(args, image, "udf", error=-1)

        padded = wims["none"] + bytes(-len(wims["none"]) % 2048)
        for long_ad in (False, True):
            image = make_image(root, "FRAGMENT_" + str(int(long_ad)),
                               {"/Sources/install.wim": padded}, args.mkisofs)
            fragment_file(image, padded, long_ad)
            probe(args, image, "udf", expected=x64)

        check_output_limits(args, root)
    print(f"WIM single-file extraction: {runs} probes pass; exact bytes, PE architecture, limits,")
    print("selected views, compression, integrity, cancellation, retry and resource cleanup verified.")


if __name__ == "__main__":
    main()
