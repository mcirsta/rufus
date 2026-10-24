# SPDX-License-Identifier: GPL-3.0-or-later
"""Qualify only Rufus's ext2/ext3 GRUB2 MBR/core layout on sparse files."""
import argparse
import errno
from pathlib import Path
import re
import subprocess
import tempfile

from test_mbr_image import inspect_mbr, invoke, make_image

MIB = 1024 * 1024
SECTOR = 512
DISK_SIZE = 256 * MIB
CLUSTER_DEFAULT = 0x100


def check_core(probe, image, payload, file_system):
    with image.open('rb') as stream:
        before_mbr = stream.read(SECTOR)
    result = subprocess.run([probe, str(image), str(payload), str(DISK_SIZE), file_system,
                             str(CLUSTER_DEFAULT), 'none'], capture_output=True, timeout=10)
    assert result.returncode == 0, (result.returncode, result.stdout, result.stderr)
    values = {key.decode(): int(value) for key, value in re.findall(rb'([A-Z_]+)=(-?\d+)', result.stdout)}
    assert values['ERROR'] == 0 and values['BYTES'] == payload.stat().st_size, values
    assert values['SYNCS'] == 1 and values['FDS'] == 0 and values['FLAGS'] == 1, values
    with image.open('rb') as stream:
        assert stream.read(SECTOR) == before_mbr
        assert stream.read(payload.stat().st_size) == payload.read_bytes()


def check_file_system(args, root, file_system):
    absent = root / 'absent'
    invoke(args.mbr_probe, absent, DISK_SIZE, CLUSTER_DEFAULT, operation='plan',
           file_system=file_system, boot_code='grub2')
    invoke(args.mbr_probe, absent, DISK_SIZE - SECTOR, CLUSTER_DEFAULT,
           operation='plan', file_system=file_system, boot_code='grub2', error=errno.EINVAL)
    invoke(args.mbr_probe, absent, DISK_SIZE, SECTOR, operation='plan',
           file_system=file_system, boot_code='grub2', error=errno.EINVAL)
    invoke(args.mbr_probe, absent, DISK_SIZE, CLUSTER_DEFAULT, operation='plan',
           file_system=file_system, boot_code='syslinux', error=errno.ENOTSUP)
    invoke(args.mbr_probe, absent, DISK_SIZE, CLUSTER_DEFAULT, operation='plan',
           file_system=file_system, boot_code='grub2', uefi_payload=MIB, error=errno.ENOTSUP)

    image = make_image(root, file_system + '.img', DISK_SIZE)
    invoke(args.mbr_probe, image, DISK_SIZE, cluster=CLUSTER_DEFAULT,
           file_system=file_system, boot_code='grub2')
    inspect_mbr(image, DISK_SIZE, CLUSTER_DEFAULT, file_system=file_system,
                boot_code='grub2', sfdisk=args.sfdisk)
    invoke(args.mbr_probe, image, DISK_SIZE, cluster=CLUSTER_DEFAULT,
           file_system=file_system, boot_code='grub2', operation='check')
    check_core(args.core_probe, image, args.core, file_system)
    with image.open('r+b') as stream:
        stream.seek(450)  # MBR partition type.
        assert stream.read(1) == b'\x83'
        stream.seek(450)
        stream.write(b'\x07')
    invoke(args.mbr_probe, image, DISK_SIZE, cluster=CLUSTER_DEFAULT,
           file_system=file_system, boot_code='grub2', operation='check', error=errno.EBADMSG)
    with image.open('rb') as stream:
        stream.seek(450)
        assert stream.read(1) == b'\x07', 'Readback repaired damaged partition metadata'
    image.unlink()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mbr_probe', type=Path)
    parser.add_argument('core_probe', type=Path)
    parser.add_argument('core', type=Path)
    parser.add_argument('--sfdisk', default='sfdisk')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='rufus-grub2-ext-layout-') as temporary:
        root = Path(temporary)
        for file_system in ('ext2', 'ext3'):
            check_file_system(args, root, file_system)
        invoke(args.mbr_probe, root / 'absent', DISK_SIZE, cluster=CLUSTER_DEFAULT,
               file_system='ext4', boot_code='grub2', operation='plan', error=errno.ENOTSUP)
    print('GRUB2 ext2/ext3 MBR and original core layout passed; temporary images removed.')


if __name__ == '__main__':
    main()
