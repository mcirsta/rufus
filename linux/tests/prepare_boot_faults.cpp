// SPDX-License-Identifier: GPL-3.0-or-later
// Guest-only wrappers; the production helper has no payload fault controls.
#include "platform/uefi_ntfs.h"
#include "platform/rufus_reporting.h"
#include <QByteArrayView>
#include <QCryptographicHash>
#include <QFile>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

namespace
{
constexpr quint64 fixtureBytes = 64 * 1024 * 1024;
constexpr size_t partialBytes = 4096;
constexpr off_t corruptionOffset = 23;
QByteArray fault;
int activeTarget = -1;
uint64_t bootOffset;
bool wrote;

QByteArray outsideDigest(int fd, uint64_t offset, uint64_t payloadBytes)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QByteArray buffer(64 * 1024, Qt::Uninitialized);
    const uint64_t starts[] = {0, offset + payloadBytes};
    const uint64_t ends[] = {offset, fixtureBytes};
    for (size_t region = 0; region < 2; ++region)
    {
        uint64_t position = starts[region];
        while (position < ends[region])
        {
            const size_t count = size_t(qMin(uint64_t(buffer.size()), ends[region] - position));
            const ssize_t read = pread(fd, buffer.data(), count, off_t(position));
            if (read < 0 && errno == EINTR)
                continue;
            if (read <= 0)
                qFatal("Cannot hash bytes outside the boot payload");
            hash.addData(QByteArrayView(buffer.constData(), qsizetype(read)));
            position += uint64_t(read);
        }
    }
    return hash.result();
}
}

extern "C" int __real_WriteUefiNtfsImage(const IMAGE_FILE_SOURCE*, const char*, int,
    const GPT_IMAGE_OPTIONS*, RAW_WRITE_RESULT*);
extern "C" ssize_t __real_pwrite64(int, const void*, size_t, off_t);
extern "C" int __real_fsync(int);

extern "C" ssize_t __wrap_pwrite64(int fd, const void* data, size_t size, off_t offset)
{
    if (fd != activeTarget || fault != "pause" || wrote)
        return __real_pwrite64(fd, data, size, offset);
    const ssize_t count = __real_pwrite64(fd, data, qMin(size, partialBytes), offset);
    if (count <= 0)
        return count;
    wrote = true;
    QFile marker(QStringLiteral("/run/rufus-boot-writer.pid"));
    if (!marker.open(QIODevice::WriteOnly) || marker.write(QByteArray::number(getpid())) <= 0)
        qFatal("Cannot publish paused boot writer");
    marker.close();
    while (!IsRufusJobCancelled())
        usleep(10000);
    return count;
}

extern "C" int __wrap_fsync(int fd)
{
    const int result = __real_fsync(fd);
    if (result != 0 || fd != activeTarget)
        return result;
    if (fault == "sync")
    {
        errno = EIO;
        return -1;
    }
    if (fault == "corrupt")
    {
        char byte;
        const off_t offset = off_t(bootOffset) + corruptionOffset;
        if (pread(fd, &byte, 1, offset) != 1)
            qFatal("Cannot read corruption byte");
        byte ^= 1;
        if (__real_pwrite64(fd, &byte, 1, offset) != 1 || __real_fsync(fd) != 0)
            qFatal("Cannot inject boot corruption");
    }
    return 0;
}

extern "C" int __wrap_WriteUefiNtfsImage(const IMAGE_FILE_SOURCE* source, const char* path,
    int target, const GPT_IMAGE_OPTIONS* options, RAW_WRITE_RESULT* result)
{
    QFile marker(QStringLiteral("/etc/rufus-ntfs-fixture"));
    if (geteuid() != 0 || !marker.open(QIODevice::ReadOnly) || marker.readAll() != "RUFUS-NTFS-TEST\n")
        qFatal("Boot fault probe requires the NTFS fixture guest");
    if (options->disk_size != fixtureBytes || options->device == 0)
        qFatal("Unexpected fixture target");
    QFile mode(QStringLiteral("/home/rufus/boot-fault"));
    if (!mode.open(QIODevice::ReadOnly))
        qFatal("Missing boot fault mode");
    fault = mode.readAll();
    GPT_IMAGE_RESULT layout;
    if (PlanGptImagePartition(target, options, &layout) != 0)
        qFatal("Invalid boot fixture layout");
    const int other = open("/dev/sda", O_RDWR | O_EXCL | O_CLOEXEC);
    const int error = errno;
    if (other >= 0)
        close(other);
    if (other >= 0 || error != EBUSY)
        qFatal("Whole-disk claim was lost before writing the boot payload");
    bootOffset = layout.uefi_ntfs_offset;
    const QByteArray before = outsideDigest(target, bootOffset, options->uefi_ntfs_size);
    activeTarget = target;
    const int outcome = __real_WriteUefiNtfsImage(source, path, target, options, result);
    activeTarget = -1;
    if (outsideDigest(target, bootOffset, options->uefi_ntfs_size) != before)
        qFatal("Boot writer changed bytes outside its payload");
    QFile report(QStringLiteral("/run/rufus-boot-report"));
    const QByteArray text = QByteArray::number(getpid()) + ' ' + QByteArray::number(outcome) + ' '
        + QByteArray::number(result->bytes_written) + ' ' + QByteArray::number(result->bytes_verified);
    if (!report.open(QIODevice::WriteOnly | QIODevice::Truncate) || report.write(text) != text.size())
        qFatal("Cannot publish boot boundary check");
    uprintf("RUFUS_BOOT_OUTSIDE_PRESERVED WRITTEN=%llu VERIFIED=%llu",
        quint64(result->bytes_written), quint64(result->bytes_verified));
    return outcome;
}
