#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Combined native download qualification on an owned networkless USB guest."""
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
    binary = args.build / 'test_download_workflow'
    add_executable(init, binary)
    for name in ('setpriv', 'mkfs.ntfs', 'sync', 'poweroff', 'blockdev', 'ip', 'cat'):
        add_executable(init, Path('/usr/bin') / name)
    plugin = Path('/usr/lib/qt6/plugins/platforms/libqoffscreen.so')
    add_executable(init, plugin)
    plugins = init / 'usr/lib/qt6/plugins/platforms'
    plugins.mkdir(parents=True)
    shutil.copy2(init / 'usr/bin/libqoffscreen.so', plugins / 'libqoffscreen.so')
    for plugin in Path('/usr/lib/qt6/plugins/tls').glob('*.so'):
        add_executable(init, plugin)
        destination=init/'usr/lib/qt6/plugins/tls'/plugin.name
        destination.parent.mkdir(parents=True,exist_ok=True)
        shutil.copy2(init/'usr/bin'/plugin.name,destination)
    if Path('/etc/fonts').is_dir(): shutil.copytree('/etc/fonts',init/'etc/fonts')
    font=Path('/usr/share/fonts/TTF/DejaVuSans.ttf')
    if font.is_file():
        (init/'usr/share/fonts/TTF').mkdir(parents=True,exist_ok=True)
        shutil.copy2(font,init/'usr/share/fonts/TTF'/font.name)
    shutil.copytree('/usr/lib/locale/C.utf8', init / 'usr/lib/locale/C.utf8')
    # The component binary's path refers to this inert fixture within the guest.
    fixtures = Path(__file__).resolve().parents[1] / 'fixtures/fido'
    shutil.copytree(fixtures, init / str(fixtures).lstrip('/'))
    kernel = Path('/usr/lib/modules') / args.kernel
    for line in subprocess.check_output(['modprobe', '--show-depends', '-S', args.kernel, 'ntfs'], text=True).splitlines():
        if line.startswith('insmod '):
            module = Path(line.split()[1]).resolve()
            destination = init / 'usr/lib/modules' / args.kernel / module.relative_to(kernel)
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(module, destination)
    run(['depmod', '-b', str(init), args.kernel])
    (init / 'mnt/downloads').mkdir(parents=True)
    (init / 'tmp').mkdir(mode=0o1777, exist_ok=True)
    (init / 'tmp').chmod(0o1777)
    prepare.write(init, 'etc/rufus-download-fixture', 'downloads\n')
    prepare.write(init, 'etc/passwd', 'root:x:0:0:root:/root:/bin/bash\ncaller:x:1000:1000:Caller:/tmp:/bin/bash\n')
    prepare.write(init, 'etc/group', 'root:x:0:\ncaller:x:1000:\n')
    prepare.write(init, 'etc/nsswitch.conf', 'passwd: files\ngroup: files\n')
    prepare.write(init, 'init', r'''#!/bin/bash
set -eu
[[ $$ == 1 ]] || exit 77
export PATH=/usr/bin LC_ALL=C.UTF-8 QT_QPA_PLATFORM=offscreen
export ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
mount -t devtmpfs devtmpfs /dev
mount -t proc proc /proc
mount -t sysfs sysfs /sys
finish() { echo "RUFUS_DOWNLOADS_EXIT $?"; sync; poweroff -f; }
trap finish EXIT
[[ $(cat /sys/class/dmi/id/sys_vendor) == RufusTest ]]
[[ $(cat /sys/class/dmi/id/product_name) == RufusDownloadFixture ]]
[[ $(cat /etc/rufus-download-fixture) == downloads ]]
for module in xhci_pci usb_storage sd_mod ntfs; do modprobe "$module"; done
for ((n=0;n<100;n++)); do [[ -b /dev/sda ]] && break; sleep .1; done
[[ $(blockdev --getsize64 /dev/sda) == 536870912 ]]
serial_ok=false
for path in /sys/bus/usb/devices/*/serial; do
  [[ -f "$path" ]] || continue
  if [[ $(cat "$path") == RUFUS-DOWNLOADS ]]; then serial_ok=true; fi
done
[[ $serial_ok == true ]]
# Only this guarded disposable USB is written for fixture setup.
mkfs.ntfs -Q -F -L DOWNLOADS /dev/sda
mount -t ntfs -o uid=1000,gid=1000 /dev/sda /mnt/downloads
ip link set lo up
export TMPDIR=/mnt/downloads XDG_CACHE_HOME=/tmp/rufus-download-cache
setpriv --reuid=1000 --regid=1000 --clear-groups /usr/bin/test_download_workflow \
  signedCatalogAndNativeProtocol shellAndChooserBacktracking streamAndPublish \
  transferRefusals:cancel transferRefusals:disk-full transferRefusals:sync-failure \
  splitButtonAndAutomaticSelection
sync
echo RUFUS_DOWNLOADS_BATCH_DONE
''', 0o755)
    names = run(['find', '.', '-print0'], cwd=init, capture_output=True).stdout
    with (base / 'initramfs.cpio').open('wb') as output:
        run(['cpio', '--null', '-o', '-H', 'newc', '--owner=0:0'], cwd=init, input=names, stdout=output)
    disk = base / 'usb.raw'
    with disk.open('xb') as output:
        output.truncate(512 * 1024 * 1024)
    serial = base / 'serial.log'
    command = ['qemu-system-x86_64', '-accel', args.accel, '-cpu', 'max', '-m', '1536',
               '-nic', 'none', '-display', 'none', '-monitor', 'none', '-no-reboot',
               '-serial', 'file:' + str(serial), '-kernel', str(base / 'vmlinuz'),
               '-initrd', str(base / 'initramfs.cpio'), '-append', 'console=ttyS0 panic=-1',
               '-smbios', 'type=1,manufacturer=RufusTest,product=RufusDownloadFixture',
               '-device', 'qemu-xhci,id=xhci', '-drive', 'if=none,id=stick,format=raw,file=' + str(disk),
               '-device', 'usb-storage,drive=stick,removable=on,serial=RUFUS-DOWNLOADS']
    (base / 'qemu-command.json').write_text(json.dumps(command, indent=2) + '\n')
    with (base / 'qemu.log').open('wb') as output:
        subprocess.run(command, timeout=240, stdout=output, stderr=subprocess.STDOUT, check=True)
    text = serial.read_text(errors='replace')
    assert 'RUFUS_DOWNLOADS_BATCH_DONE' in text and 'RUFUS_DOWNLOADS_EXIT 0' in text, serial
    assert 'FAIL!' not in text and 'AddressSanitizer' not in text and 'runtime error:' not in text, serial
    assert 'Totals: 9 passed, 0 failed, 0 skipped' in text, serial
    result = {'download_uid': 1000, 'filesystem': 'native-ntfs', 'external_network': False,
              'signed_catalog_and_original_provider_api': True, 'native_shell_chooser': True,
              'transactional_download_readback': True, 'disk_full_sync_cancel_preserve_existing': True,
              'ctrl_select_and_automatic_image_analysis': True, 'real_cases': 7,
              'probe_sha256': digest(binary)}
    (base / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print('Combined download guest passed:', base)


if __name__ == '__main__':
    main()
