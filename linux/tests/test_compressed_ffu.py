#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Batched FFU parser, mapping, XPRESS, integrity and native-write checks."""
import errno
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

from ffu_fixture import BLOCK, make_ffu


def invoke(probe, path, maximum):
    result = subprocess.run([probe, str(maximum), 'hash', str(path)], text=True,
                            capture_output=True, check=True, timeout=30)
    return dict(line.split(' ', 1) for line in result.stdout.splitlines())


def check(probe, path, image, logical=None, expected=0, maximum=2**63 - 1):
    path.write_bytes(image)
    report = invoke(probe, path, maximum)
    assert int(report['error']) == expected, report
    if expected:
        assert report['decoded'] == '0' and report['sha256'] == '', report
    else:
        assert int(report['type']) == 10 and int(report['decoded']) == len(logical), report
        assert report['sha256'] == hashlib.sha256(logical).hexdigest(), report
        assert report['prefix'] == logical[:512].hex(), report


def main():
    probe, writer, fault = sys.argv[1:]
    prefix = bytearray(b'A' * BLOCK)
    prefix[510:512] = b'\x55\xaa'
    entries = [(bytes(BLOCK), [(0, 0)]), (b'B' * BLOCK, [(0, 3), (0, 2)]),
               (b'Z' * BLOCK, [(2, 0)]), (bytes(prefix), [(0, 0)])]
    count = 0
    with tempfile.TemporaryDirectory(prefix='rufus-ffu-') as temporary:
        root = Path(temporary)
        path = root / 'source.ffu'
        for version, algorithm, sector, extended in ((1, 0, 512, False), (2, 0, 512, False),
                (3, 0, 512, False), (3, 1, 512, True), (3, 3, 512, False), (1, 0, 4096, True)):
            image, logical, offsets = make_ffu(entries, version=version, algorithm=algorithm,
                                                sector=sector, extended=extended)
            check(probe, path, image, logical)
            target = root / 'target.img'
            target.write_bytes(bytes(len(logical)))
            result = subprocess.run([writer, str(path), str(target), str(sector), '0', str(2**63 - 1), 'none'],
                                    text=True, capture_output=True, check=True, timeout=30)
            report = json.loads(result.stdout)
            assert report['error'] == 0 and report['compression'] == 10, report
            assert target.read_bytes() == logical, report
            count += 2
        # A contiguous multi-block payload is legal in an uncompressed store.
        image, logical, offsets = make_ffu([(bytes(prefix) + b'Q' * BLOCK, [(0, 0)])])
        check(probe, path, image, logical)
        count += 1
        image, logical, offsets = make_ffu([(bytes(prefix) + b'Q' * BLOCK, [(0, 0), (0, 1)])])
        check(probe, path, image, logical)
        count += 1
        # Exercise the 32-bit extended match length (one literal and offset-one match).
        packed = struct.pack('<I', 0x40000000) + b'R' + b'\x07\x00\x0f\xff\x00\x00' + struct.pack('<I', BLOCK - 4)
        image, logical, offsets = make_ffu([(b'R' * BLOCK, [(0, 0)], packed)], version=3, algorithm=3)
        check(probe, path, image, logical)
        count += 1
        for packed_bad in (packed[:-1], packed[:5] + b'\xff\xff' + packed[7:],
                           packed[:5] + b'\x07\x00\x0f\xff\x01\x00', bytes(4)):
            bad, unused, unused_offsets = make_ffu([(b'R' * BLOCK, [(0, 0)], packed_bad)], version=3, algorithm=3)
            check(probe, path, bad, expected=errno.EBADMSG)
            count += 1
        image, logical, offsets = make_ffu(entries)
        # Corruption in unused/overwritten payloads is still rejected before output.
        for offset in (offsets['image'] + 100, offsets['store'] + 30, offsets['data'], len(image) - 1, 32):
            bad = image.copy()
            bad[offset] ^= 1
            check(probe, path, bad, expected=errno.EBADMSG)
            count += 1
        for offset, fmt, value, error in ((4, '<B', 0, errno.EBADMSG), (16, '<I', 0, errno.ENOTSUP),
                (20, '<I', 0x8004, errno.ENOTSUP), (28, '<I', 1, errno.EBADMSG),
                (offsets['store'], '<I', 1, errno.EBADMSG),
                (offsets['store'] + 4, '<H', 5, errno.ENOTSUP),
                (offsets['store'] + 212, '<I', 15, errno.EBADMSG),
                (offsets['store'] + 216, '<I', 1, errno.EBADMSG),
                (offsets['store'] + 240, '<I', 1000, errno.EBADMSG),
                (offsets['descriptors'], '<I', 0, errno.EBADMSG),
                (offsets['descriptors'] + 4, '<I', 0, errno.EBADMSG),
                (offsets['descriptors'] + 8, '<I', 1, errno.ENOTSUP),
                (offsets['descriptors'] + 12, '<I', 99, errno.EBADMSG)):
            bad = image.copy()
            struct.pack_into(fmt, bad, offset, value)
            check(probe, path, bad, expected=error)
            count += 1
        for length in (0, 31, offsets['image'] + 23, offsets['store'] + 247, len(image) - 1):
            check(probe, path, image[:length], expected=errno.EBADMSG)
            count += 1
        check(probe, path, image + b'junk', expected=errno.EBADMSG)
        check(probe, path, image, expected=errno.EFBIG, maximum=len(logical) - 1)
        count += 2
        for algorithm in (99,):
            bad, unused, unused_offsets = make_ffu(entries, version=3, algorithm=algorithm)
            check(probe, path, bad, expected=errno.ENOTSUP)
            count += 1
        bad, unused, extra = make_ffu(entries, version=2)
        struct.pack_into('<H', bad, extra['store'] + 248, 2)
        check(probe, path, bad, expected=errno.ENOTSUP)
        count += 1
        # Corrupt containers must fail preparation with the poisoned target intact.
        for version, algorithm in ((1, 0), (3, 3)):
            bad, unused, extra = make_ffu(entries, version=version, algorithm=algorithm)
            bad[-1] ^= 1
            path.write_bytes(bad)
            target = root / 'refused.img'
            poison = b'\xa5' * len(logical)
            target.write_bytes(poison)
            result = subprocess.run([writer, str(path), str(target), '512', '0', str(2**63 - 1), 'none'],
                                    text=True, capture_output=True, check=True, timeout=30)
            report = json.loads(result.stdout)
            assert report['error'] == errno.EBADMSG and target.read_bytes() == poison, report
            result = subprocess.run([fault, '0', str(len(logical)), str(path)],
                                    text=True, capture_output=True, check=True, timeout=30)
            report = dict(line.split(' ', 1) for line in result.stdout.splitlines())
            assert report['received'] == '0' and int(report['error']) == errno.EBADMSG, report
            count += 2
        path.write_bytes(image)
        for mode, error in ((1, 0), (2, 0), (3, errno.EIO), (4, errno.EIO), (5, errno.EIO),
                            (6, errno.ECANCELED), (7, errno.ECANCELED), (8, errno.ENOSPC), (9, errno.EIO)):
            result = subprocess.run([fault, str(mode), str(len(logical)), str(path)], check=True,
                                    text=True, capture_output=True, timeout=30)
            report = dict(line.split(' ', 1) for line in result.stdout.splitlines())
            assert int(report['error']) == error and report['offset'] == '7' and report['readonly'] == '1', report
            assert report['result_empty'] == ('0' if error == 0 else '1'), report
            count += 1
        print(f'FFU: {count} format/mapping/XPRESS/integrity/source/native-write cases passed; shared helper faults reused')


if __name__ == '__main__':
    main()
