#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""One grouped persistence helper/Qt/real-device route and two boots of its output."""
import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

import prepare
from fat_extract import add_executable, digest, run

DISK_BYTES = 2 * 1024 ** 3


def pack(init, output):
    names = run(['find', '.', '-print0'], cwd=init, capture_output=True).stdout
    result = run(['cpio', '--null', '-o', '-H', 'newc', '--owner=0:0'], cwd=init,
                 input=names, capture_output=True)
    output.write_bytes(gzip.compress(result.stdout, compresslevel=1, mtime=0))


LIVE_INIT = r'''#!/bin/bash
set -eu
export PATH=/usr/bin
mount -t devtmpfs devtmpfs /dev
mount -t proc proc /proc
mount -t sysfs sysfs /sys
[[ $$ == 1 && $(< /sys/class/dmi/id/product_name) == RufusPersistenceFixture ]]
finish() { result=$?; trap - EXIT; echo "RUFUS_LIVE_EXIT $result"; echo o > /proc/sysrq-trigger; while :; do sleep 1; done; }
trap finish EXIT
for module in xhci_pci usb_storage sd_mod ext4; do modprobe "$module"; done
for attempt in {1..100}; do [[ -b /dev/sda2 ]] && break; sleep .1; done
[[ " $(< /proc/cmdline) " == *" persistence "* ]]
[[ $(blkid -s LABEL -o value /dev/sda2) == persistence ]]
mkdir /rw
mount -t ext4 /dev/sda2 /rw
[[ $(< /rw/persistence.conf) == '/ union' ]]
if [[ -f /rw/marker ]]; then
    [[ $(< /rw/marker) == 'rufus-persistence-survived' ]]
    echo RUFUS_PERSISTENCE_SECOND_BOOT
else
    echo rufus-persistence-survived > /rw/marker
    sync /rw/marker
    echo RUFUS_PERSISTENCE_FIRST_BOOT
fi
umount /rw
'''


INSPECT = r'''#!/bin/bash
set -eu
read -r version label < /run/check-persistence
[[ $(blockdev --getsize64 /dev/sda) == 2147483648 ]]
[[ $(blkid -s LABEL -o value /dev/sda2) == "$label" ]]
e2fsck -fn /dev/sda2
mkdir -p /run/persistence-readback
mount -t ext4 -o ro /dev/sda2 /run/persistence-readback
if [[ $label == persistence ]]; then
    [[ $(< /run/persistence-readback/persistence.conf) == '/ union' ]]
else
    [[ ! -e /run/persistence-readback/persistence.conf ]]
fi
umount /run/persistence-readback
rmdir /run/persistence-readback
mkdir /run/persistence-main
mount -t vfat -o ro /dev/sda1 /run/persistence-main
config=$(< /run/persistence-main/isolinux/isolinux.cfg)
if [[ $label == persistence ]]; then
    [[ $config == *'boot=live persistence'* ]]
else
    [[ $config == *'boot=casper persistent'* ]]
fi
umount /run/persistence-main
rmdir /run/persistence-main
echo "RUFUS_PERSISTENCE_READBACK EXT=$version LABEL=$label"
'''


CREATE_INIT = r'''#!/bin/bash
set -eu
[[ $$ == 1 ]] || exit 77
export PATH=/usr/bin LC_ALL=C.UTF-8
export QT_QPA_PLATFORM=offscreen QT_QPA_PLATFORM_PLUGIN_PATH=/usr/lib/qt6/plugins/platforms
export ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1
mount -t devtmpfs devtmpfs /dev
mount -t proc proc /proc
mount -t sysfs sysfs /sys
[[ $(< /sys/class/dmi/id/product_name) == RufusPersistenceFixture ]]
finish() { result=$?; trap - EXIT; echo "RUFUS_CREATE_EXIT $result"; echo o > /proc/sysrq-trigger; while :; do sleep 1; done; }
trap finish EXIT
for module in xhci_pci usb_storage sd_mod vfat nls_cp437 nls_iso8859_1 ext4; do modprobe "$module"; done
for attempt in {1..100}; do [[ -b /dev/sda ]] && break; sleep .1; done
[[ $(< /sys/block/sda/device/model) == 'QEMU HARDDISK '* ]]
[[ $(blockdev --getsize64 /dev/sda) == 2147483648 ]]
mkdir -p /tmp /run/caller-home
chmod 1777 /tmp
chown 1000:1000 /casper.iso /debian.iso /run/caller-home
test_persistence_guest
'''


LAUNCHER = r'''#!/bin/bash
[[ $(< /sys/class/dmi/id/product_name) == RufusPersistenceFixture ]] || exit 77
[[ $1 == --disable-internal-agent ]] || exit 64
shift
echo launch >> /run/format-launches
export PKEXEC_UID=1000
"$@"
result=$?
echo held > /run/format-exit-held
for attempt in {1..3000}; do
    [[ -f /run/format-exit-release ]] && exit "$result"
    sleep .01
done
exit 78
'''


def bootstrap(base, kernel):
    if (base / 'initramfs').is_dir():
        return base / 'initramfs'
    base.mkdir()
    root = base / 'root'
    root.mkdir()
    (root / 'lib').symlink_to('usr/lib')
    prepare.bootstrap(base, root, kernel)
    return base / 'initramfs'


def make_sources(args, base):
    live = base / 'live'
    init = bootstrap(live, args.kernel)
    for name in ('mkdir', 'blkid', 'umount', 'sync'):
        add_executable(init, Path('/usr/bin') / name)
    prepare.write(init, 'init', LIVE_INIT, 0o755)
    pack(init, live / 'live.cpio.gz')
    hashes = {}
    for casper in (False, True):
        name = 'casper' if casper else 'debian'
        tree = base / (name + '-source')
        (tree / 'isolinux').mkdir(parents=True)
        folder = 'casper' if casper else 'live'
        (tree / folder).mkdir()
        (tree / 'boot').mkdir()
        shutil.copy2(live / 'vmlinuz', tree / folder / 'vmlinuz')
        shutil.copy2(live / 'live.cpio.gz', tree / 'boot/initrd.gz')
        for source, target in [('bios/core/isolinux.bin', 'isolinux.bin'),
                               ('bios/com32/elflink/ldlinux/ldlinux.c32', 'ldlinux.c32')]:
            shutil.copy2(args.upstream / source, tree / 'isolinux' / target)
        config = ('DEFAULT live\nPROMPT 0\nTIMEOUT 1\nLABEL live\n'
                  f'  KERNEL /{folder}/vmlinuz\n'
                  f'  APPEND initrd=/boot/initrd.gz boot={"casper" if casper else "live"} console=ttyS0 panic=-1\n')
        (tree / 'isolinux/isolinux.cfg').write_text(config)
        image = base / (name + '.iso')
        run([args.mkisofs, '-quiet', '-r', '-J', '-V', 'RUFUS_LIVE', '-o', image, tree])
        hashes[name] = {'sha256': digest(image), 'config': config}
    (base / 'sources.json').write_text(json.dumps(hashes, indent=2) + '\n')


def make_creation_guest(args, base):
    init = bootstrap(base / 'creation', args.kernel)
    binaries = [args.build / name for name in ('test_persistence_guest', 'udisks_fixture', 'rufus-prepare-helper')]
    binaries += [Path('/usr/bin') / name for name in ('mkdir', 'chmod', 'rm', 'rmdir', 'umount',
                 'mke2fs', 'mkntfs', 'mkfs.fat', 'mcopy', 'mattrib', 'e2fsck', 'blkid', 'blockdev', 'dbus-daemon')]
    for binary in binaries:
        add_executable(init, binary)
    if not (init / 'usr/bin/sh').exists():
        (init / 'usr/bin/sh').symlink_to('bash')
    installer = init / 'usr/local/libexec/rufus-syslinux'
    installer.parent.mkdir(parents=True, exist_ok=True)
    add_executable(init, args.installer)
    shutil.copy2(args.installer, installer)
    for source in ('casper.iso', 'debian.iso'):
        shutil.copy2(base / source, init / source)
    shutil.copy2(prepare.HERE.parents[2] / 'res/uefi/uefi-ntfs.img', init / 'uefi-ntfs.img')
    shutil.copytree('/usr/lib/locale/C.utf8', init / 'usr/lib/locale/C.utf8', dirs_exist_ok=True)
    shutil.copytree('/usr/lib/gconv', init / 'usr/lib/gconv', dirs_exist_ok=True)
    platforms = init / 'usr/lib/qt6/plugins/platforms'
    platforms.mkdir(parents=True, exist_ok=True)
    shutil.copy2('/usr/lib/qt6/plugins/platforms/libqoffscreen.so', platforms)
    prepare.write(init, 'etc/passwd', 'root:x:0:0:root:/root:/bin/bash\ncaller:x:1000:1000:Caller:/run/caller-home:/bin/bash\n')
    prepare.write(init, 'etc/group', 'root:x:0:\ncaller:x:1000:\n')
    prepare.write(init, 'etc/nsswitch.conf', 'passwd: files\ngroup: files\n')
    prepare.write(init, 'etc/machine-id', '3f07774940c94eabb55932f000000001\n')
    (init / 'run/dbus').mkdir(parents=True, exist_ok=True)
    prepare.write(init, 'etc/rufus-test-bus.conf', '<busconfig><type>system</type><listen>unix:path=/run/dbus/system_bus_socket</listen>'
                  '<auth>EXTERNAL</auth><policy context="default"><allow user="root"/><allow user="caller"/>'
                  '<allow own="*"/><allow send_destination="*"/><allow receive_sender="*"/></policy></busconfig>\n')
    prepare.write(init, 'usr/bin/pkexec', LAUNCHER, 0o755)
    (init / 'usr/bin/mke2fs').rename(init / 'usr/bin/mke2fs.real')
    prepare.write(init, 'usr/bin/mke2fs', '#!/bin/bash\n[[ -f /run/fail-persistence ]] && exit 1\nexec /usr/bin/mke2fs.real "$@"\n', 0o755)
    prepare.write(init, 'inspect-persistence', INSPECT, 0o755)
    prepare.write(init, 'init', CREATE_INIT, 0o755)
    pack(init, base / 'creation/creation.cpio.gz')


def boot(args, base, name, direct=False):
    serial = base / (name + '.serial.log')
    command = ['qemu-system-x86_64', '-accel', args.accel, '-cpu', 'max', '-m', '2048',
               '-display', 'none', '-monitor', 'none', '-serial', 'file:' + str(serial),
               '-no-reboot', '-nic', 'none', '-smbios',
               'type=1,manufacturer=RufusTest,product=RufusPersistenceFixture',
               '-device', 'qemu-xhci,id=xhci',
               '-drive', 'if=none,id=stick,format=raw,file=' + str(base / 'usb.raw'),
               '-device', 'usb-storage,drive=stick,removable=on,bootindex=1']
    if direct:
        command += ['-kernel', str(base / 'creation/vmlinuz'), '-initrd',
                    str(base / 'creation/creation.cpio.gz'), '-append', 'console=ttyS0 panic=-1']
    (base / (name + '.command.json')).write_text(json.dumps(command, indent=2) + '\n')
    with (base / (name + '.qemu.log')).open('wb') as output:
        subprocess.run(command, check=True, timeout=900 if direct else 180, stdout=output, stderr=subprocess.STDOUT)
    return serial.read_text(errors='replace')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--upstream', type=Path, required=True)
    parser.add_argument('--installer', type=Path, required=True)
    parser.add_argument('--mkisofs', type=Path, required=True)
    parser.add_argument('--kernel', required=True)
    parser.add_argument('--accel', default='tcg')
    parser.add_argument('--resume', action='store_true', help='Reuse this route\'s verified source and staging files after a failed run')
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    base = args.output.resolve()
    base.mkdir(exist_ok=args.resume)
    if args.resume:
        sources = json.loads((base / 'sources.json').read_text())
        for name, expected in sources.items():
            assert digest(base / (name + '.iso')) == expected['sha256'], 'Resume source differs'
    else:
        make_sources(args, base)
    make_creation_guest(args, base)
    if not args.resume:
        with (base / 'usb.raw').open('xb') as output:
            output.truncate(DISK_BYTES)
    assert (base / 'usb.raw').stat().st_size == DISK_BYTES
    creation = boot(args, base, 'creation', direct=True)
    assert 'RUFUS_PERSISTENCE_GUEST_OK' in creation and 'RUFUS_CREATE_EXIT 0' in creation, base / 'creation.serial.log'
    assert creation.count('RUFUS_PERSISTENCE_JOB ') == 4, creation[-4000:]
    assert creation.count('RUFUS_PERSISTENCE_READBACK ') == 3, creation[-4000:]
    assert 'RUFUS_PERSISTENCE_THREE_PARTITION_OK' in creation, base / 'creation.serial.log'
    table = json.loads(run(['sfdisk', '--json', base / 'usb.raw'], capture_output=True, text=True).stdout)
    (base / 'partition-table.json').write_text(json.dumps(table, indent=2) + '\n')
    first = boot(args, base, 'first-boot')
    assert 'RUFUS_PERSISTENCE_FIRST_BOOT' in first and 'RUFUS_LIVE_EXIT 0' in first, base / 'first-boot.serial.log'
    second = boot(args, base, 'second-boot')
    assert 'RUFUS_PERSISTENCE_SECOND_BOOT' in second and 'RUFUS_LIVE_EXIT 0' in second, base / 'second-boot.serial.log'
    checks = {'jobs': 4, 'successful_real_device_ext_versions': [2, 3, 4],
              'casper_label_no_conf': True, 'debian_label_conf': True,
              'formatter_failure': True, 'one_authorization_each': True,
              'joined_ready': True, 'boot_argument': 'persistence',
              'gpt_ntfs_ext4_uefi_three_partition_backend': True,
              'boot_restarts': 2, 'surviving_file': '/marker',
              'source_sha256': json.loads((base / 'sources.json').read_text()),
              'table': table, 'disk_sha256': digest(base / 'usb.raw')}
    (base / 'checks.json').write_text(json.dumps(checks, indent=2) + '\n')
    for name in ('live', 'creation', 'casper-source', 'debian-source'):
        shutil.rmtree(base / name)
    for name in ('usb.raw', 'casper.iso', 'debian.iso'):
        (base / name).unlink()
    print('PASS: grouped persistence Qt/helper route, ext2/3/4 native USB readback, refusal, joined READY and two persistent BIOS boots; disposable media removed.')


if __name__ == '__main__':
    main()
