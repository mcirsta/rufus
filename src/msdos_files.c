/* SPDX-License-Identifier: GPL-3.0-or-later
 * MS-DOS resource names and WinME patches extracted from Rufus's dos.c.
 * Copyright (c) 2011-2026 Pete Batard <pete@akeo.ie>
 */
#include "msdos_files.h"
#include <string.h>

static const MSDOS_FILE files[] = {
    { "MSDOS   SYS", "MSDOS.SYS", true },
    { "COMMAND COM", "COMMAND.COM", true },
    { "IO      SYS", "IO.SYS", true },
    { "MODE    COM", "MODE.COM", false },
    { "KEYB    COM", "KEYB.COM", false },
    { "KEYBOARDSYS", "KEYBOARD.SYS", false },
    { "KEYBRD2 SYS", "KEYBRD2.SYS", false },
    { "KEYBRD3 SYS", "KEYBRD3.SYS", false },
    { "KEYBRD4 SYS", "KEYBRD4.SYS", false },
    { "DISPLAY SYS", "DISPLAY.SYS", false },
    { "EGA     CPI", "EGA.CPI", false },
    { "EGA2    CPI", "EGA2.CPI", false },
    { "EGA3    CPI", "EGA3.CPI", false }
};

const MSDOS_FILE* GetMsDosFile(size_t index)
{
    return index < sizeof(files) / sizeof(files[0]) ? &files[index] : NULL;
}

bool PatchMsDosFile(const char* name, uint8_t* bytes, size_t size)
{
    if (name == NULL || bytes == NULL)
        return false;
    if (strcmp(name, "COMMAND.COM") == 0) {
        const uint8_t expected[8] = { 0x15, 0x80, 0xFA, 0x03, 0x75, 0x10, 0xB8, 0x0E };
        if (size != 93040 || memcmp(bytes + 0x650c, expected, sizeof(expected)) != 0)
            return false;
        bytes[0x6510] = 0xeb;
    } else if (strcmp(name, "IO.SYS") == 0) {
        const uint8_t expected[8] = { 0xFA, 0x80, 0x75, 0x09, 0x8D, 0xB6, 0x99, 0x00 };
        if (size != 116736 || memcmp(bytes + 0x3a8, expected, sizeof(expected)) != 0)
            return false;
        bytes[0x3aa] = 0xeb;
    }
    return true;
}
