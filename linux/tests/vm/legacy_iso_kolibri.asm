; SPDX-License-Identifier: GPL-3.0-or-later
; Bare MTLD_F32 handoff target. No KolibriOS program is run.
bits 16
org 0x8000
    cli
    xor ax, ax
    mov ds, ax
    mov ss, ax
    mov sp, 0x7000
    cld
    mov si, message
    mov bp, 0x10
    cmp dword [tail], 0x4c4f534b
    je serial_init
    mov si, failure
    mov bp, 0x11
serial_init:
    mov dx, 0x3f9
    xor al, al
    out dx, al
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
    mov ax, bp
    out dx, ax
    hlt
    jmp done
message: db 'RUFUS_KOLIBRI_ISO_BIOS_OK tail=ok',13,10,0
failure: db 'RUFUS_KOLIBRI_ISO_BIOS_FAILED tail',13,10,0
times 16384-4-($-$$) db 0xa5
tail: dd 0x4c4f534b
