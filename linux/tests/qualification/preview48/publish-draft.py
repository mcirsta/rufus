from pathlib import Path
import json
import subprocess
import time
root=Path('/home/marius/proj/lufus')
base=root/'.rufus-test-artifacts/cp79'
out=root/'build/releases'
manifest=json.loads((out/'rufus-linux-preview.48-package-manifest.json').read_text())
assert manifest['tag']=='linux-preview.48'
assets=[out/row['name'] for row in manifest['assets']]+[out/'rufus-linux-preview.48-package-manifest.json',out/'SHA256SUMS']
assert len(assets)==9 and all(path.is_file() for path in assets)
command=['gh','release','create','linux-preview.48','--repo','mcirsta/rufus','--verify-tag','--draft','--prerelease','--title','Rufus Linux preview 48','--notes-file',str(out/'rufus-linux-preview.48-notes.md')]+[str(path) for path in assets]
start=time.monotonic()
run=subprocess.run(command,cwd=root/'rufus',capture_output=True,text=True)
(base/'github-draft-create.json').write_text(json.dumps({'command':command,'exit_code':run.returncode,'seconds':time.monotonic()-start,'stdout':run.stdout,'stderr':run.stderr},indent=2)+'\n')
print(run.stdout,end='')
print(run.stderr,end='')
raise SystemExit(run.returncode)
