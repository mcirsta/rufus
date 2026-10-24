#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""One grouped multi-store FFU corpus; reuse the shared writer's fault matrices."""
import errno
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

from ffu_fixture import BLOCK, make_multistore


def read(probe, path, store):
    output = subprocess.check_output([probe, str(2**63 - 1), 'hash', str(path), str(store)], text=True)
    return dict(line.split(' ', 1) for line in output.splitlines())


def main():
    reader, writer = sys.argv[1:]
    cases = []
    with tempfile.TemporaryDirectory(prefix='rufus-ffu-stores-') as temporary:
        root = Path(temporary)
        source, target = root / 'stores.ffu', root / 'target.img'
        stores = [
            {'payloads': [(b'A' * BLOCK, [(0, 0)]), (b'B' * BLOCK * 2, [(0, 2), (2, 1)]),
                          (b'C' * BLOCK, [(0, 0)])]},
            {'sector': 4096, 'disk_blocks': 10, 'payloads': [(b'D' * BLOCK, [(0, 1)]), (b'E' * BLOCK, [(2, 0)])]},
            {'disk_bytes': 12 * BLOCK + 1536, 'payloads': [(b'F' * BLOCK, [(0, 3)]), (b'G' * BLOCK, [(2, 0)])]}
        ]
        packed, logicals, offsets = make_multistore(stores)
        source.write_bytes(packed)
        listing = json.loads(subprocess.check_output([reader, 'ffu-stores', str(source)], text=True))
        assert listing['error'] == 0 and len(listing['stores']) == 3, listing
        for index, description in enumerate(listing['stores']):
            assert description['index'] == index + 1 and description['bytes'] == len(logicals[index])
            assert description['sector'] == stores[index].get('sector', 512)
            assert bytes.fromhex(description['path']).decode('utf-16le') == 'VenHw(Fixture)'
        cases.append('authenticated store list, independent geometries and UTF-16 paths')
        for index, logical in enumerate(logicals, 1):
            report = read(reader, source, index)
            assert report['error'] == '0' and report['store'] == str(index) and report['stores'] == '3', report
            assert report['sha256'] == hashlib.sha256(logical).hexdigest() and report['prefix'] == logical[:512].hex()
            cases.append(f'store {index} logical reconstruction and caller offset')
            for larger in (False, True):
                sector = stores[index - 1].get('sector', 512)
                capacity = len(logical) + (4 * BLOCK if larger else 0)
                poison = bytes([0xa5]) * capacity
                target.write_bytes(poison)
                expected = bytearray(poison)
                for data, locations in stores[index - 1]['payloads']:
                    for method, block in locations:
                        position = block * BLOCK if method == 0 else capacity - (block + 1) * BLOCK
                        expected[position:position + len(data)] = data
                result = json.loads(subprocess.check_output([writer, str(source), str(target), str(sector), '1',
                    str(2**63 - 1), 'none', str(index)], text=True))
                assert result['error'] == 0 and result['written'] == result['verified'] > 0, result
                assert target.read_bytes() == expected and source.read_bytes() == packed
                cases.append(f'store {index} dirty-target write/readback, larger={larger}')
        for index in (0, 4, 65535):
            report = read(reader, source, index)
            assert int(report['error']) == (errno.EDESTADDRREQ if index == 0 else errno.EINVAL), report
            assert report['decoded'] == '0' and report['sha256'] == ''
            cases.append(f'no implicit or invalid store {index}')
        poison = bytes([0xa5]) * (16 * BLOCK)
        corruptions = [offsets['image'] + 60, offsets['stores'][1] + 30]
        corruptions += [position + 7 for position in offsets['data']]
        for position in corruptions:
            bad = packed.copy()
            bad[position] ^= 1
            source.write_bytes(bad)
            report = read(reader, source, 1)
            assert int(report['error']) == errno.EBADMSG and report['decoded'] == '0', report
            target.write_bytes(poison)
            result = json.loads(subprocess.check_output([writer, str(source), str(target), '512', '1',
                str(2**63 - 1), 'none', '1'], text=True))
            assert result['error'] == errno.EBADMSG and result['written'] == 0 and target.read_bytes() == poison
            cases.append(f'corruption in any metadata/payload at {position} refuses before writes')
        for location, fmt, value in ((offsets['stores'][1] + 248, '<H', 2),
                                     (offsets['stores'][1] + 250, '<H', 1),
                                     (offsets['stores'][2] + 252, '<Q', BLOCK)):
            bad = packed.copy()
            struct.pack_into(fmt, bad, location, value)
            source.write_bytes(bad)
            report = read(reader, source, 1)
            assert int(report['error']) in (errno.ENOTSUP, errno.EBADMSG) and report['decoded'] == '0', report
            cases.append('inconsistent store count/index/payload length')
        partial_stores = [stores[0], {'payloads': [(b'U' * BLOCK, [(0, 0)])],
            'partial': True, 'validations': [(0, 0, b'BASE')]}]
        partial, unused, unused_offsets = make_multistore(partial_stores)
        source.write_bytes(partial)
        for selected, matches in ((1, False), (2, True), (2, False)):
            initial = bytearray(poison)
            if matches: initial[:4] = b'BASE'
            target.write_bytes(initial)
            result = json.loads(subprocess.check_output([writer, str(source), str(target), '512', '1',
                str(2**63 - 1), 'none', str(selected)], text=True))
            expected_error = errno.ESTALE if selected == 2 and not matches else 0
            assert result['error'] == expected_error, result
            if expected_error:
                assert result['written'] == 0 and target.read_bytes() == initial
            else:
                expected = bytearray(initial)
                for data, locations in partial_stores[selected - 1]['payloads']:
                    for method, block in locations:
                        position = block * BLOCK if method == 0 else len(expected) - (block + 1) * BLOCK
                        expected[position:position + len(data)] = data
                assert target.read_bytes() == expected and result['written'] == result['verified'] > 0
            cases.append(f'only selected store base comparisons apply: store={selected}, matches={matches}')
        # A different store with identical logical bytes must still change the frozen layout.
        identical, unused, unused_offsets = make_multistore([stores[0], stores[0]])
        source.write_bytes(identical)
        target.write_bytes(poison)
        result = json.loads(subprocess.check_output([writer, str(source), str(target), '512', '1',
            str(2**63 - 1), 'ffu-store-change', '1'], text=True))
        assert result['error'] == errno.ESTALE and result['written'] == 0 and target.read_bytes() == poison, result
        cases.append('frozen store identity differs even for identical decoded content')
    assert not root.exists()
    print(json.dumps({'passed': len(cases), 'cases': cases, 'temporary_media_removed': True}, indent=2))


if __name__ == '__main__':
    main()
