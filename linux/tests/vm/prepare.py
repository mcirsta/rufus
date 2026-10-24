#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline Arch/CachyOS guest recipe. No host installation, mounts or root needed."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import openwrt

HERE = Path(__file__).resolve().parent
ROOT_PACKAGES = '''filesystem bash coreutils systemd dbus udisks2 polkit-kde-agent
hivex qt6-base xorg-server-xvfb xdotool shadow kmod util-linux grep sed procps-ng ttf-dejavu'''.split()


def run(args, **kwargs):
    return subprocess.run(args, check=True, **kwargs)


def cached_packages(cache):
    packages, providers = {}, {}
    for desc in Path('/var/lib/pacman/local').glob('*/desc'):
        sections = desc.read_text().split('%')
        info = dict(zip(sections[1::2], sections[2::2]))
        name = info['NAME'].strip()
        packages[name] = info
        for provided in info.get('PROVIDES', '').split():
            providers[re.split('[<>=]', provided)[0]] = name
    pending, selected = list(ROOT_PACKAGES), set()
    while pending:
        name = re.split('[<>=]', pending.pop())[0]
        name = name if name in packages else providers.get(name, name)
        if name in selected:
            continue
        if name not in packages:
            raise RuntimeError('Unresolved installed dependency: ' + name)
        selected.add(name)
        pending.extend(packages[name].get('DEPENDS', '').split())
    paths = []
    for name in sorted(selected):
        info = packages[name]
        filename = name + '-' + info['VERSION'].strip() + '-' + info['ARCH'].strip() + '.pkg.tar.zst'
        path = cache / filename
        if not path.is_file():
            raise RuntimeError('Missing cached package: ' + str(path))
        paths.append(path)
    return paths


def write(root, path, content, mode=0o644):
    path = root / path
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content)
    path.chmod(mode)


def link(root, path, target):
    path = root / path
    path.parent.mkdir(parents=True, exist_ok=True)
    path.unlink(missing_ok=True)
    path.symlink_to(target)


def configure_guest(root, binary):
    password = subprocess.check_output(['openssl', 'passwd', '-6', '-salt', 'rufusfixture',
                                        'rufus-fixture'], text=True).strip()
    write(root, 'etc/passwd', 'root:x:0:0:root:/root:/bin/bash\nrufus:x:1000:1000:Rufus fixture:/home/rufus:/bin/bash\n')
    write(root, 'etc/group', 'root:x:0:\nwheel:x:10:rufus\nrufus:x:1000:\n')
    write(root, 'etc/shadow', 'root:!:20000:0:99999:7:::\nrufus:' + password + ':20000:0:99999:7:::\n', 0o600)
    write(root, 'etc/gshadow', 'root:!::\nwheel:!::rufus\nrufus:!::\n', 0o600)
    write(root, 'etc/hostname', 'rufus-usb-fixture\n')
    for path in ['etc/machine-id', 'etc/fstab']:
        write(root, path, '')
    write(root, 'etc/rufus-usb-fixture', 'RUFUS-TEST-USB\n')
    write(root, 'etc/pam.d/rufus-fixture', 'auth required pam_permit.so\naccount required pam_permit.so\nsession required pam_systemd.so\n')
    # Choose the fixture administrator; do not grant actions without authentication.
    write(root, 'etc/polkit-1/rules.d/49-rufus-fixture-admin.rules',
          'polkit.addAdminRule(function(action, subject) { return ["unix-user:rufus"]; });\n')
    (root / 'home/rufus').mkdir(parents=True)
    write(root, 'etc/systemd/system/rufus-fixture.service', '''[Unit]
Description=Disposable Rufus USB fixture
After=systemd-user-sessions.service systemd-logind.service dbus.service
Wants=systemd-user-sessions.service systemd-logind.service
[Service]
Type=oneshot
ExecStart=/opt/rufus/guest-root.sh
StandardOutput=journal+console
StandardError=journal+console
''')
    write(root, 'etc/systemd/system/rufus-gui.service', '''[Unit]
After=systemd-logind.service
[Service]
Type=oneshot
User=rufus
PAMName=rufus-fixture
TimeoutStopSec=5
Environment=HOME=/home/rufus
ExecStart=/usr/bin/dbus-run-session /opt/rufus/guest-user.sh
StandardOutput=journal+console
StandardError=journal+console
''')
    link(root, 'etc/systemd/system/default.target', '/usr/lib/systemd/system/multi-user.target')
    link(root, 'etc/systemd/system/serial-getty@ttyS0.service', '/dev/null')
    link(root, 'etc/systemd/system/multi-user.target.wants/rufus-fixture.service', '../rufus-fixture.service')
    for name in ['guest-root.sh', 'guest-user.sh']:
        write(root, 'opt/rufus/' + name, (HERE / name).read_text(), 0o755)
    shutil.copy2(binary, root / 'opt/rufus/test_usb_guest')
    shutil.copy2(binary.parent / 'check_usb_mount', root / 'opt/rufus/check_usb_mount')
    shutil.copy2(binary.parent / 'rufus-prepare-helper', root / 'opt/rufus/rufus-prepare-helper')


def bootstrap(base, root, release):
    kernel = Path('/usr/lib/modules') / release
    shutil.copy2(kernel / 'vmlinuz', base / 'vmlinuz')
    modules = set()
    for name in ['virtio_pci', 'virtio_blk', 'ext4', 'xhci_pci', 'usb_storage', 'sd_mod', 'vfat',
                 'nls_cp437', 'nls_iso8859_1']:
        lines = subprocess.check_output(['modprobe', '--show-depends', '-S', release, name], text=True)
        for line in lines.splitlines():
            if line.startswith('insmod '):
                modules.add(Path(line.split()[1]).resolve())
    for module in modules:
        dest = root / 'usr/lib/modules' / release / module.relative_to(kernel)
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(module, dest)
    for name in ['modules.builtin', 'modules.builtin.modinfo', 'modules.order']:
        shutil.copy2(kernel / name, root / 'usr/lib/modules' / release / name)
    run(['depmod', '-b', str(root), release])
    init = base / 'initramfs'
    for name in ['dev', 'proc', 'sys', 'run', 'newroot', 'usr/bin', 'usr/lib']:
        (init / name).mkdir(parents=True, exist_ok=True)
    for name, target in [('bin', 'usr/bin'), ('sbin', 'usr/bin'), ('lib', 'usr/lib'), ('lib64', 'usr/lib')]:
        (init / name).symlink_to(target)
    for name in ['bash', 'mount', 'chown', 'switch_root', 'sleep', 'kmod']:
        binary = Path('/usr/bin') / name
        shutil.copy2(binary, init / 'usr/bin' / name)
        output = subprocess.check_output(['ldd', str(binary)], text=True)
        for lib in re.findall(r'(/[^\s()]+)', output):
            lib = Path(lib)
            if lib.is_file():
                shutil.copy2(lib, init / 'usr/lib' / lib.name)
    (init / 'usr/bin/modprobe').symlink_to('kmod')
    shutil.copytree(root / 'usr/lib/modules', init / 'usr/lib/modules')
    write(init, 'init', (HERE / 'guest-init').read_text(), 0o755)
    names = subprocess.check_output(['find', '.', '-print0'], cwd=init)
    with (base / 'initramfs.cpio').open('wb') as output:
        run(['cpio', '--null', '-o', '-H', 'newc', '--owner=0:0'], cwd=init, input=names, stdout=output)


def boot_image(base, root):
    partition = base / 'partition.raw'
    with partition.open('xb') as file:
        file.truncate(16 * 1024 * 1024)
    run(['mkfs.fat', '--invariant', '-F', '16', '-n', 'RUFUS_TEST', str(partition)])
    run(['nasm', '-f', 'bin', str(HERE / 'boot.asm'), '-o', str(base / 'boot.mbr')])
    with (base / 'boot.img').open('xb') as file:
        file.write((base / 'boot.mbr').read_bytes())
        file.write(bytes([0x37]) * (1024 * 1024 - 512))
        file.write(partition.read_bytes())
    shutil.copy2(base / 'boot.img', root / 'opt/rufus/boot.img')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--kernel', required=True, help='Installed kernel release with matching modules')
    parser.add_argument('--cache', type=Path, default=Path('/var/cache/pacman/pkg'))
    parser.add_argument('--openwrt', type=Path, help='Directory of the two verified, pinned gzip archives')
    parser.add_argument('directory', type=Path, help='New output directory (must not exist)')
    args = parser.parse_args()
    binary = args.build.resolve() / 'test_usb_guest'
    if not binary.is_file():
        parser.error('Build with BUILD_TESTING=ON and RUFUS_BUILD_VM_TEST=ON first')
    packages = cached_packages(args.cache)
    base = args.directory.resolve()
    base.mkdir()
    root = base / 'root'
    root.mkdir()
    manifest = []
    for package in packages:
        with package.open('rb') as file:
            manifest.append({'file': package.name, 'sha256': hashlib.file_digest(file, 'sha256').hexdigest()})
        run(['bsdtar', '-xpf', str(package), '--no-same-owner', '--no-xattrs',
             '--exclude=.PKGINFO', '--exclude=.BUILDINFO', '--exclude=.MTREE', '--exclude=.INSTALL', '-C', str(root)])
    # The unused dbus-daemon helper is execute-only in the package; mkfs needs to read it.
    helper = root / 'usr/lib/dbus-daemon-launch-helper'
    if helper.exists():
        helper.chmod(helper.stat().st_mode | 0o400)
    configure_guest(root, binary)
    bootstrap(base, root, args.kernel)
    boot_image(base, root)
    linux_images = openwrt.unpack(args.openwrt, base) if args.openwrt else []
    for record in linux_images:
        shutil.copy2(base / record['file'], root / 'opt/rufus' / record['file'])
    with (base / 'system.raw').open('xb') as file:
        file.truncate(3 * 1024 * 1024 * 1024)
    run(['mkfs.ext4', '-q', '-F', '-d', str(root), str(base / 'system.raw')])
    for path in [root / 'opt/rufus/test_usb_guest', root / 'opt/rufus/check_usb_mount', root / 'opt/rufus/rufus-prepare-helper',
                 base / 'vmlinuz', base / 'initramfs.cpio', base / 'boot.img', base / 'system.raw']:
        with path.open('rb') as file:
            manifest.append({'file': path.name, 'sha256': hashlib.file_digest(file, 'sha256').hexdigest()})
    manifest.extend(linux_images)
    (base / 'manifest.json').write_text(json.dumps({
        'kernel': args.kernel, 'files': manifest, 'linux_images': linux_images
    }, indent=2) + '\n')
    print('Guest prepared:', base, flush=True)


if __name__ == '__main__':
    main()
