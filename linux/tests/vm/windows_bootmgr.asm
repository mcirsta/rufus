; SPDX-License-Identifier: GPL-3.0-or-later
; Disposable real-mode fixture, never a Windows loader or production payload.
bits 16
%ifdef SECOND_DISK
org 0x7c00
%else
org 0
%endif
SERIAL_PORT equ 0x3f8
DEBUG_EXIT_PORT equ 0xf4
BOOTMGR_SEGMENT equ 0x2000
%ifndef EXPECTED_DRIVE
%define EXPECTED_DRIVE 0x80
%endif
TAIL_SEGMENT_DELTA equ 0x1000
TAIL_OFFSET equ 0x1ffc
TAIL_MARKER equ 0x52474d42

    cli
    cld
    push cs
    pop ds
%ifndef SECOND_DISK
    ; The unchanged NTFS PBR far-returns to 2000:0000 with BIOS drive in DL.
    mov ax, cs
    cmp ax, BOOTMGR_SEGMENT
    jne failed
    cmp dl, EXPECTED_DRIVE
    jne failed
    add ax, TAIL_SEGMENT_DELTA
    mov es, ax
    cmp dword [es:TAIL_OFFSET], TAIL_MARKER
    jne failed
%endif
    mov si, message
    mov bp, 0x10
    jmp serial_init
failed:
    mov si, failure
    mov bp, 0x11
serial_init:
    mov dx, SERIAL_PORT + 1
    xor al, al
    out dx, al
    mov dx, SERIAL_PORT + 3
    mov al, 0x80
    out dx, al
    mov dx, SERIAL_PORT
    mov al, 1
    out dx, al
    inc dx
    xor al, al
    out dx, al
    mov dx, SERIAL_PORT + 3
    mov al, 3
    out dx, al
next:
    lodsb
    test al, al
    jz done
    mov bl, al
wait_tx:
    mov dx, SERIAL_PORT + 5
    in al, dx
    test al, 0x20
    jz wait_tx
    mov dx, SERIAL_PORT
    mov al, bl
    out dx, al
    jmp next
done:
    mov dx, DEBUG_EXIT_PORT
    mov ax, bp
    out dx, ax
    hlt
    jmp done
%ifdef SECOND_DISK
message: db 'RUFUS_WINDOWS_SECOND_DISK_OK', 13, 10, 0
%else
%if EXPECTED_DRIVE == 0x81
message: db 'RUFUS_WINDOWS_BOOTMGR_OK cs=2000 drive=81 tail=ok', 13, 10, 0
%else
message: db 'RUFUS_WINDOWS_BOOTMGR_OK cs=2000 drive=80 tail=ok', 13, 10, 0
%endif
%endif
failure: db 'RUFUS_WINDOWS_BOOTMGR_FAILED', 13, 10, 0
%ifdef SECOND_DISK
times 446-($-$$) db 0
; The original Rufus MBR scans this active entry before offering its prompt.
db 0x80, 0xfe, 0xff, 0xff, 0x07, 0xfe, 0xff, 0xff
dd 2048, 2048
times 510-($-$$) db 0
dw 0xaa55
%else
; Cross the 64-KiB boundary to prove nonresident, multi-cluster loading.
times 0x12000-4-($-$$) db 0xa5
dd TAIL_MARKER
%endif
