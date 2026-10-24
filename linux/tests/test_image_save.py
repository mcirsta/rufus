#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Batched capture/atomic-output semantics and independent qemu format decoding."""
import argparse
import errno
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import uuid
import zlib

MIB = 1024 * 1024


def gpt_image(size=8 * MIB, sector=512, name='EFI System'):
    image = bytearray(size)
    image[510:512] = b'\x55\xaa'
    image[446:462] = struct.pack('<B3sB3sII', 0, b'\0\2\0', 0xee, b'\xff' * 3, 1, min(size // 512 - 1, 0xffffffff))
    table = bytearray(128 * 128)
    table[:16] = uuid.UUID('c12a7328-f81f-11d2-ba4b-00a0c93ec93b').bytes_le
    table[16:32] = uuid.UUID('12345678-1234-5678-aaaa-123456789abc').bytes_le
    struct.pack_into('<QQQ', table, 32, MIB // sector, size // sector - 2 - len(table) // sector, 0)
    table[56:56 + len(name.encode('utf-16le'))] = name.encode('utf-16le')
    image[2 * sector:2 * sector + len(table)] = table
    backup_table = size - sector - len(table)
    image[backup_table:backup_table + len(table)] = table
    for location, other, entries in ((sector, size // sector - 1, 2), (size - sector, 1, backup_table // sector)):
        header = bytearray(sector)
        struct.pack_into('<8sIIIIQQQQ16sQIII', header, 0, b'EFI PART', 0x10000, 92, 0, 0, location // sector, other,
                         2 + len(table) // sector, size // sector - 2 - len(table) // sector,
                         uuid.UUID('87654321-1234-5678-aaaa-123456789abc').bytes_le, entries, 128, 128, zlib.crc32(table))
        struct.pack_into('<I', header, 16, zlib.crc32(header[:92]))
        image[location:location + sector] = header
    image[2 * MIB:2 * MIB + 65536] = bytes(range(256)) * 256
    return image


def check_case(probe, root, data, suffix, mode='success', sector=512, expected=0, old=None):
    source, output = root / 'source.raw', root / ('saved.' + suffix)
    source.write_bytes(data)
    output.unlink(missing_ok=True)
    if old is not None:
        output.write_bytes(old)
    before = source.stat()
    report = json.loads(subprocess.check_output([str(probe), mode, str(source), str(output), str(sector), str(len(data))], text=True))
    assert report['error'] == expected, (mode, suffix, report, expected)
    if expected == 0:
        assert report['committed'] and report['bytes'] == len(data), report
        assert report['sha256'] == hashlib.sha256(data).hexdigest(), report
        if suffix == 'vhdx':
            # QEMU's VHDX driver accepts only 512-byte logical sectors. Also
            # inspect the BAT directly so the 4Kn encoder has an independent
            # logical reconstruction check.
            with output.open('rb') as saved:
                for block, offset in enumerate(range(0, len(data), 32 * MIB)):
                    ratio = (1 << 23) * sector // (32 * MIB)
                    saved.seek(3 * MIB + (block + block // ratio) * 8)
                    entry = struct.unpack('<Q', saved.read(8))[0]
                    count = min(32 * MIB, len(data) - offset)
                    if entry & 7 == 2:
                        raw_block = bytes(count)
                    else:
                        assert entry & 7 == 6, entry
                        saved.seek(entry & ~((1 << 20) - 1))
                        raw_block = saved.read(count)
                    assert raw_block == data[offset:offset + count]
        if suffix == 'vhd' or (suffix == 'vhdx' and sector == 512):
            decoded = root / 'decoded.raw'
            subprocess.run(['qemu-img', 'convert', '-f', 'vpc' if suffix == 'vhd' else 'vhdx', '-O', 'raw', str(output), str(decoded)], check=True, capture_output=True)
            # VHD geometry may round up the virtual CHS size; the exact stored
            # current-size field and Rufus decoder must retain the captured size.
            raw = decoded.read_bytes()
            assert raw[:len(data)] == data and not any(raw[len(data):]), (suffix, len(raw), len(data))
            decoded.unlink()
        after = source.stat()
        assert (before.st_size, before.st_mtime_ns, before.st_ctime_ns) == (after.st_size, after.st_mtime_ns, after.st_ctime_ns)
    elif mode == 'final-sync-fail':
        assert report['committed'] and output.exists(), report
    else:
        assert not report['committed'], report
        expected_output = b'RACED' if mode == 'destination-race' else old
        assert (output.read_bytes() if output.exists() else None) == expected_output, (mode, report)
    assert not list(root.glob('.rufus-image-*')), mode
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('probe', type=Path)
    args = parser.parse_args()
    assert shutil.which('qemu-img'), 'Independent qemu-img decoder is required'
    checks = 0
    with tempfile.TemporaryDirectory(prefix='rufus-save-') as scratch:
        root = Path(scratch)
        data = gpt_image()
        for suffix, sector in (('vhd', 512), ('vhdx', 512), ('ffu', 512), ('vhdx', 4096), ('ffu', 4096)):
            check_case(args.probe, root, data if sector == 512 else gpt_image(sector=sector), suffix, sector=sector)
            checks += 1
        for data_case in (bytes(512), bytes(33 * MIB + 512), b'\x77' * (2 * MIB + 512)):
            check_case(args.probe, root, data_case, 'vhdx')
            checks += 1
        for mode, error in (('short', 0), ('read-fail', errno.EIO), ('write-fail', errno.ENOSPC),
                            ('sync-fail', errno.EIO), ('cancel-copy', errno.ECANCELED),
                            ('cancel-verify', errno.ECANCELED), ('mutate', errno.ESTALE),
                            ('corrupt', errno.ESTALE), ('source-rw', errno.EINVAL), ('nonempty', errno.EINVAL),
                            ('destination-race', errno.ESTALE), ('directory-sync-fail', errno.EIO), ('final-sync-fail', errno.EIO)):
            check_case(args.probe, root, data, 'vhd', mode, expected=error, old=b'OLD BACKUP')
            checks += 1
        check_case(args.probe, root, data, 'vhd', old=b'OLD BACKUP')
        check_case(args.probe, root, data, 'vhd', sector=4096, expected=errno.ENOTSUP)
        check_case(args.probe, root, data[:-512], 'ffu', expected=errno.ENOTSUP)
        check_case(args.probe, root, bytes(len(data)), 'ffu', expected=errno.ENOTSUP)
        check_case(args.probe, root, gpt_image(name='EFI\u03a9'), 'ffu')
        checks += 5
        check_case(args.probe, root, data, 'vhd', 'destination-race', expected=errno.ESTALE)
        bad_gpt = bytearray(data)
        bad_gpt[512 + 16] ^= 1
        check_case(args.probe, root, bad_gpt, 'ffu', expected=errno.EBADMSG, old=b'OLD BACKUP')
        source, output = root / 'source.raw', root / 'saved.vhdx'
        source.write_bytes(data)
        output.unlink(missing_ok=True)
        output.symlink_to(source)
        report = json.loads(subprocess.check_output([str(args.probe), 'success', str(source), str(output), '512', str(len(data))], text=True))
        assert report['error'] == errno.EINVAL and output.is_symlink() and source.read_bytes() == data
        checks += 3
    print(f'Device image saving: {checks} batched format, consistency, cancellation and atomic-output cases')


if __name__ == '__main__':
    main()
