# SPDX-License-Identifier: GPL-3.0-or-later
"""Generated legacy setup source, complete helper readback and BIOS handoff."""
import hashlib
import errno
import json
from pathlib import Path
import struct

from winpe_bios import boot, run, digest, CRC_OFFSET, PATH_OFFSET, DRIVE_OFFSET, EXPECTED_SIF
from windows_bios import DISK_BYTES, CLUSTER_BYTES, FAT32_CLUSTER_BYTES, SECTOR_BYTES, read_window, inspect_fat
from grub2_iso import inspect_ntfs
from fat_extract import values
from test_mbr_image import extent, inspect_mbr


def add_syslinux_fixture(tree, mode):
    config = 'boot/i386/loader/isolinux.cfg' if mode == 'overwrite' else 'isolinux/isolinux.cfg'
    path = tree / config
    path.parent.mkdir(parents=True)
    path.write_bytes(b'DEFAULT test\r\nappend LABEL=RUFUS_SOURCE\r\n')
    expected = {config: hashlib.sha256(b'DEFAULT test\nappend LABEL=RUFUS_TEST\n').hexdigest()}
    root_text = b'DEFAULT root\nappend LABEL=RUFUS_TEST\n'
    if mode != 'create':
        (tree / 'syslinux.cfg').write_bytes(b'DEFAULT root\r\nappend LABEL=RUFUS_SOURCE\r\n')
    if mode == 'keep':
        expected['syslinux.cfg'] = hashlib.sha256(root_text).hexdigest()
    else:
        if mode == 'overwrite':
            expected['syslinux.org'] = hashlib.sha256(root_text).hexdigest()
        text = ('DEFAULT loadconfig\n\nLABEL loadconfig\n  CONFIG /' + config
                + '\n  APPEND /' + str(Path(config).parent) + '/\n').encode()
        expected['syslinux.cfg'] = hashlib.sha256(text).hexdigest()
    return expected


def add_grub_fixture(tree, mode):
    expected = {}
    if mode in ('grub2', 'both'):
        directory = tree / 'boot/grub/i386-pc'
        directory.mkdir(parents=True)
        module = bytearray(4096)
        banner = b'GRUB  version %s\0' + b'2.12\0'
        module[64:64 + len(banner)] = banner
        (directory / 'normal.mod').write_bytes(module)
        (tree / 'boot/grub/grub.cfg').write_bytes(b'linux /vmlinuz LABEL=RUFUS_SOURCE\r\n')
        expected['boot/grub/grub.cfg'] = hashlib.sha256(b'linux /vmlinuz LABEL=RUFUS_TEST\n').hexdigest()
    if mode in ('grub4dos', 'both'):
        (tree / 'grldr').write_bytes(b'original GRUB4DOS loader data')
        (tree / 'menu.lst').write_bytes(b'kernel /vmlinuz LABEL=RUFUS_SOURCE\r\n')
    return expected


def make_fixture(args, base):
    family = args.windows_winpe
    minint_directory = family == 'minint'
    uses_minint = minint_directory or args.windows_winpe_partial_minint
    tree = base / 'windows-source'
    loader_directory = tree / ('minint' if minint_directory else 'i386')
    setup_directory = tree / family
    loader_directory.mkdir(parents=True)
    setup_directory.mkdir(exist_ok=True)
    loader = loader_directory / 'setupldr.bin'
    drive = 0x80 if uses_minint else 0x81
    assembly = Path(__file__).with_name('windows_bootmgr.asm')
    run(['nasm', '-f', 'bin', '-DEXPECTED_DRIVE=' + hex(drive), assembly, '-o', loader], base, 'nasm')
    source_loader = bytearray(loader.read_bytes())
    source_loader[CRC_OFFSET:CRC_OFFSET + 2] = b'\x74\x03'
    marker = b'\\minint\\txtsetup.sif\x00'
    source_loader[PATH_OFFSET:PATH_OFFSET + len(marker)] = marker
    marker = b'rdisk(0)\x00'
    source_loader[DRIVE_OFFSET:DRIVE_OFFSET + len(marker)] = marker
    loader.write_bytes(source_loader)
    # Independent expected bytes for this fixed generated loader, also usable
    # as the already-prepared root input. Production still uses the shared C patcher.
    prepared = bytearray(source_loader)
    if not uses_minint:
        prepared[CRC_OFFSET:CRC_OFFSET + 2] = b'\xeb\x1a'
        marker = ('\\' + family + '\\txtsetup.sif\x00').encode()
        prepared[PATH_OFFSET:PATH_OFFSET + len(marker)] = marker
        prepared[DRIVE_OFFSET + 6] = ord('1')
    if args.windows_winpe_root_loader:
        root_loader = prepared if args.windows_winpe_root_loader_content == 'prepatched' else source_loader
        (tree / 'BoOtMgR').write_bytes(root_loader)
    detector = b'generated detector fixture\r\n'
    (loader_directory / 'ntdetect.com').write_bytes(detector)
    root_detector = detector
    if args.windows_winpe_root_detector_content == 'different':
        root_detector = b'different retained root detector\r\n' + bytes(range(256)) * 512 + b'tail'
    if args.windows_winpe_root_detector:
        (tree / 'NtDetect.Com').write_bytes(root_detector)
    options = b'/fastdetect /minint' if uses_minint else b'/fastdetect'
    (setup_directory / 'txtsetup.sif').write_bytes(b'[SetupData]\r\nOsLoadOptions=' + options + b'\r\n')
    if args.windows_winpe_partial_minint:
        (tree / 'minint').mkdir()
        (tree / 'minint/TxTsEtUp.SiF').write_bytes(b'unselected minint SIF\r\n')
    if args.windows_winpe_root_sif:
        (tree / 'TxTsEtUp.SiF').write_bytes(b';root-only\n[SetupData]\nRootValue=keep\n')
    (tree / 'data').mkdir()
    (tree / 'data/copy.bin').write_bytes(bytes(range(256)) * 8192 + b'tail')
    syslinux_mode = getattr(args, 'windows_winpe_syslinux', None)
    syslinux_expected = add_syslinux_fixture(tree, syslinux_mode) if syslinux_mode else {}
    grub_mode = getattr(args, 'windows_winpe_grub_data', None)
    grub_expected = add_grub_fixture(tree, grub_mode) if grub_mode else {}
    files = {str(path.relative_to(tree)): digest(path) for path in tree.rglob('*') if path.is_file()}
    expected = dict(files)
    expected.update(syslinux_expected)
    expected.update(grub_expected)
    if not args.windows_winpe_failure:
        if not uses_minint:
            if args.windows_winpe_root_sif:
                edited_sif = (b';root-only\r\n[SetupData]\r\n'
                              b'SetupSourceDevice = "\\device\\harddisk1\\partition1"\r\nRootValue=keep\r\n')
                expected['TxTsEtUp.SiF'] = hashlib.sha256(edited_sif).hexdigest()
            else:
                expected['TXTSETUP.SIF'] = hashlib.sha256(EXPECTED_SIF).hexdigest()
        loader_name = 'BoOtMgR' if args.windows_winpe_root_loader else 'BOOTMGR'
        expected[loader_name] = hashlib.sha256(prepared).hexdigest()
        detector_name = 'NtDetect.Com' if args.windows_winpe_root_detector else 'ntdetect.com'
        expected[detector_name] = hashlib.sha256(root_detector).hexdigest()
    args.iso = base / 'windows-source.iso'
    command = [args.mkisofs, '-quiet', '-R', '-J', '-V', 'RUFUS_SOURCE', '-o', args.iso, tree]
    if args.windows_udf:
        command.insert(1, '-udf')
    run(command, base, 'mkisofs')
    manifest = {'family': family, 'uses_minint': uses_minint,
                'partial_minint': args.windows_winpe_partial_minint, 'syslinux_config': syslinux_mode, 'grub_data': grub_mode,
                'iso_sha256': digest(args.iso),
                'identical_root_detector': args.windows_winpe_root_detector and args.windows_winpe_root_detector_content == 'identical',
                'root_detector_content': args.windows_winpe_root_detector_content if args.windows_winpe_root_detector else None,
                'existing_root_sif': args.windows_winpe_root_sif,
                'identical_root_loader': args.windows_winpe_root_loader and args.windows_winpe_root_loader_content == 'identical',
                'root_loader_content': args.windows_winpe_root_loader_content if args.windows_winpe_root_loader else None,
                'udf_bridge': args.windows_udf, 'files': files, 'expected_drive': drive,
                'limit': 'Generated legacy layout and loader, no Windows Setup qualification'}
    (base / 'windows-source-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    (base / 'windows-target-manifest.json').write_text(json.dumps({'files': expected}, indent=2) + '\n')


def inspect_written(args, base, disk):
    minint_directory = args.windows_winpe == 'minint'
    uses_minint = minint_directory or args.windows_winpe_partial_minint
    code = args.windows_bios if uses_minint or args.windows_winpe_failure else 'rufus-masquerade'
    cluster = FAT32_CLUSTER_BYTES if args.windows_fat32 else CLUSTER_BYTES
    offset, size = extent(DISK_BYTES, cluster)
    has_sbr = args.windows_winpe_grub_data and not args.windows_winpe_syslinux
    grub4dos_sbr = has_sbr and args.windows_winpe_grub_data == 'grub4dos'
    grub2_sbr = has_sbr and not grub4dos_sbr
    resources = Path(__file__).parents[3] / 'res'
    core = (resources / 'grub/grldr.mbr').read_bytes()[SECTOR_BYTES:] if grub4dos_sbr else (
        (resources / 'grub2/core.img').read_bytes() if grub2_sbr else b'')
    with disk.open('rb') as stream:
        mbr = stream.read(SECTOR_BYTES)
        stream.seek(SECTOR_BYTES)
        assert stream.read(offset - SECTOR_BYTES) == core + bytes(offset - SECTOR_BYTES - len(core))
    signature = struct.unpack_from('<I', mbr, 440)[0]
    inspect_mbr(disk, DISK_BYTES, cluster, file_system=32 if args.windows_fat32 else 'ntfs',
                signature=signature, boot_code=code)
    window = read_window(disk, offset)
    if args.windows_fat32:
        readback = inspect_fat(base, disk, offset, size, window, legacy=True,
                              masquerade=not uses_minint, installed=not args.windows_winpe_failure)
    else:
        readback = inspect_ntfs(base, disk, offset, size, window, 'windows-target-manifest.json', 'windows-image')
    actual = {str(path.relative_to(readback)): digest(path) for path in readback.rglob('*') if path.is_file()}
    expected = json.loads((base / 'windows-target-manifest.json').read_text())['files']
    assert actual == expected, (actual, expected)
    source = json.loads((base / 'windows-source-manifest.json').read_text())['files']
    if args.windows_winpe_root_detector:
        directory = 'minint' if minint_directory else 'i386'
        root_hash, directory_hash = actual['NtDetect.Com'], actual[directory + '/ntdetect.com']
        assert root_hash == source['NtDetect.Com']
        assert directory_hash == source[directory + '/ntdetect.com']
        assert (root_hash == directory_hash) == (args.windows_winpe_root_detector_content == 'identical')
    if args.windows_winpe_root_loader:
        directory = 'minint' if minint_directory else 'i386'
        root_hash, directory_hash = source['BoOtMgR'], source[directory + '/setupldr.bin']
        assert actual[directory + '/setupldr.bin'] == directory_hash
        prepatched = args.windows_winpe_root_loader_content == 'prepatched'
        assert (root_hash != directory_hash) == prepatched
        if prepatched or uses_minint:
            assert actual['BoOtMgR'] == root_hash
    serial = (base / 'serial-512.log').read_text(errors='replace')
    report = values(serial.split('RUFUS_WINDOWS_IMAGE_DONE ', 1)[1].splitlines()[0])
    expected_error = errno.EIO if args.windows_winpe_failure else 0
    assert (report['ERROR'], report['OFFSET'], report['SIZE'], report['BYTES']) == (expected_error, offset, size, size), report
    expected_fat = 32 if args.windows_fat32 else 0
    assert report['COPIED'] > 0 and report['FAT'] == expected_fat and report['CLUSTER'] == cluster, report
    if args.windows_winpe_syslinux:
        assert 'RUFUS_WINPE_SYSLINUX_DATA_DONE' in serial, 'Missing no-installer/unknown-version audit'
    if args.windows_winpe_grub_data:
        marker = 'RUFUS_WINPE_GRUB4DOS_SBR_DONE' if grub4dos_sbr else ('RUFUS_WINPE_GRUB2_SBR_DONE' if grub2_sbr else 'RUFUS_WINPE_GRUB_DATA_DONE')
        assert marker in serial, 'Missing independent SBR dispatch audit'
    result = {'family': args.windows_winpe, 'uses_minint': uses_minint,
              'partial_minint': args.windows_winpe_partial_minint, 'syslinux_config': args.windows_winpe_syslinux,
              'grub_data': args.windows_winpe_grub_data,
              'grub2_sbr': bool(grub2_sbr), 'grub4dos_sbr': bool(grub4dos_sbr), 'core_sha256': hashlib.sha256(core).hexdigest() if core else None,
              'selected_mbr': args.windows_bios, 'effective_mbr': code,
              'identical_root_detector': args.windows_winpe_root_detector and args.windows_winpe_root_detector_content == 'identical',
              'root_detector_content': args.windows_winpe_root_detector_content if args.windows_winpe_root_detector else None,
              'existing_root_sif': args.windows_winpe_root_sif,
              'identical_root_loader': args.windows_winpe_root_loader and args.windows_winpe_root_loader_content == 'identical',
              'root_loader_content': args.windows_winpe_root_loader_content if args.windows_winpe_root_loader else None,
              'files': actual, 'driver': 'vfat' if args.windows_fat32 else args.windows_ntfs,
              'root_write_failure': args.windows_winpe_failure,
              'error': expected_error, 'one_simulated_authorization': True,
              'helper_exit_before_completion': True, 'sync_cleanup_complete': True,
              'filesystem_clean': True, 'windows_setup_qualified': False}
    if args.windows_fat32:
        result['fsinfo_hints'] = [struct.unpack_from('<II', window, base + SECTOR_BYTES + 488)
                                  for base in (0, 6 * SECTOR_BYTES)]
    if not args.windows_winpe_failure:
        output = base / 'winpe-helper-bios'
        output.mkdir()
        result['bios'] = boot(disk, output, 0x80 if uses_minint else 0x81, None)
    (base / 'winpe-helper-readback.json').write_text(json.dumps(result, indent=2) + '\n')
    return result
