#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""One grouped file-policy corpus, using disposable directories only."""
import errno
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile


def main():
    probe, original = sys.argv[1:]
    icon = Path(original).read_bytes(); checks = []
    with tempfile.TemporaryDirectory(prefix='test_media_appearance-') as scratch:
        root = Path(scratch)
        def run(label='Măriuș 安装 💾 / long original label', fault='none', error=0, preserved=False):
            report = json.loads(subprocess.check_output([probe, str(root), label, fault], text=True))
            assert report['error'] == error and report['preserved'] == preserved, (fault, report)
            assert not list(root.glob('.rufus-autorun-*')), (fault, list(root.iterdir()))
            if error == 0 and not preserved:
                assert report['created'] and (root / 'autorun.ico').read_bytes() == icon
                data = (root / 'autorun.inf').read_bytes()
                assert data[:2] == b'\xff\xfe'
                text = data[2:].decode('utf-16le')
                assert '[autorun]\r\nicon  = autorun.ico\r\nlabel = ' + label + '\r\n' in text, text
                assert '\n' not in text.replace('\r\n', '')
                reserved, kind, count = struct.unpack_from('<HHH', icon)
                assert reserved == 0 and kind == 1 and 0 < count <= 64
                for i in range(count):
                    size, offset = struct.unpack_from('<II', icon, 6 + i * 16 + 8)
                    assert size > 0 and offset + size <= len(icon)
            checks.append({'fault': fault, 'error': error, 'preserved': preserved})
            return report
        def clear():
            for name in ('autorun.inf','autorun.ico'):
                path = root / name
                if path.is_symlink() or path.is_file(): path.unlink()
                elif path.is_dir(): path.rmdir()
        for label in ('', 'Măriuș 安装 💾 / long original label', 'L' * 127):
            run(label); clear()
        for fs in ('ntfs','exfat','fuse','short','eintr'):
            run(fault=fs); clear()
        for kind in ('file','directory','symlink'):
            path = root / 'autorun.inf'
            if kind == 'file': path.write_bytes(b'original')
            elif kind == 'directory': path.mkdir()
            else: path.symlink_to('/does-not-exist')
            (root / 'autorun.ico').write_bytes(b'original icon')
            report = run(preserved=True)
            assert int(report['written']) == 0 and (root / 'autorun.ico').read_bytes() == b'original icon'
            clear()
        external = root / 'outside'; external.write_bytes(b'untouched')
        for kind in ('symlink','hardlink'):
            if kind == 'symlink': (root / 'autorun.ico').symlink_to(external)
            else: (root / 'autorun.ico').hardlink_to(external)
            run()
            assert external.read_bytes() == b'untouched'
            clear()
        external.unlink()
        run(fault='race', preserved=True)
        assert (root / 'autorun.inf').read_bytes() == b'racer' and not (root / 'autorun.ico').exists()
        clear()
        for fault, error in (('readonly', errno.EROFS), ('unsupported', errno.ENOTSUP),
                             ('cancel-before', errno.ECANCELED), ('cancel-icon', errno.ECANCELED),
                             ('cancel-inf', errno.ECANCELED), ('write-inf', errno.ENOSPC),
                             ('corrupt', errno.EBADMSG), ('sync', errno.EIO), ('rename', errno.EACCES),
                             ('directory-sync', errno.EIO), ('surrogate', errno.EINVAL), ('nul', errno.EINVAL)):
            run(fault=fault, error=error)
            if fault != 'directory-sync': assert not (root / 'autorun.inf').exists()
            clear()
        for label in ('X' * 128, 'bad\nsection', 'bad\rsection', 'bad\tsection'):
            run(label=label, error=errno.EINVAL); clear()
    print(json.dumps({'passed': len(checks), 'cases': checks, 'temporary_media_removed': True}, indent=2))


if __name__ == '__main__': main()
