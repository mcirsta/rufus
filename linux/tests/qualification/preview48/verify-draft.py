from pathlib import Path
import hashlib,json,subprocess
root=Path('/home/marius/proj/lufus'); base=root/'.rufus-test-artifacts/cp79'; out=root/'build/releases'
release=json.loads((base/'draft-release.json').read_text())
assert release['draft'] and release['prerelease'] and release['tag_name']=='linux-preview.48'
assert len(release['assets'])==9
expected=json.loads((base/'publication-preparation.json').read_text())['assets']
remote={row['name']:row for row in release['assets']}
assert set(remote)==set(expected)
verified=[]
for name in expected:
    path=out/name
    with path.open('rb') as stream: digest=hashlib.file_digest(stream,'sha256').hexdigest()
    asset=remote[name]
    assert asset['state']=='uploaded' and asset['size']==path.stat().st_size
    assert asset['digest']=='sha256:'+digest,(name,asset.get('digest'))
    verified.append({'name':name,'bytes':asset['size'],'sha256':digest,'github_digest_matches':True})
assert subprocess.check_output(['git','status','--porcelain'],cwd=root/'rufus')==b''
assert subprocess.check_output(['git','rev-parse','linux-preview.47^{}'],cwd=root/'rufus',text=True).strip()=='a598716360d2050a0d40399552916d1c0c09c095'
assert subprocess.check_output(['git','rev-parse','linux-preview.48^{}'],cwd=root/'rufus',text=True).strip()=='a09b9d7d8c365b6c6ccce87763c172c028a5aeaa'
receipt={'draft_release_id':release['id'],'tag':release['tag_name'],'commit':'a09b9d7d8c365b6c6ccce87763c172c028a5aeaa','verified_assets':verified,'prerelease':True,'source_clean':True,'preview47_unchanged':True}
(base/'draft-assets-verification.json').write_text(json.dumps(receipt,indent=2)+'\n')
request={'tag_name':'linux-preview.48','target_commitish':receipt['commit'],'draft':False,'prerelease':True,'make_latest':'false'}
(base/'publish-request.json').write_text(json.dumps(request,indent=2)+'\n')
print('All nine uploaded draft assets match local SHA256 and sizes; source clean and tags verified. Release ID:',release['id'])
