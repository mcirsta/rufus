/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Destructive test and recovery order derived from src/badblocks.c, by Remy
 * Card, Theodore Ts'o, David Beattie and Pete Batard (1992-2024). Native I/O,
 * cancellation, exact-sector tails and bounded reporting are Linux adapters. */
#include "badblocks_test.h"
#include "target_io.h"
#include "raw_write.h"
#include <stdio.h>
#include "rufus_reporting.h"
#include "progress_policy.h"
#include "partition_layout.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/random.h>
#include <linux/fs.h>
#include <unistd.h>

enum { TEST_BATCH_BYTES = BB_BLOCKS_AT_ONCE * BADBLOCK_BLOCK_SIZE };
static const uint64_t maximum_bytes = UINT64_C(1) << 50;
static const unsigned patterns[BADLOCKS_PATTERN_TYPES][BADBLOCK_PATTERN_COUNT] = {
    BADBLOCK_PATTERN_ONE_PASS, BADBLOCK_PATTERN_TWO_PASSES, BADBLOCK_PATTERN_SLC,
    BADCLOCK_PATTERN_MLC, BADBLOCK_PATTERN_TLC
};

typedef struct {
    int fd, original;
    uint64_t device, capacity, sequence;
    WRITABLE_TARGET initial;
    uint64_t processed, total;
    BADBLOCK_RESULT* result;
    badblock_error_t callback;
    void* context;
} BADBLOCK_JOB;

static int CheckJob(const BADBLOCK_JOB* job)
{
    WRITABLE_TARGET current;
    int error = InspectWritableTarget(job->original, job->device, &current);
    if (error != 0) return error;
    const bool same_file = current.info.st_dev == job->initial.info.st_dev
        && current.info.st_ino == job->initial.info.st_ino;
    if (!same_file || current.size != job->capacity) return ESTALE;
    if (job->device != 0) {
        uint64_t sequence = 0;
        if (ioctl(job->original, BLKGETDISKSEQ, &sequence) != 0) return errno;
        if (sequence != job->sequence) return ESTALE;
        return CheckRawBlockTarget(job->original, job->device, job->capacity, job->capacity);
    }
    return 0;
}

static int SyncJob(BADBLOCK_JOB* job)
{
    while (fsync(job->fd) != 0) {
        if (errno == EINTR) continue;
        job->result->sync_error = errno;
        return errno;
    }
    return 0;
}

static void PatternFill(uint8_t* buffer, unsigned pattern, uint64_t position)
{
    if (pattern <= 255) { memset(buffer, pattern, TEST_BATCH_BYTES); return; }
    uint8_t bytes[sizeof(pattern)];
    unsigned count = 0;
    for (; pattern != 0; pattern >>= 8) bytes[count++] = (uint8_t)pattern;
    size_t filled = 0;
    while (filled < TEST_BATCH_BYTES) {
        const size_t within = (size_t)((position + filled) % TEST_BATCH_BYTES);
        const size_t room = TEST_BATCH_BYTES - filled;
        const size_t segment = TEST_BATCH_BYTES - within < room ? TEST_BATCH_BYTES - within : room;
        const size_t initial = count < segment ? count : segment;
        for (size_t i = 0; i < initial; i++) buffer[filled + i] = bytes[count - 1 - (within + i) % count];
        size_t copied = initial;
        while (copied < segment) {
            const size_t remaining = segment - copied;
            const size_t batch = remaining < copied ? remaining : copied;
            memcpy(buffer + filled + copied, buffer + filled, batch); copied += batch;
        }
        filled += segment;
    }
}

static void StampBlocks(uint8_t* buffer, uint64_t first, size_t bytes, size_t tag)
{
    for (size_t offset = 0; offset < bytes; offset += BADBLOCK_BLOCK_SIZE) {
        const uint64_t id = first + offset / BADBLOCK_BLOCK_SIZE;
        // Windows writes a little-endian blk64_t. Avoid its unaligned cast.
        for (unsigned i = 0; i < 8; i++) buffer[offset + tag + i] = (uint8_t)(id >> (8 * i));
    }
}

static void AddBadBlock(BADBLOCK_JOB* job, uint64_t block, unsigned type)
{
    BADBLOCK_RESULT* result = job->result;
    for (uint32_t i = 0; i < result->count; i++) if (result->errors[i].block == block) return;
    if (result->count == BB_BAD_BLOCKS_THRESHOLD) return;
    result->errors[result->count++] = (BADBLOCK_ERROR){block, type};
    if (type == BADBLOCK_READ_ERROR) result->read_errors++;
    else if (type == BADBLOCK_WRITE_ERROR) result->write_errors++;
    else result->corruption_errors++;
    if (result->count == BB_BAD_BLOCKS_THRESHOLD) result->threshold_reached = true;
    if (job->callback != NULL) job->callback(job->context, block, type);
}

/* Complete short transfers and EINTR; on a device error return only the whole
 * prefix blocks. The caller retries the failed region one 512-KiB block at a
 * time, then restores 32-MiB batches at the upstream recovery boundary. */
static int Transfer(BADBLOCK_JOB* job, uint8_t* buffer, size_t bytes, uint64_t offset,
    bool write, size_t* transferred)
{
    *transferred = 0;
    while (*transferred < bytes) {
        if (IsRufusJobCancelled()) return ECANCELED;
        const size_t remaining = bytes - *transferred;
        const uint64_t position = offset + *transferred;
        const ssize_t count = write ? pwrite(job->fd, buffer + *transferred, remaining, (off_t)position)
            : pread(job->fd, buffer + *transferred, remaining, (off_t)position);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            const int error = count < 0 ? errno : EIO;
            return error == ENODEV || error == ENXIO || error == ENOMEDIUM ? error : EIO;
        }
        *transferred += (size_t)count;
        if (write) job->result->bytes_written += (uint64_t)count;
        else job->result->bytes_read += (uint64_t)count;
    }
    return 0;
}

static int TestPass(BADBLOCK_JOB* job, uint8_t* expected, uint8_t* actual, int pass,
    int passes, unsigned pattern, bool fake, size_t tag, bool write)
{
    uint64_t position = 0, recover = UINT64_MAX;
    size_t batch = TEST_BATCH_BYTES;
    const uint64_t total = job->capacity * 2 * (unsigned)passes;
    const uint64_t base = job->capacity * (2 * (unsigned)pass + (write ? 0 : 1));
    job->processed = base; job->total = total;
    _UpdateProgressWithInfo(OP_BADBLOCKS, 0, base, total, 1);
    while (position < job->capacity) {
        if (IsRufusJobCancelled()) return ECANCELED;
        if (job->result->count == BB_BAD_BLOCKS_THRESHOLD) {
            job->result->threshold_reached = true; return 0;
        }
        int error = CheckJob(job);
        if (error != 0) return error;
        const size_t bytes = job->capacity - position < batch ? (size_t)(job->capacity - position) : batch;
        // Retain the normal Windows 32-MiB pattern grid while retrying single
        // blocks. Restarting a three-byte TLC pattern at every retry would
        // otherwise report healthy neighbours as corrupt on the read pass.
        if (pattern > 255) PatternFill(expected, pattern, position);
        if (fake) StampBlocks(expected, position / BADBLOCK_BLOCK_SIZE, bytes, tag);
        size_t transferred = 0;
        error = Transfer(job, write ? expected : actual, bytes, position, write, &transferred);
        if (error != 0 && error != EIO) return error;
        const size_t complete = error == 0 ? transferred : transferred / BADBLOCK_BLOCK_SIZE * BADBLOCK_BLOCK_SIZE;
        if (!write) {
            for (size_t offset = 0; offset < complete; offset += BADBLOCK_BLOCK_SIZE) {
                const size_t count = complete - offset < BADBLOCK_BLOCK_SIZE ? complete - offset : BADBLOCK_BLOCK_SIZE;
                if (memcmp(actual + offset, expected + offset, count) != 0)
                    AddBadBlock(job, (position + offset) / BADBLOCK_BLOCK_SIZE, BADBLOCK_CORRUPTION_ERROR);
            }
        }
        position += complete;
        if (error != 0) {
            if (bytes <= BADBLOCK_BLOCK_SIZE && complete == 0) {
                AddBadBlock(job, position / BADBLOCK_BLOCK_SIZE, write ? BADBLOCK_WRITE_ERROR : BADBLOCK_READ_ERROR);
                position += bytes;
            }
            batch = BADBLOCK_BLOCK_SIZE;
            if (recover == UINT64_MAX) recover = position - complete + TEST_BATCH_BYTES;
        } else if (position >= recover) {
            batch = TEST_BATCH_BYTES; recover = UINT64_MAX;
        }
        job->processed = base + position;
        _UpdateProgressWithInfo(OP_BADBLOCKS, 0, job->processed, total, 0);
    }
    return 0;
}

static int TestBlocks(int original, uint64_t device, uint64_t capacity, int pattern,
    bool fake, uint32_t seed, badblock_error_t callback, void* context, BADBLOCK_RESULT* result)
{
    if (result == NULL) return EINVAL;
    memset(result, 0, sizeof(*result));
    if (pattern < 0 || pattern >= BADLOCKS_PATTERN_TYPES) return EINVAL;
    if (capacity < 512 || capacity > maximum_bytes || capacity % 512 != 0) return EINVAL;
    BADBLOCK_JOB job = {.fd = original, .original = original, .device = device, .capacity = capacity,
        .result = result, .callback = callback, .context = context};
    int error = InspectWritableTarget(original, device, &job.initial);
    if (error == 0 && job.initial.size != capacity) error = ESTALE;
    if (error == 0 && device != 0 && ioctl(original, BLKGETDISKSEQ, &job.sequence) != 0) error = errno;
    if (error == 0) error = CheckJob(&job);
    uint8_t* buffer = NULL;
    const size_t alignment = device == 0 || job.initial.sector_size < 4096 ? 4096 : job.initial.sector_size;
    if (error == 0 && posix_memalign((void**)&buffer, alignment, 2 * TEST_BATCH_BYTES) != 0) error = ENOMEM;
    if (error == 0 && device != 0) {
        char path[64]; snprintf(path, sizeof(path), "/proc/self/fd/%d", original);
        job.fd = open(path, O_RDWR | O_DIRECT | O_CLOEXEC);
        if (job.fd < 0) error = errno;
        else {
            struct stat direct;
            if (fstat(job.fd, &direct) != 0) error = errno;
            else if (!S_ISBLK(direct.st_mode) || (uint64_t)direct.st_rdev != device) error = ESTALE;
        }
    }
    const int passes = pattern == 0 ? 1 : pattern == 1 ? 2 : 4;
    const size_t tail = (size_t)(capacity % BADBLOCK_BLOCK_SIZE);
    const size_t smallest = tail != 0 ? tail : BADBLOCK_BLOCK_SIZE;
    const size_t tag = seed % (smallest - sizeof(uint64_t) + 1);
    for (int pass = 0; error == 0 && pass < passes; pass++) {
        PatternFill(buffer, patterns[pattern][pass], 0);
        error = TestPass(&job, buffer, buffer + TEST_BATCH_BYTES, pass, passes, patterns[pattern][pass], fake && pass == 0, tag, true);
        const int sync = SyncJob(&job);
        if (error == 0) error = sync;
        if (error == 0 && !result->threshold_reached)
            error = TestPass(&job, buffer, buffer + TEST_BATCH_BYTES, pass, passes, patterns[pattern][pass], fake && pass == 0, tag, false);
        if (result->threshold_reached) break;
    }
    if (result->bytes_written != 0 && error != 0) SyncJob(&job);
    if (error == 0) error = CheckJob(&job);
    if (job.total != 0) _UpdateProgressWithInfo(OP_BADBLOCKS, 0, job.processed, job.total, 1);
    if (job.fd >= 0 && job.fd != original && close(job.fd) != 0 && error == 0) error = errno;
    free(buffer);
    return error;
}

int TestBadBlocksDevice(int fd, uint64_t device, uint64_t capacity, int pattern,
    bool fake, badblock_error_t callback, void* context, BADBLOCK_RESULT* result)
{
    if (device == 0) { if (result != NULL) memset(result, 0, sizeof(*result)); return EINVAL; }
    if (result == NULL) return EINVAL;
    memset(result, 0, sizeof(*result));
    uint32_t seed = 0;
    if (fake) {
        ssize_t count;
        do { count = getrandom(&seed, sizeof(seed), GRND_NONBLOCK); } while (count < 0 && errno == EINTR);
        if (count != (ssize_t)sizeof(seed)) return count < 0 ? errno : EIO;
    }
    return TestBlocks(fd, device, capacity, pattern, fake, seed, callback, context, result);
}

int TestBadBlocksFile(int fd, int pattern, bool fake, uint32_t seed,
    badblock_error_t callback, void* context, BADBLOCK_RESULT* result)
{
    WRITABLE_TARGET target;
    int error = InspectWritableTarget(fd, 0, &target);
    if (error != 0) { if (result != NULL) memset(result, 0, sizeof(*result)); return error; }
    return TestBlocks(fd, 0, target.size, pattern, fake, seed, callback, context, result);
}

/* ClearMBRGPT housekeeping after a destructive test. Ignore a pending cancel
 * only for these bounded metadata ranges, then flush and read back before the
 * caller releases its claim. Leave the test pattern elsewhere as on Windows. */
int ClearBadBlocksHeaders(int fd, uint64_t device, uint64_t capacity, int* sync_error)
{
    if (sync_error == NULL) return EINVAL;
    *sync_error = 0;
    BADBLOCK_RESULT result = {0};
    BADBLOCK_JOB job = {.fd = fd, .original = fd, .device = device, .capacity = capacity, .result = &result};
    int error = InspectWritableTarget(fd, device, &job.initial);
    if (error == 0 && device != 0 && ioctl(fd, BLKGETDISKSEQ, &job.sequence) != 0) error = errno;
    if (error == 0) error = CheckJob(&job);
    const unsigned sector = device == 0 ? 512 : job.initial.sector_size;
    const uint64_t prefix_limit = (uint64_t)MAX_SECTORS_TO_CLEAR * sector;
    const uint64_t prefix = capacity < prefix_limit ? capacity : prefix_limit;
    const uint64_t remaining = capacity - prefix;
    const uint64_t tail = remaining < prefix_limit / 8 ? remaining : prefix_limit / 8;
    const uint64_t offsets[] = {0, capacity - tail}, lengths[] = {prefix, tail};
    enum { cleanup_bytes = 1024 * 1024 };
    uint8_t* buffer = NULL;
    const size_t alignment = sector < 4096 ? 4096 : sector;
    if (error == 0 && posix_memalign((void**)&buffer, alignment, 2 * cleanup_bytes) != 0) error = ENOMEM;
    if (error == 0 && device != 0) {
        char path[64]; snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
        job.fd = open(path, O_RDWR | O_DIRECT | O_CLOEXEC);
        if (job.fd < 0) error = errno;
    }
    if (buffer != NULL) memset(buffer, 0, cleanup_bytes);
    for (unsigned phase = 0; error == 0 && phase < 2; phase++) {
        for (unsigned range = 0; error == 0 && range < 2; range++) {
            for (uint64_t done = 0; error == 0 && done < lengths[range];) {
                error = CheckJob(&job);
                if (error != 0) break;
                const size_t bytes = lengths[range] - done < cleanup_bytes
                    ? (size_t)(lengths[range] - done) : cleanup_bytes;
                uint8_t* data = phase == 0 ? buffer : buffer + cleanup_bytes;
                size_t transferred = 0;
                while (error == 0 && transferred < bytes) {
                    const off_t offset = (off_t)(offsets[range] + done + transferred);
                    const ssize_t count = phase == 0 ? pwrite(job.fd, data + transferred, bytes - transferred, offset)
                        : pread(job.fd, data + transferred, bytes - transferred, offset);
                    if (count < 0 && errno == EINTR) continue;
                    if (count <= 0) { error = count < 0 ? errno : EIO; break; }
                    if (phase == 0) result.bytes_written += (uint64_t)count;
                    transferred += (size_t)count;
                }
                if (error == 0 && phase != 0 && memcmp(buffer, data, bytes) != 0) error = EBADMSG;
                done += transferred;
            }
        }
        if (phase == 0 && (error == 0 || result.bytes_written != 0)) {
            const int sync = SyncJob(&job);
            if (error == 0) error = sync;
        }
    }
    *sync_error = result.sync_error;
    if (job.fd >= 0 && job.fd != fd && close(job.fd) != 0 && error == 0) error = errno;
    if (error == 0) error = CheckJob(&job);
    if (error == 0 && device != 0 && ioctl(fd, BLKRRPART, 0) != 0) error = errno;
    free(buffer);
    return error;
}
