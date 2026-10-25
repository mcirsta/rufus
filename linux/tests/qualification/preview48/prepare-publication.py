from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path('/home/marius/proj/lufus')
REPO = ROOT / 'rufus'
BASE = ROOT / '.rufus-test-artifacts/cp79'
OUT = ROOT / 'build/releases'
TAG = 'linux-preview.48'
COMMIT = 'a09b9d7d8c365b6c6ccce87763c172c028a5aeaa'
URL = 'https://github.com/mcirsta/rufus/releases/tag/' + TAG
SOURCE_URL = 'https://github.com/mcirsta/rufus/archive/refs/tags/' + TAG + '.tar.gz'

def sha256(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()

def write_json(path, value):
    path.write_text(json.dumps(value, indent=2) + '\n')

assert subprocess.check_output(['git','rev-parse',TAG+'^{}'], cwd=REPO, text=True).strip() == COMMIT
assert subprocess.check_output(['git','status','--porcelain'], cwd=REPO) == b''
for family in ('debian','fedora'):
    for mode in ('build','qualify'):
        receipt = json.loads((BASE/'distributions'/family/(mode+'-run.json')).read_text())
        assert receipt['exit_code'] == 0, (family, mode, receipt)
assert all(row['exit_code'] == 0 for row in json.loads((BASE/'packages-run.json').read_text()))
# Publish the same Debian bytes under the filename GitHub will expose.
original_deb = OUT/'rufus-linux-preview_4.15.2404~preview48-1_amd64.deb'
public_deb = OUT/'rufus-linux-preview_4.15.2404.preview48-1_amd64.deb'
shutil.copyfile(original_deb, public_deb)
files = [
    ('rufus-linux-preview-4.15.2404-linux-preview.48-x86_64.tar.gz','generic install tree'),
    ('rufus-linux-preview-4.15.2404.preview48-1-x86_64.pkg.tar.zst','Arch/CachyOS binary package'),
    (public_deb.name,'Debian 13 binary package'),
    ('rufus-linux-preview-4.15.2404-0.preview48.1.x86_64.rpm','Fedora 44 binary package'),
    ('rufus-linux-preview.48-github-source.tar.gz','actual GitHub tag source archive'),
    ('rufus-linux-preview-4.15.2404.preview48-aur.tar.gz','manual AUR submission files'),
]
assets = []
for name, kind in files:
    path = OUT/name
    assert path.is_file() and path.stat().st_size > 0, name
    assets.append({'name':name,'kind':kind,'bytes':path.stat().st_size,'sha256':sha256(path)})
proofs = {}
for name in ('source-provenance.json','release-source-freeze.json','checks-packages.json',
             'distributions/package-checks.json','distributions/package-payloads-final.json',
             'public-source-verification.json','aur/verification.json','aur/bundle.json'):
    path = BASE/name
    assert path.is_file(), name
    proofs[name] = {'artifact':'cp79/'+name,'sha256':sha256(path)}
qualification = {
    'checkpoint':'CP79','status':'qualified_for_publication','tag':TAG,'commit':COMMIT,
    'release_url':URL,'full_windows_parity_complete':False,
    'code_qualification':{
        'ci_url':'https://github.com/mcirsta/rufus/actions/runs/40087395293',
        'tested_commit':'db82f6b5662a63728b61ec68c86783034b77e640',
        'ubuntu_jobs':['22.04','24.04'],'passed_suites_per_job':252,
        'optional_exfat_capability_skips_per_job':1,'failed_suites':0,
        'staged_integration_and_iso_probe':'passed on both jobs',
        'focused_repairs':'Normal and ASan/UBSan repair checks passed, including 54 standalone/ISO GRUB rows per build; receipts retained under cp78/github-ci',
        'release_commit_delta_from_tested_code':'Version 47 to 48, documentation and qualification metadata only',
    },
    'retained_evidence':{
        'source_delta':'Eight existing media files changed plus one new platform header;189 old media files and Qt/catalog/resources unchanged',
        'retained_checkpoint_receipt_hashes_verified':26,
        'cp78_desktop_cases_reused':32,
        'cp78_full_qt_substantive_cases_reused':2217,
        'cp78_localization_update_cases_per_normal_sanitizer_build_reused':168,
        'media_boot_and_geometry':'Reuse earlier qualified paths with focused coverage for the standalone GRUB4DOS selection repair; no new full media matrix',
        'clean_arch_source_build':'CP78 recipe/provider build and runtime evidence reused; Preview48 source and all pinned source checksums freshly validated, without claiming a new clean AUR build',
    },
    'new_release_checks':{
        'arch_generic':'Release build, five executable/helper paths, libraries, desktop/polkit/docs, CLI; isolated unprivileged GUI/locale/single-instance smoke for Arch payload',
        'debian_fedora':'Native Release builds; fresh installation, real published47 to48 upgrade, unprivileged GUI/settings and removal checks',
        'actual_public_source':'All regular members match tagged git archive; required Linux inputs retained',
        'aur':'PKGBUILD/.SRCINFO/LICENSE regenerated from actual public archive; all four pinned sources verified; manual submission by owner',
    },
    'localization':{'original_languages':38,'linux_specific_english_fallback':True,'full_new_native_translations_claimed':False},
    'evidence_receipts':proofs,'payload_assets':assets,
    'limits':[
        'Existing UDF 1.02–2.01 remains; UDF 2.50/2.60, ReFS formatting and Dev Drive recognition are user-deferred.',
        'The actual Windows UDF indexing attribute is unavailable; the optional diagnostic preserves formatting.',
        'Full new native translations, non-x64 To Go and complete i686/aarch64 host qualification remain open.',
        'Physical USB write/immediate-unplug/reconnect/4Kn and standalone Xorg remain unqualified; prior desktop checks used native KDE Wayland/Xwayland.',
        'Container package checks do not qualify desktop polkit agents, kernel filesystem drivers or physical media.',
        'No additional installed-Windows/customization or original Windows execution matrix was performed.',
    ],
    'generated_utc':datetime.now(timezone.utc).isoformat(),
}
qualification_path = OUT/'rufus-linux-preview.48-qualification.json'
write_json(qualification_path, qualification)
assets.append({'name':qualification_path.name,'kind':'qualification and retained evidence summary','bytes':qualification_path.stat().st_size,'sha256':sha256(qualification_path)})
manifest = {'tag':TAG,'commit':COMMIT,'version':'4.15.2404-linux-preview.48',
            'source_url':SOURCE_URL,'source_sha256':sha256(OUT/'rufus-linux-preview.48-github-source.tar.gz'),
            'assets':assets,'notes':['Binary packages require matching system libraries.','GitHub-facing Debian filename uses a period; internal package version retains its tilde.','AUR submission is manual; set the Maintainer identity before submission.']}
manifest_path = OUT/'rufus-linux-preview.48-package-manifest.json'
write_json(manifest_path, manifest)
checks = [(item['name'],item['sha256']) for item in assets]
checks.append((manifest_path.name,sha256(manifest_path)))
(OUT/'SHA256SUMS').write_text(''.join(digest+'  '+name+'\n' for name,digest in checks))
notes = '''# Rufus Linux preview 48

Native Qt6 Linux port based on Rufus 4.15.2404, for x86-64.

## Fixes in this release

- Preserve the selected standalone GRUB4DOS bootloader during device preparation.
- Support older Qt APIs and Linux headers, preferring platform filesystem definitions with guarded fallbacks.
- Complete native Ubuntu 22.04/24.04 CI dependencies and repair shared fixtures and the standalone ISO probe. MinGW CI is disabled.

The existing USB creation, device/image saving, ISO export, Windows installer/To Go, VHD/VHDX and FFU workflows remain available. See the [support guide](https://github.com/mcirsta/rufus/blob/linux-preview.48/docs/linux-support.md) for eligibility and limits. Original Rufus translations cover 38 languages where native messages match; Linux-specific text may remain English.

## Installation

Use the package for your distribution, then run `rufus-linux` as your desktop user. The matching helper requests polkit authorization when needed.

- Arch/CachyOS: `sudo pacman -U ./rufus-linux-preview-4.15.2404.preview48-1-x86_64.pkg.tar.zst`
- Debian 13: `sudo apt install ./rufus-linux-preview_4.15.2404.preview48-1_amd64.deb`
- Fedora 44: `sudo dnf install ./rufus-linux-preview-4.15.2404-0.preview48.1.x86_64.rpm`
- AUR: the attached `*-aur.tar.gz` contains `PKGBUILD`, `.SRCINFO` and `LICENSE` for manual submission. Set the Maintainer name/contact before submitting. [AUR instructions](https://github.com/mcirsta/rufus/blob/linux-preview.48/linux/packaging/aur/README.md).

The generic archive is an install tree requiring compatible system libraries. Verify downloads with `sha256sum -c SHA256SUMS`; no separate release signing key is introduced. Enable beta releases in Settings to discover this preview; updates open the GitHub release page for package-manager installation.

## Qualification and limits

[Green native CI](https://github.com/mcirsta/rufus/actions/runs/40087395293) passes on Ubuntu 22.04 and 24.04: 252 suites plus the separate ISO classification fixture per job, zero failures, one optional exFAT provider-capability skip. Focused normal/sanitizer repair checks and previous media/desktop evidence are reused. All four binary assets are rebuilt; Arch/generic inspection and Debian/Fedora fresh install, real 47→48 upgrade, GUI/settings and removal checks pass. AUR recipe/provider inputs are unchanged; the actual Preview48 source archive and all four pinned source checksums are freshly verified. A second clean AUR build or full media/regression matrix was not repeated.

This is a preview; full Windows feature parity is not claimed. Existing UDF 1.02–2.01 remains supported. UDF 2.50/2.60, ReFS formatting and Dev Drive recognition are deferred. The actual Windows UDF indexing attribute and full new native translations remain unavailable. Non-x64 To Go, complete additional host architectures, physical USB/immediate-unplug/4Kn and standalone Xorg qualification remain open. Container smoke tests do not qualify physical media or a desktop authorization agent. Details and retained evidence are in the attached qualification summary and [evidence index](https://github.com/mcirsta/rufus/blob/linux-preview.48/docs/linux-release-evidence.md).
'''
(OUT/'rufus-linux-preview.48-notes.md').write_text(notes)
write_json(BASE/'publication-preparation.json', {'tag':TAG,'commit':COMMIT,'assets':[item['name'] for item in assets]+[manifest_path.name,'SHA256SUMS'],'checksums':len(checks),'source_tree_clean':True})
print('Preview48 manifest, frozen qualification, notes and checksums prepared; nine upload assets.')
