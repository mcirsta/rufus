#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""One ZIP supplement corpus: codecs, paths, replacement, bounds and failures."""
import errno
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zipfile
from test_compressed_zip import archive, record


def main():
    probe = sys.argv[1]
    cases = []
    with tempfile.TemporaryDirectory(prefix='rufus-zip-supplement-') as scratch:
        root = Path(scratch); source = root / 'extra.zip'

        def run(packed, expected=0, mode='none', check=False, maximum_file=2**63-1, maximum_total=2**63-1):
            source.write_bytes(packed)
            output = root / f'output-{len(cases)}'
            output.mkdir()
            report = json.loads(subprocess.check_output([probe, str(source), '-' if check else str(output), mode,
                str(maximum_file), str(maximum_total)], text=True))
            assert report['error'] == expected and report['positionKept'], report
            if mode != 'mutate-write': assert source.read_bytes() == packed
            return output, report

        for method in (0, 8, 12, 14, 95):
            for zip64 in (False, True):
                rows = [record(b'', 0, b'empty/'), record(b'first', method, b'folder/first.txt'),
                        record(b'Linux supplement\0' * 10000, method, 'folder/fișiere.txt'.encode(), flags=0x800 | (2 if method == 14 else 0)),
                        record(b'', method, b'zero.txt')]
                output, report = run(archive(rows, zip64=zip64))
                assert report['files'] == 3 and report['directories'] == 1
                assert int(report['bytes']) == 5 + len(b'Linux supplement\0' * 10000)
                assert (output / 'folder/first.txt').read_bytes() == b'first'
                assert (output / 'folder/fișiere.txt').read_bytes() == b'Linux supplement\0' * 10000
                assert (output / 'empty').is_dir() and (output / 'zero.txt').read_bytes() == b''
                cases.append(f'multi-entry method {method}, ZIP64={zip64}, independent bytes and source offset')
        for name, expected_name in ((b'folder\\file.txt', 'folder/file.txt'), (b'./folder//file.txt', 'folder/file.txt'),
                                    (b'caf\x82.txt', 'café.txt')):
            output, report = run(archive([record(b'content', 8, name)]))
            assert (output / expected_name).read_bytes() == b'content'
            cases.append('normalized portable/CP437 name ' + expected_name)
        for order, expected in ((None, b'new'), ([1, 0], b'old')):
            output, report = run(archive([record(b'old', 8, b'same.txt'), record(b'new', 8, b'same.txt')], order=order))
            assert report['files'] == 2 and (output / 'same.txt').read_bytes() == expected
            cases.append('CDF order replacement ' + str(order))
        output, report = run(archive([]))
        assert not list(output.iterdir()) and report['files'] == 0
        cases.append('empty archive succeeds without output')
        output, report = run(archive([record(b'streamed content', 8, b'stream.txt', flags=8)]))
        assert (output / 'stream.txt').read_bytes() == b'streamed content'
        cases.append('streaming data descriptor uses the CDF sizes and CRC')
        rows = [record(b'', 0, f'{index}.txt'.encode()) for index in range(65536)]
        packed = bytearray(archive(rows, zip64=True))
        zip64_end = packed.rfind(b'PK\x06\x06')
        directory = struct.unpack_from('<Q', packed, zip64_end + 48)[0]
        struct.pack_into('<I', packed, len(packed) - 6, directory)
        output, report = run(packed, check=True)
        assert report['files'] == 65536 and not list(output.iterdir())
        cases.append('ZIP64 count sentinel with a fitting ordinary directory offset')
        del rows, packed
        # Full validation prevents a bad later entry from publishing earlier files.
        for name in (b'../outside', b'folder/../../outside', b'/absolute', b'\\\\server\\share', b'C:\\drive',
                     b'a\0b', b'/'.join([b'a'] * 129)):
            expected = errno.EBADMSG if b'\0' in name else errno.ENAMETOOLONG if name.count(b'/') == 128 else errno.EPERM
            output, report = run(archive([record(b'keep', 0, b'first.txt'), record(b'bad', 8, name)]), expected)
            assert not list(output.iterdir())
            cases.append('unsafe later path refused before writes: ' + repr(name[:40]))
        for kind in ('crc', 'method', 'encrypted', 'count', 'local-name', 'unicode'):
            rows = [record(b'keep', 0, b'first.txt'), record(b'bad content', 8, b'second.txt')]
            expected = errno.EBADMSG
            if kind == 'method': rows[1] = record(b'bad', 99, b'second.txt'); expected = errno.ENOTSUP
            if kind == 'encrypted': rows[1] = record(b'bad', 0, b'second.txt', flags=1); expected = errno.ENOTSUP
            if kind == 'crc': rows[1]['crc'] ^= 1
            if kind == 'local-name': rows[1]['name'] = b'changed.txt'
            if kind == 'unicode': rows[1] = record(b'bad', 0, b'\xff.txt', flags=0x800); expected = errno.EILSEQ
            packed = bytearray(archive(rows))
            if kind == 'count': struct.pack_into('<HH', packed, len(packed) - 14, 3, 3)
            output, report = run(bytes(packed), expected)
            assert not list(output.iterdir()), kind
            cases.append('malformed later entry prewrite refusal: ' + kind)
        packed = archive([record(b'first', 0, b'first.txt'), record(b'data' * 50000, 8, b'second.txt')])
        for limits in ((4, 2**63-1), (2**63-1, 100)):
            output, report = run(packed, errno.EFBIG, maximum_file=limits[0], maximum_total=limits[1])
            assert not list(output.iterdir())
            cases.append('file/total expanded bound ' + str(limits))
        for mode, error in (('short', 0), ('write-fail', errno.ENOSPC), ('cancel', errno.ECANCELED),
                            ('cancel-write', errno.ECANCELED), ('sync-fail', errno.EIO), ('mutate-write', errno.ESTALE)):
            output, report = run(packed, error, mode=mode)
            if mode == 'short': assert (output / 'second.txt').read_bytes() == b'data' * 50000
            elif mode in ('write-fail', 'cancel-write'):
                assert (output / 'first.txt').read_bytes() == b'first' and not (output / 'second.txt').exists()
            elif mode == 'sync-fail': assert report['syncError'] == errno.EIO and not (output / 'first.txt').exists()
            else: assert not (output / 'first.txt').exists()
            cases.append('copy failure/cleanup ' + mode)
        for mode in ('parent-link', 'file-link', 'hardlink', 'directory-conflict'):
            packed = archive([record(b'new content', 8, b'folder/file.txt')])
            source.write_bytes(packed); output = root / f'output-{len(cases)}'; output.mkdir()
            outside = root / f'outside-{len(cases)}'; outside.mkdir()
            sentinel = outside / 'file.txt'; sentinel.write_bytes(b'PRESERVED')
            if mode == 'parent-link': (output / 'folder').symlink_to(outside, target_is_directory=True)
            else:
                (output / 'folder').mkdir()
                target = output / 'folder/file.txt'
                if mode == 'file-link': target.symlink_to(sentinel)
                elif mode == 'hardlink': target.hardlink_to(sentinel)
                else: target.mkdir()
            report = json.loads(subprocess.check_output([probe, str(source), str(output), 'none'], text=True))
            assert report['error'] in (errno.ELOOP, errno.ENOTDIR, errno.EMLINK, errno.EISDIR), report
            assert sentinel.read_bytes() == b'PRESERVED'
            cases.append('target ' + mode + ' preserves outside content')
    print(json.dumps({'passed': len(cases), 'cases': cases}, indent=2))


if __name__ == '__main__':
    main()
