// SPDX-License-Identifier: GPL-3.0-or-later
#include "device_exclusions.h"
#include "native_disk_facts.h"
#include <QFile>
#include <QtEndian>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <zlib.h>
namespace rufus
{
namespace
{
int diskGuid(int fd, quint64 device, QString& guid)
{
    struct stat info = {};
    if (fstat(fd, &info) != 0) return errno;
    if (!S_ISBLK(info.st_mode) || quint64(info.st_rdev) != device) return ESTALE;
    unsigned sector = 0;
    if (ioctl(fd, BLKSSZGET, &sector) != 0) return errno;
    if (sector < 512 || sector > 65536 || (sector & (sector - 1)) != 0) return EINVAL;
    QByteArray primary(qsizetype(sector), Qt::Uninitialized), backup(qsizetype(sector), Qt::Uninitialized);
    ssize_t count;
    do { count = pread(fd, primary.data(), sector, sector); } while (count < 0 && errno == EINTR);
    if (count != ssize_t(sector)) return count < 0 ? errno : EIO;
    auto* header = reinterpret_cast<unsigned char*>(primary.data());
    if (memcmp(header, "EFI PART", 8) != 0) {
        unsigned char mbr[512];
        if (pread(fd, mbr, sizeof(mbr), 0) != ssize_t(sizeof(mbr))) return EIO;
        for (unsigned entry = 0; entry < 4; ++entry) if (mbr[446 + entry * 16 + 4] == 0xee) return EBADMSG;
        guid.clear(); return 0;
    }
    quint64 capacity = 0;
    if (ioctl(fd, BLKGETSIZE64, &capacity) != 0) return errno;
    if (capacity < 3 * quint64(sector) || capacity % sector != 0) return EBADMSG;
    const quint64 finalSector = capacity / sector - 1;
    if (qFromLittleEndian<quint64>(header + 24) != 1 || qFromLittleEndian<quint64>(header + 32) != finalSector) return EBADMSG;
    do { count = pread(fd, backup.data(), sector, off_t(finalSector * sector)); } while (count < 0 && errno == EINTR);
    if (count != ssize_t(sector)) return count < 0 ? errno : EIO;
    for (QByteArray* copy : {&primary, &backup}) {
        auto* bytes = reinterpret_cast<unsigned char*>(copy->data());
        const quint32 length = qFromLittleEndian<quint32>(bytes + 12), expected = qFromLittleEndian<quint32>(bytes + 16);
        if (memcmp(bytes, "EFI PART", 8) != 0 || length < 92 || length > sector) return EBADMSG;
        qToLittleEndian<quint32>(0, bytes + 16);
        const quint32 actual = quint32(crc32(0, bytes, length));
        qToLittleEndian(expected, bytes + 16);
        if (actual != expected) return EBADMSG;
    }
    const auto* secondary = reinterpret_cast<const unsigned char*>(backup.constData());
    if (qFromLittleEndian<quint64>(secondary + 24) != finalSector || qFromLittleEndian<quint64>(secondary + 32) != 1
        || memcmp(header + 56, secondary + 56, 16) != 0) return EBADMSG;
    const QUuid id(qFromLittleEndian<quint32>(header + 56), qFromLittleEndian<quint16>(header + 60),
        qFromLittleEndian<quint16>(header + 62), header[64], header[65], header[66], header[67],
        header[68], header[69], header[70], header[71]);
    guid = id.toString();
    return id.isNull() ? EBADMSG : 0;
}
int cachedDiskGuid(quint64 device, QString& guid)
{
    QFile file(QStringLiteral("/run/udev/data/b%1:%2").arg(major(dev_t(device))).arg(minor(dev_t(device))));
    if (!file.open(QIODevice::ReadOnly)) return EACCES;
    const QByteArray metadata = file.read(1024 * 1024 + 1);
    if (file.error() != QFileDevice::NoError || metadata.size() > 1024 * 1024) return EIO;
    QByteArray type, uuid;
    for (const QByteArray& line : metadata.split('\n')) {
        if (line.startsWith("E:ID_PART_TABLE_TYPE=")) type = line.mid(21);
        if (line.startsWith("E:ID_PART_TABLE_UUID=")) uuid = line.mid(21);
    }
    if (type != "gpt") { guid.clear(); return 0; }
    const QUuid id(QString::fromLatin1(uuid));
    if (id.isNull()) return EBADMSG;
    guid = id.toString(); return 0;
}
}
int CheckDeviceExclusions(const QString& path, quint64 device, quint64 size,
    const DeviceExclusions& exclusions, int descriptor)
{
    if (!exclusions.valid()) return EINVAL;
    if (exclusions.empty()) return 0;
    NativeDiskFacts facts;
    const int error = ReadNativeDiskFacts(device, size, facts);
    if (error != 0) return error;
    const quint32 usbId = (quint32(facts.usbVendor) << 16) | facts.usbProduct;
    if (facts.usbIdsKnown && exclusions.usb.contains(usbId)) return EPERM;
    if (exclusions.disks.isEmpty()) return 0;
    QString guid;
    int checked = 0;
    if (descriptor >= 0) checked = diskGuid(descriptor, device, guid);
    else {
        const int fd = open(QFile::encodeName(path).constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        if (fd >= 0) { checked = diskGuid(fd, device, guid); close(fd); }
        else checked = cachedDiskGuid(device, guid);
    }
    if (checked != 0) return checked;
    return exclusions.disks.contains(guid) ? EPERM : 0;
}
}
