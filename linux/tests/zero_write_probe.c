/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "platform/zero_write.h"
#include "progress_policy.h"
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char* fault;
static int active, target, cancelled, reads, writes, syncs, synced;
static RAW_WRITE_RESULT result;
static int format_extent, format_progress;
int IsRufusJobCancelled(void) { return cancelled; }
int IsIsoScanCancelled(void) { return cancelled; }
void uprintf(const char* format, ...) { (void)format; }
void _UpdateProgressWithInfo(int phase, int message, uint64_t done, uint64_t total, int forced)
{
    (void)message; (void)done; (void)total; (void)forced;
    if (format_extent) {
        if (phase != OP_FORMAT || total != 100 || done > 90) abort();
        format_progress = (int)done;
    }
    if (phase == RAW_WRITE_VERIFY && strcmp(fault, "verify-cancel") == 0) cancelled = 1;
}
static int Fault(const char* name) { return strcmp(fault, name) == 0; }
ssize_t __real_pread64(int, void*, size_t, off_t);
ssize_t __real_pwrite64(int, const void*, size_t, off_t);
int __real_fsync(int);

ssize_t __wrap_pread64(int fd, void* bytes, size_t count, off_t offset)
{
    if (!active || fd != target) return __real_pread64(fd, bytes, count, offset);
    if (count > 1024 * 1024 || offset < 0) abort();
    reads++;
    if (Fault("eintr") && reads == 1) { errno = EINTR; return -1; }
    if (Fault("read-eof")) return 0;
    if (Fault("read-error")) { errno = EIO; return -1; }
    if (Fault("short") && count > 70013) count = 70013;
    const ssize_t read = __real_pread64(fd, bytes, count, offset);
    if (synced && Fault("verify-mismatch") && read > 0) ((unsigned char*)bytes)[0] ^= 1;
    return read;
}

ssize_t __wrap_pwrite64(int fd, const void* bytes, size_t count, off_t offset)
{
    if (!active || fd != target) return __real_pwrite64(fd, bytes, count, offset);
    if (count > 1024 * 1024 || offset < 0 || synced) abort();
    writes++;
    if (Fault("eintr") && writes == 1) { errno = EINTR; return -1; }
    if (Fault("write-error")) { errno = ENOSPC; return -1; }
    if (Fault("write-zero")) return 0;
    if (Fault("write-cancel") && writes == 2) cancelled = 1;
    if (Fault("short") && count > 70013) count = 70013;
    return __real_pwrite64(fd, bytes, count, offset);
}

int __wrap_fsync(int fd)
{
    if (!active || fd != target) return __real_fsync(fd);
    syncs++;
    if (Fault("eintr") && syncs == 1) { errno = EINTR; return -1; }
    synced = 1;
    if (Fault("sync-error")) { errno = EIO; return -1; }
    if (Fault("resize")) {
        off_t size = lseek(fd, 0, SEEK_END);
        if (size < 0 || ftruncate(fd, size + 512) != 0) abort();
    }
    return __real_fsync(fd);
}

int main(int argc, char** argv)
{
    if (argc != 4 && argc != 6) return 2;
    fault = argv[3];
    format_extent = argc == 6;
    int flags = Fault("readonly") ? O_RDONLY : O_RDWR;
    if (Fault("append")) flags |= O_APPEND;
    target = open(argv[1], flags | O_CLOEXEC);
    if (target < 0) return 2;
    if (lseek(target, 17, SEEK_SET) != 17) return 2;
    active = 1;
    const int error = argc == 6 ? ZeroFormatExtent(target, 0, strtoull(argv[4], NULL, 10),
        strtoull(argv[5], NULL, 10), &result) : ZeroFileData(target, atoi(argv[2]), &result);
    active = 0;
    if (lseek(target, 0, SEEK_CUR) != 17 && !Fault("resize")) abort();
    printf("{\"error\":%d,\"phase\":%d,\"written\":%llu,\"verified\":%llu,\"sync_error\":%d,\"syncs\":%d,\"format_progress\":%d}\n",
        error, result.phase, (unsigned long long)result.bytes_written,
        (unsigned long long)result.bytes_verified, result.sync_error, syncs, format_progress);
    return close(target) == 0 ? 0 : 2;
}
