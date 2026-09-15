#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fetch the two pinned OpenWrt raw images used by the optional USB guest test."""
import argparse
import gzip
import hashlib
from pathlib import Path
import shutil
import subprocess

VERSION = '25.12.5'
URL = 'https://downloads.openwrt.org/releases/25.12.5/targets/x86/64/'
# Published at URL + sha256sums. Gzip is fixture preparation, not an app feature.
IMAGES = {
    'bios': ('openwrt-25.12.5-x86-64-generic-ext4-combined.img.gz',
             '23e2538e8ab0eb52dfed1c65d608ecdb71ffd432dd54885da138ae67cd9e4461'),
    'uefi': ('openwrt-25.12.5-x86-64-generic-ext4-combined-efi.img.gz',
             'c8ee59ce7b0f635a6b50c1b7307b07ee7785214a6faf2af42280dd4ef4310290'),
}


def digest(path):
    with path.open('rb') as file:
        return hashlib.file_digest(file, 'sha256').hexdigest()


def unpack(directory, output):
    records = []
    for firmware, (name, expected) in IMAGES.items():
        archive = directory / name
        if not archive.is_file() or digest(archive) != expected:
            raise RuntimeError('Missing or changed pinned OpenWrt archive: ' + str(archive))
        target = output / ('openwrt-' + firmware + '.img')
        with gzip.open(archive, 'rb') as source, target.open('xb') as dest:
            shutil.copyfileobj(source, dest, 1024 * 1024)
        records.append({'file': target.name, 'sha256': digest(target), 'size': target.stat().st_size,
                        'firmware': firmware, 'version': VERSION, 'url': URL + name,
                        'archive_sha256': expected})
    return records


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    args.directory.mkdir(parents=True, exist_ok=True)
    for name, expected in IMAGES.values():
        path = args.directory / name
        if not path.exists():
            partial = path.with_suffix('.part')
            subprocess.run(['curl', '--fail', '--location', '--proto', '=https', '--proto-redir', '=https',
                            '--max-time', '90', '--max-filesize', '20000000', '--output', str(partial),
                            URL + name], check=True)
            if digest(partial) != expected:
                raise RuntimeError('OpenWrt download hash mismatch: ' + name)
            partial.rename(path)
        if digest(path) != expected:
            raise RuntimeError('OpenWrt archive hash mismatch: ' + name)
        print('Verified:', name)


if __name__ == '__main__':
    main()
