#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Grouped independent full-FFU deployment oracle; no physical host devices."""
import errno
import hashlib
import json
from pathlib import Path
import random
import struct
import subprocess
import sys
import tempfile
import zlib
from ffu_fixture import BLOCK, make_ffu
from test_image_save import gpt_image


def relocated_gpt(image, capacity, sector):
    result = bytearray(image)
    old = len(image) // sector
    new = capacity // sector
    primary = bytearray(image[sector:2 * sector])
    secondary = bytearray(image[-sector:])
    table_lba = struct.unpack_from('<Q', secondary, 72)[0]
    count, entry_bytes = struct.unpack_from('<II', secondary, 80)
    table_bytes = (count * entry_bytes + sector - 1) // sector * sector
    table = image[table_lba * sector:table_lba * sector + table_bytes]
    for header, offset in ((primary, sector), (secondary, capacity - sector)):
        for field in (24, 32, 48, 72):
            value = struct.unpack_from('<Q', header, field)[0]
            if field == 48 or field == 24 and offset != sector or field in (32, 72) and value > 2:
                struct.pack_into('<Q', header, field, value + new - old)
        struct.pack_into('<I', header, 16, 0)
        struct.pack_into('<I', header, 16, zlib.crc32(header[:92]))
    mbr = bytearray(image[:sector])
    struct.pack_into('<I', mbr, 458, min(new - 1, 0xffffffff))
    return [(0, mbr), (sector, primary), ((table_lba + new - old) * sector, table), (capacity - sector, secondary)]


def main():
    writer = sys.argv[1]
    cases = []
    with tempfile.TemporaryDirectory(prefix='rufus-ffu-deploy-') as temporary:
        root = Path(temporary)
        source, target = root / 'source.ffu', root / 'target.img'

        def run(entries, capacity=12 * BLOCK + 1536, sector=512, version=1, algorithm=0,
                captured=8 * BLOCK, patches=(), expected_error=0, fault='none', label=''):
            packed, logical, offsets = make_ffu(entries, disk_blocks=captured // BLOCK,
                sector=sector, version=version, algorithm=algorithm)
            source.write_bytes(packed)
            poison = b'\xa5' * capacity
            target.write_bytes(poison)
            expected = bytearray(poison)
            for data, locations in entries:
                for method, block in locations:
                    position = block * BLOCK if method == 0 else capacity - (block + 1) * BLOCK
                    expected[position:position + len(data)] = data
            for position, data in patches: expected[position:position + len(data)] = data
            report = json.loads(subprocess.check_output([writer, str(source), str(target), str(sector),
                '1', str(2**63 - 1), fault], text=True, timeout=60))
            assert report['error'] == expected_error, (label, report)
            actual = target.read_bytes()
            assert actual == (poison if expected_error else expected), (label, report)
            if not expected_error:
                assert 0 < report['written'] <= capacity and report['verified'] == report['written'], report
                assert report['syncs'] == 1 and report['phase'] == 4, report
            assert source.read_bytes() == packed
            cases.append({'case': label, 'error': report['error'], 'written': report['written'],
                'sha256': hashlib.sha256(actual).hexdigest()})

        first = bytearray(b'A' * BLOCK); first[510:512] = b'\x55\xaa'
        base = [(bytes(first), [(0, 0)]), (b'B' * (2 * BLOCK), [(2, 1)]),
                (b'C' * BLOCK, [(0, 7)]), (b'D' * BLOCK, [(2, 0)])]
        for version, algorithm in ((1, 0), (2, 0), (3, 0), (3, 1), (3, 2), (3, 3), (3, 4)):
            for capacity in (8 * BLOCK, 8 * BLOCK + 512, 12 * BLOCK + 1536):
                run(base, capacity=capacity, version=version, algorithm=algorithm,
                    label=f'V{version} codec{algorithm} capacity{capacity}')
        randomizer = random.Random(40)
        for iteration in range(16):
            entries = [(bytes(first), [(0, 0)])]
            for i in range(12):
                count = randomizer.randrange(1, 4)
                method = randomizer.choice((0, 2))
                block = randomizer.randrange(0, 8 - count + 1) if method == 0 else randomizer.randrange(count - 1, 8)
                entries.append((bytes([50 + i]) * (count * BLOCK), [(method, block)]))
            run(entries, capacity=8 * BLOCK + randomizer.randrange(1, 8) * 512,
                label=f'ordered overlapping begin/end grids {iteration}')
        run(base, fault='plan-vtsi-layout', expected_error=errno.ESTALE, label='frozen container layout refusal')
        run(base, capacity=8 * BLOCK + 1, expected_error=errno.EINVAL, label='unaligned capacity refusal')
        for sector in (512, 4096):
            raw = gpt_image(sector=sector)
            entries = [(raw[:-BLOCK], [(0, 0)]), (raw[-BLOCK:], [(2, 0)])]
            # A native full capture uses beginning-relative descriptors throughout.
            for end_relative in (False, True):
                if not end_relative: entries[-1] = (raw[-BLOCK:], [(0, len(raw) // BLOCK - 1)])
                else: entries[-1] = (raw[-BLOCK:], [(2, 0)])
                for capacity in (len(raw) + 3 * sector, len(raw) * 2):
                    run(entries, captured=len(raw), capacity=capacity, sector=sector,
                        patches=relocated_gpt(raw, capacity, sector), label=f'GPT {sector} end{end_relative} size{capacity}')
            malformed = bytearray(raw); malformed[-sector + 16] ^= 1
            run([(malformed, [(0, 0)])], captured=len(raw), capacity=len(raw) * 2, sector=sector,
                expected_error=errno.EBADMSG, label=f'GPT {sector} bad backup CRC prewrite refusal')
        # Same captured reconstruction, different placement on a larger disk.
        # Fake reads changing only the authenticated layout must not bypass the
        # frozen plan before copy or produce successful READY after verification.
        original = [(bytes(first), [(0, 0)]), (b'Z' * BLOCK, [(0, 7)])]
        alternative = [(bytes(first), [(0, 0)]), (b'Z' * BLOCK, [(2, 0)])]
        source.write_bytes(make_ffu(original)[0])
        Path(str(source) + '.replacement.ffu').write_bytes(make_ffu(alternative)[0])
        for phase in ('preflight', 'copy', 'verify'):
            capacity = 12 * BLOCK
            poison = b'\xa5' * capacity; target.write_bytes(poison)
            report = json.loads(subprocess.check_output([writer, str(source), str(target), '512', '1',
                str(2**63 - 1), phase + '-layout-substitute'], text=True, timeout=60))
            assert report['error'] == errno.ESTALE, (phase, report)
            if phase != 'verify': assert report['written'] == 0 and target.read_bytes() == poison, report
            else: assert report['written'] > 0 and report['verified'] == 0, report
            cases.append({'case': 'same-content layout substitution ' + phase, 'error': report['error']})
        # The source/helper claim matrix is reused. Exercise only the new sparse
        # transfer callback's partial write, cancellation, sync and readback paths.
        packed, logical, offsets = make_ffu(base)
        source.write_bytes(packed)
        for fault, error in (('short', 0), ('eintr', 0), ('write-fail', errno.ENOSPC),
                ('sync-fail', errno.EIO), ('corrupt', errno.EBADMSG), ('write-cancel', errno.ECANCELED),
                ('verify-fail', errno.EIO), ('verify-cancel', errno.ECANCELED)):
            target.write_bytes(b'\xa5' * len(logical))
            report = json.loads(subprocess.check_output([writer, str(source), str(target), '512', '1',
                str(2**63 - 1), fault], text=True, timeout=60))
            assert report['error'] == error and report['written'] > 0, (fault, report)
            assert report['syncs'] > 0 and (error or report['verified'] == report['written']), (fault, report)
            cases.append({'case': 'sparse transfer ' + fault, 'error': error, 'written': report['written']})
        print(json.dumps({'passed': len(cases), 'cases': cases}, indent=2))


if __name__ == '__main__':
    main()
