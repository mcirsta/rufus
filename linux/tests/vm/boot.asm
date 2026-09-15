; SPDX-License-Identifier: GPL-3.0-or-later
; Original, freely redistributable BIOS fixture. Print to COM1, then exit QEMU.
bits 16
org 0x7c00
    cli
    xor ax, ax
    mov ds, ax
    mov ss, ax
    mov sp, 0x7c00
    cld
    mov dx, 0x3fb
    mov al, 0x80
    out dx, al
    mov dx, 0x3f8
    mov al, 1
    out dx, al
    inc dx
    xor al, al
    out dx, al
    mov dx, 0x3fb
    mov al, 3
    out dx, al
    mov si, message
next:
    lodsb
    test al, al
    jz done
    mov bl, al
wait_tx:
    mov dx, 0x3fd
    in al, dx
    test al, 0x20
    jz wait_tx
    mov dx, 0x3f8
    mov al, bl
    out dx, al
    jmp next
done:
    mov dx, 0xf4
    mov ax, 0x10
    out dx, ax
    hlt
    jmp done
message: db 'RUFUS_RAW_BOOT_OK', 13, 10, 0
times 446-($-$$) db 0
; One 16 MiB FAT16 partition starting at 1 MiB.
db 0x80, 0xfe, 0xff, 0xff, 0x0e, 0xfe, 0xff, 0xff
dd 2048, 32768
times 510-($-$$) db 0
dw 0xaa55
