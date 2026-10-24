# SPDX-License-Identifier: GPL-3.0-or-later
"""Independent DOS-resource readback and a bare ReactOS boot-record handoff."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile

import prepare
from boot_linux import firmware_options
from fat_extract import digest, run

RESOURCE_SHA256 = '95fc0786f5bc0a6db5c0604b31ac18fbed0502a2c6858e5fb02a647983ae03c7'

def stage_fixture(args, base, init):
    source = args.msdos_resource.read_bytes()
    assert len(source)==0x16ee00 and hashlib.sha256(source).hexdigest()==RESOURCE_SHA256
    prepare.write(init,'etc/rufus-legacy-boot-fixture','legacy-boot\n')
    prepare.write(init,'etc/rufus-format-helper-fixture','format-helper\n')
    (init/'tmp').mkdir(mode=0o1777,exist_ok=True); (init/'tmp').chmod(0o1777)
    shutil.copy2(args.msdos_resource,init/'diskcopy.dll')
    floppy = base/'reference-floppy.img'; floppy.write_bytes(source[0x66d8:0x66d8+0x168000])
    manifest={}
    names=['MSDOS.SYS','COMMAND.COM','IO.SYS','MODE.COM','KEYB.COM','KEYBOARD.SYS','KEYBRD2.SYS','KEYBRD3.SYS','KEYBRD4.SYS','DISPLAY.SYS','EGA.CPI','EGA2.CPI','EGA3.CPI']
    try:
        with tempfile.TemporaryDirectory(prefix='reference-',dir=base) as temporary:
            for index,name in enumerate(names):
                target=Path(temporary)/name
                run(['mcopy','-i',str(floppy),'::/'+name,str(target)],capture_output=True)
                data=bytearray(target.read_bytes())
                if name=='COMMAND.COM': assert data[0x6510]==0x75; data[0x6510]=0xeb
                if name=='IO.SYS': assert data[0x3aa]==0x75; data[0x3aa]=0xeb
                path=name if index<3 else 'LOCALE/'+name
                manifest[path]={'size':len(data),'sha256':hashlib.sha256(data).hexdigest(),'attributes':39 if name in ('IO.SYS','MSDOS.SYS') else 32}
    finally:
        floppy.unlink()
    prepare.write(init,'legacy-files.json',json.dumps(manifest,indent=2)+'\n')
    (base/'legacy-files.json').write_text(json.dumps(manifest,indent=2)+'\n')

def qualify_boot(args,base,disk):
    output=base/'legacy-bios'; output.mkdir()
    loader=output/'FREELDR.SYS'
    run(['nasm','-f','bin',str(prepare.HERE/'legacy_freeldr.asm'),'-o',str(loader)],capture_output=True)
    with disk.open('rb') as stream:
        stream.seek(446+8); offset=struct.unpack('<I',stream.read(4))[0]*512
    run(['mcopy','-i',str(disk)+'@@'+str(offset),str(loader),'::/FREELDR.SYS'],capture_output=True)
    expected=digest(disk)
    command=['qemu-system-x86_64','-accel',args.accel,'-cpu','max','-m','64','-display','none','-monitor','none','-serial','file:'+str(output/'serial.log'),'-no-reboot','-nic','none','-snapshot','-device','isa-debug-exit,iobase=0xf4,iosize=0x04','-drive','if=none,id=stick,format=raw,file='+str(disk),'-device','qemu-xhci,id=xhci','-device','usb-storage,drive=stick,removable=on,bootindex=1']
    command+=firmware_options(output,'bios','legacy')
    (output/'qemu-command.json').write_text(json.dumps(command,indent=2)+'\n')
    try:
        with (output/'qemu.log').open('wb') as log:
            result=subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,timeout=60,env=dict(os.environ,TMPDIR=str(output)))
        serial=(output/'serial.log').read_text(errors='replace')
        assert result.returncode==33 and 'RUFUS_REACTOS_BIOS_OK drive=80 tail=ok' in serial,(result.returncode,serial)
        assert digest(disk)==expected,'Snapshot boot changed the verified media'
    finally:
        for path in output.glob('*.fd'):path.unlink()
    return {'original_mbr_pbr':True,'bare_multicluster_loader':True,'backing_media_unchanged':True,'microsoft_or_reactos_application_executed':False}
