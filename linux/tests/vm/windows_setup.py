#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Boot a verified USB in isolated Windows Setup, optionally with an install disk."""
import argparse
import json
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import time

from fat_extract import digest

INSTALL_DISK_BYTES = 80 * 1024**3
VM_TIMEOUT_SECONDS = 3600
BOOT_TIMEOUT_SECONDS = 600
SOCKET_TIMEOUT_SECONDS = 10
STOP_TIMEOUT_SECONDS = 5
FIRMWARE = Path('/usr/share/OVMF/x64')


def stop(process):
    if process is None or process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=STOP_TIMEOUT_SECONDS)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()


def interrupt(signum, frame):
    # Unwind the supervisor so its finally block can stop both children.
    raise SystemExit(128 + signum)


def record_processes(output, phase, vm, tpm):
    error = sys.exc_info()[1]
    record = {'phase': phase, 'time': time.time(),
              'error': None if error is None else type(error).__name__ + ': ' + str(error),
              'vm_pid': None if vm is None else vm.pid,
              'vm_exit_code': None if vm is None else vm.poll(),
              'tpm_pid': None if tpm is None else tpm.pid,
              'tpm_exit_code': None if tpm is None else tpm.poll()}
    with (output / 'process-status.jsonl').open('a') as log:
        log.write(json.dumps(record) + '\n')


def run_vm(output, tpm_command, command, timeout=VM_TIMEOUT_SECONDS):
    previous_handler = signal.signal(signal.SIGTERM, interrupt)
    try:
        supervise_vm(output, tpm_command, command, timeout)
    finally:
        signal.signal(signal.SIGTERM, previous_handler)


def supervise_vm(output, tpm_command, command, timeout):
    tpm = vm = None
    with (output / 'swtpm.log').open('wb') as tpm_log, (output / 'qemu.log').open('wb') as vm_log:
        try:
            tpm = subprocess.Popen(tpm_command, stdout=tpm_log, stderr=subprocess.STDOUT, start_new_session=True)
            deadline = time.monotonic() + SOCKET_TIMEOUT_SECONDS
            while not (output / 'tpm.sock').exists():
                if tpm.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError('The virtual TPM did not start')
                time.sleep(.05)
            vm = subprocess.Popen(command, stdout=vm_log, stderr=subprocess.STDOUT)
            record_processes(output, 'running', vm, tpm)
            print('Windows Setup VM running; QMP:', output / 'qmp.sock', flush=True)
            vm.wait(timeout=timeout)
            record_processes(output, 'vm_exited', vm, tpm)
            if vm.returncode != 0:
                raise RuntimeError('Windows VM failed; inspect qemu.log')
        finally:
            # Defer another interrupt until both children are reaped. Do not
            # ignore it: restoring the mask delivers pending signals afterward.
            previous_mask = signal.pthread_sigmask(signal.SIG_BLOCK, {signal.SIGTERM, signal.SIGINT})
            try:
                try:
                    record_processes(output, 'cleanup_started', vm, tpm)
                finally:
                    try:
                        stop(vm)
                    finally:
                        stop(tpm)
                record_processes(output, 'stopped', vm, tpm)
            finally:
                signal.pthread_sigmask(signal.SIG_SETMASK, previous_mask)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--usb', required=True, type=Path, help='Verified regular-file USB disk; used as an unchanged backing image')
    parser.add_argument('--boot-only', action='store_true',
                        help='Observe Setup without creating or attaching an installation disk; ten-minute timeout')
    parser.add_argument('output', type=Path, help='New directory for VM state and an optional 80 GiB sparse install disk')
    args = parser.parse_args()
    usb = args.usb.resolve()
    output = args.output.resolve()
    if not usb.is_file():
        parser.error('The USB must be a regular file')
    if any(char in str(usb) + str(output) for char in ',\r\n'):
        parser.error('Paths must not contain QEMU option separators')
    output.mkdir()
    tpm_dir = output / 'tpm'
    tpm_dir.mkdir(mode=0o700)
    firmware = []
    for name in ('OVMF_CODE.secboot.4m.fd', 'OVMF_VARS.4m.fd'):
        source = FIRMWARE / name
        shutil.copyfile(source, output / name)
        firmware.append({'source': str(source), 'sha256': digest(source)})
    before = digest(usb)
    install_options = []
    if not args.boot_only:
        disk = output / 'installed.qcow2'
        subprocess.run(['qemu-img', 'create', '-f', 'qcow2', str(disk), str(INSTALL_DISK_BYTES)], check=True)
        install_options = ['-drive', 'if=none,id=installed,format=qcow2,file=' + str(disk),
                           '-device', 'ide-hd,drive=installed,bus=ide.0,bootindex=2']
    # Windows PE refuses a hardware-write-protected mkntfs volume even when
    # CHKDSK reports it clean. Permit normal mount writes in a disposable overlay.
    usb_overlay = output / 'usb-overlay.qcow2'
    subprocess.run(['qemu-img', 'create', '-f', 'qcow2', '-F', 'raw', '-b', str(usb), str(usb_overlay)], check=True)
    tpm_command = ['swtpm', 'socket', '--tpm2', '--tpmstate', 'dir=' + str(tpm_dir),
                   '--ctrl', 'type=unixio,path=' + str(output / 'tpm.sock') + ',terminate']
    command = ['qemu-system-x86_64', '-accel', 'kvm', '-machine', 'q35,smm=on', '-cpu', 'host',
               '-smp', '4', '-m', '8192', '-display', 'none', '-monitor', 'none', '-nic', 'none',
               '-qmp', 'unix:' + str(output / 'qmp.sock') + ',server=on,wait=off',
               '-serial', 'file:' + str(output / 'serial.log'),
               '-global', 'driver=cfi.pflash01,property=secure,value=on',
               '-drive', 'if=pflash,format=raw,unit=0,readonly=on,file=' + str(output / 'OVMF_CODE.secboot.4m.fd'),
               '-drive', 'if=pflash,format=raw,unit=1,file=' + str(output / 'OVMF_VARS.4m.fd'),
               '-chardev', 'socket,id=chrtpm,path=' + str(output / 'tpm.sock'),
               '-tpmdev', 'emulator,id=tpm0,chardev=chrtpm', '-device', 'tpm-crb,tpmdev=tpm0',
               '-device', 'qemu-xhci,id=xhci', '-device', 'usb-tablet',
               '-drive', 'if=none,id=stick,format=qcow2,file=' + str(usb_overlay),
               '-device', 'usb-storage,id=usb,drive=stick,removable=on,bootindex=1,serial=RUFUS-WINDOWS-USB']
    command.extend(install_options)
    manifest = {'usb': str(usb), 'usb_sha256': before, 'firmware': firmware,
                'qemu': subprocess.check_output(['qemu-system-x86_64', '--version'], text=True).strip(),
                'swtpm': subprocess.check_output(['swtpm', '--version'], text=True).strip(),
                'secure_boot_capable': True, 'secure_boot': False, 'network': False,
                'boot_only': args.boot_only, 'install_disk_bytes': 0 if args.boot_only else INSTALL_DISK_BYTES,
                'usb_overlay': str(usb_overlay)}
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    (output / 'qemu-command.json').write_text(json.dumps(command, indent=2) + '\n')
    (output / 'swtpm-command.json').write_text(json.dumps(tpm_command, indent=2) + '\n')
    run_vm(output, tpm_command, command, BOOT_TIMEOUT_SECONDS if args.boot_only else VM_TIMEOUT_SECONDS)
    assert digest(usb) == before, 'The verified USB backing image changed'
    (output / 'usb-unchanged.json').write_text(json.dumps({'sha256': before, 'unchanged': True}) + '\n')
    print('VM stopped and USB hash is unchanged. Boot/installation success requires recorded guest evidence.', flush=True)


if __name__ == '__main__':
    main()
