/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <unistd.h>

static int matches(const char* path, const char* expected)
{
    char text[128] = {0};
    FILE* file = fopen(path, "r");
    if (file == NULL) return 0;
    const size_t size = fread(text, 1, sizeof(text) - 1, file);
    const int failed = ferror(file);
    fclose(file);
    return !failed && size == strlen(expected) && strcmp(text, expected) == 0;
}

int main(int argc, char** argv)
{
    if (geteuid() != 0 || argc != 2
        || !matches("/etc/rufus-usb-fixture", "RUFUS-TEST-USB\n")
        || !matches("/sys/devices/virtual/dmi/id/product_name", "RufusUSBFixture\n"))
        return 77;
    const int busy = strcmp(argv[1], "busy") == 0;
    if (!busy && strcmp(argv[1], "released") != 0) return 77;
    const int result = mount("/dev/sda1", "/run/rufus-exclusive", "vfat", MS_RDONLY, "");
    const int error = errno;
    if (result == 0 && umount("/run/rufus-exclusive") != 0) return 1;
    if ((busy && result == -1 && error == EBUSY) || (!busy && result == 0))
    {
        puts(argv[1]);
        return 0;
    }
    fprintf(stderr, "Guest mount check: result=%d errno=%d (%s)\n", result, error, strerror(error));
    return 1;
}
