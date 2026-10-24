#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Qualify explicit languages as UID1000 in a networkless, diskless guest."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import prepare
from fat_extract import add_executable, run


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
    add_executable(init, args.build / 'test_localization')
    for name in ('setpriv', 'sync', 'poweroff', 'cat', 'id'):
        add_executable(init, Path('/usr/bin') / name)
    plugin = Path('/usr/lib/qt6/plugins/platforms/libqoffscreen.so')
    add_executable(init, plugin)
    plugins = init / 'usr/lib/qt6/plugins/platforms'
    plugins.mkdir(parents=True)
    shutil.copy2(init / 'usr/bin/libqoffscreen.so', plugins / 'libqoffscreen.so')
    if Path('/etc/fonts').is_dir():
        shutil.copytree('/etc/fonts', init / 'etc/fonts')
    font = Path('/usr/share/fonts/TTF/DejaVuSans.ttf')
    if font.is_file():
        destination = init / 'usr/share/fonts/TTF'
        destination.mkdir(parents=True)
        shutil.copy2(font, destination / font.name)
    shutil.copytree('/usr/lib/locale/C.utf8', init / 'usr/lib/locale/C.utf8')
    (init / 'tmp').mkdir(mode=0o1777)
    (init / 'tmp').chmod(0o1777)
    prepare.write(init, 'etc/passwd', 'root:x:0:0:root:/root:/bin/bash\ncaller:x:1000:1000:Caller:/tmp:/bin/bash\n')
    prepare.write(init, 'etc/group', 'root:x:0:\ncaller:x:1000:\n')
    prepare.write(init, 'etc/nsswitch.conf', 'passwd: files\ngroup: files\n')
    prepare.write(init, 'etc/rufus-localization-fixture', 'localization\n')
    prepare.write(init, 'init', r'''#!/bin/bash
set -eu
[[ $$ == 1 ]] || exit 77
export PATH=/usr/bin LC_ALL=C.UTF-8 QT_QPA_PLATFORM=offscreen
export TMPDIR=/tmp XDG_CACHE_HOME=/tmp/rufus-localization-cache
export ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
mount -t devtmpfs devtmpfs /dev
mount -t proc proc /proc
mount -t sysfs sysfs /sys
finish() { echo "RUFUS_LOCALIZATION_EXIT $?"; sync; poweroff -f; }
trap finish EXIT
[[ $(cat /sys/class/dmi/id/sys_vendor) == RufusTest ]]
[[ $(cat /sys/class/dmi/id/product_name) == RufusLocalizationFixture ]]
[[ $(cat /etc/rufus-localization-fixture) == localization ]]
[[ $(setpriv --reuid=1000 --regid=1000 --clear-groups id -u) == 1000 ]]
setpriv --reuid=1000 --regid=1000 --clear-groups /usr/bin/test_localization
echo RUFUS_LOCALIZATION_BATCH_DONE
''', 0o755)
    names = run(['find', '.', '-print0'], cwd=init, capture_output=True).stdout
    with (base / 'initramfs.cpio').open('wb') as output:
        run(['cpio', '--null', '-o', '-H', 'newc', '--owner=0:0'], cwd=init, input=names, stdout=output)
    serial = base / 'serial.log'
    command = ['qemu-system-x86_64', '-accel', args.accel, '-cpu', 'max', '-m', '1536',
               '-nic', 'none', '-display', 'none', '-monitor', 'none', '-no-reboot',
               '-serial', 'file:' + str(serial), '-kernel', str(base / 'vmlinuz'),
               '-initrd', str(base / 'initramfs.cpio'), '-append', 'console=ttyS0 panic=-1',
               '-smbios', 'type=1,manufacturer=RufusTest,product=RufusLocalizationFixture']
    (base / 'qemu-command.json').write_text(json.dumps(command, indent=2) + '\n')
    with (base / 'qemu.log').open('wb') as output:
        subprocess.run(command, timeout=240, stdout=output, stderr=subprocess.STDOUT, check=True)
    text = serial.read_text(errors='replace')
    assert 'RUFUS_LOCALIZATION_BATCH_DONE' in text and 'RUFUS_LOCALIZATION_EXIT 0' in text, serial
    assert 'FAIL!' not in text and 'AddressSanitizer' not in text and 'runtime error:' not in text, serial
    (base / 'checks.json').write_text(json.dumps({
        'batch': 'localization', 'guest_uid': 1000, 'network': False,
        'attached_disks': 0, 'guest_device_writes': False, 'all_catalog_languages': 38,
        'exit_code': 0, 'serial': str(serial)}, indent=2) + '\n')
    print('Combined catalog/parser/live-language/persistence qualification passed as UID1000.')


if __name__ == '__main__':
    main()
