#!/usr/bin/env python3
"""Inspect the two locally generated release packages without installing them."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import xml.etree.ElementTree as ET

root = Path('/home/marius/proj/lufus')
packages = sorted((root / 'build/releases').glob('rufus-linux-preview-*-linux-preview.48-*.tar.gz'))
packages += sorted((root / 'build/releases').glob('rufus-linux-preview-*.preview48-1-*.pkg.tar.zst'))
assert len(packages) == 2
results = {}
for package in packages:
    with tempfile.TemporaryDirectory(prefix='rufus-preview-check-', dir='/tmp') as temporary:
        stage = Path(temporary)
        subprocess.run(['bsdtar', '-xf', str(package), '-C', str(stage)], check=True)
        applications = list(stage.glob('usr/bin/rufus-linux')) + list(stage.glob('*/usr/bin/rufus-linux'))
        assert len(applications) == 1
        application = applications[0]
        usr = application.parents[1]
        private = usr / 'lib/rufus-linux-preview'
        expected = [application, private / 'rufus-prepare-helper', private / 'rufus-syslinux',
                    private / 'rufus-syslinux4', private / 'rufus-extlinux']
        assert all(path.is_file() and os.access(path, os.X_OK) for path in expected)
        binaries = set((usr / 'bin').iterdir()) | set(private.iterdir())
        assert binaries == set(expected), binaries
        policy = usr / 'share/polkit-1/actions/org.rufus.PrepareDevice.policy'
        actions = {entry.attrib['id']: entry for entry in ET.parse(policy).iter('action')}
        assert set(actions) == {'org.rufus.PrepareDevice', 'org.rufus.SaveDevice'}
        for action, argument in (('org.rufus.PrepareDevice', '--device'), ('org.rufus.SaveDevice', '--save-device')):
            annotations = {entry.attrib['key']: entry.text for entry in actions[action].iter('annotate')}
            assert annotations['org.freedesktop.policykit.exec.path'] == '/usr/lib/rufus-linux-preview/rufus-prepare-helper'
            assert annotations['org.freedesktop.policykit.exec.argv1'] == argument
        assert (usr / 'share/licenses/rufus-linux-preview/LICENSE.txt').is_file()
        assert (usr / 'share/licenses/rufus-linux-preview/LICENSE.img2ffu').is_file()
        for name in ('LICENSE.uefi-md5sum', 'NOTICE.uefi-md5sum'):
            installed = usr / 'share/licenses/rufus-linux-preview' / name
            assert installed.read_bytes() == (root / 'rufus/linux/packaging' / name).read_bytes()
        documentation = usr / 'share/doc/rufus-linux-preview'
        assert (documentation / 'docs/linux-support.md').is_file()
        assert (documentation / 'docs/linux-release-plan.md').is_file()
        assert (documentation / 'docs/linux-desktop-controls.md').read_bytes() == (root / 'rufus/docs/linux-desktop-controls.md').read_bytes()
        assert (documentation / 'docs/linux-device-source-operations.md').read_bytes() == (root / 'rufus/docs/linux-device-source-operations.md').read_bytes()
        assert (documentation / 'docs/linux-iso-efi-resources.md').read_bytes() == (root / 'rufus/docs/linux-iso-efi-resources.md').read_bytes()
        assert (documentation / 'docs/linux-legacy-iso-routes.md').read_bytes() == (root / 'rufus/docs/linux-legacy-iso-routes.md').read_bytes()
        assert (documentation / 'docs/linux-sector-geometry.md').read_bytes() == (root / 'rufus/docs/linux-sector-geometry.md').read_bytes()
        for name in ('linux-support.md', 'linux-device-discovery.md', 'linux-parity-register.json'):
            assert (documentation / 'docs' / name).read_bytes() == (root / 'rufus/docs' / name).read_bytes()
        assert 'Rufus Linux preview 48' in (documentation / 'README.md').read_text()
        assert '[Read-only]' in (documentation / 'docs/linux-support.md').read_text()
        assert 'UDF-only' in (documentation / 'docs/linux-support.md').read_text()
        assert 'Current GUI through **CP78' in (documentation / 'docs/linux-support.md').read_text()
        assert 'Plain XPRESS-compressed single-store capture' in (documentation / 'docs/linux-support.md').read_text()
        assert 'CP49 — extended label/icon files' in (documentation / 'docs/linux-support.md').read_text()
        assert 'Alt+M' in (documentation / 'docs/linux-support.md').read_text()
        assert 'Ctrl+Alt+Z' in (documentation / 'docs/linux-support.md').read_text()
        assert 'partial FFU' in (documentation / 'docs/linux-feature-parity.md').read_text()
        assert 'Persistent partition size' in (documentation / 'docs/linux-support.md').read_text()
        if package.name.endswith('.pkg.tar.zst'):
            assert 'depend = e2fsprogs' in (stage / '.PKGINFO').read_text()
            assert 'optdepend = udftools: UDF data formatting' in (stage / '.PKGINFO').read_text()
        ntfs_guide = documentation / 'docs/linux-ntfs-options.md'
        assert ntfs_guide.read_bytes() == (root / 'rufus/docs/linux-ntfs-options.md').read_bytes()
        assert 'Alt+N' in ntfs_guide.read_text() and 'Alt+Q' in ntfs_guide.read_text()
        if package.name.endswith('.pkg.tar.zst'):
            assert 'ntfs-3g: compressed or non-512-byte NTFS creation' in (stage / '.PKGINFO').read_text()
        assert 'CP64' in (documentation / 'docs/testing-and-checkpoints.md').read_text()
        assert (documentation / 'docs/linux-filesystem-formatting.md').is_file()
        assert (documentation / 'docs/linux-filesystem-metadata-policy.md').read_bytes() == (root / 'rufus/docs/linux-filesystem-metadata-policy.md').read_bytes()
        assert 'not implemented' in (documentation / 'docs/linux-filesystem-metadata-policy.md').read_text()
        assert 'runtime validation' in (documentation / 'docs/linux-runtime-validation.md').read_text().lower()
        guide = (documentation / 'docs/linux-secure-boot.md').read_text()
        assert 'START' in guide and 'remote policy refresh' in guide and 'CP62' in guide and 'CP63' in guide
        policy_guide = (documentation / 'docs/linux-policy-updates.md').read_text()
        assert 'CP63' in policy_guide and 'atomic' in policy_guide and 'Check now' in policy_guide
        legacy = (documentation / 'docs/linux-legacy-boot.md').read_text()
        assert 'CP64' in legacy and 'MS-DOS' in legacy and 'ReactOS' in legacy and 'Creation time' in legacy
        discovery = (documentation / 'docs/linux-device-discovery.md').read_text()
        assert 'CP71' in discovery and 'Ctrl+Alt+F' in discovery and 'VMware' in discovery
        assert 'CP65' in discovery and 'Btrfs' in discovery and '4096' in discovery
        assert 'Multiple Volumes' in discovery and '64 KiB' in discovery
        downloads = (documentation / 'docs/linux-image-downloads.md').read_text()
        assert 'CP66' in downloads and 'Fido v1.71' in downloads and 'Ctrl+SELECT' in downloads
        assert '4 GiB' in downloads
        assert 'native NTFS' in downloads and 'recovery' in downloads
        assert not list(usr.rglob('diskcopy.dll'))
        assert 'Standalone MS-DOS and ReactOS' in (documentation / 'docs/linux-support.md').read_text()
        assert 'qualification in progress' not in guide
        assert 'Secure Boot issuer and revocation checks' in (documentation / 'docs/linux-support.md').read_text()
        assert 'UDF and advanced ext2/ext3' in (documentation / 'README.md').read_text()
        assert (documentation / 'docs/linux-ffu-parity.md').is_file()
        assert (documentation / 'docs/linux-supplementary-zip.md').is_file()
        assert (documentation / 'docs/linux-windows-to-go.md').is_file()
        assert 'Windows To Go and standalone WIM/ESD' in (documentation / 'docs/linux-support.md').read_text()
        if package.name.endswith('.pkg.tar.zst'):
            assert 'depend = ntfsprogs-plus' in (stage / '.PKGINFO').read_text()
            assert 'depend = hivex' in (stage / '.PKGINFO').read_text()
        assert (documentation / 'docs/linux-desktop-settings.md').is_file()
        assert 'Ctrl+P' in (documentation / 'docs/linux-desktop-settings.md').read_text()
        assert 'Ctrl+SELECT' in (documentation / 'docs/linux-support.md').read_text()
        assert 'XPRESS-compressed FFU' in (documentation / 'README.md').read_text()
        assert 'Optimized FFU deployment (CP55)' in (documentation / 'docs/linux-support.md').read_text()
        assert 'FFU catalog signatures (CP54)' in (documentation / 'docs/linux-support.md').read_text()
        assert 'FFU store selection (CP53)' in (documentation / 'docs/linux-support.md').read_text()
        desktop = usr / 'share/applications/org.rufus.Rufus.desktop'
        subprocess.run(['desktop-file-validate', str(desktop)], check=True)
        env = dict(os.environ)
        for name in ('DISPLAY', 'WAYLAND_DISPLAY', 'QT_QPA_PLATFORM', 'LD_LIBRARY_PATH'):
            env.pop(name, None)
        version = subprocess.check_output([str(application), '--version'], env=env, text=True).strip()
        assert version == 'Rufus Linux preview 4.15.2404-linux-preview.48', version
        help_text = subprocess.check_output([str(application), '--help'], env=env, text=True)
        assert 'Create bootable USB drives' in help_text
        assert '--iso <path>' in help_text and '[image]' in help_text
        assert '--extra-devs' in help_text and '--portable' in help_text
        assert '--filesystem <filesystem>' in help_text and '--wait <timeout>' in help_text
        assert '--locale <locale>' in help_text
        assert '38' in (documentation / 'docs/linux-localization.md').read_text()
        assert (documentation / 'linux/packaging/release-evidence.json').read_bytes() == (root / 'rufus/linux/packaging/release-evidence.json').read_bytes()
        assert (documentation / 'docs/linux-release-evidence.md').read_bytes() == (root / 'rufus/docs/linux-release-evidence.md').read_bytes()
        assert 'mcirsta/rufus' in (documentation / 'docs/linux-application-updates.md').read_text()
        assert 'Exec=rufus-linux %f' in desktop.read_text()
        for binary in expected:
            assert not subprocess.check_output(['patchelf', '--print-rpath', str(binary)], text=True).strip(), binary
            dependencies = subprocess.check_output(['ldd', str(binary)], text=True)
            assert 'not found' not in dependencies, dependencies
        gui_launch = False
        portable_startup = False
        instance_refusal = False
        if package.name.endswith('.pkg.tar.zst'):
            import time
            env['QT_QPA_PLATFORM'] = 'offscreen'
            env['DBUS_SYSTEM_BUS_ADDRESS'] = 'unix:path=' + str(stage / 'no-system-bus')
            env['XDG_CONFIG_HOME'] = str(stage / 'settings')
            env['XDG_DATA_HOME'] = str(stage / 'data')
            temporary_logs = stage / 'temporary-logs'
            temporary_logs.mkdir(mode=0o700)
            env['TMPDIR'] = str(temporary_logs)
            runtime = stage / 'runtime'
            runtime.mkdir(mode=0o700)
            env['XDG_RUNTIME_DIR'] = str(runtime)
            command = [str(application), '--portable', '-x', '-g', '--filesystem', 'NTFS', '--wait=0', '--locale=fr-FR']
            first = subprocess.Popen(command, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            try:
                deadline = time.monotonic() + 5
                lock = runtime / 'rufus-linux.lock'
                ini = application.parent / 'rufus.ini'
                while time.monotonic() < deadline and first.poll() is None:
                    if lock.exists() and ini.exists():
                        break
                    time.sleep(0.05)
                assert first.poll() is None, first.communicate()
                assert lock.is_file() and ini.is_file()
                deadline = time.monotonic() + 5
                while time.monotonic() < deadline and 'fr-FR' not in ini.read_text():
                    time.sleep(0.05)
                assert 'Locale=fr-FR' in ini.read_text()
                gui_launch = portable_startup = True
                try:
                    second = subprocess.run([str(application), '--wait=0'], env=env, capture_output=True, timeout=0.5)
                except subprocess.TimeoutExpired as error:
                    instance_refusal = b'Another Rufus instance is running.' in (error.stderr or b'')
                else:
                    instance_refusal = b'Another Rufus instance is running.' in second.stderr and second.returncode == 1
                assert instance_refusal
            finally:
                if first.poll() is None:
                    first.terminate()
                first.communicate(timeout=5)
        results[package.name] = {'version': version, 'executables': len(expected),
            'policy_matches_helper': True, 'desktop_valid': True, 'libraries_resolve': True,
            'runtime_rpath_empty': True, 'headless_help_version': True, 'documentation': True,
            'isolated_gui_launch': gui_launch, 'portable_startup': portable_startup,
            'second_instance_refused': instance_refusal,
            'bytes': package.stat().st_size,
            'sha256': hashlib.file_digest(package.open('rb'), 'sha256').hexdigest()}
    assert not stage.exists()
checks = root / '.rufus-test-artifacts/cp79/checks-packages.json'
checks.write_text(json.dumps(results, indent=2) + '\n')
for name, result in results.items():
    (root / 'build/releases' / (name + '.sha256')).write_text(result['sha256'] + '  ' + name + '\n')
print('Both packages verified: five executables, policy, desktop, dependencies, docs and headless CLI; temporary stages removed.')
