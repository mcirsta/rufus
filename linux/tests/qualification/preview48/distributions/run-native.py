#!/usr/bin/env python3
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
mode, family = sys.argv[1:]
assert mode in ('build', 'qualify', 'restage') and family in ('debian', 'fedora')
work = outputs / family
work.mkdir(exist_ok=True)
podman = ['podman', '--root', str(base / 'containers/storage'), '--runroot', '/tmp/rufus-cp79-containers']
command = podman + ['run', '--rm', '--name', 'rufus-cp79-' + mode + '-' + family,
                    '-v', str(work) + ':/work:rw', '-v', str(outputs) + ':/harness:ro']
if mode in ('build', 'restage'):
    command += ['--network=none', '-v', str(root / 'rufus') + ':/source:ro',
                '-v', str(root / '.rufus-test-artifacts/deps') + ':/upstream:ro',
                'localhost/rufus-cp79-' + family, '/harness/' + ('build-native.sh' if mode == 'build' else 'restage-native.sh'), family]
else:
    images = json.loads((outputs / 'pinned-base-images.json').read_text())
    command += [images[family], '/harness/qualify-native.sh', family]
environment = dict(os.environ, XDG_RUNTIME_DIR='/tmp/rufus-cp79-runtime')
start = time.monotonic()
log = work / (mode + '.log')
with log.open('wb') as output:
    result = subprocess.run(command, cwd=root, env=environment, stdout=output, stderr=subprocess.STDOUT)
record = {'command': command, 'exit_code': result.returncode, 'seconds': time.monotonic() - start,
          'family': family, 'mode': mode, 'log': str(log)}
(work / (mode + '-run.json')).write_text(json.dumps(record, indent=2) + '\n')
print(json.dumps(record, indent=2))
sys.exit(result.returncode)
