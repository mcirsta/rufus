#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Synthetic native To Go sources for one combined, disposable Linux guest."""
import json
import shutil
import subprocess
from fat_extract import digest
import prepare


def stage_fixture(args, base, init):
    inputs = []
    for kind in ('wim', 'esd'):
        image = base / ('source.' + kind)
        subprocess.run([str(args.build / 'windows_to_go_fixture'), str(image), kind,
                        str(args.build / 'uefi-handoff.efi')], check=True, timeout=60)
        tree = base / (kind + '-tree')
        (tree / 'sources').mkdir(parents=True)
        shutil.copy2(image, tree / ('sources/install.' + kind))
        (tree / 'bootmgr').write_bytes(b'Windows ISO detector fixture')
        (tree / 'setup.exe').write_bytes(b'Windows ISO detector fixture')
        efi = tree / 'efi/boot'; efi.mkdir(parents=True)
        shutil.copy2(args.build / 'uefi-handoff.efi', efi / 'bootx64.efi')
        iso = base / (kind + '.iso')
        subprocess.run([args.mkisofs, '-quiet', '-R', '-J', '-o', str(iso), str(tree)], check=True, timeout=30)
        for source in (image, iso):
            shutil.copy2(source, init / source.name)
            inputs.append({'file': source.name, 'bytes': source.stat().st_size, 'sha256': digest(source)})
        shutil.rmtree(tree)
    (base / 'togo-inputs.json').write_text(json.dumps(inputs, indent=2) + '\n')
    prepare.write(init, 'etc/rufus-windows-togo-fixture', 'windows-togo\n')
