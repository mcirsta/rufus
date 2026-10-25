#!/usr/bin/env python3
"""Record package payloads and prove documentation restaging keeps qualified code."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile

base = Path('/home/marius/proj/lufus/.rufus-test-artifacts/cp79/distributions')
phase = sys.argv[1]
assert phase in ('qualified', 'final')
results = {}
for family, suffix in (('debian', '.deb'), ('fedora', '.rpm')):
    package = next((base / family / 'packages').glob('*' + suffix))
    with tempfile.TemporaryDirectory(prefix='rufus-cp79-payload-', dir='/tmp') as temporary:
        stage = Path(temporary)
        if family == 'debian':
            members = subprocess.check_output(['bsdtar', '-tf', str(package)], text=True).splitlines()
            member = next(name for name in members if name.startswith('data.tar.'))
            archive = subprocess.check_output(['bsdtar', '-xOf', str(package), member])
            subprocess.run(['bsdtar', '-xf', '-', '-C', str(stage)], input=archive, check=True)
        else:
            subprocess.run(['bsdtar', '-xf', str(package), '-C', str(stage)], check=True)
        paths = ['usr/bin/rufus-linux'] + ['usr/lib/rufus-linux-preview/' + name for name in (
            'rufus-prepare-helper', 'rufus-syslinux', 'rufus-syslinux4', 'rufus-extlinux')]
        binaries = {}
        for name in paths:
            file = stage / name
            assert file.is_file()
            with file.open('rb') as source:
                binaries[name] = hashlib.file_digest(source, 'sha256').hexdigest()
        with package.open('rb') as source:
            digest = hashlib.file_digest(source, 'sha256').hexdigest()
        metadata = {}
        for name in ('usr/share/polkit-1/actions/org.rufus.PrepareDevice.policy',
                     'usr/share/applications/org.rufus.Rufus.desktop',
                     'usr/share/doc/rufus-linux-preview/README.md',
                     'usr/share/doc/rufus-linux-preview/linux/README.md',
                     'usr/share/doc/rufus-linux-preview/linux/packaging/README.md',
                     'usr/share/doc/rufus-linux-preview/linux/packaging/release-evidence.json'):
            file = stage / name
            assert file.is_file(), name
            metadata[name] = hashlib.sha256(file.read_bytes()).hexdigest()
        results[family] = {'package': package.name, 'sha256': digest,
                           'bytes': package.stat().st_size, 'binaries': binaries,
                           'metadata': metadata}
if phase == 'final':
    qualified = json.loads((base / 'package-payloads-qualified.json').read_text())
    for family in results:
        assert results[family]['binaries'] == qualified[family]['binaries'], family
        results[family]['qualified_code_identical'] = True
(base / ('package-payloads-' + phase + '.json')).write_text(json.dumps(results, indent=2) + '\n')
print(json.dumps(results, indent=2))
