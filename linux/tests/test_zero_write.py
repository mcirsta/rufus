#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Grouped full/fast zeroing, exact tails, backoff and native transfer faults."""
import errno
import json
from pathlib import Path
import subprocess
import sys
import tempfile

MIB = 1024 * 1024
REGION = 32 * MIB


def main():
    probe = sys.argv[1]
    cases = []
    with tempfile.TemporaryDirectory(prefix='rufus-zero-') as temporary:
        target = Path(temporary) / 'target.raw'

        def run(label, initial, mode, expected=None, error=0, fault='none', written=None):
            target.write_bytes(initial)
            result = json.loads(subprocess.check_output([probe, str(target), str(mode), fault], text=True, timeout=60))
            assert result['error'] == error, (label, result)
            if expected is not None:
                assert target.read_bytes() == expected, (label, result)
            if error == 0:
                assert result['phase'] == 4 and result['written'] == result['verified'], result
                assert result['syncs'] == (2 if fault == 'eintr' else 1), result
            if written is not None:
                assert result['written'] == written, (label, result)
            cases.append(label)

        for size in (512, REGION + 1536):
            data = b'X' * size
            run(f'full {size}', data, 1, bytes(size), written=size)
            run(f'fast dirty {size}', data, 2, b'\xff' * size, written=size)
            for byte in (0, 255):
                data = bytes([byte]) * size
                run(f'fast blank {byte} {size}', data, 2, data, written=0)
        run('zero then erased regions both skipped', bytes(REGION) + b'\xff' * REGION, 2,
            bytes(REGION) + b'\xff' * REGION, written=0)
        run('mixed region is overwritten completely', bytes(REGION) + b'X' + bytes(REGION - 1), 2,
            bytes(REGION) + b'\xff' * REGION, written=REGION)
        # Fifteen following regions are written without comparison; region 17
        # resumes blank detection. This expectation is independent of the core.
        with target.open('wb') as output:
            output.write(b'X')
            output.truncate(17 * REGION + 512)
        result = json.loads(subprocess.check_output([probe, str(target), '2', 'none'], text=True, timeout=60))
        assert result['error'] == 0 and result['written'] == result['verified'] == 16 * REGION, result
        with target.open('rb') as source:
            for offset in range(0, 17 * REGION + 512, MIB):
                data = source.read(MIB)
                assert data == bytes([255 if offset < 16 * REGION else 0]) * len(data)
        cases.append('exact 15-region fast backoff')
        for mode in (1, 2):
            for fault in ('short', 'eintr'):
                data = b'X' * (2 * MIB + 512)
                run(f'mode {mode} {fault}', data, mode, bytes(len(data)) if mode == 1 else b'\xff' * len(data), fault=fault)
            for fault, error in (('write-error', errno.ENOSPC), ('write-zero', errno.EIO),
                                 ('sync-error', errno.EIO), ('write-cancel', errno.ECANCELED),
                                 ('verify-cancel', errno.ECANCELED), ('verify-mismatch', errno.EBADMSG),
                                 ('resize', errno.ESTALE), ('readonly', errno.EINVAL), ('append', errno.EINVAL)):
                run(f'mode {mode} {fault}', b'X' * (3 * MIB), mode, error=error, fault=fault)
        for fault in ('read-eof', 'read-error'):
            data = b'X' * 4096
            run(f'fast compare {fault}', data, 2, data, errno.EIO, fault, written=0)
        for mode in (0, 3):
            run(f'invalid mode {mode}', b'X' * 512, mode, b'X' * 512, errno.EINVAL, written=0)
        run('invalid final sector', b'X' * 513, 1, b'X' * 513, errno.EINVAL, written=0)
    print(json.dumps({'count': len(cases), 'cases': cases, 'temporary_media_removed': True}, indent=2))


if __name__ == '__main__':
    main()
