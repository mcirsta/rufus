#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Pattern/fake-capacity/recovery corpus, using only disposable regular files."""
import errno
import json
from pathlib import Path
import subprocess
import sys
import tempfile

MIB = 1024 * 1024
BLOCK = 512 * 1024
PATTERNS = [(0x55,), (0x55, 0xaa), (0, 0xff, 0x55, 0xaa),
            (0, 0xff, 0x33, 0xcc), (0, 0xff, 0x1c71c7, 0xe38e38)]


def main():
    probe = sys.argv[1]; checks = []
    with tempfile.TemporaryDirectory(prefix='rufus-badblocks-') as scratch:
        image = Path(scratch) / 'target.img'
        def run(size, pattern=0, fake=True, seed=17, fault='none', error=0):
            with image.open('wb') as output:
                if fault.startswith('cleanup'): output.write(bytes([0xa5]) * size)
                else: output.truncate(size)
            report = json.loads(subprocess.check_output([probe, str(image), str(pattern), str(int(fake)), str(seed), fault], text=True))
            assert report['error'] == error and report['reports'] == report['count'], (size, pattern, fake, fault, report)
            assert report['read'] + report['write'] + report['corrupt'] == report['count'] <= 256, report
            checks.append({'bytes': size, 'pattern': pattern, 'fake': fake, 'fault': fault})
            return report
        for pattern, values in enumerate(PATTERNS):
            for size in (512, BLOCK - 512, BLOCK, 8 * MIB + 512, 33 * MIB + 512):
                result = run(size, pattern)
                assert result['count'] == 0 and result['syncs'] == len(values)
                assert result['written'] == result['read_bytes'] == size * len(values)
                assert result['progress'] == result['total'] == size * len(values) * 2
                raw = image.read_bytes(); value = values[-1]
                pattern_bytes = value.to_bytes(max(1, (value.bit_length() + 7) // 8), 'big')
                for begin in range(0, size, 32 * MIB):
                    count = min(32 * MIB, size - begin)
                    expected = bytearray((pattern_bytes * (count // len(pattern_bytes) + 1))[:count])
                    if len(values) == 1:
                        smallest = size % BLOCK or BLOCK; tag = 17 % (smallest - 8 + 1)
                        for block in range(0, count, BLOCK):
                            expected[block + tag:block + tag + 8] = ((begin + block) // BLOCK).to_bytes(8, 'little')
                    assert raw[begin:begin + count] == expected, (pattern, size, begin)
        for fake in (False, True):
            result = run(16 * MIB, fake=fake, fault='alias')
            assert result['corrupt'] == (24 if fake else 0), result
        result = run(256 * MIB, fault='alias')
        assert result['count'] == 256 and result['threshold'], result
        for fault in ('eintr', 'short'):
            result = run(8 * MIB + 512, fault=fault)
            assert result['count'] == 0
        for fault, field in (('read-error', 'read'), ('write-error', 'write'), ('corrupt', 'corrupt'), ('tlc-write-error', 'write')):
            result = run(40 * MIB + 512, pattern=4 if fault == 'tlc-write-error' else 0, fault=fault)
            assert result['count'] == result[field] == 1, (fault, result)
        for fault, error in (('cancel-before', errno.ECANCELED), ('cancel-write', errno.ECANCELED),
                             ('cancel-read', errno.ECANCELED), ('sync-error', errno.EIO),
                             ('resize', errno.ESTALE), ('readonly', errno.EINVAL),
                             ('append', errno.EINVAL), ('unplug', errno.ENODEV)):
            result = run(40 * MIB + 512, fault=fault, error=error)
            if result['written']: assert result['syncs'] > 0, result
        for pattern in (-1, 5):
            result = run(8 * MIB, pattern=pattern, error=errno.EINVAL)
            assert result['written'] == result['read_bytes'] == 0
        for size, fault in ((512, 'cleanup'), (8 * MIB, 'cleanup'), (8 * MIB + 512, 'cleanup'),
                            (10 * MIB + 512, 'cleanup'), (10 * MIB, 'cleanup-cancel')):
            result = run(size, fault=fault)
            raw = image.read_bytes(); prefix = min(size, 8 * MIB); tail = min(size - prefix, MIB)
            assert raw[:prefix] == bytes(prefix)
            assert raw[prefix:size - tail] == bytes([0xa5]) * (size - prefix - tail)
            if tail: assert raw[-tail:] == bytes(tail)
            assert result['syncs'] == 1
        result = run(10 * MIB, fault='cleanup-sync-error', error=errno.EIO)
        assert result['sync_error'] == errno.EIO
        if len(sys.argv) > 2:
            from ffu_fixture import make_ffu
            for partial, validations, required in ((False, [], 0), (True, [], 0), (True, [(0, 7, b'base')], 1)):
                packed, _, _ = make_ffu([(bytes(128 * 1024), [(0, 0)])], disk_bytes=8 * 128 * 1024, partial=partial, validations=validations)
                source = Path(scratch) / 'base.ffu'; source.write_bytes(packed)
                report = json.loads(subprocess.check_output([sys.argv[2], 'ffu-base', str(source)], text=True))
                assert report == {'error': 0, 'required': required}, report
                checks.append({'ffu_base': required, 'partial': partial})
    print(json.dumps({'passed': len(checks), 'cases': checks, 'temporary_media_removed': True}, indent=2))


if __name__ == '__main__': main()
