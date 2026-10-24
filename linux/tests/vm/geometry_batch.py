# SPDX-License-Identifier: GPL-3.0-or-later
"""Finite B5 geometry batch: three real USB geometries in one Linux guest."""
import shutil
import zipfile

import prepare

DISK_BYTES = 320 * 1024 * 1024
GEOMETRIES = ((512, 512), (512, 4096), (4096, 4096))


def stage_fixture(args, base, init):
    prepare.write(init, 'etc/rufus-geometry-fixture', 'geometry-batch\n')
    prepare.write(init, 'etc/rufus-format-helper-fixture', 'format-helper\n')
    (init / 'tmp').mkdir(mode=0o1777, exist_ok=True)
    (init / 'tmp').chmod(0o1777)
    provider = init / str(args.mkudffs).lstrip('/')
    provider.parent.mkdir(parents=True, exist_ok=True)
    if provider != init / 'usr/bin/mkudffs':
        shutil.copy2(init / 'usr/bin/mkudffs', provider)
    efi = (args.build / 'uefi-handoff.efi').read_bytes()
    (init / 'geometry-original.efi').write_bytes(efi)
    with zipfile.ZipFile(init / 'geometry-overlay.zip', 'w', zipfile.ZIP_DEFLATED) as archive:
        archive.writestr('EFI/BOOT/BOOTX64.EFI', efi)
        archive.writestr('geometry.txt', b'B5 exact native geometry payload\n')


def append_devices(command, base):
    """The ordinary fixture stick is 512/512. Add 512e and 4Kn sticks."""
    for index, (logical, physical) in enumerate(GEOMETRIES[1:], 1):
        disk = base / ('geometry-' + str(index) + '.raw')
        with disk.open('xb') as output:
            output.truncate(DISK_BYTES)
        # Linux's BOT driver deliberately tries READ CAPACITY(10) first, which
        # lacks the physical exponent. UAS retains real USB transport while
        # allowing a modern SCSI target to report READ CAPACITY(16) geometry.
        command.extend(['-drive', 'if=none,id=geometry' + str(index) + ',format=raw,file=' + str(disk),
            '-device', 'usb-uas,id=geometry-usb' + str(index) + ',serial=RUFUS-GEOMETRY-' + str(index),
            '-device', 'scsi-hd,bus=geometry-usb' + str(index) + '.0,drive=geometry' + str(index)
                + ',lun=0,removable=on,scsi_version=5,logical_block_size=' + str(logical)
                + ',physical_block_size=' + str(physical)])


def qualify_boot(args, base, disk):
    from mbr import boot_syslinux_uefi
    boot_syslinux_uefi(args, base / 'geometry-fat32-uefi', disk, native_fat=True)
    return {'native_fat32_efi': True, 'backing_unchanged': True,
            'operating_system_executed': False}
