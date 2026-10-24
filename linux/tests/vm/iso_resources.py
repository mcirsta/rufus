# SPDX-License-Identifier: GPL-3.0-or-later
"""One B3 source corpus, four native jobs and shared BIOS/EFI handoffs."""
import hashlib,json,os,re,shutil,subprocess,time
from pathlib import Path

def stage_fixture(args,base,init):
    from prepare import write
    repo=Path(__file__).resolve().parents[3]
    fixtures=repo/'linux/tests/fixtures/download'
    efi=(args.build/'uefi-handoff.efi').read_bytes()
    (init/'original.efi').write_bytes(efi)
    (init/'usr/bin/sh').symlink_to('bash')
    flags=(args.build/'build.ninja').read_text()
    for definition,name in [('RUFUS_NATIVE_SYSLINUX_PATH','rufus-syslinux'),('RUFUS_NATIVE_SYSLINUX4_PATH','rufus-syslinux4'),('RUFUS_NATIVE_EXTLINUX_PATH','rufus-extlinux')]:
        path=re.search(definition+r'=\\"([^"\\]+)',flags)[1]
        target=init/path.lstrip('/');target.parent.mkdir(parents=True,exist_ok=True)
        if target != init/'usr/bin'/name:target.symlink_to('/usr/bin/'+name)

    manifest=[]
    for number,version in enumerate(('5.10','6.03','3.86','4.07')):
        tree=base/('resources-source-'+str(number));tree.mkdir()
        files={'data/file.txt':b'B3 source-only resolved payload\n',
            'isolinux/isolinux.cfg':b'SERIAL 0 115200\nSAY RUFUS_B3_SYSLINUX_CONFIG_OK\nPROMPT 1\nTIMEOUT 600\n',
            'isolinux/syslnx64.cfg':b'PROMPT 1\n',
            'isolinux/isolinux.bin':bytes(64)+b'ISOLINUX '+version.encode()+b' '+bytes(1024),
            'isolinux/menu.c32':b'o'*53500,'isolinux/vesamenu.c32':b'o'*148000}
        if number < 2:files['isolinux/ldlinux.c32']=(fixtures/('syslinux-'+version)/'ldlinux.c32').read_bytes()
        fat=base/('resources-efi-'+str(number)+'.fat')
        subprocess.run(['mkfs.fat','-F','16','-C',str(fat),'16384'],check=True,capture_output=True)
        subprocess.run(['mmd','-i',str(fat),'::EFI','::EFI/BOOT'],check=True,capture_output=True)
        subprocess.run(['mcopy','-i',str(fat),str(init/'original.efi'),'::EFI/BOOT/BOOTX64.EFI'],check=True,capture_output=True)
        files['efiboot.img']=fat.read_bytes()
        for path,data in files.items():
            target=tree/path;target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(data)
        for path,target in [('notes-link','data/file.txt'),('dir-link','data'),('syslinux','isolinux')]:
            (tree/path).symlink_to(target)
        (tree/'md5sum.txt').write_text(''.join(hashlib.md5(data).hexdigest()+'  ./'+path+'\n' for path,data in files.items())
            +hashlib.md5(files['data/file.txt']).hexdigest()+'  ./notes-link\n')
        output=init/('resources-'+str(number)+'.iso')
        subprocess.run([str(args.mkisofs),'-quiet','-R','-J','-V','B3SOURCE','-o',str(output),str(tree)],check=True,capture_output=True)
        cache=init/'b3-cache'
        if number<2:
            for name in ('ldlinux.sys','ldlinux.bss'):
                path=cache/('syslinux-'+version)/name;path.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(fixtures/('syslinux-'+version)/name,path)
        else:
            for name in ('menu.c32','vesamenu.c32'):
                path=cache/'syslinux-4.07'/name;path.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(fixtures/'syslinux-4.07'/name,path)
        manifest.append({'job':number,'source_version':version,'iso_sha256':hashlib.sha256(output.read_bytes()).hexdigest()})
        fat.unlink()
    for name in ('menu.c32','vesamenu.c32'):(init/name).write_bytes((fixtures/'syslinux-4.07'/name).read_bytes())
    write(init,'etc/rufus-iso-resources-fixture','iso-resources\n')
    write(init,'etc/rufus-format-helper-fixture','format-helper\n')
    (init/'tmp').mkdir(mode=0o1777,exist_ok=True);(init/'tmp').chmod(0o1777)
    (base/'source-corpus.json').write_text(json.dumps(manifest,indent=2)+'\n')

def boot_bios(args,base,disk):
    from boot_linux import firmware_options
    from fat_extract import digest
    output=base/'resources-bios';output.mkdir();before=digest(disk);serial=output/'serial.log'
    command=['qemu-system-x86_64','-accel',args.accel,'-m','128','-display','none','-monitor','none','-serial','file:'+str(serial),'-nic','none','-no-reboot','-snapshot','-device','qemu-xhci,id=xhci','-drive','if=none,id=stick,format=raw,file='+str(disk),'-device','usb-storage,drive=stick,removable=on,bootindex=1']
    command+=firmware_options(output,'bios','syslinux')
    (output/'qemu-command.json').write_text(json.dumps(command,indent=2)+'\n')
    with (output/'qemu.log').open('wb') as log:
        process=subprocess.Popen(command,stdout=log,stderr=subprocess.STDOUT,env=dict(os.environ,TMPDIR=str(output)))
        try:
            deadline=time.monotonic()+60
            while time.monotonic()<deadline and process.poll() is None:
                if serial.exists() and b'RUFUS_B3_SYSLINUX_CONFIG_OK' in serial.read_bytes():break
                time.sleep(.1)
            assert serial.exists() and b'RUFUS_B3_SYSLINUX_CONFIG_OK' in serial.read_bytes(),serial.read_text(errors='replace') if serial.exists() else 'no serial'
        finally:
            if process.poll() is None:process.terminate()
            process.wait(timeout=10)
            for path in output.glob('*.fd'):path.unlink()
    assert digest(disk)==before
    return {'bios_original_syslinux4_to_source_config':True,'efi_embedded_to_original_uefi_ntfs':True,'backing_unchanged':True}
