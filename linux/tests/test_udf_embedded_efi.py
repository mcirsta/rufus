# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise embedded FAT through real UDF extents and the shared coordinator."""
import argparse
import binascii
from pathlib import Path
import subprocess
import tempfile

from test_embedded_efi import ARCHES, inspect, make_fat, partitioned
from test_iso_analyze import analyze
from test_udf_images import make_image, rename_udf_entry


def check_formats(directory, args):
    first = None
    for fat, sector, filler in ((12, 512, 0), (16, 512, 0), (32, 512, 0),
                                (12, 4096, 0), (32, 512, 33554432), (12, 512, 1430000)):
        name = f"FAT{fat}_{sector}_{filler}"
        data = make_fat(directory, name, args, ["boot" + arch + ".efi" for arch in ARCHES],
                        fat=fat, sector=sector, filler=filler)
        image = make_image(directory, name, {"/boot/efi.img": data}, args.mkisofs)
        values, logs = inspect(args.probe, image, found=True, view="udf")
        assert int(values["BLOCKS"]) < 128, values
        for arch in ARCHES:
            assert f"'boot{arch}.efi'" in logs, logs
        analyze(args.scanner, image, status=3, pending=32, EFI=0x8000, FILESYSTEM=2)
        if first is None:
            first = data, image, values
    data, image, values = first
    for point in range(1, int(values["READS"]) + 1):
        inspect(args.probe, image, status=3, fault=f"fail-read:{point}", view="udf")
    for point in (1, 3, 6, 12):
        inspect(args.probe, image, status=2, fault=f"cancel:{point}", view="udf")
    for fault, status in (("fail-context", 1), ("fail-open", 3), ("fail-cache", 3),
                          ("fail-seek", 3), ("fail-root", 3), ("fail-file", 3)):
        inspect(args.probe, image, status=status, fault=fault, view="udf")
    inspect(args.probe, image, found=True, view="udf")
    return data


def check_results_and_views(directory, args, fat):
    empty = make_fat(directory, "empty", args)
    for name, content, pending, found in (
        ("ABSENT", empty, 0, False), ("UNREADABLE", bytes(512), 8, False),
        ("EMPTY", b"", 8, False), ("TRUNCATED", fat[:len(fat) // 2], 8, False),
        ("PARTITIONED", partitioned(fat), 32, True),
    ):
        image = make_image(directory, name, {"/boot/efi.img": content}, args.mkisofs)
        inspect(args.probe, image, found=found, status=3 if pending == 8 else 0, view="udf")
        analyze(args.scanner, image, status=3 if pending else 0, pending=pending,
                EFI=0x8000 if found else 0, FILESYSTEM=2)
    image = make_image(directory, "VIEWS", {"/boot/efi.img": fat, "/boot/alt.img": empty}, args.mkisofs)
    # Swap valid UDF names without changing the ISO/RR directory view.
    rename_udf_entry(image, "efi.img", "tmp.img")
    rename_udf_entry(image, "alt.img", "efi.img")
    rename_udf_entry(image, "tmp.img", "alt.img")
    inspect(args.probe, image, found=True, view="iso")
    inspect(args.probe, image, found=False, view="udf")
    analyze(args.scanner, image, EFI=0, FILESYSTEM=2)
    rename_udf_entry(image, "efi.img", "goneimg")
    inspect(args.probe, image, status=3, view="udf")
    inspect(args.probe, image, status=3, path="/boot", view="udf")
    inspect(args.probe, image, found=True, view="iso")
    analyze(args.scanner, image, EFI=0, FILESYSTEM=2)

    image = make_image(directory, "UDF_ONLY", {"/MiXeD/ș-é/EFI.IMG": fat}, args.mkisofs)
    data = bytearray(image.read_bytes())
    for offset in range(16 * 2048, 256 * 2048, 2048):
        if data[offset + 1:offset + 6] == b"CD001":
            data[offset:offset + 2048] = bytes(2048)
    image.write_bytes(data)
    inspect(args.probe, image, found=True, path="/MiXeD/ș-é/EFI.IMG", view="udf")
    inspect(args.probe, image, status=3, path="/mixed/ș-é/EFI.IMG", view="udf")
    analyze(args.scanner, image, status=3, pending=32, EFI=0x8000,
            FILESYSTEM=2, VOLUME_SIZE_KNOWN=0)


def file_entry(data, size):
    matches = []
    for offset in range(0, len(data) - 2048, 2048):
        if (data[offset:offset + 2] == b"\x05\x01" and data[offset + 27] == 5
                and int.from_bytes(data[offset + 56:offset + 64], "little") == size):
            matches.append(offset)
    assert len(matches) == 1, matches
    return matches[0]


def update_tag(data, offset, body_size):
    data[offset + 10:offset + 12] = body_size.to_bytes(2, "little")
    crc = binascii.crc_hqx(data[offset + 16:offset + 16 + body_size], 0)
    data[offset + 8:offset + 10] = crc.to_bytes(2, "little")
    data[offset + 4] = 0
    data[offset + 4] = sum(data[offset:offset + 16]) & 255


def fragment_file(image, content, long_ad):
    # Reorder blocks within this file's existing allocation. Its UDF descriptors
    # restore the original logical order; no host device or external extent exists.
    data = bytearray(image.read_bytes())
    entry = file_entry(data, len(content))
    assert data[entry + 34] & 7 == 0
    attrs = int.from_bytes(data[entry + 168:entry + 172], "little")
    assert int.from_bytes(data[entry + 172:entry + 176], "little") == 8
    allocation = entry + 176 + attrs
    location = int.from_bytes(data[allocation + 4:allocation + 8], "little")
    physical = data.find(content)
    assert physical >= 0 and physical % 2048 == 0 and len(content) % 2048 == 0
    lengths = (2 * 2048, 3 * 2048, len(content) - 5 * 2048)
    data[physical:physical + len(content)] = content[5 * 2048:] + content[:5 * 2048]
    positions = (location + lengths[2] // 2048, location + lengths[2] // 2048 + 2, location)
    descriptors = bytearray()
    for length, position in zip(lengths, positions):
        descriptors += length.to_bytes(4, "little") + position.to_bytes(4, "little")
        if long_ad:
            descriptors += bytes(8)  # Partition map zero, implementation-use zero.
    data[entry + 34] = (data[entry + 34] & ~7) | int(long_ad)
    data[entry + 172:entry + 176] = len(descriptors).to_bytes(4, "little")
    assert attrs + len(descriptors) <= 2048 - 176
    data[allocation:allocation + len(descriptors)] = descriptors
    update_tag(data, entry, 160 + attrs + len(descriptors))
    image.write_bytes(data)
    return entry, attrs


def read_range(probe, image, offset, expected, fault=False):
    command = [str(probe), str(image), "/boot/efi.img", str(offset), "16"]
    if fault:
        command.append("short-read")
    result = subprocess.run(command, capture_output=True, timeout=10)
    assert result.returncode == (1 if expected is None else 0), (offset, result.stderr)
    assert result.stdout == (b"" if expected is None else expected), (offset, result.stderr)


def check_extents(directory, args, fat):
    for long_ad in (False, True):
        image = make_image(directory, f"EXTENTS_{int(long_ad)}", {"/boot/efi.img": fat}, args.mkisofs)
        entry, attrs = fragment_file(image, fat, long_ad)
        # One read stops at the current extent, including when the seek is inside it.
        for offset, end in ((0, 4096), (2048, 4096), (4096, 10240), (8192, 10240),
                            (10240, 43008), (len(fat) - 2048, len(fat)), (len(fat), len(fat))):
            read_range(args.reader, image, offset, fat[offset:end])
        read_range(args.reader, image, 1, None)
        read_range(args.reader, image, len(fat) + 2048, None)
        read_range(args.reader, image, 4096, None, fault=True)
        values, logs = inspect(args.probe, image, found=True, view="udf")
        for arch in ARCHES:
            assert f"'boot{arch}.efi'" in logs, logs
        analyze(args.scanner, image, status=3, pending=32, EFI=0x8000, FILESYSTEM=2)
        for point in range(1, int(values["READS"]) + 1):
            inspect(args.probe, image, status=3, fault=f"fail-read:{point}", view="udf")
        # Corrected-reader-only bounds/unsupported-layout checks use a copied FE.
        original = image.read_bytes()
        changes = [(168, (1873).to_bytes(4, "little")), (172, (7).to_bytes(4, "little")),
                   (176 + attrs, bytes(4)), (176 + attrs, (0x80000800).to_bytes(4, "little")),
                   (34, (3).to_bytes(2, "little")), (34, (2).to_bytes(2, "little"))]
        if long_ad:
            changes.append((184 + attrs, (1).to_bytes(2, "little")))
        for relative, value in changes:
            data = bytearray(original)
            data[entry + relative:entry + relative + len(value)] = value
            image.write_bytes(data)
            inspect(args.probe, image, status=3, view="udf")
        image.write_bytes(original)
        inspect(args.probe, image, found=True, view="udf")

    # Final bytes may end within a UDF block; padding is never returned as file data.
    content = fat + b"tail"
    image = make_image(directory, "TAIL", {"/boot/efi.img": content}, args.mkisofs)
    read_range(args.reader, image, len(fat), b"tail")
    read_range(args.reader, image, len(fat) - 2048, content[-2052:])
    inspect(args.probe, image, found=True, view="udf")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("probe", type=Path)
    parser.add_argument("scanner", type=Path)
    parser.add_argument("reader", type=Path)
    parser.add_argument("--mkisofs", required=True)
    parser.add_argument("--mkfs-fat", required=True)
    parser.add_argument("--mcopy", required=True)
    parser.add_argument("--mmd", required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="rufus-udf-efi-") as temporary:
        directory = Path(temporary)
        fat = check_formats(directory, args)
        check_results_and_views(directory, args, fat)
        check_extents(directory, args, fat)
    print("UDF embedded EFI: native FAT formats, selected views, fragmented extents,")
    print("bounded seek/read, failure/cancellation and shared coordination pass.")


if __name__ == "__main__":
    main()
