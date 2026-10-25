from pathlib import Path
import hashlib
import json
import os
import shlex
import subprocess
import time

ROOT=Path('/home/marius/proj/lufus')
BASE=ROOT/'.rufus-test-artifacts/cp79'
BUILD=ROOT/'.rufus-test-artifacts/cp22n71-work/linux'
SOURCE=ROOT/'rufus/linux/tests/test_application_updates.cpp'
assert (BASE/'public-release-listing.json').is_file()
text=SOURCE.read_text()
replacements={
    'QStringLiteral("4.15.2404-linux-preview.47")':'QStringLiteral("4.15.2404-linux-preview.48")',
    'QStringLiteral("https://github.com/mcirsta/rufus/releases/tag/linux-preview.47")':'QStringLiteral("https://github.com/mcirsta/rufus/releases/tag/linux-preview.48")',
}
for before,after in replacements.items():
    assert text.count(before)==1, before
    text=text.replace(before,after)
copy=BASE/'test_application_updates_preview48.cpp'
copy.write_text(text)
commands=(BASE/'application-updates-build-commands.txt').read_text().splitlines()
compile_command=None
link_command=None
original_object='CMakeFiles/test_application_updates.dir/tests/test_application_updates.cpp.o'
object_path=BASE/'test_application_updates_preview48.o'
executable=BASE/'test_public_release48'
for line in commands:
    if ' -c '+str(SOURCE) in line:
        compile_command=shlex.split(line)
    if ' -o test_application_updates ' in line:
        link_command=shlex.split(line)
assert compile_command and link_command
compile_command[compile_command.index('-o')+1]=str(object_path)
compile_command[compile_command.index('-c')+1]=str(copy)
if '-MF' in compile_command:
    compile_command[compile_command.index('-MF')+1]=str(BASE/'test_application_updates_preview48.d')
compile_command+=['-I'+str(SOURCE.parent)]
assert link_command[:2]==[':','&&'] and link_command[-2:]==['&&',':']
link_command=link_command[2:-2]
link_command[link_command.index('-o')+1]=str(executable)
link_command[link_command.index(original_object)]=str(object_path)
for index,part in enumerate(link_command):
    if part.startswith('-Wl,--dependency-file='):
        link_command[index]='-Wl,--dependency-file='+str(BASE/'test_public_release48-link.d')
records=[]
for name,command in (('build-publication-fixture',compile_command),('link-publication-fixture',link_command)):
    start=time.monotonic()
    with (BASE/(name+'.log')).open('wb') as output:
        run=subprocess.run(command,cwd=BUILD,stdout=output,stderr=subprocess.STDOUT)
    records.append({'phase':name,'exit_code':run.returncode,'seconds':time.monotonic()-start})
    assert run.returncode==0, (name,run.returncode)
env=dict(os.environ,QT_QPA_PLATFORM='offscreen',RUFUS_PUBLISHED_RELEASE_INPUT=str(BASE/'public-release-listing.json'))
start=time.monotonic()
with (BASE/'public-release-handoff.log').open('wb') as output:
    run=subprocess.run([str(executable),'publishedRelease'],cwd=BUILD,env=env,stdout=output,stderr=subprocess.STDOUT)
records.append({'phase':'publishedRelease48','exit_code':run.returncode,'seconds':time.monotonic()-start})
log=(BASE/'public-release-handoff.log').read_text()
assert run.returncode==0 and '3 passed, 0 failed, 0 skipped' in log, log
receipt={'checkpoint':'CP79','scope':'One published-release parser/channel/order/page-button slot, using actual anonymous GitHub JSON',
         'production_inputs':'Unchanged CI-qualified normal-cache libraries; no production source modification or full regression rerun',
         'expected_metadata_adapter':'Private copy changes exactly the two opt-in Preview47 expected version/page constants to48; test assertions retained',
         'original_test_sha256':hashlib.sha256(SOURCE.read_bytes()).hexdigest(),
         'private_test_sha256':hashlib.sha256(copy.read_bytes()).hexdigest(),
         'anonymous_listing_sha256':hashlib.sha256((BASE/'public-release-listing.json').read_bytes()).hexdigest(),
         'expected_tag':'linux-preview.48','result':'passed','phases':records}
(BASE/'public-release-handoff.json').write_text(json.dumps(receipt,indent=2)+'\n')
print('Preview48 public release selection, channel filtering, version ordering and explicit page-button handoff passed; no browser opened.')
