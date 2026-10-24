# SPDX-License-Identifier: GPL-3.0-or-later
"""Qualify native ext2/ext3 formatting on disposable partition-sized files."""
import argparse
import errno
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

MIB = 1024 * 1024
DEFAULT_SIZE = 255 * MIB
ENVIRONMENT = dict(os.environ, LC_ALL='C.UTF-8')
runs = 0


def make_image(root, name, size=DEFAULT_SIZE):
    image = root / name
    with image.open('wb') as stream:
        stream.truncate(size)
        stream.seek(1024)
        stream.write(b'R' * 1024)
        stream.seek(size - 512)
        stream.write(b'S' * 512)
    return image


def sample(image):
    with image.open('rb') as stream:
        stream.seek(1024)
        first = stream.read(1024)
        stream.seek(-512, 2)
        last = stream.read(512)
    return image.stat().st_size, first, last


def invoke(args, image, file_system='ext2', size=DEFAULT_SIZE, label='RufusExt',
           fault='none', program=None, error=0, started=True, persistence=False):
    global runs
    command = [str(args.probe), str(program or args.mkfs), str(image), str(size),
               file_system, label, fault]
    if persistence:
        command.append('persistence')
    result = subprocess.run(command, capture_output=True, timeout=30, env=dict(os.environ, LC_ALL='C'))
    runs += 1
    assert result.returncode == 0, (command, result.returncode, result.stdout, result.stderr)
    values = {key.decode(): int(value) for key, value in re.findall(rb'([A-Z_]+)=(-?\d+)', result.stdout)}
    assert values['ERROR'] == error and values['STARTED'] == started, (command, values, result.stderr)
    assert values['FLAGS'] == 1 and values['FDS'] == 0, values
    assert values['OFFSET'] == (-1 if fault == 'pipe' else 17), values
    assert values['CHILD'] == -1 and values['CHILD_ERRNO'] == errno.ECHILD, values
    assert values['SYNCS'] >= int(started), values
    if error == 0:
        assert values['EXIT'] == 0 and values['BYTES'] == size, values
    return values


def inspect(args, image, file_system, size, label, block_size, inode_size, blocks=None, reserve=None):
    report = subprocess.run([args.dump, '-h', str(image)], capture_output=True,
                            check=True, timeout=30, env=ENVIRONMENT)
    fields = {}
    for line in report.stdout.decode().splitlines():
        if ':' in line:
            key, value = line.split(':', 1)
            fields[key.strip()] = value.strip()
    expected = {'sparse_super', 'large_file', 'filetype', 'dir_index', 'ext_attr'}
    if file_system in ('ext3', 'ext4'):
        expected.add('has_journal')
    if file_system == 'ext4':
        expected.add('extent')
    assert set(fields['Filesystem features'].split()) == expected, fields
    assert fields['Filesystem volume name'] == label, fields
    assert fields['Filesystem OS type'] == 'Linux', fields
    assert fields['Filesystem state'] == 'clean', fields
    assert fields['Errors behavior'] == 'Continue', fields
    assert int(fields['Block size']) == block_size, fields
    assert int(fields['Inode size']) == inode_size, fields
    assert int(fields['Block count']) == (size // block_size if blocks is None else blocks), fields
    assert int(fields['Reserved block count']) == (int(fields['Block count']) * 5 // 100 if reserve is None else reserve), fields
    checked = subprocess.run([args.fsck, '-fn', str(image)], capture_output=True,
                             timeout=45, env=ENVIRONMENT)
    assert checked.returncode == 0, checked.stdout + checked.stderr


def valid_cases(args, root):
    for file_system, size, block, inode, label in (
            ('ext2', DEFAULT_SIZE, 1024, 128, 'RufusExt2'),
            ('ext3', DEFAULT_SIZE, 1024, 128, 'RufusExt3'),
            ('ext4', DEFAULT_SIZE, 1024, 128, 'RufusExt4'),
            ('ext3', 512 * MIB, 4096, 256, 'Rufus-ä')):
        image = make_image(root, file_system + str(size) + '.img', size)
        invoke(args, image, file_system, size, label)
        inspect(args, image, file_system, size, label, block, inode)
        if file_system == 'ext3' and size == DEFAULT_SIZE:
            invoke(args, image, file_system, size, 'Recreated')
            inspect(args, image, file_system, size, 'Recreated', block, inode)
        image.unlink()


def refusal_cases(args, root):
    image = make_image(root, 'refused.img')
    before = sample(image)
    cases = [
        ('device-zero', {}, errno.EINVAL), ('device-file', {}, errno.ENOTSUP),
        ('readonly', {}, errno.EINVAL), ('append', {}, errno.EINVAL),
        ('cancel-before', {}, errno.ECANCELED), ('pipe', {}, errno.ENOTSUP),
        ('label-nul', {}, errno.EINVAL), ('label-surrogate', {}, errno.EINVAL),
        ('none', {'size': 0}, errno.EINVAL),
        ('none', {'size': 16 * MIB - 512}, errno.EINVAL),
        ('none', {'size': DEFAULT_SIZE + 1}, errno.EINVAL),
        ('none', {'size': DEFAULT_SIZE + 512}, errno.ESTALE),
        ('none', {'label': 'x' * 17}, errno.EINVAL),
        ('none', {'program': 'mke2fs'}, errno.EINVAL),
        ('none', {'program': root / 'missing'}, errno.ENOENT),
    ]
    for fault, settings, error in cases:
        invoke(args, image, fault=fault, error=error, started=False, **settings)
        assert sample(image) == before, (fault, settings)
    image.unlink()


def failure_cases(args, root):
    cases = [('false-success', '/usr/bin/true', 'none', errno.EBADMSG),
             ('exit-failure', '/usr/bin/false', 'none', errno.EIO),
             ('sync-fail', args.mkfs, 'sync-fail', errno.EIO),
             ('read-fail', args.mkfs, 'read-fail', errno.EIO),
             ('corrupt-feature', args.mkfs, 'corrupt-feature', errno.EBADMSG),
             ('corrupt-label', args.mkfs, 'corrupt-label', errno.EBADMSG),
             ('shrink', args.mkfs, 'shrink', errno.ESTALE)]
    for name, program, fault, error in cases:
        image = make_image(root, name + '.img')
        report = invoke(args, image, file_system='ext3', program=program,
                        fault=fault, error=error)
        if fault == 'sync-fail':
            assert report['SYNC'] == errno.EIO, report
        image.unlink()
    image = make_image(root, 'sync-eintr.img')
    report = invoke(args, image, fault='sync-eintr')
    assert report['SYNCS'] == 2, report
    image.unlink()


def persistence_cases(args, root):
    before = set(Path('/tmp').glob('rufus-persist-*'))
    for file_system in ('ext2', 'ext3', 'ext4'):
        # A 63-sector track-rounded extent leaves a tiny last group, which
        # e2fsprogs removes. Keep independent block/reserve expectations.
        size = 268466688
        image = make_image(root, file_system + '-persistent.img', size)
        invoke(args, image, file_system, size, 'persistence', persistence=True)
        inspect(args, image, file_system, size, 'persistence', 1024, 128, blocks=262145, reserve=13106)
        contents = subprocess.run([args.debugfs, '-R', 'cat /persistence.conf', str(image)],
                                  capture_output=True, check=True, timeout=15)
        assert contents.stdout == b'/ union\n', contents
        image.unlink()
    image = make_image(root, 'missing-conf.img', 256 * MIB)
    program = root / 'omit-seed'
    program.write_text('#!/usr/bin/python3\nimport os, sys\n'
                       'args = sys.argv[1:]\ni = args.index("-d")\ndel args[i:i+2]\n'
                       f'os.execv({str(args.mkfs)!r}, [{str(args.mkfs)!r}] + args)\n')
    program.chmod(0o700)
    invoke(args, image, 'ext3', 256 * MIB, 'persistence', program=program,
           persistence=True, error=errno.EIO)
    invoke(args, image, 'ext3', 256 * MIB, 'persistence', persistence=True,
           fault='sync-fail', error=errno.EIO, started=False)
    image.unlink()
    assert set(Path('/tmp').glob('rufus-persist-*')) == before, 'seed directory leaked'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('probe', type=Path)
    parser.add_argument('--mkfs', type=Path, required=True)
    parser.add_argument('--fsck', type=Path, required=True)
    parser.add_argument('--dump', type=Path, required=True)
    args = parser.parse_args()
    args.debugfs = shutil.which('debugfs')
    assert args.debugfs, 'debugfs from e2fsprogs is required'
    with tempfile.TemporaryDirectory(prefix='rufus-ext-format-') as temporary:
        root = Path(temporary)
        refusal_cases(args, root)
        valid_cases(args, root)
        failure_cases(args, root)
        persistence_cases(args, root)
    print(f'{runs} ext2/ext3/ext4 native and persistence format, refusal, readback and sync cases passed; temporary files removed.')


if __name__ == '__main__':
    main()
