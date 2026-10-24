; SPDX-License-Identifier: GPL-3.0-or-later
; Bare real-mode handoff fixture. No ReactOS or Microsoft program is run.
; The original ReactOS PBR far-jumps to FREELDR.SYS at offset zero.
bits 16
org 0
    cli
    cld
    push cs
    pop ds
    call location
location:
    pop bx
    sub bx, location
    ; Normalize CS:load_offset into a data segment before reading beyond 64 KiB
    ; physical memory. The bundled PBR enters at 0000:F800, newer ones at seg:0.
    mov ax, cs
    mov cx, bx
    shr cx, 4
    add ax, cx
    mov ds, ax
    and bx, 15
    cmp dl, 0x80
    jne drive_failed
    cmp dword [bx+0x7ffc], 0x524c5246
    jne tail_failed
    lea si, [bx+message]
    mov bp, 0x10
    jmp serial_init
drive_failed:
    lea si, [bx+drive_failure]
    jmp failure
tail_failed:
    lea si, [bx+tail_failure]
failure:
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
message: db 'RUFUS_REACTOS_BIOS_OK drive=80 tail=ok',13,10,0
drive_failure: db 'RUFUS_REACTOS_BIOS_FAILED drive',13,10,0
tail_failure: db 'RUFUS_REACTOS_BIOS_FAILED tail',13,10,0
times 0x8000-4-($-$$) db 0xa5
dd 0x524c5246
