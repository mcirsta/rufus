// SPDX-License-Identifier: GPL-3.0-or-later
// Link-only guest faults. UID, kernel facts, native mount checks and the
// exclusive block claim all remain real. Never installed or run by host CTest.
#include "platform/compressed_write.h"
#include "platform/badblocks_test.h"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

namespace
{
thread_local int target = -1, input = -1;
thread_local bool badBlocksAlias = false;
thread_local uint64_t badBlocksDevice = 0;
thread_local unsigned badBlocksRounds = 0;
thread_local unsigned writes = 0, syncs = 0, reads = 0, sourceReads = 0, claims = 0;
thread_local char mode[32] = {};
thread_local off64_t inputSize = 0;
thread_local unsigned long long flipOffset = 0;
thread_local unsigned flipMask = 0;

bool fileEquals(const char* path, const char* expected)
{
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    char bytes[80] = {};
    const ssize_t count = read(fd, bytes, sizeof(bytes));
    close(fd);
    return count == ssize_t(strlen(expected)) && memcmp(bytes, expected, size_t(count)) == 0;
}

void checkClaim()
{
    const int fd = open("/dev/sda", O_RDWR | O_EXCL | O_CLOEXEC);
    const int error = errno;
    if (fd >= 0) close(fd);
    if (fd >= 0 || error != EBUSY) _exit(79);
    claims++;
}

void marker(const char* path)
{
    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0 || write(fd, "held\n", 5) != 5 || close(fd) != 0) _exit(80);
}
}

extern "C" int __real_WriteCompressedImageToDevice(const IMAGE_FILE_SOURCE*, const char*,
    const COMPRESSED_WRITE_PLAN*, int, uint64_t, uint64_t, RAW_WRITE_RESULT*);
extern "C" int __real_uname(struct utsname*);
extern "C" int __wrap_uname(struct utsname* info)
{
    const int error = __real_uname(info);
    const bool fixture = fileEquals("/sys/class/dmi/id/product_name", "RufusMBRFixture\n")
        && fileEquals("/etc/rufus-media-appearance-fixture", "appearance\n");
    if (error == 0 && fixture && fileEquals("/run/appearance-force-fuse", "fallback\n"))
        strcpy(info->release, "6.18.0"); // Exercise the real native FUSE provider on the same guest kernel.
    return error;
}

extern "C" int __real_TestBadBlocksDevice(int, uint64_t, uint64_t, int, bool, badblock_error_t, void*, BADBLOCK_RESULT*);
extern "C" int __wrap_TestBadBlocksDevice(int fd, uint64_t device, uint64_t capacity, int pattern,
    bool fake, badblock_error_t callback, void* context, BADBLOCK_RESULT* result)
{
    const bool guarded = getuid() == 0 && geteuid() == 0
        && fileEquals("/sys/class/dmi/id/product_name", "RufusMBRFixture\n")
        && fileEquals("/etc/rufus-badblocks-fixture", "badblocks\n");
    if (!guarded) return __real_TestBadBlocksDevice(fd, device, capacity, pattern, fake, callback, context, result);
    badBlocksAlias = fileEquals("/run/badblocks-fault", "alias")
        || (fileEquals("/run/badblocks-fault", "alias-once") && badBlocksRounds == 0);
    badBlocksDevice = device; ++badBlocksRounds;
    checkClaim();
    const int error = __real_TestBadBlocksDevice(fd, device, capacity, pattern, fake, callback, context, result);
    badBlocksAlias = false; badBlocksDevice = 0;
    checkClaim();
    return error;
}

bool aliasBadBlocks(int fd, size_t& size, off64_t& offset)
{
    if (!badBlocksAlias) return false;
    struct stat info;
    if (fstat(fd, &info) != 0 || !S_ISBLK(info.st_mode) || uint64_t(info.st_rdev) != badBlocksDevice) return false;
    checkClaim();
    constexpr off64_t physicalBytes = 4 * 1024 * 1024;
    offset %= physicalBytes;
    const size_t remaining = size_t(physicalBytes - offset);
    if (size > remaining) size = remaining;
    return true;
}

extern "C" ssize_t __real_pread64(int, void*, size_t, off64_t);
extern "C" int __wrap_WriteCompressedImageToDevice(const IMAGE_FILE_SOURCE* source, const char* path,
    const COMPRESSED_WRITE_PLAN* plan, int fd, uint64_t device, uint64_t capacity, RAW_WRITE_RESULT* result)
{
    const bool guarded = getuid() == 0 && geteuid() == 0
        && fileEquals("/sys/class/dmi/id/product_name", "RufusMBRFixture\n")
        && (fileEquals("/etc/rufus-compressed-helper-fixture", "compressed-helper\n")
            || fileEquals("/etc/rufus-lzma-helper-fixture", "lzma-helper\n")
            || fileEquals("/etc/rufus-zip-helper-fixture", "zip-helper\n")
            || fileEquals("/etc/rufus-zstd-helper-fixture", "zstd-helper\n")
            || fileEquals("/etc/rufus-vtsi-helper-fixture", "vtsi-helper\n")
            || fileEquals("/etc/rufus-vhd-helper-fixture", "vhd-helper\n")
            || fileEquals("/etc/rufus-lzw-helper-fixture", "lzw-helper\n")
            || fileEquals("/etc/rufus-lzw-qt-fixture", "lzw-qt\n"));
    if (!guarded) _exit(77);
    struct stat info, disk;
    if (fstat(fd, &info) != 0 || stat("/dev/sda", &disk) != 0
        || !S_ISBLK(info.st_mode) || info.st_rdev != disk.st_rdev) _exit(78);
    const int control = open("/run/compressed-fault", O_RDONLY | O_CLOEXEC);
    if (control < 0) _exit(80);
    const ssize_t count = read(control, mode, sizeof(mode) - 1);
    if (close(control) != 0 || count <= 0) _exit(80);
    target = fd;
    input = source->fd;
    inputSize = source->info.st_size;
    const bool lzwData = strcmp(mode, "copy-data") == 0 || strcmp(mode, "verify-data") == 0;
    if (lzwData || strcmp(mode, "late-code") == 0)
    {
        if (inputSize < 5) _exit(80);
        unsigned char magic[2] = {};
        const bool fallback = plan->image.compression_type == BLED_COMPRESSION_GZIP
            && __real_pread64(source->fd, magic, sizeof(magic), 0) == sizeof(magic)
            && magic[0] == 0x1f && magic[1] == 0x9d;
        if (plan->image.compression_type != BLED_COMPRESSION_LZW && !fallback) _exit(80);
    }
    if (lzwData)
    {
        const int config = open("/run/lzw-flip-control", O_RDONLY | O_CLOEXEC);
        char bytes[80] = {};
        if (config < 0 || read(config, bytes, sizeof(bytes) - 1) <= 0 || close(config) != 0
            || sscanf(bytes, "%llu %u", &flipOffset, &flipMask) != 2) _exit(80);
        if (flipOffset >= static_cast<unsigned long long>(inputSize) || flipMask == 0
            || flipMask > 255 || (flipMask & (flipMask - 1)) != 0) _exit(80);
    }
    checkClaim();
    return __real_WriteCompressedImageToDevice(source, path, plan, fd, device, capacity, result);
}

extern "C" ssize_t __real_pwrite64(int, const void*, size_t, off64_t);
extern "C" ssize_t __wrap_pwrite64(int fd, const void* data, size_t size, off64_t offset)
{
    if (aliasBadBlocks(fd, size, offset)) return __real_pwrite64(fd, data, size, offset);
    if (fd != target) return __real_pwrite64(fd, data, size, offset);
    checkClaim();
    if (strcmp(mode, "write") == 0 && writes != 0) { errno = EIO; return -1; }
    if (strcmp(mode, "write") == 0 && size > 17) size = 17;
    const ssize_t count = __real_pwrite64(fd, data, size, offset);
    if (count > 0) writes++;
    checkClaim();
    if (count > 0 && writes == 1 && strcmp(mode, "cancel") == 0)
    {
        marker("/run/compressed-copy-held");
        unsigned attempt = 0;
        while (access("/run/compressed-copy-release", F_OK) != 0)
        {
            if (++attempt > 10000) _exit(81);
            usleep(1000);
        }
    }
    return count;
}

extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd)
{
    if (fd != target) return __real_fsync(fd);
    checkClaim();
    const int result = __real_fsync(fd);
    syncs++;
    if (result == 0 && strcmp(mode, "sync") == 0) { errno = EIO; return -1; }
    return result;
}

extern "C" ssize_t __real_pread64(int, void*, size_t, off64_t);
extern "C" ssize_t __wrap_pread64(int fd, void* data, size_t size, off64_t offset)
{
    if (aliasBadBlocks(fd, size, offset)) return __real_pread64(fd, data, size, offset);
    if (fd == input)
    {
        checkClaim();
        sourceReads++;
        if (writes != 0 && strcmp(mode, "source-read") == 0) { errno = EIO; return -1; }
        if (writes != 0 && strcmp(mode, "source-eof") == 0) return 0;
        const ssize_t count = __real_pread64(fd, data, size, offset);
        if (count > 0 && writes != 0 && strcmp(mode, "late-code") == 0)
        {
            // The independently staged CLEAR tail has 37 nine-bit literals.
            const off64_t position = inputSize - 2;
            if (position >= offset && position - offset < count)
                static_cast<unsigned char*>(data)[position - offset] |= 0xf0;
            if (position + 1 >= offset && position + 1 - offset < count)
                static_cast<unsigned char*>(data)[position + 1 - offset] |= 0x1f;
        }
        const bool copyData = writes != 0 && syncs == 0 && strcmp(mode, "copy-data") == 0;
        const bool verifyData = syncs != 0 && strcmp(mode, "verify-data") == 0;
        if (count > 0 && (copyData || verifyData) && flipOffset >= static_cast<unsigned long long>(offset)
            && flipOffset - static_cast<unsigned long long>(offset) < static_cast<unsigned long long>(count))
            static_cast<unsigned char*>(data)[flipOffset - static_cast<unsigned long long>(offset)] ^= flipMask;
        return count;
    }
    if (fd != target) return __real_pread64(fd, data, size, offset);
    checkClaim();
    const ssize_t count = __real_pread64(fd, data, size, offset);
    if (count > 0 && syncs != 0)
    {
        reads++;
        if (strcmp(mode, "verify") == 0) static_cast<char*>(data)[0] ^= 1;
    }
    return count;
}

extern "C" int __real_close(int);
extern "C" int __wrap_close(int fd)
{
    if (fd != target) return __real_close(fd);
    checkClaim();
    const int result = __real_close(fd);
    target = -1; // Linux close is never retried, including an injected error.
    const int report = open("/run/compressed-observations", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (report < 0) _exit(80);
    const int count = dprintf(report, "WRITES=%u SYNCS=%u READS=%u CLAIMS=%u CLOSES=1 SOURCE_READS=%u\n",
        writes, syncs, reads, claims, sourceReads);
    if (count <= 0 || __real_close(report) != 0) _exit(80);
    if (result == 0 && strcmp(mode, "close") == 0) { errno = EIO; return -1; }
    return result;
}
