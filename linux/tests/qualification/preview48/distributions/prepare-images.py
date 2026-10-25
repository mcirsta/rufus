#!/usr/bin/env python3
"""Prepare private pinned Debian/Fedora dependency images, without source builds."""
import json
import os
from pathlib import Path
import subprocess
import sys
import time

root = Path('/home/marius/proj/lufus')
base = root / '.rufus-test-artifacts/cp79'
outputs = base / 'distributions'
assert os.getuid() != 0
environment = dict(os.environ, XDG_RUNTIME_DIR='/tmp/rufus-cp79-runtime')
podman = ['podman', '--root', str(base / 'containers/storage'), '--runroot', '/tmp/rufus-cp79-containers']
images = json.loads((outputs / 'pinned-base-images.json').read_text())
for family in ('debian', 'fedora'):
    work = outputs / family
    command = podman + ['build', '-f', str(root / 'rufus/linux/packaging' / ('Containerfile.' + family)),
                        '-t', 'localhost/rufus-cp79-' + family, str(root / 'rufus/linux/packaging')]
    start = time.monotonic()
    with (work / 'prepare-image.log').open('wb') as log:
        result = subprocess.run(command, cwd=root, env=environment, stdout=log, stderr=subprocess.STDOUT)
    record = {'family': family, 'command': command, 'exit_code': result.returncode,
              'seconds': time.monotonic() - start, 'pinned_base': images[family]}
    (work / 'prepare-image-run.json').write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps(record), flush=True)
    if result.returncode:
        sys.exit(result.returncode)
    for image, destination in ((images[family], 'base-image.json'),
                               ('localhost/rufus-cp79-' + family, 'build-image.json')):
        manifest = subprocess.check_output(podman + ['image', 'inspect', image], env=environment, text=True)
        (work / destination).write_text(manifest)
