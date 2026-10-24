# SPDX-License-Identifier: GPL-3.0-or-later
"""Grouped formatter sector/cluster boundaries on bounded-I/O sparse files."""
import argparse
import errno
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile

MIB = 1024 * 1024
GIB = 1024 * MIB
CASES = 0


def invoke(command, logical, physical, error=0, started=True):
    global CASES
    environment = dict(os.environ, RUFUS_TEST_SECTOR_SIZE=str(logical),
                       RUFUS_TEST_PHYSICAL_SECTOR_SIZE=str(physical), LC_ALL='C')
    result = subprocess.run([str(item) for item in command], env=environment,
                            capture_output=True, timeout=45)
    assert result.returncode == 0, (command, result.stdout, result.stderr)
    fields = {key.decode(): int(value) for key, value in
              re.findall(rb'([A-Z_]+)=(-?\d+)', result.stdout)}
    assert fields['ERROR'] == error, (command, fields, result.stderr)
    if 'STARTED' in fields:
        assert fields['STARTED'] == started, (fields, result.stderr)
        assert fields['FDS'] == 0 and fields['FLAGS'] == 1 and fields['OFFSET'] == 17, fields
        assert fields['CHILD'] == -1 and fields['CHILD_ERRNO'] == errno.ECHILD, fields
    CASES += 1
    return fields


def target(root, name, size):
    path = root / name
    with path.open('wb') as output:
        output.truncate(size)
    return path


def read_at(path, offset, count):
    with path.open('rb') as stream:
        stream.seek(offset)
        return stream.read(count)


def fat_cases(args, root):
    for logical, physical in ((512, 512), (512, 4096), (1024, 4096), (2048, 65536), (4096, 4096)):
        for bits, size, cluster in ((16, 64 * MIB, 4096),
                                    (32, 512 * MIB, 4096),
                                    (32, 6 * GIB, 65536)):
            path = target(root, f'fat{bits}-{logical}-{physical}', MIB + size + 4096)
            with path.open('r+b') as output:
                output.write(b'G' * MIB)
                output.seek(MIB + size)
                output.write(b'T' * 4096)
            command = [args.fat_probe, args.mkfs_fat, path, MIB, size, cluster, 'GEOMETRY', 'none']
            if bits == 16:
                command.append('16')
            result = invoke(command, logical, physical)
            boot = read_at(path, MIB, logical)
            assert struct.unpack_from('<H', boot, 11)[0] == logical
            assert boot[13] * logical == cluster and boot[510:512] == b'\x55\xaa'
            assert struct.unpack_from('<I', boot, 28)[0] == MIB // logical
            assert result['BYTES'] <= size and size - result['BYTES'] <= 62 * logical
            assert read_at(path, 0, MIB) == b'G' * MIB
            assert read_at(path, MIB + size, 4096) == b'T' * 4096
            check = command.copy()
            check[7] = 'check-only'
            invoke(check, logical, physical, started=False)
            if logical == 4096:
                before = read_at(path, MIB, 8192)
                refused = invoke([args.fat_boot_probe, path, MIB, size, cluster,
                                  'GEOMETRY', bits, 'none', 'standard'], logical, physical,
                                 error=errno.ENOTSUP, started=False)
                assert refused['BYTES'] == 0 and refused['WRITES'] == 0
                assert read_at(path, MIB, 8192) == before
            if logical == 4096 and bits == 32 and cluster == 4096:
                # A valid-looking BPB whose root cluster escapes the data extent
                # must fail readback without allocating a capacity-sized bitmap.
                with path.open('r+b') as output:
                    output.seek(MIB + 44)
                    output.write(struct.pack('<I', 0x0ffffff0))
                invoke(check, logical, physical, error=errno.EBADMSG, started=False)
            path.unlink()
    path = target(root, 'fat-invalid', 64 * MIB)
    command = [args.fat_probe, args.mkfs_fat, path, 0, 64 * MIB, 4096, 'GEOMETRY', 'none']
    for logical, physical in ((0, 4096), (4096, 512), (3072, 4096), (512, 768)):
        # C's zero-valued fields intentionally preserve old 512-byte callers.
        if logical == 0:
            continue
        invoke(command, logical, physical, error=errno.EINVAL, started=False)
    path.unlink()


def ntfs_cases(args, root):
    for logical, physical in ((512, 4096), (1024, 4096), (2048, 65536), (4096, 4096)):
        size = 64 * MIB
        path = target(root, f'ntfs-{logical}', size)
        command = [args.ntfs_probe, args.mkfs_ntfs, path, MIB, size, 4096, 'Rufus test', 'none']
        invoke(command, logical, physical)
        boot = read_at(path, 0, logical)
        assert struct.unpack_from('<H', boot, 11)[0] == logical and boot[13] * logical == 4096
        assert struct.unpack_from('<Q', boot, 40)[0] == size // logical - 1
        assert read_at(path, size - logical, logical) == boot
        if logical == 4096:
            before = read_at(path, 0, 8192)
            result = invoke([args.ntfs_boot_probe, path, MIB, size, 4096, 'none'],
                            logical, physical, error=errno.ENOTSUP, started=False)
            assert result['BYTES'] == 0 and result['WRITES'] == 0
            assert read_at(path, 0, 8192) == before
            corrupt = command.copy()
            corrupt[7] = 'corrupt-backup-padding'
            invoke(corrupt, logical, physical, error=errno.EBADMSG)
        path.unlink()
    path = target(root, 'ntfs-invalid', 64 * MIB)
    command = [args.ntfs_probe, args.mkfs_ntfs, path, MIB, 64 * MIB, 512, 'Rufus test', 'none']
    invoke(command, 4096, 4096, error=errno.EINVAL, started=False)
    path.unlink()


def exfat_cases(args, root):
    for logical, physical in ((512, 4096), (1024, 4096), (2048, 65536), (4096, 4096)):
        size = 64 * MIB
        path = target(root, f'exfat-{logical}', size)
        command = [args.exfat_probe, args.mkfs_exfat, path, size, 4096, 'Geometry', 'none']
        invoke(command, logical, physical)
        regions = read_at(path, 0, 24 * logical)
        assert regions[:12 * logical] == regions[12 * logical:]
        assert 1 << regions[108] == logical
        assert logical << regions[109] == 4096
        assert struct.unpack_from('<Q', regions, 72)[0] == size // logical
        if logical == 4096:
            corrupt = command.copy()
            corrupt[6] = 'corrupt-backup'
            invoke(corrupt, logical, physical, error=errno.EBADMSG)
        path.unlink()


def ext_cases(args, root):
    for logical, physical in ((1024, 4096), (2048, 65536), (4096, 4096)):
        for file_system in ('ext2', 'ext3', 'ext4'):
            size = 64 * MIB
            path = target(root, f'{file_system}-{logical}', size)
            invoke([args.ext_probe, args.mkfs_ext, path, size, file_system, 'Geometry', 'none'], logical, physical)
            superblock = read_at(path, 1024, 1024)
            assert struct.unpack_from('<H', superblock, 56)[0] == 0xef53
            assert 1024 << struct.unpack_from('<I', superblock, 24)[0] == logical
            assert struct.unpack_from('<H', superblock, 88)[0] == 128
            path.unlink()


def main():
    parser = argparse.ArgumentParser()
    for option in ('fat-probe', 'fat-boot-probe', 'ntfs-probe', 'exfat-probe', 'ext-probe', 'ntfs-boot-probe',
                   'mkfs-fat', 'mkfs-ntfs', 'mkfs-exfat', 'mkfs-ext'):
        parser.add_argument('--' + option, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='rufus-geometry-') as directory:
        root = Path(directory)
        fat_cases(args, root)
        ntfs_cases(args, root)
        exfat_cases(args, root)
        ext_cases(args, root)
    print(f'{CASES} grouped formatter geometry cases passed')


if __name__ == '__main__':
    main()
