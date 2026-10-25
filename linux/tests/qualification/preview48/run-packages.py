import json,os,shutil,subprocess,time
from pathlib import Path
root=Path('/home/marius/proj/lufus');base=root/'.rufus-test-artifacts/cp79';release=root/'.rufus-test-artifacts/cp32/release'
assert os.getuid()!=0
records=[]
def run(command,cwd,log):
    start=time.monotonic()
    with (base/log).open('wb') as output:
        result=subprocess.run(command,cwd=cwd,env=dict(os.environ,PKGDEST=str(root/'build/releases')),stdout=output,stderr=subprocess.STDOUT)
    records.append({'command':command,'cwd':str(cwd),'exit_code':result.returncode,'seconds':time.monotonic()-start,'log':str(base/log)})
    (base/'packages-run.json').write_text(json.dumps(records,indent=2)+'\n')
    assert result.returncode==0,(command,result.returncode)
run(['cmake','--build',str(release)],root,'build-release-final.log')
arch=base/'arch-package';arch.mkdir(exist_ok=True);assert arch.is_dir() and not arch.is_symlink()
shutil.copy2(release/'PKGBUILD',arch/'PKGBUILD')
assert 'preview@' not in (arch/'PKGBUILD').read_text()
run(['cpack','--config',str(release/'CPackConfig.cmake'),'-B',str(root/'build/releases')],root,'package-tgz.log')
run(['makepkg','--force','--noconfirm'],arch,'package-arch.log')
run(['python',str(base/'check-packages.py')],root,'package-check.log')
print('Preview48 native and generic packages built and inspected; no host installation.')
