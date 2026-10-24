#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Real Windows ISO input and independent readback for the existing USB guest."""
import hashlib
import json
import shutil
import subprocess
import xml.etree.ElementTree as ET

from alpine_iso import tree_contents
from fat_extract import digest

DISK_BYTES = 16 * 1024**3
SYSTEM_BYTES = 16 * 1024**3
IO_TIMEOUT_SECONDS = 600
GUEST_TIMEOUT_SECONDS = 1800
LABEL = 'Windows 11'
CLUSTER_BYTES = 4096
UEFI_NTFS_FLAG = 4
ANSWER_PATH = 'sources/$OEM$/$$/Panther/unattend.xml'
ANSWER_DIRECTORIES = ['sources/$OEM$', 'sources/$OEM$/$$', 'sources/$OEM$/$$/Panther']


def prepare_image(args, base, root):
    # 7-Zip selects UDF independently of Rufus. libarchive only sees the small
    # ISO9660 compatibility tree in Microsoft's UDF-bridge installation images.
    reference = base / 'windows-reference'
    reference.mkdir()
    with (base / 'windows-7z.log').open('wb') as log:
        subprocess.run(['7z', 'x', '-y', '-o' + str(reference), str(args.windows)],
                       check=True, stdout=log, stderr=subprocess.STDOUT, timeout=IO_TIMEOUT_SECONDS)
    contents = tree_contents(reference)
    required = ('efi/boot/bootx64.efi', 'bootmgr.efi', 'sources/boot.wim', 'sources/install.wim', 'setup.exe')
    for name in required:
        assert name in contents['files'], name
    copied_bytes = sum(path.stat().st_size for path in reference.rglob('*') if path.is_file())
    result = subprocess.run([str(args.build / 'iso_write_check_probe'), str(args.windows),
                             str(DISK_BYTES), str(CLUSTER_BYTES), LABEL, 'ntfs'],
                            check=True, capture_output=True, text=True, timeout=IO_TIMEOUT_SECONDS)
    preflight = json.loads(result.stdout)
    assert preflight['error'] == 0, preflight
    assert preflight['extraPartitions'] == UEFI_NTFS_FLAG, preflight
    assert int(preflight['files']) == len(contents['files']), preflight
    assert int(preflight['bytes']) == copied_bytes, preflight
    (base / 'windows-preflight.log').write_text(result.stderr)
    record = {'source': str(args.windows), 'sha256': digest(args.windows),
              'iso_bytes': args.windows.stat().st_size, 'target_label': LABEL,
              'contents': contents, 'preflight': preflight}
    record['privacy_customization'] = args.windows_privacy
    (base / 'windows-source.json').write_text(json.dumps(record, indent=2) + '\n')
    shutil.rmtree(reference)
    # mkfs.ext4 imports the source once. A hard link avoids an additional 8 GiB
    # host copy and is never modified; fall back when Downloads is another FS.
    destination = root / 'home/rufus/WINDOWS.iso'
    try:
        destination.hardlink_to(args.windows)
    except OSError:
        shutil.copyfile(args.windows, destination)
    (root / 'etc/rufus-windows-fixture').write_text('RUFUS-WINDOWS-TEST\n')
    if args.windows_privacy:
        (root / 'etc/rufus-windows-privacy').write_text('RUFUS-WINDOWS-PRIVACY-TEST\n')


def check_copied(base):
    expected = json.loads((base / 'windows-source.json').read_text())
    for name in ('windows-files.sha256z', 'windows-directories'):
        with (base / name).open('wb') as output:
            subprocess.run(['debugfs', '-R', 'cat /home/rufus/' + name, str(base / 'system.raw')],
                           check=True, stdout=output, stderr=subprocess.PIPE, timeout=30)
    files = {}
    for entry in (base / 'windows-files.sha256z').read_bytes().split(b'\0'):
        if not entry:
            continue
        assert len(entry) > 66 and entry[64:66] == b'  ', entry
        name = entry[66:].decode('utf-8')
        assert name not in files, name
        files[name] = entry[:64].decode('ascii')
    directories = (base / 'windows-directories').read_bytes().split(b'\0')
    actual = {'files': files, 'directories': sorted(name.decode('utf-8') for name in directories if name)}
    if expected.get('privacy_customization'):
        check_privacy_answer(base, actual, expected['contents'])
    assert actual == expected['contents'], 'Windows source and independently read NTFS tree differ'
    print('PASS: every Windows file matches independent UDF/NTFS3 readback.', flush=True)


def check_privacy_answer(base, actual, original):
    path = base / 'windows-unattend.xml'
    with path.open('wb') as output:
        subprocess.run(['debugfs', '-R', 'cat /home/rufus/windows-unattend.xml', str(base / 'system.raw')],
                       check=True, stdout=output, stderr=subprocess.PIPE, timeout=30)
    data = path.read_bytes()
    checksum = hashlib.sha256(data).hexdigest()
    assert actual['files'].pop(ANSWER_PATH) == checksum
    ns = '{urn:schemas-microsoft-com:unattend}'
    root = ET.fromstring(data)
    assert [element.tag for element in root.iter()] == [ns + name for name in
        ('unattend', 'settings', 'component', 'OOBE', 'HideEULAPage', 'ProtectYourPC')]
    assert root[0].attrib == {'pass': 'oobeSystem'}
    assert root[0][0].attrib == {'name': 'Microsoft-Windows-Shell-Setup',
        'processorArchitecture': 'amd64', 'language': 'neutral',
        'publicKeyToken': '31bf3856ad364e35', 'versionScope': 'nonSxS'}
    assert root[0][0][0][0].text == 'true'
    assert root[0][0][0][1].text == '3'
    for directory in ANSWER_DIRECTORIES:
        if directory not in original['directories']:
            actual['directories'].remove(directory)
    report = {'option': 'Disable data collection (Skip privacy questions)',
              'answer_path': ANSWER_PATH, 'sha256': checksum, 'bytes': len(data),
              'verified': True}
    (base / 'windows-customization.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS: only the selected privacy answer file and its OEM directories were added.', flush=True)
