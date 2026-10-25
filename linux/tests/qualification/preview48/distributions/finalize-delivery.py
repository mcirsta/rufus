#!/usr/bin/env python3
"""Export qualified native packages and compact transaction/payload receipts."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

root = Path('/home/marius/proj/lufus')
base = root / '.rufus-test-artifacts/cp79/distributions'
release = root / 'build/releases'
freeze = json.loads((base / 'source-freeze.json').read_text())
assert subprocess.check_output(['git', '-C', str(root/'rufus'), 'rev-parse', 'HEAD'], text=True).strip() == freeze['source_commit']
payloads = json.loads((base / 'package-payloads-final.json').read_text())
checks = {'checkpoint': 'CP79', 'preview': 48, 'source_commit': freeze['source_commit'],
          'scope': 'Native Release packages, fresh installation/removal, published Preview47 to 48 upgrade, installed provider paths and unprivileged runtime; no component, Qt or media suites rerun.',
          'families': {}}
for family in ('debian', 'fedora'):
    work = base / family
    build = json.loads((work/'build-run.json').read_text())
    runtime = json.loads((work/'qualify-run.json').read_text())
    assert build['exit_code'] == runtime['exit_code'] == 0
    package = work / 'packages' / payloads[family]['package']
    digest = hashlib.sha256(package.read_bytes()).hexdigest()
    assert digest == payloads[family]['sha256']
    assert digest in (work/'qualified-package.sha256').read_text()
    assert payloads[family]['qualified_code_identical']
    phases = {}
    for phase, revision in (('core', '48'), ('fresh', '48'), ('previous', '47'), ('upgraded', '48')):
        receipt = work / ('installed-' + phase + '.json')
        record = json.loads(receipt.read_text())
        assert record['version'].endswith('preview.' + revision)
        phases[phase] = {'version': record['version'],
                         'receipt_sha256': hashlib.sha256(receipt.read_bytes()).hexdigest()}
        if phase != 'core':
            assert record['unprivileged_offscreen_gui'] and record['second_instance_refused']
    transaction = (work/'transaction-result.txt').read_text().strip()
    assert 'published Preview47 to Preview48 upgrade' in transaction
    destination = release / package.name
    assert 'preview48' in package.name
    shutil.copy2(package, destination)
    (release/(package.name+'.sha256')).write_text(digest+'  '+package.name+'\n')
    checks['families'][family] = {'package': payloads[family], 'phases': phases,
                                'build_seconds': build['seconds'], 'smoke_seconds': runtime['seconds'],
                                'transaction': transaction,
                                'build_os_release': (work/'build-os-release').read_text(),
                                'runtime_os_release': (work/'runtime-os-release').read_text(),
                                'provider_paths': json.loads((work/'installed-upgraded.json').read_text())['providers']}
(base/'package-checks.json').write_text(json.dumps(checks, indent=2)+'\n')
print(json.dumps({family: {'package': value['package']['package'],
                           'sha256': value['package']['sha256'],
                           'fresh': value['phases']['fresh']['version'],
                           'upgrade': value['phases']['previous']['version']+' -> '+value['phases']['upgraded']['version']}
                  for family, value in checks['families'].items()}, indent=2))
