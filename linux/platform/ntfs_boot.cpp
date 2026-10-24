// SPDX-License-Identifier: GPL-3.0-or-later
#include "ntfs_boot.h"
#include "target_io.h"
#include "rufus_reporting.h"
extern "C" {
#include "ms-sys/inc/ntfs.h"
}
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <unistd.h>

namespace rufus
{
namespace
{
constexpr size_t sectorBytes = 512;
constexpr size_t ntfsCodeOffset = 0x54;
constexpr size_t ntfsCodeBytes = 4052; // Size of Rufus's br_ntfs_0x54.h array.
constexpr size_t recordBytes = ntfsCodeOffset + ntfsCodeBytes;

int transferBytes(int fd, unsigned char* data, size_t size, quint64 offset,
    bool writing, NtfsBootResult& result)
{
    size_t done = 0;
    while (done < size)
    {
        if (IsRufusJobCancelled())
            return ECANCELED;
        const ssize_t count = writing
            ? pwrite(fd, data + done, size - done, off_t(offset + done))
            : pread(fd, data + done, size - done, off_t(offset + done));
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return count == 0 ? EIO : errno;
        done += size_t(count);
        if (writing)
            result.bytesWritten += quint64(count);
    }
    return 0;
}

int stageRecord(unsigned char* record)
{
    FILE* stream = fmemopen(record, recordBytes + 1, "r+b");
    if (stream == nullptr)
        return errno;
    const bool valid = is_ntfs_fs(stream);
    const bool written = valid && write_ntfs_br(stream);
    const bool matches = written && entire_ntfs_br_matches(stream);
    int error = matches ? 0 : EBADMSG;
    if (fclose(stream) != 0 && error == 0)
        error = EIO;
    return error;
}
}

int InstallNtfsBootRecord(int fd, quint64 device, const NtfsFormatOptions& options,
    NtfsBootResult& result)
{
    result = {};
    if (options.sectorSize != sectorBytes) return ENOTSUP;
    int error = CheckNtfsFormattedVolume(fd, device, options);
    if (error != 0)
        return error;
    unsigned char record[recordBytes + 1] = {};
    unsigned char original[recordBytes] = {};
    error = transferBytes(fd, original, recordBytes, 0, false, result);
    if (error != 0)
        return error;
    memcpy(record, original, recordBytes);
    error = stageRecord(record);
    if (error != 0)
        return error;
    error = transferBytes(fd, record, recordBytes, 0, true, result);
    if (result.bytesWritten != 0)
    {
        while (fsync(fd) != 0)
        {
            if (errno == EINTR)
                continue;
            result.syncError = errno;
            break;
        }
    }
    if (error == 0)
        error = result.syncError;
    unsigned char actual[recordBytes] = {};
    if (error == 0)
        error = transferBytes(fd, actual, recordBytes, 0, false, result);
    if (error == 0 && memcmp(actual, record, recordBytes) != 0)
        error = EBADMSG;
    if (error == 0)
        error = transferBytes(fd, actual, sectorBytes, options.size - sectorBytes, false, result);
    if (error == 0 && memcmp(actual, original, sectorBytes) != 0)
        error = EBADMSG; // Windows changes only the primary NTFS boot record.
    if (error == 0)
    {
        WRITABLE_TARGET target;
        error = InspectWritableTarget(fd, device, &target);
        if (error == 0 && target.size != options.size)
            error = ESTALE;
    }
    if (error == 0 && IsRufusJobCancelled())
        error = ECANCELED;
    return error;
}
}
