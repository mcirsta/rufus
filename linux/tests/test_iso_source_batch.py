# SPDX-License-Identifier: GPL-3.0-or-later
"""B3 retained ISO/FAT source and extraction compatibility corpus."""
import argparse,errno,json,struct,subprocess,tempfile
from pathlib import Path
from test_iso_extract import extract
MIB=1024*1024

def efi_boot_options(mkisofs):
    result=subprocess.run([mkisofs,'-help'],capture_output=True,timeout=10)
    help_text=result.stdout+result.stderr
    if b'-eltorito-platform' in help_text:
        return ['-eltorito-platform','efi','-b','efiboot.img']
    assert b'-efi-boot' in help_text, 'Fixture encoder must expose an EFI El Torito option'
    return ['-e','efiboot.img']

def pe(machine=0x8664,wide=True):
    b=bytearray(2048);b[:2]=b'MZ';struct.pack_into('<I',b,60,64);b[64:68]=b'PE\0\0'
    struct.pack_into('<HH',b,68,machine,1);optional=240 if wide else 224
    struct.pack_into('<H',b,84,optional);struct.pack_into('<H',b,88,0x20b if wide else 0x10b)
    struct.pack_into('<I',b,124,512);struct.pack_into('<I',b,148,512);struct.pack_into('<H',b,156,10)
    struct.pack_into('<I',b,196 if wide else 180,16)
    table=88+optional;b[table:table+5]=b'.text';struct.pack_into('<IIII',b,table+8,512,4096,512,512)
    b[512:1024]=b'T'*512;return bytes(b)

def image(args,root,name,files,links=(),udf=False):
    tree=root/(name+'-tree');tree.mkdir()
    for path,data in files.items():
        file=tree/path;file.parent.mkdir(parents=True,exist_ok=True);file.write_bytes(data)
    for path,target in links:
        file=tree/path;file.parent.mkdir(parents=True,exist_ok=True);file.symlink_to(target)
    iso=root/(name+'.iso');command=[args.mkisofs,'-quiet','-R','-J','-input-charset','utf-8','-o',str(iso)]
    if udf:command+=['-udf']
    if name in ('broken','bad-fat'):
        (tree/'bios.img').write_bytes(bytes(2048))
        command+=['-b','bios.img','-no-emul-boot','-eltorito-alt-boot']+efi_boot_options(args.mkisofs)+['-no-emul-boot','-boot-load-size','32768']
    subprocess.run(command+[str(tree)],check=True,capture_output=True,timeout=30);return iso

def preflight(args,iso):
    result=subprocess.run([args.write_check,str(iso),str(64*MIB),'512','B3_TEST'],capture_output=True,timeout=30)
    report=json.loads(result.stdout);assert result.returncode==(0 if report['error']==0 else 1),(report,result.stderr)
    return report

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--write-check',required=True);parser.add_argument('--extract',required=True);parser.add_argument('--mkisofs',required=True);args=parser.parse_args();checks=[]
    with tempfile.TemporaryDirectory(prefix='rufus-b3-iso-') as temporary:
        root=Path(temporary)
        fat=root/'embedded.fat';subprocess.run(['mkfs.fat','-F','16','-C',str(fat),'16384'],check=True,capture_output=True)
        subprocess.run(['mmd','-i',str(fat),'::EFI','::EFI/BOOT'],check=True,capture_output=True)
        loader=root/'loader';loader.write_bytes(pe())
        subprocess.run(['mcopy','-i',str(fat),str(loader),'::EFI/BOOT/BOOTX64.EFI'],check=True,capture_output=True)
        config=root/'config';config.write_bytes(b'options LABEL=SOURCE\n');subprocess.run(['mcopy','-i',str(fat),str(config),'::EFI/BOOT/loader.conf'],check=True,capture_output=True)
        for kind in ('embedded','broken','precedence'):
            files={'efiboot.img':fat.read_bytes()};links=[]
            if kind=='broken':links=[('EFI/BOOT/BOOTX64.EFI','missing.efi')]
            if kind=='precedence':files['EFI/BOOT/BOOTX64.EFI']=pe()+b'PRESERVE'
            iso=image(args,root,kind,files,links);before=iso.read_bytes();report=preflight(args,iso);assert report['error']==0,report
            assert report['secureStatus'] in (0,3) and not report['securityPending'],report
            output=root/(kind+'-out');output.mkdir();extract(args.extract,iso,output)
            # FAT short names are uppercase; normal source spelling has precedence.
            actual={str(p.relative_to(output)).lower():p.read_bytes() for p in output.rglob('*') if p.is_file()}
            assert actual['efi/boot/bootx64.efi']==files.get('EFI/BOOT/BOOTX64.EFI',pe())
            if kind!='precedence':assert actual['efi/boot/loader.conf']==config.read_bytes()
            assert iso.read_bytes()==before
            checks.append(kind+' EFI preflight/security/extraction/readback')
        bad=bytearray(fat.read_bytes());bad[11:13]=b'\x00\x00'
        iso=image(args,root,'bad-fat',{'efiboot.img':bad,'EFI/BOOT/BOOTX64.EFI':b'broken'})
        report=preflight(args,iso);assert report['error']!=0,report;out=root/'bad-fat-out';out.mkdir();extract(args.extract,iso,out,error=errno.EBADMSG);assert not list(out.iterdir());checks.append('invalid embedded FAT refuses before copying')
        files={'data.bin':b'data','firmware/payload.bin':b'firmware','boot/isolinux/isolinux.cfg':b'default fixture\n','boot/isolinux/syslnx64.cfg':b'kernel /vmlinuz\n'}
        links=[('literal','data.bin'),('firmware/duplicate.bin','payload.bin'),('boot/syslinux','isolinux')]
        iso=image(args,root,'links',files,links);out=root/'links-out';out.mkdir();extract(args.extract,iso,out)
        assert (out/'literal').read_bytes()==b'data.bin';assert (out/'firmware/duplicate.bin').read_bytes()==b'firmware'
        assert (out/'boot/syslinux/syslnx64.cfg').read_bytes()==b'DEFAULT loadconfig\n\nLABEL loadconfig\n  CONFIG /boot/isolinux/syslnx64.cfg\n  APPEND /boot/isolinux\n';checks.append('FAT literal/firmware/Syslinux links and companion config')
        for name,links,error in [('cycle',[('one','two'),('two','one')],errno.ELOOP),('escape',[('one','../outside')],errno.EINVAL)]:
            iso=image(args,root,name,{'data':b'keep'},links);out=root/(name+'-out');out.mkdir();extract(args.extract,iso,out,error=error);assert not list(out.iterdir());checks.append(name+' source link refused before writes')
        iso=image(args,root,'missing-parent',{'gapXfile.txt':b'missing parent payload'})
        raw=iso.read_bytes();assert b'gapXfile.txt' in raw;iso.write_bytes(raw.replace(b'gapXfile.txt',b'gap/file.txt'))
        out=root/'missing-parent-out';out.mkdir();extract(args.extract,iso,out);assert (out/'gap/file.txt').read_bytes()==b'missing parent payload';checks.append('safe missing directory mastering recovery')
        for root_efi in (False,True):
            files={'slax/boot/EFI/BOOT/BOOTX64.EFI':pe()}
            if root_efi:files['EFI/preserve']=b'root EFI wins'
            iso=image(args,root,'slax-'+str(root_efi),files);out=root/('slax-out-'+str(root_efi));out.mkdir();extract(args.extract,iso,out)
            assert (out/('slax/boot/EFI/BOOT/BOOTX64.EFI' if root_efi else 'EFI/BOOT/BOOTX64.EFI')).read_bytes()==pe()
            if root_efi:assert (out/'EFI/preserve').read_bytes()==b'root EFI wins'
            else:assert not (out/'slax/boot/EFI').exists()
            checks.append('Slax relocation root precedence '+str(root_efi))
    print(json.dumps({'passed':len(checks),'checks':checks},indent=2))
if __name__=='__main__':main()
