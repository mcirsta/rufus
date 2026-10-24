/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "zero_write.h"
#include "target_io.h"
#include "rufus_reporting.h"
#include "progress_policy.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { ZERO_IO_BYTES = 1024 * 1024, ZERO_MAP_BUDGET = 64 * 1024 * 1024,
    ZERO_COMPARE_BACKOFF = 15, ZERO_SKIPPED_ZERO = 1, ZERO_SKIPPED_FF = 2 };

static int CheckZeroTarget(int fd, uint64_t device, uint64_t capacity, const WRITABLE_TARGET* initial)
{
    WRITABLE_TARGET current;
    int error = InspectWritableTarget(fd, device, &current);
    if (error != 0) return error;
    const bool same_file = initial == NULL || (current.info.st_dev == initial->info.st_dev
        && current.info.st_ino == initial->info.st_ino);
    if (!same_file || current.size != capacity) return ESTALE;
    return device == 0 ? 0 : CheckRawBlockTarget(fd, device, capacity, capacity);
}

static int ReadZeroBlock(int fd, uint8_t* bytes, size_t count, uint64_t offset)
{
    while (count != 0) {
        if (IsRufusJobCancelled()) return ECANCELED;
        const ssize_t read = pread(fd, bytes, count, (off_t)offset);
        if (read < 0 && errno == EINTR) continue;
        if (read <= 0) return read < 0 ? errno : EIO;
        bytes += read; offset += (uint64_t)read; count -= (size_t)read;
    }
    return 0;
}

static void ZeroProgress(bool format, int phase, uint64_t done, uint64_t total, int forced)
{
    if (!format) { _UpdateProgressWithInfo(phase, 0, done, total, forced); return; }
    if (phase == RAW_WRITE_SYNC || total == 0) return;
    const uint64_t percent = (uint64_t)(45.0 * (double)done / (double)total);
    _UpdateProgressWithInfo(OP_FORMAT, 0, percent + (phase == RAW_WRITE_VERIFY ? 45 : 0), 100, forced);
}

static int ZeroTarget(int fd, uint64_t device, uint64_t capacity, uint64_t base, uint64_t size, int mode, bool format, RAW_WRITE_RESULT* result)
{
    if (result == NULL) return EINVAL;
    memset(result, 0, sizeof(*result));
    if (mode != ZERO_FULL && mode != ZERO_FAST) return EINVAL;
    if (capacity < 512 || capacity > INT64_MAX || capacity % 512 != 0) return EINVAL;
    if (base > capacity || size == 0 || size > capacity - base || base % 512 != 0 || size % 512 != 0) return EINVAL;
    const uint64_t regions = (size + ZERO_COMPARE_BYTES - 1) / ZERO_COMPARE_BYTES;
    if (mode == ZERO_FAST && regions > ZERO_MAP_BUDGET) return ENOTSUP;
    WRITABLE_TARGET initial;
    int error = InspectWritableTarget(fd, device, &initial);
    if (error == 0) error = CheckZeroTarget(fd, device, capacity, &initial);
    if (error != 0) return error;
    uint8_t* buffer = malloc(2 * ZERO_IO_BYTES);
    uint8_t* compare = buffer != NULL ? buffer + ZERO_IO_BYTES : NULL;
    uint8_t* skipped = mode == ZERO_FAST ? calloc((size_t)regions, 1) : NULL;
    if (buffer == NULL || (mode == ZERO_FAST && skipped == NULL)) { free(buffer); free(skipped); return ENOMEM; }
    uint64_t copied = 0;
    unsigned backoff = 0;
    result->phase = RAW_WRITE_COPY;
    ZeroProgress(format, RAW_WRITE_COPY, 0, size, 1);
    for (uint64_t region = 0; error == 0 && region < regions; region++) {
        const uint64_t start = region * ZERO_COMPARE_BYTES;
        const uint64_t end = size - start < ZERO_COMPARE_BYTES ? size : start + ZERO_COMPARE_BYTES;
        bool empty = mode == ZERO_FAST && backoff == 0;
        uint8_t value = 0;
        if (empty) {
            for (uint64_t offset = start; error == 0 && offset < end; offset += ZERO_IO_BYTES) {
                const size_t bytes = end - offset < ZERO_IO_BYTES ? (size_t)(end - offset) : ZERO_IO_BYTES;
                error = ReadZeroBlock(fd, buffer, bytes, base + offset);
                if (error == 0 && offset == start) { value = buffer[0]; memset(compare, value, ZERO_IO_BYTES); }
                if (error == 0 && (value != 0 && value != 0xff)) empty = false;
                if (error == 0 && empty && memcmp(buffer, compare, bytes) != 0) empty = false;
            }
            if (error == 0 && empty) {
                skipped[region] = value == 0 ? ZERO_SKIPPED_ZERO : ZERO_SKIPPED_FF;
                copied = end;
                ZeroProgress(format, RAW_WRITE_COPY, copied, size, 0);
                continue;
            }
            backoff = ZERO_COMPARE_BACKOFF;
        } else if (backoff != 0) backoff--;
        memset(buffer, mode == ZERO_FAST ? 0xff : 0, ZERO_IO_BYTES);
        for (uint64_t offset = start; error == 0 && offset < end;) {
            if (IsRufusJobCancelled()) { error = ECANCELED; break; }
            const size_t bytes = end - offset < ZERO_IO_BYTES ? (size_t)(end - offset) : ZERO_IO_BYTES;
            const ssize_t written = pwrite(fd, buffer, bytes, (off_t)(base + offset));
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) { error = written < 0 ? errno : EIO; break; }
            result->bytes_written += (uint64_t)written; offset += (uint64_t)written; copied = offset;
            ZeroProgress(format, RAW_WRITE_COPY, copied, size, 0);
        }
    }
    if (error == 0 || result->bytes_written != 0) {
        ZeroProgress(format, RAW_WRITE_COPY, copied, size, 1);
        result->phase = RAW_WRITE_SYNC;
        ZeroProgress(format, RAW_WRITE_SYNC, 0, 0, 1);
        while (fsync(fd) != 0) {
            if (errno == EINTR) continue;
            result->sync_error = errno; break;
        }
    }
    if (error == 0) error = result->sync_error;
    if (error == 0) error = CheckZeroTarget(fd, device, capacity, &initial);
    if (error == 0) {
        result->phase = RAW_WRITE_VERIFY;
        ZeroProgress(format, RAW_WRITE_VERIFY, 0, size, 1);
        for (uint64_t offset = 0; error == 0 && offset < size;) {
            const size_t bytes = size - offset < ZERO_IO_BYTES ? (size_t)(size - offset) : ZERO_IO_BYTES;
            const uint8_t skipped_region = skipped != NULL ? skipped[offset / ZERO_COMPARE_BYTES] : 0;
            const uint8_t expected = mode == ZERO_FULL || skipped_region == ZERO_SKIPPED_ZERO ? 0 : 0xff;
            error = ReadZeroBlock(fd, buffer, bytes, base + offset);
            memset(compare, expected, ZERO_IO_BYTES);
            if (error == 0 && memcmp(buffer, compare, bytes) != 0) error = EBADMSG;
            if (error != 0) break;
            if (skipped_region == 0) result->bytes_verified += bytes;
            offset += bytes;
            ZeroProgress(format, RAW_WRITE_VERIFY, offset, size, 0);
        }
    }
    if (error == 0) error = CheckZeroTarget(fd, device, capacity, &initial);
    if (error == 0 && IsRufusJobCancelled()) error = ECANCELED;
    if (error == 0) {
        ZeroProgress(format, RAW_WRITE_VERIFY, size, size, 1);
        result->phase = RAW_WRITE_DONE;
    }
    free(skipped); free(buffer);
    return error;
}

int ZeroDeviceData(int fd, uint64_t device, uint64_t capacity, int mode, RAW_WRITE_RESULT* result)
{
    if (device == 0) { if (result != NULL) memset(result, 0, sizeof(*result)); return EINVAL; }
    return ZeroTarget(fd, device, capacity, 0, capacity, mode, false, result);
}

int ZeroFileData(int fd, int mode, RAW_WRITE_RESULT* result)
{
    WRITABLE_TARGET target;
    const int error = InspectWritableTarget(fd, 0, &target);
    if (error != 0) { if (result != NULL) memset(result, 0, sizeof(*result)); return error; }
    return ZeroTarget(fd, 0, target.size, 0, target.size, mode, false, result);
}

int ZeroFormatExtent(int fd, uint64_t device, uint64_t offset, uint64_t bytes, RAW_WRITE_RESULT* result)
{
    WRITABLE_TARGET target;
    int error = InspectWritableTarget(fd, device, &target);
    if (error == 0 && device != 0 && (offset != 0 || target.size != bytes)) error = EINVAL;
    if (error != 0) { if (result != NULL) memset(result, 0, sizeof(*result)); return error; }
    return ZeroTarget(fd, device, target.size, offset, bytes, ZERO_FULL, true, result);
}
