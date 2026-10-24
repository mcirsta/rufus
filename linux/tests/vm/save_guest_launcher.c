/* SPDX-License-Identifier: GPL-3.0-or-later */
/* A setuid launcher inside the disposable guest only, never installed on host. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static int marker(const char* path, const char* expected)
{
    char bytes[128] = {0};
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return 0;
    ssize_t count = read(fd, bytes, sizeof(bytes) - 1);
    close(fd);
    return count > 0 && strcmp(bytes, expected) == 0;
}
int main(int argc, char** argv)
{
    if (argc != 4 || getuid() != 1000 || geteuid() != 0 || strcmp(argv[1], "--disable-internal-agent")
        || strcmp(argv[2], "/usr/bin/rufus-save-guest-helper") || strcmp(argv[3], "--save-device")
        || !marker("/etc/rufus-save-export-fixture", "save-export\n")
        || !marker("/sys/class/dmi/id/product_name", "RufusMBRFixture\n")) return 77;
    if (setenv("PKEXEC_UID", "1000", 1) != 0 || setuid(0) != 0) return 77;
    int audit = open("/run/save-authorizations", O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW, 0600);
    if (audit < 0 || write(audit, "launch\n", 7) != 7 || close(audit) != 0) return 77;
    execl(argv[2], argv[2], argv[3], (char*)0);
    return 77;
}
