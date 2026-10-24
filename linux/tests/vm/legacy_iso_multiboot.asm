; SPDX-License-Identifier: GPL-3.0-or-later
; Generated multiboot handoff target, never ReactOS code.
bits 32
org 0x100000
    dd 0x1badb002, 0x10000, -(0x1badb002 + 0x10000)
    dd 0x100000, 0x100000, image_end, image_end, entry
entry:
    cli
    mov esp, 0x90000
    mov esi, message
    mov ebp, 0x10
    cmp eax, 0x2badb002
    je serial_init
    mov esi, failure
    mov ebp, 0x11
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
    mov eax, ebp
    out dx, eax
    hlt
    jmp done
message: db 'RUFUS_REACTOS_ISO_MULTIBOOT_OK',13,10,0
failure: db 'RUFUS_REACTOS_ISO_MULTIBOOT_FAILED',13,10,0
times 16384-($-$$) db 0xa5
image_end:
