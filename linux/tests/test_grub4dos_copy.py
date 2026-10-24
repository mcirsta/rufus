# SPDX-License-Identifier: GPL-3.0-or-later
"""GRUB4DOS loader copy/readback using the existing file fault driver."""
import argparse
import errno
from pathlib import Path
import subprocess
import tempfile
from test_freedos_copy import values

LOADER = Path(__file__).parent / 'fixtures/download/grub4dos-0.4.6a/grldr'


def run(probe, root, source, fault, error, files):
    result = subprocess.run([probe, str(root), fault, '--grub4dos', str(source)],
                            capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, (fault, result.stdout, result.stderr)
    report = values(result.stdout)
    assert report['ERROR'] == error and report['FILES'] == files, (fault, report)
    assert report['FDS'] == 0 and report['OFFSET'] == report['FLAGS'] == 1, report
    assert report['SETS'] == report['PROGRESS'] == 0, report
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('probe')
    args = parser.parse_args()
    payload = LOADER.read_bytes()
    assert len(payload) == 322965
    cases = [('none', 0, 1), ('ntfs', 0, 1), ('ntfs-3g', 0, 1), ('short-io', 0, 1), ('write-eintr', 0, 1),
             ('read-eintr', 0, 1), ('sync-eintr', 0, 1),
             ('wrong-fs', errno.ENOTSUP, 0), ('opath', errno.EINVAL, 0),
             ('cancel-before', errno.ECANCELED, 0), ('cancel-partial', errno.ECANCELED, 0),
             ('cancel-file', errno.ECANCELED, 1), ('cancel-sync', errno.ECANCELED, 0),
             ('open-fail', errno.EACCES, 0), ('write-fail', errno.ENOSPC, 0),
             ('write-zero', errno.EIO, 0), ('partial-fail', errno.ENOSPC, 0),
             ('partial-sync-fail', errno.ENOSPC, 0), ('read-fail', errno.EIO, 0),
             ('read-eof', errno.EIO, 0), ('read-corrupt', errno.EBADMSG, 0),
             ('close-fail', errno.EIO, 0), ('sync-1', errno.EIO, 0), ('sync-2', errno.EIO, 1)]
    with tempfile.TemporaryDirectory(prefix='rufus-grub4dos-copy-') as temporary:
        root = Path(temporary)
        for fault, error, files in cases:
            target = root / fault
            target.mkdir()
            report = run(args.probe, target, LOADER, fault, error, files)
            actual = (target / 'grldr').read_bytes() if (target / 'grldr').exists() else b''
            assert payload.startswith(actual) and len(actual) == report['BYTES'], (fault, report)
            if error == 0:
                assert actual == payload and report['SYNC'] == 0
                assert sorted(p.name for p in target.iterdir()) == ['grldr']
            if fault in ('partial-fail', 'partial-sync-fail', 'cancel-partial'):
                assert report['BYTES'] == 17, report
            if fault in ('sync-1', 'sync-2', 'partial-sync-fail'):
                assert report['SYNC'] == errno.EIO, report
        # Extending loader copying must not admit NTFS for DOS attributes.
        for filesystem in ('ntfs', 'ntfs-3g'):
            target = root / ('dos-' + filesystem)
            target.mkdir()
            result = subprocess.run([args.probe, target, filesystem], capture_output=True, text=True, timeout=10)
            assert result.returncode == 0 and values(result.stdout)['ERROR'] == errno.ENOTSUP
            assert not list(target.iterdir())
        outside = root / 'outside'
        outside.write_bytes(b'preserve')
        for kind in ('file', 'directory', 'symlink', 'hidden'):
            target = root / kind
            target.mkdir()
            entry = target / ('.hidden' if kind == 'hidden' else 'grldr')
            if kind == 'directory':
                entry.mkdir()
            elif kind == 'symlink':
                entry.symlink_to(outside)
            else:
                entry.write_bytes(b'preserve')
            report = run(args.probe, target, LOADER, 'none', errno.ENOTEMPTY, 0)
            assert report['BYTES'] == report['SYNCS'] == 0
            assert len(list(target.iterdir())) == 1
        assert outside.read_bytes() == b'preserve'
        target = root / 'race'
        target.mkdir()
        report = run(args.probe, target, LOADER, 'race-file', errno.EEXIST, 0)
        assert (target / 'grldr').read_bytes() == b'preserve raced file'
        assert report['BYTES'] == 0
        for size in (0, 511, 512, 513, 1048064, 1048065):
            source = root / 'bounded-loader'
            source.write_bytes(b'l' * size)
            target = root / str(size)
            target.mkdir()
            valid = 512 <= size <= 1048064
            report = run(args.probe, target, source, 'none', 0 if valid else errno.EINVAL, int(valid))
            if valid:
                assert (target / 'grldr').read_bytes() == source.read_bytes()
            else:
                assert report['BYTES'] == report['SYNCS'] == 0 and not list(target.iterdir())
    print(f'{len(cases) + 13} GRUB4DOS copy cases passed; retained loader, exclusive creation, '
          'bounded I/O/readback, cancellation and sync faults. Temporary outputs removed.')


if __name__ == '__main__':
    main()
