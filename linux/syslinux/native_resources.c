/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Native adapter: select already validated owned bytes before upstream main
 * performs I/O. No cache/network lookup or executable input is accepted. */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include "syslinux.h"
static uint32_t little32(const unsigned char* p) { return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24); }
int RufusLoadSyslinuxResources(void)
{
    const char* path = getenv("RUFUS_SYSLINUX_RESOURCES");
    if (!path) return 0;
    int fd = open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    if (fd < 0) return -1;
    struct stat file;
    if (fstat(fd,&file) != 0 || !S_ISREG(file.st_mode) || file.st_nlink != 1 || file.st_uid != geteuid()
        || file.st_size < 540 || file.st_size > 1024*1024) { close(fd); return -1; }
    size_t size = (size_t)file.st_size, done = 0;
    unsigned char* bytes = malloc(size);
    if (!bytes) { close(fd); return -1; }
    while (done < size) { ssize_t count = read(fd,bytes+done,size-done); if (count <= 0) break; done += (size_t)count; }
    int error = close(fd);
    if (done != size || error || memcmp(bytes,"RSLX0001",8)) { free(bytes); return -1; }
    uint32_t lengths[5]; size_t total = 28;
    for (int i = 0; i < 5; ++i) { lengths[i] = little32(bytes+8+4*i); if (lengths[i] > size-total) { free(bytes); return -1; } total += lengths[i]; }
    if (total != size || lengths[0] < 512 || lengths[1] != 512 || (lengths[2] && lengths[2] < 256)) { free(bytes); return -1; }
    size_t padded = (lengths[0]+511U)&~511U;
    unsigned char* loader = calloc(1,padded);
    if (!loader) { free(bytes); return -1; }
    memcpy(loader,bytes+28,lengths[0]);
    /* The upstream x86 accessors assume aligned scalar fields. Bundles may
     * place the VBR after an odd-sized loader, so retain an aligned copy. */
    unsigned char* boot = malloc(512);
    if (!boot) { free(loader); free(bytes); return -1; }
    memcpy(boot,bytes+28+lengths[0],512);
    syslinux_ldlinux = loader; syslinux_ldlinux_len = (unsigned int)padded;
    syslinux_bootsect = boot; syslinux_bootsect_len = 512;
    syslinux_ldlinuxc32 = bytes+28+lengths[0]+512; syslinux_ldlinuxc32_len = lengths[2];
    /* Owned until process exit; the normal runner joins the process group. */
    return 0;
}
