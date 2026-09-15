# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare exact random ranges in ISO/UDF views, including generated WIM bytes."""
import argparse
from pathlib import Path
import random
import re
import subprocess
import tempfile

from test_udf_images import make_image, rename_udf_entry
from test_udf_embedded_efi import fragment_file

commands_checked = 0
ranges_checked = 0
bytes_checked = 0


def read_ranges(probe, image, view, path, content, ranges, status=0):
    global commands_checked, ranges_checked, bytes_checked
    command = [str(probe), str(image), view, path]
    for offset, count in ranges:
        command.extend((str(offset), str(count)))
    result = subprocess.run(command, capture_output=True, timeout=10)
    assert result.returncode == status, (command, result.stderr)
    expected = b"" if status else b"".join(content[offset:offset + count] for offset, count in ranges)
    assert result.stdout == expected, (view, path, ranges, len(result.stdout), len(expected))
    commands_checked += 1
    ranges_checked += len(ranges)
    bytes_checked += len(expected)
    metrics = dict((key.decode(), int(value)) for key, value in
                   re.findall(rb"(SIZE|READS|BLOCKS|MAX_BLOCKS|ISO_READS)=(\d+)", result.stderr))
    assert metrics["MAX_BLOCKS"] <= 16, metrics
    if view == "udf":
        assert metrics["ISO_READS"] == 0, metrics
    return metrics


def check_content(args, image, view, path, content):
    size = len(content)
    if size == 0:
        ranges = [(0, 0)]
    else:
        ranges = [(0, min(size, 128)), (size - 1, 1), (0, 1), (size, 0), (0, size)]
        for offset in (1, 2047, 2048, 4095, 10239, 32767, 32768, 65535):
            if offset < size:
                ranges.append((offset, min(size - offset, 32771)))
        rng = random.Random(42)
        for _ in range(30):
            offset = rng.randrange(size)
            ranges.append((offset, rng.randrange(min(size - offset, 65536) + 1)))
    values = read_ranges(args.probe, image, view, path, content, ranges)
    assert values["SIZE"] == size, values
    for offset, count in ((size, 1), (size + 1, 0), (2**64 - 1, 1)):
        values = read_ranges(args.probe, image, view, path, content, [(offset, count)], status=1)
        assert values["READS"] == 0, values
    if size >= 32768:
        values = read_ranges(args.probe, image, view, path, content,
                             [(0, 4), (123, 5), (30000, 100), (0, 4)])
        # A single cached block group serves nonsequential reads in that range.
        assert values["READS"] == 1, values


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("probe", type=Path)
    parser.add_argument("wim_fixture", type=Path)
    parser.add_argument("--mkisofs", required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="rufus-iso-file-") as temporary:
        directory = Path(temporary)
        first = directory / "win11.wim"
        second = directory / "win7.wim"
        for path, version in ((first, "win11"), (second, "win7")):
            subprocess.run([str(args.wim_fixture), str(path), version], check=True, timeout=10)
        wim, alternate = first.read_bytes(), second.read_bytes()
        assert len(wim) > 65536 and len(wim) < 131072
        print(f"Generated WIM sizes: {len(wim)} and {len(alternate)} bytes.")
        # Valid WIMs may have trailing bytes; pad only for the existing UDF
        # extent fixture builder, which rearranges whole allocated blocks.
        padded_wim = wim + bytes(-len(wim) % 2048)
        rng = random.Random(63)
        content = rng.randbytes(81920)
        entries = {"/Sources/Install.wim": wim, "/Sources/Otherxx.wim": alternate,
                   "/Ranges/data.bin": content + b"tail", "/Ranges/empty": b"",
                   "/Ranges/one": b"a", "/ș-é/Name.bin": b"UTF8 path"}
        image = make_image(directory, "RANGES", entries, args.mkisofs)
        for view in ("rr", "joliet", "udf"):
            for path, data in entries.items():
                check_content(args, image, view, path, data)
            for path in ("/Sources", "/absent"):
                read_ranges(args.probe, image, view, path, b"", [(0, 1)], status=3)

        rename_udf_entry(image, "Install.wim", "Movedxx.wim")
        rename_udf_entry(image, "Otherxx.wim", "Install.wim")
        check_content(args, image, "udf", "/Sources/Install.wim", alternate)
        check_content(args, image, "rr", "/Sources/Install.wim", wim)
        rename_udf_entry(image, "Install.wim", "Missing.wim")
        read_ranges(args.probe, image, "udf", "/Sources/Install.wim", b"", [(0, 1)], status=3)

        for long_ad in (False, True):
            image = make_image(directory, "FRAGMENT_" + str(int(long_ad)),
                               {"/Sources/Install.wim": padded_wim}, args.mkisofs)
            fragment_file(image, padded_wim, long_ad)
            ranges = [(0, len(padded_wim)), (4095, 2050), (10239, 4097),
                      (32767, 32769), (4, 19), (len(padded_wim) - 20, 20)]
            read_ranges(args.probe, image, "udf", "/Sources/Install.wim", padded_wim, ranges)
            # The ISO view still uses physical order and must not masquerade as UDF.
            physical = padded_wim[5 * 2048:] + padded_wim[:5 * 2048]
            read_ranges(args.probe, image, "rr", "/Sources/Install.wim", physical, [(0, len(physical))])
    print("ISO/UDF file ranges: exact WIM/data bytes, selected views, cache reuse,")
    print("unaligned reads, multi-cache reads, EOF and fragmented short/long UDF extents pass.")
    print(f"{commands_checked} probe runs, {ranges_checked} ranges, {bytes_checked} bytes compared.")


if __name__ == "__main__":
    main()
