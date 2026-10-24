/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "platform/badblocks_test.h"
#include "platform/rufus_reporting.h"
#include "progress_policy.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static int target, cancelled, reads, writes, syncs, active;
static uint64_t progress, total;
static const char* fault;
static unsigned reports;
static const off_t bad_offset = 3 * BADBLOCK_BLOCK_SIZE;
ssize_t __real_pread64(int, void*, size_t, off_t);
ssize_t __real_pwrite64(int, const void*, size_t, off_t);
int __real_fsync(int);
static int Fault(const char* name) { return strcmp(fault, name) == 0; }
int IsRufusJobCancelled(void) { return cancelled || Fault("cancel-before") || Fault("cleanup-cancel"); }
int IsIsoScanCancelled(void) { return IsRufusJobCancelled(); }
void uprintf(const char* format, ...) { (void)format; }
void _UpdateProgressWithInfo(int operation, int message, uint64_t done, uint64_t size, int force)
{
    (void)message; (void)force;
    if (operation != OP_BADBLOCKS || done > size || done < progress) abort();
    progress = done; total = size;
    if (Fault("cancel-read") && reads > 0) cancelled = 1;
}
static void Report(void* context, uint64_t block, unsigned type)
{
    (void)context; (void)block; (void)type;
    reports++;
    if (reports > 256) abort();
}
ssize_t __wrap_pwrite64(int fd, const void* buffer, size_t count, off_t offset)
{
    if (!active || fd != target) return __real_pwrite64(fd, buffer, count, offset);
    if (count > 32 * 1024 * 1024 || offset < 0) abort();
    writes++;
    if (Fault("eintr") && writes == 1) { errno = EINTR; return -1; }
    if (Fault("cancel-write") && writes == 2) cancelled = 1;
    if (Fault("unplug")) { errno = ENODEV; return -1; }
    if (Fault("write-error") || Fault("tlc-write-error")) {
        if (offset >= bad_offset && offset < bad_offset + BADBLOCK_BLOCK_SIZE) { errno = EIO; return -1; }
        if (offset < bad_offset && count > (size_t)(bad_offset - offset)) count = (size_t)(bad_offset - offset);
    }
    if (Fault("alias")) {
        offset %= 4 * 1024 * 1024;
        const size_t room = 4 * 1024 * 1024 - (size_t)offset;
        if (count > room) count = room;
    }
    if (Fault("short") && count > 70013) count = 70013;
    return __real_pwrite64(fd, buffer, count, offset);
}
ssize_t __wrap_pread64(int fd, void* buffer, size_t count, off_t offset)
{
    if (!active || fd != target) return __real_pread64(fd, buffer, count, offset);
    if (count > 32 * 1024 * 1024 || offset < 0) abort();
    reads++;
    if (Fault("eintr") && reads == 1) { errno = EINTR; return -1; }
    if (Fault("read-error")) {
        if (offset >= bad_offset && offset < bad_offset + BADBLOCK_BLOCK_SIZE) { errno = EIO; return -1; }
        if (offset < bad_offset && count > (size_t)(bad_offset - offset)) count = (size_t)(bad_offset - offset);
    }
    if (Fault("alias")) {
        offset %= 4 * 1024 * 1024;
        const size_t room = 4 * 1024 * 1024 - (size_t)offset;
        if (count > room) count = room;
    }
    if (Fault("short") && count > 70013) count = 70013;
    const ssize_t read = __real_pread64(fd, buffer, count, offset);
    if (read > 0 && Fault("corrupt") && offset <= bad_offset && bad_offset - offset < read)
        ((unsigned char*)buffer)[bad_offset - offset] ^= 1;
    return read;
}
int __wrap_fsync(int fd)
{
    if (!active || fd != target) return __real_fsync(fd);
    syncs++;
    if (Fault("sync-error") || Fault("cleanup-sync-error")) { errno = EIO; return -1; }
    if (Fault("resize")) return ftruncate(fd, 1024);
    return __real_fsync(fd);
}
int main(int argc, char** argv)
{
    if (argc != 6) return 2;
    fault = argv[5];
    target = open(argv[1], (Fault("readonly") ? O_RDONLY : O_RDWR) | O_CLOEXEC | (Fault("append") ? O_APPEND : 0));
    if (target < 0 || lseek(target, 17, SEEK_SET) != 17) return 2;
    BADBLOCK_RESULT result;
    active = 1;
    memset(&result, 0, sizeof(result));
    int error;
    if (strncmp(fault, "cleanup", 7) == 0) {
        struct stat info;
        if (fstat(target, &info) != 0) return 2;
        error = ClearBadBlocksHeaders(target, 0, (uint64_t)info.st_size, &result.sync_error);
    } else error = TestBadBlocksFile(target, atoi(argv[2]), atoi(argv[3]), (uint32_t)strtoul(argv[4], NULL, 10), Report, NULL, &result);
    active = 0;
    if (lseek(target, 0, SEEK_CUR) != 17) abort();
    printf("{\"error\":%d,\"count\":%u,\"read\":%u,\"write\":%u,\"corrupt\":%u,\"threshold\":%d,\"written\":%" PRIu64 ",\"read_bytes\":%" PRIu64 ",\"sync_error\":%d,\"syncs\":%d,\"reports\":%u,\"progress\":%" PRIu64 ",\"total\":%" PRIu64 "}\n",
        error, result.count, result.read_errors, result.write_errors, result.corruption_errors,
        result.threshold_reached, result.bytes_written, result.bytes_read, result.sync_error, syncs, reports, progress, total);
    return close(target) == 0 ? 0 : 2;
}
