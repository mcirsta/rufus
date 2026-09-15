// SPDX-License-Identifier: GPL-3.0-or-later
#include "prepare_device.h"
#include "udisks_client.h"
#include "rufus_reporting.h"

#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusReply>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QThread>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

namespace rufus
{
namespace
{
constexpr int sectorBytes = 512;
constexpr int snapshotTimeoutMs = 5000;
constexpr int partitionTimeoutMs = 5000;
constexpr int partitionPollMs = 25;
constexpr int diskOpenFlags = O_RDWR | O_EXCL | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK;
constexpr int gptTableBytes = 128 * 128;
constexpr int gptPrimaryBytes = gptTableBytes + 2 * sectorBytes;
constexpr int gptBackupBytes = gptTableBytes + sectorBytes;

QString deviceNumbers(quint64 device)
{
    return QStringLiteral("%1:%2").arg(major(dev_t(device))).arg(minor(dev_t(device)));
}

quint64 sysfsNumber(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return 0;
    bool valid = false;
    const quint64 value = file.read(128).trimmed().toULongLong(&valid);
    return valid ? value : 0;
}

bool readAt(int fd, char* bytes, size_t size, quint64 offset)
{
    ssize_t count;
    do { count = pread(fd, bytes, size, off_t(offset)); }
    while (count < 0 && errno == EINTR);
    return count == ssize_t(size);
}
}

quint64 ReadDiskSequence(quint64 device)
{
    return sysfsNumber(QStringLiteral("/sys/dev/block/") + deviceNumbers(device) + QStringLiteral("/diskseq"));
}

PrepareDeviceTarget::PrepareDeviceTarget(const PrepareImageRequest& request) : request_(request)
{
    qDBusRegisterMetaType<UDisksInterfaces>();
    qDBusRegisterMetaType<UDisksObjects>();
}

PrepareDeviceTarget::~PrepareDeviceTarget()
{
    if (fd_ >= 0)
        ::close(fd_);
}

int PrepareDeviceTarget::checkSnapshot()
{
    const QDBusConnection connection = QDBusConnection::systemBus();
    QDBusMessage message = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.DBus"),
        QStringLiteral("/org/freedesktop/DBus"), QStringLiteral("org.freedesktop.DBus"), QStringLiteral("GetNameOwner"));
    message.setArguments({QStringLiteral("org.freedesktop.UDisks2")});
    const QDBusReply<QString> owner = connection.call(message, QDBus::Block, snapshotTimeoutMs);
    if (!owner.isValid())
        return EHOSTDOWN;
    if (owner.value() != request_.serviceOwner)
        return ESTALE;
    message = QDBusMessage::createMethodCall(owner.value(), QStringLiteral("/org/freedesktop/UDisks2"),
        QStringLiteral("org.freedesktop.DBus.ObjectManager"), QStringLiteral("GetManagedObjects"));
    const QDBusReply<UDisksObjects> reply = connection.call(message, QDBus::Block, snapshotTimeoutMs);
    if (!reply.isValid())
        return EHOSTDOWN;
    const UDisksObjects objects = reply.value();
    Device current;
    if (!DescribeUDisksDevice(objects, request_.objectPath, current))
        return ENOTSUP;
    const bool sameIdentity = current.number == request_.device && current.identity == request_.identity;
    const bool sameMedia = current.path == request_.path && current.size == request_.size;
    if (!sameIdentity || !sameMedia)
        return ESTALE;
    const QString problem = CheckUDisksTargetUse(objects, current, 0);
    if (!problem.isEmpty())
    {
        uprintfs(problem.toUtf8().constData());
        return EBUSY;
    }
    return IsRufusJobCancelled() ? ECANCELED : 0;
}

int PrepareDeviceTarget::checkDescriptor(int fd, quint64 device, quint64 size)
{
    struct stat info;
    uint64_t capacity = 0, sequence = 0;
    int sector = 0, readOnly = 1;
    if (fstat(fd, &info) != 0)
        return errno;
    if (!S_ISBLK(info.st_mode) || quint64(info.st_rdev) != device)
        return ESTALE;
    if (ioctl(fd, BLKGETSIZE64, &capacity) != 0 || ioctl(fd, BLKGETDISKSEQ, &sequence) != 0)
        return errno;
    if (ioctl(fd, BLKSSZGET, &sector) != 0 || ioctl(fd, BLKROGET, &readOnly) != 0)
        return errno;
    if (capacity != size || sequence != request_.diskSequence)
        return ESTALE;
    if (readOnly)
        return EROFS;
    if (sector != sectorBytes)
        return ENOTSUP;
    return 0;
}

int PrepareDeviceTarget::check()
{
    const int error = checkDescriptor(fd_, request_.device, request_.size);
    return error == 0 ? checkSnapshot() : error;
}

int PrepareDeviceTarget::open()
{
    if (getuid() != 0 || geteuid() != 0 || !request_.isDevice || fd_ >= 0)
        return EPERM;
    const int error = checkSnapshot();
    if (error != 0)
        return error;
    fd_ = ::open(QFile::encodeName(request_.path).constData(), diskOpenFlags);
    if (fd_ < 0)
        return errno;
    return check();
}

int PrepareDeviceTarget::openPartition(const FAT32_FORMAT_OPTIONS& options, quint64& device)
{
    const QString parent = QStringLiteral("/sys/dev/block/") + deviceNumbers(request_.device);
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < partitionTimeoutMs)
    {
        if (IsRufusJobCancelled())
            return -ECANCELED;
        if (ReadDiskSequence(request_.device) != request_.diskSequence)
            return -ESTALE;
        const QStringList entries = QDir(parent).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString& entry : entries)
        {
            const QString path = parent + QLatin1Char('/') + entry;
            if (sysfsNumber(path + QStringLiteral("/partition")) != 1)
                continue;
            const bool correctStart = sysfsNumber(path + QStringLiteral("/start")) == options.offset / sectorBytes;
            const bool correctSize = sysfsNumber(path + QStringLiteral("/size")) == options.size / sectorBytes;
            if (!correctStart || !correctSize)
                return -ESTALE;
            QFile number(path + QStringLiteral("/dev"));
            if (!number.open(QIODevice::ReadOnly))
                return -EIO;
            const QList<QByteArray> parts = number.read(128).trimmed().split(':');
            if (parts.size() != 2)
                return -EIO;
            bool majorValid = false, minorValid = false;
            const uint majorNumber = parts[0].toUInt(&majorValid);
            const uint minorNumber = parts[1].toUInt(&minorValid);
            if (!majorValid || !minorValid)
                return -EIO;
            device = quint64(makedev(majorNumber, minorNumber));
            const QByteArray node = QFile::encodeName(QStringLiteral("/dev/block/") + deviceNumbers(device));
            const int fd = ::open(node.constData(), O_RDWR | O_CLOEXEC | O_NONBLOCK);
            if (fd < 0 && errno == ENOENT)
                break; // Wait for udev to publish the kernel partition's node.
            if (fd < 0)
                return -errno;
            const int error = checkDescriptor(fd, device, options.size);
            if (error == 0)
                return fd;
            ::close(fd);
            return -error;
        }
        QThread::msleep(partitionPollMs);
    }
    return -ETIMEDOUT;
}

int PrepareDeviceTarget::format(const FAT32_FORMAT_OPTIONS& options, const char* mkfs, FAT32_FORMAT_RESULT& result)
{
    char primary[gptPrimaryBytes], backup[gptBackupBytes], readback[gptPrimaryBytes];
    if (request_.size < sizeof(primary) + sizeof(backup))
        return EINVAL;
    const quint64 backupOffset = request_.size - sizeof(backup);
    int error = check();
    if (error != 0)
        return error;
    const bool savedPrimary = readAt(fd_, primary, sizeof(primary), 0);
    const bool savedBackup = readAt(fd_, backup, sizeof(backup), backupOffset);
    if (!savedPrimary || !savedBackup)
        return EIO;
    if (ioctl(fd_, BLKRRPART, 0) != 0)
        return errno;
    quint64 partitionDevice = 0;
    const int partition = openPartition(options, partitionDevice);
    if (partition < 0)
        return -partition;
    // The non-exclusive partition fd identifies the extent for mkfs's own open.
    // No whole-disk claim is inherited by the formatter.
    const int released = ::close(fd_);
    fd_ = -1;
    if (released != 0)
        error = errno;
    if (error == 0)
        error = checkSnapshot();
    if (error == 0)
        error = checkDescriptor(partition, partitionDevice, options.size);
    if (error == 0)
        error = FormatFat32Partition(partition, partitionDevice, mkfs, &options, &result);
    if (::close(partition) != 0 && error == 0)
        error = errno;
    // Attempt reacquisition even on cancellation/failure, without another launch
    // or authorization. A busy or replaced disk is never bypassed or retried.
    fd_ = ::open(QFile::encodeName(request_.path).constData(), diskOpenFlags);
    int reacquired = fd_ < 0 ? errno : checkDescriptor(fd_, request_.device, request_.size);
    if (reacquired == 0)
    {
        const bool primaryMatches = readAt(fd_, readback, sizeof(primary), 0)
            && memcmp(primary, readback, sizeof(primary)) == 0;
        const bool backupMatches = readAt(fd_, readback, sizeof(backup), backupOffset)
            && memcmp(backup, readback, sizeof(backup)) == 0;
        if (!primaryMatches || !backupMatches)
            reacquired = EBADMSG;
    }
    if (reacquired != 0)
        uprintf("Cannot revalidate the disk after formatting: %s", strerror(reacquired));
    return error != 0 ? error : reacquired;
}
}
