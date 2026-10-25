#!/usr/bin/env python3
"""Q03 installed-package checks; no block devices or host services are used."""
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import time
import xml.etree.ElementTree as ET

family, phase, revision = sys.argv[1:]
assert revision in ('47', '48')
work = Path('/work')
application = Path('/usr/bin/rufus-linux')
private = Path('/usr/lib/rufus-linux-preview')
binaries = [application] + [private / name for name in (
    'rufus-prepare-helper', 'rufus-syslinux', 'rufus-syslinux4', 'rufus-extlinux')]
assert all(path.is_file() and os.access(path, os.X_OK) for path in binaries)
assert set(private.iterdir()) == set(binaries[1:])
policy = Path('/usr/share/polkit-1/actions/org.rufus.PrepareDevice.policy')
actions = {action.attrib['id']: action for action in ET.parse(policy).iter('action')}
assert set(actions) == {'org.rufus.PrepareDevice', 'org.rufus.SaveDevice'}
for identifier, argument in (('org.rufus.PrepareDevice', '--device'),
                             ('org.rufus.SaveDevice', '--save-device')):
    values = {entry.attrib['key']: entry.text for entry in actions[identifier].iter('annotate')}
    assert values['org.freedesktop.policykit.exec.path'] == str(private / 'rufus-prepare-helper')
    assert values['org.freedesktop.policykit.exec.argv1'] == argument
for binary in binaries:
    libraries = subprocess.check_output(['ldd', str(binary)], text=True)
    assert 'not found' not in libraries, libraries
    assert not subprocess.check_output(['patchelf', '--print-rpath', str(binary)], text=True).strip()
subprocess.run(['desktop-file-validate', '/usr/share/applications/org.rufus.Rufus.desktop'], check=True)
documentation = Path('/usr/share/doc/rufus-linux-preview')
assert 'Rufus Linux preview ' + revision in (documentation / 'README.md').read_text()
assert (documentation / 'linux/packaging/release-evidence.json').is_file()
for name in ('LICENSE.txt', 'LICENSE.img2ffu', 'LICENSE.uefi-md5sum', 'NOTICE.uefi-md5sum'):
    assert (Path('/usr/share/licenses/rufus-linux-preview') / name).stat().st_size > 0
for size in (16, 24, 32, 48, 64, 128, 256, 512):
    assert Path('/usr/share/icons/hicolor/' + str(size) + 'x' + str(size) + '/apps/org.rufus.Rufus.png').is_file()
installer_versions = {}
for name, expected in (('rufus-syslinux', '6.04'), ('rufus-syslinux4', '4.07'), ('rufus-extlinux', '6.04')):
    result = subprocess.run([str(private / name), '--version'], capture_output=True, text=True, timeout=5)
    version_text = result.stdout + result.stderr
    assert result.returncode == 0 and expected in version_text, (name, result.returncode, version_text)
    installer_versions[name] = version_text.strip()
environment = dict(os.environ)
for key in ('DISPLAY', 'WAYLAND_DISPLAY', 'QT_QPA_PLATFORM', 'LD_LIBRARY_PATH'):
    environment.pop(key, None)
version = subprocess.check_output([str(application), '--version'], env=environment, text=True).strip()
assert version == 'Rufus Linux preview 4.15.2404-linux-preview.' + revision, version
help_text = subprocess.check_output([str(application), '--help'], env=environment, text=True)
assert 'Create bootable USB drives' in help_text and '--locale <locale>' in help_text
record = {'family': family, 'phase': phase, 'os_release': Path('/etc/os-release').read_text(),
          'version': version, 'executables': len(binaries), 'installer_versions': installer_versions, 'libraries_resolve': True,
          'empty_runtime_rpaths': True, 'policy_matches_helper': True, 'headless_help_version': True}
if phase != 'core':
    providers = {}
    cache = (work / 'configured-provider-paths.txt').read_text()
    for key in ('RUFUS_MKFS_FAT', 'RUFUS_MKNTFS', 'RUFUS_NTFSRESIZE', 'RUFUS_MKE2FS',
                'RUFUS_NTFS_3G', 'RUFUS_MKUDFFS', 'RUFUS_NATIVE_MCOPY', 'RUFUS_NATIVE_MATTRIB'):
        match = re.search(r'^' + key + r':[^=]+=(.*)$', cache, re.M)
        assert match, key
        path = Path(match.group(1))
        assert path.is_file() and os.access(path, os.X_OK), (key, str(path))
        argument = '-V' if key == 'RUFUS_MKE2FS' else '--help'
        command = subprocess.run([str(path), argument], capture_output=True, text=True, timeout=5)
        transcript = command.stdout + command.stderr
        assert transcript.strip(), key
        providers[key] = {'path': str(path), 'exit_code': command.returncode,
                          'version_help_excerpt': transcript[:2000]}
    record['providers'] = providers
    session = Path('/tmp/rufus-package-session')
    runtime = session / ('runtime-' + phase)
    config = session / 'config'
    for path in (session, runtime, config):
        path.mkdir(mode=0o700, exist_ok=True)
        os.chown(path, 1000, 1000)
    ini = config / 'Rufus/Linux.ini'
    prefix = ['runuser', '-u', 'rufustest', '--', 'env', 'QT_QPA_PLATFORM=offscreen',
              'DBUS_SYSTEM_BUS_ADDRESS=unix:path=/tmp/rufus-no-system-bus',
              'XDG_RUNTIME_DIR=' + str(runtime), 'XDG_CONFIG_HOME=' + str(config),
              'XDG_DATA_HOME=' + str(session / 'data'), 'HOME=/home/rufustest']
    first = subprocess.Popen(prefix + [str(application), '--wait=0', '--locale=fr-FR'],
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True)
    try:
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline and first.poll() is None:
            if ini.is_file() and 'Locale=fr-FR' in ini.read_text() and (runtime / 'rufus-linux.lock').is_file():
                break
            time.sleep(0.05)
        assert first.poll() is None, first.communicate()
        assert ini.is_file() and 'Locale=fr-FR' in ini.read_text()
        assert (runtime / 'rufus-linux.lock').is_file()
        second = subprocess.Popen(prefix + [str(application), '--wait=0'],
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True)
        try:
            try:
                _, error = second.communicate(timeout=0.6)
            except subprocess.TimeoutExpired as failure:
                error = failure.stderr or b''
            assert b'Another Rufus instance is running.' in error, error
        finally:
            if second.poll() is None:
                os.killpg(second.pid, signal.SIGTERM)
                second.communicate(timeout=5)
    finally:
        if first.poll() is None:
            os.killpg(first.pid, signal.SIGTERM)
            first.communicate(timeout=5)
    assert ini.is_file() and 'Locale=fr-FR' in ini.read_text()
    if phase == 'fresh':
        with ini.open('a') as output:
            output.write('\n[PackageQualification]\nPreserve=cp79-user-state\n')
    else:
        assert 'Preserve=cp79-user-state' in ini.read_text()
    record.update({'unprivileged_offscreen_gui': True, 'saved_locale': True,
                   'second_instance_refused': True, 'user_state_sha256':
                   hashlib.sha256(ini.read_bytes()).hexdigest()})
(work / ('installed-' + phase + '.json')).write_text(json.dumps(record, indent=2) + '\n')
print(json.dumps(record, indent=2))
