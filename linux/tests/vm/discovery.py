#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""One disposable, networkless discovery guest; no host mounts/device writes."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import prepare
from fat_extract import add_executable, digest, run


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--kernel', required=True)
    parser.add_argument('--accel', choices=('tcg', 'kvm'), default='tcg')
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    assert os.getuid() != 0
    base = args.directory.resolve()
    base.mkdir()
    root = base / 'root'
    root.mkdir()
    (root / 'lib').symlink_to('usr/lib')
    prepare.bootstrap(base, root, args.kernel)
    init = base / 'initramfs'
    for binary in (args.build / 'test_discovery_guest', args.build / 'udisks_fixture'):
        add_executable(init, binary)
    for name in ('dbus-daemon', 'setpriv', 'mkfs.fat', 'mkfs.ntfs', 'mkfs.exfat', 'cp', 'sync', 'poweroff', 'blockdev', 'losetup'):
        add_executable(init, Path('/usr/bin') / name)
    shutil.copytree('/usr/lib/locale/C.utf8', init / 'usr/lib/locale/C.utf8')
    kernel = Path('/usr/lib/modules') / args.kernel
    for name in ('ntfs', 'exfat', 'loop'):
        for line in subprocess.check_output(['modprobe', '--show-depends', '-S', args.kernel, name], text=True).splitlines():
            if line.startswith('insmod '):
                module = Path(line.split()[1]).resolve()
                destination = init / 'usr/lib/modules' / args.kernel / module.relative_to(kernel)
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(module, destination)
    run(['depmod', '-b', str(init), args.kernel])
    # A real loop-backed executable, with the loop file residing on the USB.
    loop = init / 'app.raw'
    with loop.open('xb') as output:
        output.truncate(32 * 1024 * 1024)
    run(['mkfs.ext4', '-q', '-F', str(loop)], capture_output=True)
    run(['debugfs', '-w', '-R', 'write ' + str(args.build / 'test_discovery_guest') + ' /probe', str(loop)], capture_output=True)
    run(['debugfs', '-w', '-R', 'sif /probe mode 0100755', str(loop)], capture_output=True)
    for name in ('fat', 'ntfs', 'exfat', 'loop'):
        (init / 'mnt' / name).mkdir(parents=True)
    (init / 'run/dbus').mkdir(parents=True)
    prepare.write(init, 'etc/rufus-discovery-fixture', 'discovery\n')
    prepare.write(init, 'etc/passwd', 'root:x:0:0:root:/root:/bin/bash\ncaller:x:1000:1000:Caller:/run:/bin/bash\n')
    prepare.write(init, 'etc/group', 'root:x:0:\ncaller:x:1000:\n')
    prepare.write(init, 'etc/nsswitch.conf', 'passwd: files\ngroup: files\n')
    prepare.write(init, 'etc/machine-id', '3f072f338dfb4029b8f2c40f6a5ac313\n')
    prepare.write(init, 'etc/discovery-bus.conf', '''<busconfig>
<type>system</type><listen>unix:path=/run/dbus/system_bus_socket</listen><auth>EXTERNAL</auth>
<policy context="default"><allow user="root"/><allow user="caller"/><allow own="*"/>
<allow send_destination="*"/><allow receive_sender="*"/></policy></busconfig>
''')
    prepare.write(init, 'init', r'''#!/bin/bash
set -eu
[[ $$ == 1 ]] || exit 77
export PATH=/usr/bin LC_ALL=C.UTF-8
export ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
mount -t devtmpfs devtmpfs /dev
mount -t proc proc /proc
mount -t sysfs sysfs /sys
finish() { echo "RUFUS_DISCOVERY_EXIT $?"; sync; poweroff -f; }
trap finish EXIT
[[ $(cat /sys/class/dmi/id/sys_vendor) == RufusTest ]]
[[ $(cat /sys/class/dmi/id/product_name) == RufusDiscoveryFixture ]]
for module in xhci_pci usb_storage sd_mod vfat nls_cp437 nls_iso8859_1 ntfs exfat loop ext4; do modprobe "$module"; done
for ((n=0;n<100;n++)); do [[ -b /dev/sda3 ]] && break; sleep .1; done
[[ $(blockdev --getsize64 /dev/sda) == 335544320 ]]
serial_ok=false
for path in /sys/bus/usb/devices/*/serial; do
  [[ -f "$path" ]] || continue
  if [[ $(cat "$path") == RUFUS-DISCOVERY ]]; then serial_ok=true; fi
done
[[ $serial_ok == true ]]
# Only the guarded, disposable USB below is written during fixture setup.
mkfs.fat -F 16 -n FAT /dev/sda1
mkfs.ntfs -Q -F -L NTFS /dev/sda2
mkfs.exfat -L EXFAT /dev/sda3
mount -t vfat /dev/sda1 /mnt/fat
mount -t ntfs /dev/sda2 /mnt/ntfs
mount -t exfat /dev/sda3 /mnt/exfat
for name in fat ntfs exfat; do printf '\357\273\277[autorun]\r\nlabel=Long %s installer\r\n' "$name" > "/mnt/$name/autorun.inf"; done
cp /usr/bin/test_discovery_guest /mnt/fat/probe
cp /app.raw /mnt/fat/app.raw
sync
for name in fat ntfs exfat; do mount -o remount,ro "/mnt/$name"; done
loop=$(losetup --find --show --read-only /mnt/fat/app.raw)
mount -t ext4 -o ro "$loop" /mnt/loop
dbus-daemon --config-file=/etc/discovery-bus.conf --fork
udisks_fixture unix:path=/run/dbus/system_bus_socket > /run/fixture.log &
for ((n=0;n<100;n++)); do [[ -s /run/fixture.log ]] && break; sleep .1; done
setpriv --reuid=1000 --regid=1000 --clear-groups /usr/bin/test_discovery_guest listed
setpriv --reuid=1000 --regid=1000 --clear-groups /mnt/fat/probe self
setpriv --reuid=1000 --regid=1000 --clear-groups /mnt/loop/probe self
echo RUFUS_DISCOVERY_BATCH_DONE
''', 0o755)
    # cat/printf are shell fixture tools, never included in the application.
    add_executable(init, Path('/usr/bin/cat'))
    names = run(['find', '.', '-print0'], cwd=init, capture_output=True).stdout
    with (base / 'initramfs.cpio').open('wb') as output:
        run(['cpio', '--null', '-o', '-H', 'newc', '--owner=0:0'], cwd=init, input=names, stdout=output)
    disk = base / 'usb.raw'
    with disk.open('xb') as output:
        output.truncate(320 * 1024 * 1024)
    run(['sfdisk', str(disk)], input=b'label: dos\nstart=2048,size=262144,type=6\nstart=264192,size=131072,type=7\nstart=395264,size=131072,type=7\n', capture_output=True)
    serial = base / 'serial.log'
    command = ['qemu-system-x86_64', '-accel', args.accel, '-cpu', 'max', '-m', '1024',
               '-nic', 'none', '-display', 'none', '-monitor', 'none', '-no-reboot',
               '-serial', 'file:' + str(serial), '-kernel', str(base / 'vmlinuz'),
               '-initrd', str(base / 'initramfs.cpio'), '-append', 'console=ttyS0 panic=-1',
               '-smbios', 'type=1,manufacturer=RufusTest,product=RufusDiscoveryFixture',
               '-device', 'qemu-xhci,id=xhci', '-drive', 'if=none,id=stick,format=raw,file=' + str(disk),
               '-device', 'usb-storage,drive=stick,removable=on,serial=RUFUS-DISCOVERY']
    (base / 'qemu-command.json').write_text(json.dumps(command, indent=2) + '\n')
    with (base / 'qemu.log').open('wb') as output:
        subprocess.run(command, timeout=180, stdout=output, stderr=subprocess.STDOUT, check=True)
    text = serial.read_text(errors='replace')
    assert 'RUFUS_DISCOVERY_BATCH_DONE' in text and 'RUFUS_DISCOVERY_EXIT 0' in text, serial
    assert text.count('RUFUS_DISCOVERY_DONE self UID=1000') == 2, serial
    assert 'RUFUS_DISCOVERY_DONE listed UID=1000' in text, serial
    assert 'AddressSanitizer' not in text and 'runtime error:' not in text, serial
    result = {'mounted_filesystems': ['fat16', 'ntfs', 'exfat'], 'discovery_uid': 1000,
              'direct_and_loop_executable_disk_excluded': True, 'read_only_discovery': True,
              'network': False, 'probe_sha256': digest(args.build / 'test_discovery_guest')}
    (base / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print('Combined discovery guest passed:', base)


if __name__ == '__main__':
    main()
