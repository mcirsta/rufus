# SPDX-License-Identifier: GPL-3.0-or-later
"""Small known-Windows metadata with native EFI fixture; no Windows runs."""
import hashlib
import json
import re
import subprocess


def stage_fixture(args, base, init):
    from prepare import write
    tree = base / 'ntfs-options-source'
    (tree / 'sources').mkdir(parents=True)
    (tree / 'efi/boot').mkdir(parents=True)
    subprocess.run([str(args.build / 'windows_to_go_fixture'), str(tree / 'sources/install.wim'),
                    'wim', str(args.build / 'uefi-handoff.efi')], check=True)
    efi = (args.build / 'uefi-handoff.efi').read_bytes()
    for name in ['bootmgr', 'bootmgr.efi', 'setup.exe', 'efi/boot/bootx64.efi']:
        (tree / name).write_bytes(efi)
    # Nonresident, highly compressible data proves both actual compressed writes
    # and exact logical readback; no NTFS source records are fabricated.
    (tree / 'payload.bin').write_bytes(bytes(range(256)) * 4096)
    entries = {}
    for path in tree.rglob('*'):
        if path.is_file():
            entries[str(path.relative_to(tree))] = hashlib.sha256(path.read_bytes()).hexdigest()
    subprocess.run([str(args.mkisofs), '-quiet', '-R', '-J', '-V', 'OPTIONS',
                    '-o', str(init / 'ntfs-options.iso'), str(tree)], check=True)
    cache = (args.build / 'CMakeCache.txt').read_text()
    provider = re.search(r'^RUFUS_NTFS_3G:FILEPATH=(.+)$', cache, re.M)[1]
    assert provider.startswith('/usr/bin/') and '\n' not in provider
    write(init, 'ntfs-provider-path', provider + '\n')
    write(init, 'ntfs-options-files.json', json.dumps(entries, indent=2) + '\n')
    write(init, 'etc/rufus-ntfs-options-fixture', 'ntfs-options\n')
    write(init, 'etc/rufus-format-helper-fixture', 'format-helper\n')
    (init / 'tmp').mkdir(mode=0o1777, exist_ok=True)
    (init / 'tmp').chmod(0o1777)
