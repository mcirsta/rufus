/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Disposable x64 QEMU fixture, never linked into production. Firmware must
 * load this application from NTFS through the original UEFI:NTFS payload. */
enum { SERIAL_PORT = 0x3f8, SERIAL_READY = 0x20, DEBUG_EXIT_PORT = 0xf4 };

static void WritePort(unsigned short port, unsigned char value)
{
    __asm__ volatile("outb %0, %w1" : : "a"(value), "Nd"(port));
}

static unsigned char ReadPort(unsigned short port)
{
    unsigned char value;
    __asm__ volatile("inb %w1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

void efi_main(void)
{
    const char* message = "\r\nRUFUS_SYSLINUX_UEFI_OK\r\n";
    WritePort(SERIAL_PORT + 1, 0);    /* Disable UART interrupts. */
    WritePort(SERIAL_PORT + 3, 0x80); /* Divisor access. */
    WritePort(SERIAL_PORT, 1);        /* 115200 baud. */
    WritePort(SERIAL_PORT + 1, 0);
    WritePort(SERIAL_PORT + 3, 3);    /* Eight data bits, no parity, one stop. */
    while (*message != 0) {
        while (!(ReadPort(SERIAL_PORT + 5) & SERIAL_READY)) {}
        WritePort(SERIAL_PORT, (unsigned char)*message++);
    }
    __asm__ volatile("outl %0, %w1" : : "a"(0x10), "Nd"(DEBUG_EXIT_PORT));
    for (;;) __asm__ volatile("hlt");
}
