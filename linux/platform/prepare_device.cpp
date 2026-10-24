// SPDX-License-Identifier: GPL-3.0-or-later
#include "prepare_device.h"
#include "device_exclusions.h"
#include "virtual_disk.h"
#include "udisks_client.h"
#include "prepare_source.h"
#include "format_options.h"
#include "target_mounts.h"
#include "rufus_reporting.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QProcessEnvironment>
#include <QThread>
#include <QVersionNumber>
#include <QtEndian>
#include "compressed_ffu.h"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/mount.h>
#include <linux/fs.h>
#include "filesystem_magic.h"
#include <sys/ioctl.h>
#include <linux/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/utsname.h>
#include <unistd.h>

namespace rufus
{
int PrepareIsoSource::checkParentBacking(int disk, int callerHome) const
{
    for (const VIRTUAL_DISK_NODE* node = source_.virtual_disk; node; node = node->parent) {
        NativeTargetMounts mounts;
        if (node != source_.virtual_disk) {
            const int error = CheckNativeTargetMounts(disk, node->source.fd, mounts, callerHome);
            if (error != 0) return error;
        }
        if (node->relocation.fd >= 0) {
            const int error = CheckNativeTargetMounts(disk, node->relocation.fd, mounts, callerHome);
            if (error != 0) return error;
        }
    }
    return check();
}


namespace
{
constexpr int sectorBytes = 512;
constexpr int snapshotTimeoutMs = 5000;
constexpr int partitionTimeoutMs = 5000;
constexpr int partitionPollMs = 25;
constexpr int diskOpenFlags = O_RDWR | O_EXCL | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK;
constexpr int gptTableBytes = 128 * 128;
constexpr int mbrPartitionOffset = 446;
constexpr int mbrPartitionBytes = 4 * 16;
constexpr int mbrDiskSignatureOffset = 440;
constexpr quint32 mainPartitionNumber = 1;
constexpr quint32 uefiPartitionNumber = 2;
constexpr long ntfsMagic = 0x5346544e;
constexpr int mountTimeoutMs = 10000;
constexpr int unmountTimeoutMs = 5000;
constexpr int terminateGraceMs = 500;

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

PrepareDeviceTarget::PrepareDeviceTarget(const PrepareImageRequest& request, Table table) :
    request_(request), table_(table)
{
}

PrepareDeviceTarget::~PrepareDeviceTarget()
{
    if (!cleanupAttempted_ && (partition_ >= 0 || !mountPath_.isEmpty()))
    {
        int syncError = 0;
        const int error = finishMount(syncError);
        if (error != 0 || syncError != 0)
            uprintf("Cannot finish mount cleanup: %s", strerror(error != 0 ? error : syncError));
    }
    if (fd_ >= 0)
        ::close(fd_);
    if (callerHome_ >= 0)
        ::close(callerHome_);
}

int PrepareDeviceTarget::checkSnapshot()
{
    return CheckPrepareDeviceSelection(QDBusConnection::systemBus(), request_);
}

int PrepareDeviceTarget::finishRaw()
{
    return table_ == Table::Raw ? finish() : EINVAL;
}

int PrepareDeviceTarget::finish()
{
    const bool noMount = partition_ < 0 && directory_ < 0 && !mounted_ && mountPath_.isEmpty();
    if (!noMount)
        return EINVAL;
    int error = 0;
    if (fd_ >= 0)
    {
        const int descriptor = fd_;
        fd_ = -1;
        if (::close(descriptor) != 0)
            error = errno;
    }
    if (callerHome_ >= 0)
    {
        const int descriptor = callerHome_;
        callerHome_ = -1;
        if (::close(descriptor) != 0 && error == 0)
            error = errno;
    }
    return error;
}

int PrepareDeviceTarget::checkDescriptor(int fd, quint64 device, quint64 size)
{
    struct stat info;
    uint64_t capacity = 0, sequence = 0;
    int sector = 0, physicalSector = 0, readOnly = 1;
    if (fstat(fd, &info) != 0)
        return errno;
    if (!S_ISBLK(info.st_mode) || quint64(info.st_rdev) != device)
        return ESTALE;
    if (ioctl(fd, BLKGETSIZE64, &capacity) != 0 || ioctl(fd, BLKGETDISKSEQ, &sequence) != 0)
        return errno;
    if (ioctl(fd, BLKSSZGET, &sector) != 0 || ioctl(fd, BLKPBSZGET, &physicalSector) != 0 || ioctl(fd, BLKROGET, &readOnly) != 0)
        return errno;
    if (capacity != size || sequence != request_.diskSequence)
        return ESTALE;
    if (readOnly)
        return EROFS;
    // Preserve DD's existing range; formatted jobs must match their frozen geometry.
    constexpr int maximumRawSectorBytes = 64 * 1024;
    const bool rawSector = sector >= sectorBytes && sector <= maximumRawSectorBytes
        && (sector & (sector - 1)) == 0;
    const bool supportedSector = table_ == Table::Raw ? rawSector : ValidFormatGeometry(request_);
    if (table_ != Table::Raw && (quint32(sector) != request_.logicalSectorSize
        || quint32(physicalSector) != request_.physicalSectorSize)) return ESTALE;
    if (!supportedSector)
        return ENOTSUP;
    return 0;
}

int PrepareDeviceTarget::check()
{
    const int error = checkDescriptor(fd_, request_.device, request_.size);
    return error == 0 ? checkSnapshot() : error;
}

int PrepareDeviceTarget::waitForUnmountSnapshot()
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < snapshotTimeoutMs)
    {
        UDisksObjects objects;
        Device current;
        const int error = ReadPrepareDeviceSnapshot(QDBusConnection::systemBus(), request_, objects, current);
        if (error != 0)
            return error;
        QStringList mounts;
        const QString problem = ListUDisksTargetMounts(objects, current, request_.imageDevice, mounts, request_.allowFixedUsb);
        if (!problem.isEmpty())
        {
            uprintfs(problem.toUtf8().constData());
            return EBUSY;
        }
        if (mounts.isEmpty())
            return 0;
        QThread::msleep(partitionPollMs);
    }
    return ETIMEDOUT;
}

int PrepareDeviceTarget::prepareTargetMounts(uid_t caller, const PrepareIsoSource* source, const PrepareIsoSource* archive)
{
    const bool needsSource = !request_.imagePath.isEmpty();
    if (needsSource != (source != nullptr))
        return EINVAL;
    int error = source ? source->check() : 0;
    if (error == 0 && archive) error = archive->check();
    if (error == 0)
        error = OpenNativeCallerHome(caller, callerHome_);
    if (error != 0)
        return error;
    const int sourceFd = source ? source->retainedSource().fd : -1;
    fd_ = ::open(QFile::encodeName(request_.path).constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd_ < 0)
        return errno;
    error = checkDescriptor(fd_, request_.device, request_.size);
    UDisksObjects objects;
    Device current;
    if (error == 0)
        error = ReadPrepareDeviceSnapshot(QDBusConnection::systemBus(), request_, objects, current);
    NativeTargetMounts native;
    if (error == 0)
        error = CheckNativeTargetMounts(fd_, sourceFd, native, callerHome_);
    if (error == 0 && source)
        error = source->checkParentBacking(fd_, callerHome_);
    if (error == 0 && archive) {
        NativeTargetMounts additional;
        error = CheckNativeTargetMounts(fd_, archive->retainedSource().fd, additional, callerHome_);
    }
    if (error == 0)
    {
        const QString problem = MatchUDisksTargetMounts(objects, current, request_.imageDevice,
            request_.diskSequence, native, request_.allowFixedUsb);
        if (!problem.isEmpty())
        {
            uprintfs(problem.toUtf8().constData());
            error = ESTALE;
        }
    }
    int unmounted = 0;
    if (error == 0)
        error = UnmountNativeTarget(fd_, sourceFd, native, unmounted, callerHome_);
    if (unmounted != 0)
        uprintf("Unmounted %d target filesystem(s).", unmounted);
    if (error == 0)
        error = waitForUnmountSnapshot();
    if (error == 0 && source)
        error = source->check();
    if (error == 0 && archive) error = archive->check();
    if (error == 0)
        error = checkDescriptor(fd_, request_.device, request_.size);
    const int closed = ::close(fd_);
    if (closed != 0 && error == 0)
        error = errno;
    fd_ = -1;
    return error;
}

int PrepareDeviceTarget::open(uid_t caller, const PrepareIsoSource* source)
{
    return open(caller, source, nullptr);
}

int PrepareDeviceTarget::open(uid_t caller, const PrepareIsoSource* source, const PrepareIsoSource* archive)
{
    if (getuid() != 0 || geteuid() != 0 || !request_.isDevice || fd_ >= 0)
        return EPERM;
    if (request_.archive.present() != (archive != nullptr)) return EINVAL;
    if (archive) {
        const int checked = CheckPrepareSourceIdentity(PrepareArchiveRequest(request_.archive), archive->retainedSource());
        if (checked != 0) return checked;
    }
    const bool sfd = table_ == Table::Sfd;
    const bool fat = request_.fileSystem == FS_FAT16 || request_.fileSystem == FS_FAT32;
    const bool dataExt = request_.nonBootable && (request_.fileSystem == FS_EXT2 || request_.fileSystem == FS_EXT3);
    const bool dataUdf = request_.nonBootable && request_.fileSystem == FS_UDF;
    const bool nativeData = request_.fileSystem == FS_NTFS || request_.fileSystem == FS_EXFAT || dataExt || dataUdf;
    const bool supportedSfd = sfd && request_.nonBootable && (fat || nativeData);
    const bool raw = table_ == Table::Raw;
    const bool supportedTable = raw || table_ == Table::Gpt || table_ == Table::Mbr || supportedSfd;
    const bool nativeSyslinux = request_.syslinux &&
        (request_.syslinuxSelection == BT_IMAGE || request_.syslinuxSelection == BT_SYSLINUX_V6);
    const bool ntfsSyslinux = request_.fileSystem == FS_NTFS && nativeSyslinux;
    const bool ntfsGrub = request_.fileSystem == FS_NTFS && (request_.grub2 || request_.grub4dos);
    const bool ntfsWindows = request_.fileSystem == FS_NTFS && request_.windowsBios;
    const bool extGrub2 = (request_.fileSystem == FS_EXT2 || request_.fileSystem == FS_EXT3)
        && request_.grub2 && request_.mbr && !request_.nonBootable && !request_.sfd
        && !request_.syslinux && !request_.uefiNtfs && request_.imagePath.isEmpty();
    const bool dataTable = table_ == Table::Gpt || supportedSfd ||
        (table_ == Table::Mbr && (request_.nonBootable || request_.uefiNtfs
            || ntfsSyslinux || ntfsGrub || ntfsWindows || extGrub2));
    const bool supportedFilesystem = raw || request_.fileSystem == FS_FAT16 || request_.fileSystem == FS_FAT32 ||
        (nativeData && dataTable) || (extGrub2 && table_ == Table::Mbr);
    if (!supportedTable || !supportedFilesystem)
        return ENOTSUP;
    UDisksObjects objects;
    Device current;
    int error = ReadPrepareDeviceSnapshot(QDBusConnection::systemBus(), request_, objects, current);
    if (error != 0)
        return error;
    QStringList mounts;
    const QString problem = ListUDisksTargetMounts(objects, current, request_.imageDevice, mounts, request_.allowFixedUsb);
    if (!problem.isEmpty())
    {
        uprintfs(problem.toUtf8().constData());
        return EBUSY;
    }
    if (archive) {
        QStringList additional;
        const QString archiveProblem = ListUDisksTargetMounts(objects, current, request_.archive.device,
            additional, request_.allowFixedUsb);
        if (!archiveProblem.isEmpty()) { uprintfs(archiveProblem.toUtf8().constData()); return EBUSY; }
    }
    const bool nativeProtection = current.requiresNativeProtection || request_.allowFixedUsb || archive != nullptr || !request_.lockDevice;
    if (!mounts.isEmpty() || nativeProtection || (source && source->hasParentSources()))
        error = prepareTargetMounts(caller, source, archive);
    if (error != 0)
        return error;
    fd_ = ::open(QFile::encodeName(request_.path).constData(), diskOpenFlags & (request_.lockDevice ? ~0 : ~O_EXCL));
    if (fd_ < 0)
        return errno;
    error = check();
    if (error == 0) error = CheckDeviceExclusions(request_.path, request_.device, request_.size, request_.exclusions, fd_);
    if (error == 0 && source && source->hasParentSources())
        error = source->checkParentBacking(fd_, callerHome_);
    if (error == 0 && nativeProtection)
    {
        // Repeat native protections after acquiring the exclusive claim. An
        // empty UDisks inventory alone cannot establish that a fixed disk is unused,
        // including flash media admitted by the default HDD filter.
        NativeTargetMounts native;
        const int sourceFd = source ? source->retainedSource().fd : -1;
        error = CheckNativeTargetMounts(fd_, sourceFd, native, callerHome_);
        if (error == 0 && !native.volumes.isEmpty()) error = EBUSY;
        if (error == 0 && native.diskSequence != request_.diskSequence) error = ESTALE;
        if (error == 0 && source) error = source->check();
        if (error == 0 && archive) {
            NativeTargetMounts additional;
            error = CheckNativeTargetMounts(fd_, archive->retainedSource().fd, additional, callerHome_);
            if (error == 0 && !additional.volumes.isEmpty()) error = EBUSY;
            if (error == 0 && additional.diskSequence != request_.diskSequence) error = ESTALE;
            if (error == 0) error = archive->check();
        }
    }
    return error;
}

int PrepareDeviceTarget::openPartition(quint32 number, quint64 offset, quint64 size, quint64& device)
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
            if (sysfsNumber(path + QStringLiteral("/partition")) != number)
                continue;
            const bool correctStart = sysfsNumber(path + QStringLiteral("/start")) == offset / sectorBytes;
            const bool correctSize = sysfsNumber(path + QStringLiteral("/size")) == size / sectorBytes;
            if (!correctStart || !correctSize)
                return -ESTALE;
            QFile deviceFile(path + QStringLiteral("/dev"));
            if (!deviceFile.open(QIODevice::ReadOnly))
                return -EIO;
            const QList<QByteArray> parts = deviceFile.read(128).trimmed().split(':');
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
            const int error = checkDescriptor(fd, device, size);
            if (error == 0)
                return fd;
            ::close(fd);
            return -error;
        }
        QThread::msleep(partitionPollMs);
    }
    return -ETIMEDOUT;
}

int PrepareDeviceTarget::saveFfuPartitionTable()
{
    ffuTable_.clear();
    const quint32 sector = request_.imageSectorSize;
    const bool optimized = table_ == Table::Raw && request_.ffuMinimumSize != 0;
    if (!optimized || (sector != 512 && sector != 4096) || request_.size < 4 * sector) return EINVAL;
    QByteArray primary(2 * sector, Qt::Uninitialized), backup(sector, Qt::Uninitialized);
    if (!readAt(fd_, primary.data(), primary.size(), 0)
        || !readAt(fd_, backup.data(), backup.size(), request_.size - sector)) return EIO;
    const char* header = primary.constData() + sector;
    if (memcmp(header, "EFI PART", 8) != 0 || memcmp(backup.constData(), "EFI PART", 8) != 0) return EBADMSG;
    const quint32 entries = qFromLittleEndian<quint32>(header + 80);
    const quint32 width = qFromLittleEndian<quint32>(header + 84);
    const quint64 bytes = quint64(entries) * width;
    if (entries == 0 || width < 128 || width % 128 || bytes > FFU_GPT_TABLE_BUDGET) return EBADMSG;
    const quint64 padded = (bytes + sector - 1) / sector * sector;
    const quint64 primaryLba = qFromLittleEndian<quint64>(header + 72);
    const quint64 backupLba = qFromLittleEndian<quint64>(backup.constData() + 72);
    const quint64 sectors = request_.size / sector;
    const bool boundedTables = primaryLba >= 2 && primaryLba < sectors && backupLba < sectors
        && padded / sector <= sectors - primaryLba && padded / sector <= sectors - backupLba;
    if (!boundedTables) return EBADMSG;
    QVector<TableSnapshot> snapshots = {{0, primary}, {request_.size - sector, backup},
        {primaryLba * sector, QByteArray(qsizetype(padded), Qt::Uninitialized)},
        {backupLba * sector, QByteArray(qsizetype(padded), Qt::Uninitialized)}};
    for (qsizetype index = 2; index < snapshots.size(); index++)
        if (!readAt(fd_, snapshots[index].bytes.data(), snapshots[index].bytes.size(), snapshots[index].offset)) return EIO;
    ffuTable_ = snapshots;
    return 0;
}

int PrepareDeviceTarget::checkFfuPartitionTable()
{
    if (table_ != Table::Raw || request_.ffuMinimumSize == 0 || ffuTable_.size() != 4) return EINVAL;
    char readback[64 * 1024];
    for (const TableSnapshot& snapshot : ffuTable_)
        for (qsizetype position = 0; position < snapshot.bytes.size();)
        {
            const qsizetype bytes = qMin(qsizetype(sizeof(readback)), snapshot.bytes.size() - position);
            if (!readAt(fd_, readback, size_t(bytes), snapshot.offset + quint64(position))) return EIO;
            if (memcmp(readback, snapshot.bytes.constData() + position, size_t(bytes)) != 0) return EBADMSG;
            position += bytes;
        }
    return 0;
}

int PrepareDeviceTarget::prepareFfuPartitions()
{
    if (table_ != Table::Raw || request_.ffuMinimumSize == 0) return EINVAL;
    int error = check();
    if (error == 0) error = saveFfuPartitionTable();
    if (error == 0 && ioctl(fd_, BLKRRPART, 0) != 0) error = errno;
    if (error == 0) error = checkFfuPartitionTable();
    return error;
}

int PrepareDeviceTarget::openFfuPartition(quint32 number, quint64 offset, quint64 size, quint64& device)
{
    if (table_ != Table::Raw || request_.ffuMinimumSize == 0 || number == 0) return -EINVAL;
    int error = check();
    if (error == 0) error = checkFfuPartitionTable();
    return error == 0 ? openPartition(number, offset, size, device) : -error;
}

int PrepareDeviceTarget::savePartitionTable()
{
    if (table_ == Table::Raw) return saveFfuPartitionTable();
    // A failed new snapshot must not leave an old or partially filled one valid.
    primary_.clear();
    backup_.clear();
    const bool mbr = table_ == Table::Mbr;
    const int primaryBytes = mbr ? int(request_.logicalSectorSize) : gptTableBytes + 2 * int(request_.logicalSectorSize);
    const int backupBytes = mbr ? 0 : gptTableBytes + int(request_.logicalSectorSize);
    if (request_.size < quint64(primaryBytes + backupBytes))
        return EINVAL;
    QByteArray primary(primaryBytes, Qt::Uninitialized);
    QByteArray backup(backupBytes, Qt::Uninitialized);
    if (!readAt(fd_, primary.data(), size_t(primaryBytes), 0))
        return EIO;
    if (backupBytes != 0 && !readAt(fd_, backup.data(), size_t(backupBytes), request_.size - backupBytes))
        return EIO;
    primary_ = primary;
    backup_ = backup;
    return 0;
}

int PrepareDeviceTarget::checkSavedPartitionTable()
{
    if (table_ == Table::Raw) return checkFfuPartitionTable();
    const bool mbr = table_ == Table::Mbr;
    const int primaryBytes = mbr ? int(request_.logicalSectorSize) : gptTableBytes + 2 * int(request_.logicalSectorSize);
    const int backupBytes = mbr ? 0 : gptTableBytes + int(request_.logicalSectorSize);
    const bool complete = primary_.size() == primaryBytes && backup_.size() == backupBytes;
    if (!complete)
        return EINVAL;
    char readback[gptTableBytes + 2 * 4096];
    const bool primaryMatches = readAt(fd_, readback, size_t(primaryBytes), 0)
        && memcmp(primary_.constData(), readback, size_t(primaryBytes)) == 0;
    if (!primaryMatches)
        return EBADMSG;
    // An MBR partition can extend into this area. Its filesystem is allowed
    // to change these bytes; only GPT has a table copy at the end of the disk.
    if (backupBytes != 0)
    {
        const bool backupMatches = readAt(fd_, readback, size_t(backupBytes), request_.size - backupBytes)
            && memcmp(backup_.constData(), readback, size_t(backupBytes)) == 0;
        if (!backupMatches)
            return EBADMSG;
    }
    return 0;
}

int PrepareDeviceTarget::checkMbrIdentity()
{
    if (table_ != Table::Mbr || primary_.size() != request_.logicalSectorSize)
        return EINVAL;
    char sector[sectorBytes];
    if (!readAt(fd_, sector, sizeof(sector), 0))
        return EIO;
    const size_t identityBytes = sectorBytes - mbrDiskSignatureOffset;
    const bool sameIdentity = memcmp(primary_.constData() + mbrDiskSignatureOffset,
        sector + mbrDiskSignatureOffset, identityBytes) == 0;
    return sameIdentity ? 0 : EBADMSG;
}

int PrepareDeviceTarget::reacquire()
{
    fd_ = ::open(QFile::encodeName(request_.path).constData(), diskOpenFlags & (request_.lockDevice ? ~0 : ~O_EXCL));
    int error = fd_ < 0 ? errno : checkDescriptor(fd_, request_.device, request_.size);
    if (error == 0)
        error = table_ == Table::Sfd ? checkNoPartitions() : checkSavedPartitionTable();
    if (error != 0)
        uprintf("Cannot revalidate the disk after releasing its claim: %s", strerror(error));
    return error;
}

int PrepareDeviceTarget::checkNoPartitions()
{
    const QString parent = QStringLiteral("/sys/dev/block/") + deviceNumbers(request_.device);
    const QDir directory(parent);
    if (!directory.exists())
        return ESTALE;
    const QStringList entries = directory.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString& entry : entries)
        if (QFile::exists(parent + QLatin1Char('/') + entry + QStringLiteral("/partition")))
            return EBUSY;
    return 0;
}

int PrepareDeviceTarget::checkSfdFormat(const FAT32_FORMAT_OPTIONS& options)
{
    int error = check();
    FAT32_FORMAT_RESULT readback = {};
    if (error == 0)
        error = request_.fileSystem == FS_FAT16
            ? CheckFat16Device(fd_, request_.device, request_.size, &options, &readback)
            : CheckFat32Device(fd_, request_.device, request_.size, &options, &readback);
    if (error != 0)
        return error;
    return checkSfdPartitionEntries();
}

int PrepareDeviceTarget::checkSfdPartitionEntries()
{
    // The qualified native FAT/NTFS/exFAT boot sectors leave the MBR entry area empty.
    char entries[mbrPartitionBytes];
    const char empty[mbrPartitionBytes] = {};
    if (!readAt(fd_, entries, sizeof(entries), mbrPartitionOffset))
        return EIO;
    return memcmp(entries, empty, sizeof(entries)) == 0 ? 0 : EBADMSG;
}

int PrepareDeviceTarget::beginFormat(quint64 offset, quint64 size, int& partition, quint64& partitionDevice, quint32 number)
{
    partition = -1;
    const bool sfd = table_ == Table::Sfd;
    if (sfd && (offset != 0 || size != request_.size))
        return EINVAL;
    int error = check();
    if (error == 0 && !sfd)
        error = savePartitionTable();
    if (error != 0)
        return error;
    if (ioctl(fd_, BLKRRPART, 0) != 0)
        return errno;
    if (sfd)
    {
        error = checkNoPartitions();
        if (error != 0)
            return error;
        partitionDevice = request_.device;
        partition = ::open(QFile::encodeName(request_.path).constData(), diskOpenFlags & ~O_EXCL);
        if (partition < 0)
            return errno;
    }
    else
        partition = openPartition(number, offset, size, partitionDevice);
    if (partition < 0)
        return -partition;
    // Retain the exact partition (or whole device for SFD) for mkfs's own
    // O_EXCL open. No exclusive descriptor is inherited by the formatter.
    const int released = ::close(fd_);
    fd_ = -1;
    if (released != 0)
        error = errno;
    if (error == 0)
        error = checkSnapshot();
    if (error == 0)
        error = checkDescriptor(partition, partitionDevice, size);
    return error;
}

int PrepareDeviceTarget::finishFormat(int partition, int error)
{
    if (partition < 0)
        return error;
    if (::close(partition) != 0 && error == 0)
        error = errno;
    // Attempt reacquisition even on cancellation/failure, without another launch
    // or authorization. A busy or replaced disk is never bypassed or retried.
    const int reacquired = reacquire();
    return error != 0 ? error : reacquired;
}

int PrepareDeviceTarget::format(const FAT32_FORMAT_OPTIONS& options, const char* mkfs, FAT32_FORMAT_RESULT& result)
{
    result = {};
    result.exit_code = -1;
    if (options.full_format == request_.quickFormat) return EINVAL;
    if (request_.fileSystem != FS_FAT16 && request_.fileSystem != FS_FAT32)
        return ENOTSUP;
    if (table_ == Table::Sfd)
    {
        const int error = request_.fileSystem == FS_FAT16
            ? CheckFat16FormatOptions(&options) : CheckFat32FormatOptions(&options);
        if (error != 0)
            return error;
    }
    quint64 partitionDevice = 0;
    int partition = -1;
    int error = beginFormat(options.offset, options.size, partition, partitionDevice);
    if (error == 0)
        error = request_.fileSystem == FS_FAT16
            ? FormatFat16Partition(partition, partitionDevice, mkfs, &options, &result)
            : FormatFat32Partition(partition, partitionDevice, mkfs, &options, &result);
    error = finishFormat(partition, error);
    if (error == 0 && table_ == Table::Sfd)
        error = checkSfdFormat(options);
    return error;
}

int PrepareDeviceTarget::formatExfat(const ExfatFormatOptions& options, const QString& mkfs, FORMAT_RESULT& result)
{
    result = {};
    result.exit_code = -1;
    const bool sfd = table_ == Table::Sfd;
    const bool supportedMbr = table_ == Table::Mbr && (request_.nonBootable || request_.uefiNtfs);
    const bool supportedTable = table_ == Table::Gpt || supportedMbr || (request_.nonBootable && sfd);
    if (options.quickFormat != request_.quickFormat) return EINVAL;
    if (request_.fileSystem != FS_EXFAT || !supportedTable)
        return ENOTSUP;
    int error = CheckExfatFormatOptions(options);
    if (error != 0)
        return error;
    quint64 partitionDevice = 0;
    int partition = -1;
    error = beginFormat(options.partitionOffset, options.size, partition, partitionDevice);
    if (error == 0)
        error = FormatExfatDevice(partition, partitionDevice, mkfs, options, result, sfd);
    error = finishFormat(partition, error);
    if (error == 0 && sfd)
        error = CheckExfatVolume(fd_, options);
    if (error == 0 && sfd)
        error = checkSfdPartitionEntries();
    return error;
}

int PrepareDeviceTarget::formatNtfs(const GPT_IMAGE_RESULT& layout, const NtfsFormatOptions& options,
    const QString& mkfs, NtfsFormatResult& result)
{
    result = {};
    result.format.exit_code = -1;
    const bool sfd = table_ == Table::Sfd;
    const bool syslinux = request_.syslinux &&
        (request_.syslinuxSelection == BT_IMAGE || request_.syslinuxSelection == BT_SYSLINUX_V6);
    const bool supportedMbr = table_ == Table::Mbr &&
        (request_.nonBootable || request_.uefiNtfs || syslinux || request_.grub2 || request_.grub4dos || request_.windowsBios || request_.windowsToGo);
    const bool supportedTable = table_ == Table::Gpt || supportedMbr || (request_.nonBootable && sfd);
    if (options.quickFormat != request_.quickFormat) return EINVAL;
    if (!supportedTable || request_.fileSystem != FS_NTFS)
        return ENOTSUP;
    const bool sameExtent = options.partitionOffset == layout.offset && options.size == layout.size;
    const bool validBootPartition = request_.usesUefiNtfs()
        ? layout.uefi_ntfs_size != 0
        : layout.uefi_ntfs_size == 0 && layout.uefi_ntfs_offset == 0;
    if (!sameExtent || !validBootPartition)
        return EINVAL;
    const bool wholeDevice = layout.offset == 0 && layout.size == request_.size;
    if (sfd && !wholeDevice)
        return EINVAL;
    int error = CheckNtfsFormatOptions(options);
    if (error == 0)
        error = check();
    if (error == 0 && !sfd)
        error = savePartitionTable();
    if (error != 0)
        return error;
    if (ioctl(fd_, BLKRRPART, 0) != 0)
        return errno;
    quint64 partitionDevice = 0, bootDevice = 0;
    int partition;
    if (sfd)
    {
        error = checkNoPartitions();
        if (error != 0)
            return error;
        partitionDevice = request_.device;
        partition = ::open(QFile::encodeName(request_.path).constData(), diskOpenFlags & ~O_EXCL);
        if (partition < 0)
            return errno;
    }
    else
        partition = openPartition(request_.windowsToGo && table_ == Table::Gpt ? 3 : mainPartitionNumber,
            layout.offset, layout.size, partitionDevice);
    if (partition < 0)
        return -partition;
    int boot = -1;
    if (request_.usesUefiNtfs())
    {
        boot = openPartition(uefiPartitionNumber + (request_.persistenceSize != 0), layout.uefi_ntfs_offset, layout.uefi_ntfs_size, bootDevice);
        if (boot < 0)
        {
            ::close(partition);
            return -boot;
        }
    }
    // mkntfs opens block devices without O_EXCL. Keep our whole-disk claim
    // throughout the child and its readback, preventing concurrent mounts.
    // CLOEXEC keeps the disk and boot partition descriptors in this helper.
    error = checkSnapshot();
    if (error == 0)
        error = checkDescriptor(partition, partitionDevice, options.size);
    if (error == 0)
        error = FormatNtfsDevice(partition, partitionDevice, mkfs, options, result, sfd);
    if (error == 0 && boot >= 0)
        error = checkDescriptor(boot, bootDevice, layout.uefi_ntfs_size);
    if (::close(partition) != 0 && error == 0)
        error = errno;
    if (boot >= 0 && ::close(boot) != 0 && error == 0)
        error = errno;
    int checked = checkDescriptor(fd_, request_.device, request_.size);
    if (checked == 0)
        checked = sfd ? checkNoPartitions() : checkSavedPartitionTable();
    if (checked == 0 && sfd)
        checked = checkSfdPartitionEntries();
    return error != 0 ? error : checked;
}

int PrepareDeviceTarget::formatExt(const GPT_IMAGE_RESULT& layout, const ExtFormatOptions& options,
    const QString& mke2fs, FORMAT_RESULT& result)
{
    result = {};
    result.exit_code = -1;
    const bool ext = request_.fileSystem == FS_EXT2 || request_.fileSystem == FS_EXT3;
    const bool sfd = table_ == Table::Sfd;
    const bool volumeExtent = sfd ? layout.offset == 0 && layout.size == request_.size : layout.offset != 0;
    const bool exactLayout = options.size == layout.size && volumeExtent &&
        layout.uefi_ntfs_offset == 0 && layout.uefi_ntfs_size == 0;
    const bool sourceFreeGrub2 = request_.grub2 && request_.mbr && !request_.nonBootable
        && !request_.sfd && !request_.syslinux && !request_.uefiNtfs && request_.imagePath.isEmpty();
    if (options.quickFormat != request_.quickFormat) return EINVAL;
    const bool dataFormat = request_.nonBootable && request_.isSourceFree() && table_ != Table::Raw;
    if ((!dataFormat && (table_ != Table::Mbr || !sourceFreeGrub2)) || !ext)
        return ENOTSUP;
    if (!exactLayout || options.fileSystem != request_.fileSystem || options.label != request_.label)
        return EINVAL;
    int error = CheckExtFormatOptions(options);
    if (error != 0)
        return error;
    quint64 partitionDevice = 0;
    int partition = -1;
    error = beginFormat(layout.offset, layout.size, partition, partitionDevice);
    if (error == 0)
        error = FormatExtDevice(partition, partitionDevice, mke2fs, options, result);
    error = finishFormat(partition, error);
    if (error == 0)
        error = check();
    return error;
}

int PrepareDeviceTarget::formatUdf(const GPT_IMAGE_RESULT& layout, const UdfFormatOptions& options,
    const QString& mkfs, FORMAT_RESULT& result)
{
    result = {}; result.exit_code = -1;
    const bool sfd = table_ == Table::Sfd;
    const bool volumeExtent = sfd ? layout.offset == 0 && layout.size == request_.size : layout.offset != 0;
    const bool dataFormat = request_.nonBootable && request_.isSourceFree() && table_ != Table::Raw && request_.fileSystem == FS_UDF;
    if (!dataFormat) return ENOTSUP;
    const bool exactOptions = options.size == layout.size && volumeExtent && options.label == request_.label
        && options.revision == request_.udfRevision && options.quickFormat == request_.quickFormat;
    const bool singleVolume = layout.uefi_ntfs_offset == 0 && layout.uefi_ntfs_size == 0
        && layout.persistence_size == 0;
    if (!exactOptions || !singleVolume) return EINVAL;
    int error = CheckUdfFormatOptions(options);
    if (error != 0) return error;
    quint64 device = 0;
    int partition = -1;
    error = beginFormat(layout.offset, layout.size, partition, device);
    if (error == 0) error = FormatUdfDevice(partition, device, mkfs, options, result);
    error = finishFormat(partition, error);
    if (error == 0 && sfd) error = checkSfdPartitionEntries();
    if (error == 0) error = check();
    return error;
}

int PrepareDeviceTarget::formatPersistence(const GPT_IMAGE_RESULT& layout, bool casper,
    const QString& mke2fs, FORMAT_RESULT& result)
{
    result = {}; result.exit_code = -1;
    if (request_.persistenceSize == 0 || table_ == Table::Sfd || table_ == Table::Raw) return EINVAL;
    const bool validVersion = request_.persistenceExtVersion >= 2 && request_.persistenceExtVersion <= 4;
    if (!validVersion) return EINVAL;
    const bool validExtent = layout.persistence_offset != 0 && layout.persistence_size >= request_.persistenceSize
        && layout.persistence_offset <= request_.size && layout.persistence_size <= request_.size - layout.persistence_offset;
    if (!validExtent) return EINVAL;
    int error = 0;
    const ExtFormatOptions options = {layout.persistence_size, FS_EXT2 + request_.persistenceExtVersion - 2,
        casper ? QStringLiteral("casper-rw") : QStringLiteral("persistence"), !casper, request_.quickFormat, request_.logicalSectorSize, request_.physicalSectorSize};
    error = CheckExtFormatOptions(options);
    int partition = -1; quint64 device = 0;
    if (error == 0) error = beginFormat(layout.persistence_offset, layout.persistence_size, partition, device, 2);
    if (error == 0) error = FormatExtDevice(partition, device, mke2fs, options, result);
    error = finishFormat(partition, error);
    if (error == 0) error = check();
    return error;
}

int PrepareDeviceTarget::installNtfsBoot(const NtfsFormatOptions& options, NtfsBootResult& result)
{
    result = {};
    const bool standardBoot = request_.grub2 || request_.grub4dos || request_.windowsBios || request_.kolibriOs
        || (request_.windowsToGo && request_.windowsToGoTarget == TT_BIOS);
    // NTFS's original record choice is independent of image loader flags.
    const RUFUS_IMG_REPORT report = {};
    const auto record = GetPartitionBootRecord(BT_IMAGE, request_.fileSystem, &report);
    const bool ntfsRecord = record == RUFUS_PBR_NTFS;
    if (table_ != Table::Mbr || !standardBoot || !ntfsRecord)
        return ENOTSUP;
    int error = check();
    if (error == 0)
        error = checkMbrIdentity();
    if (error != 0)
        return error;
    quint64 partitionDevice = 0;
    const int partition = openPartition(mainPartitionNumber, options.partitionOffset,
        options.size, partitionDevice);
    if (partition < 0)
        return -partition;
    error = checkSnapshot();
    if (error == 0)
        error = InstallNtfsBootRecord(partition, partitionDevice, options, result);
    if (::close(partition) != 0 && error == 0)
        error = errno;
    const int checked = check();
    if (error == 0)
        error = checked;
    if (error == 0)
        error = checkMbrIdentity();
    return error;
}

int PrepareDeviceTarget::checkMountSupport()
{
    return checkMountSupport(request_.fileSystem);
}

int PrepareDeviceTarget::checkFfuMountSupport()
{
    if (table_ != Table::Raw || request_.ffuMinimumSize == 0) return EINVAL;
    return checkMountSupport(FS_NTFS);
}

int PrepareDeviceTarget::checkMountSupport(int filesystem)
{
    const bool supported = filesystem == FS_FAT16 || filesystem == FS_FAT32
        || filesystem == FS_EXFAT || filesystem == FS_NTFS || filesystem == FS_UDF;
    if (!supported) return ENOTSUP;
    if (filesystem == FS_EXFAT || filesystem == FS_UDF)
    {
        const int context = int(syscall(SYS_fsopen, filesystem == FS_UDF ? "udf" : "exfat", FSOPEN_CLOEXEC));
        if (context < 0) return errno;
        return ::close(context) == 0 ? 0 : errno;
    }
    if (filesystem != FS_NTFS)
        return 0;
    struct utsname kernel;
    if (uname(&kernel) != 0)
        return errno;
    // The old, limited NTFS driver used this same filesystem name. Restrict
    // automatic selection to kernels containing the new read/write driver.
    const bool modernKernel = QVersionNumber::fromString(QString::fromLatin1(kernel.release)) >= QVersionNumber(7, 1);
    // Compression creation is a separate provider capability. The tested 7.2
    // kernel does not inherit compression and can emit unreadable compressed
    // attributes when setting the flag on a new file. Choose the qualified
    // userspace provider before partitioning, never after an I/O failure.
    const bool compression = request_.ntfsCompression && request_.clusterSize <= NtfsCompressionClusterBytes;
    if (compression)
        uprintf("NTFS compressed file creation requires ntfs-3g.");
    if (windowsLinks_) uprintf("Windows-compatible NTFS reparse link creation requires ntfs-3g.");
    // The qualified kernel ntfs write path fails MFT/mirror writes on native
    // non-512 logical sectors. Select the capable provider before partitioning;
    // never retry an I/O failure or remount a partially written volume.
    const quint32 ntfsSectorSize = table_ == Table::Raw ? request_.imageSectorSize : request_.logicalSectorSize;
    const bool nativeSectors = ntfsSectorSize != 512;
    if (nativeSectors) uprintf("Non-512-byte NTFS file creation requires ntfs-3g.");
    if (modernKernel && !compression && !windowsLinks_ && !nativeSectors)
    {
        const int context = int(syscall(SYS_fsopen, "ntfs", FSOPEN_CLOEXEC));
        if (context >= 0)
        {
            ::close(context);
            kernelNtfs_ = true;
            return 0;
        }
        if (errno != ENODEV)
            return errno;
    }
    if (access(RUFUS_NTFS_3G_PATH, X_OK) != 0)
        return errno;
    const int context = int(syscall(SYS_fsopen, "fuseblk", FSOPEN_CLOEXEC));
    if (context < 0)
        return errno;
    ::close(context);
    return 0;
}

int PrepareDeviceTarget::mountNtfs3g(quint64 device)
{
    // Keep FUSE in the foreground, owned by this job, so caller EOF and failed
    // mounts cannot leave a detached daemon retaining the private namespace.
    const QString source = QStringLiteral("/proc/%1/fd/%2").arg(getpid()).arg(partition_);
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("LC_ALL"), QStringLiteral("C.UTF-8"));
    ntfs3g_.setProcessEnvironment(environment);
    ntfs3g_.setStandardInputFile(QProcess::nullDevice());
    ntfs3g_.setStandardOutputFile(QProcess::nullDevice());
    ntfs3g_.setProcessChannelMode(QProcess::ForwardedErrorChannel);
    ntfs3g_.start(QStringLiteral(RUFUS_NTFS_3G_PATH), {source, QFile::decodeName(mountPath_),
        QStringLiteral("-o"), QStringLiteral("rw,no_detach,no_def_opts,norecover,windows_names,ignore_case,umask=0077,nodev,nosuid,noexec")});
    if (!ntfs3g_.waitForStarted(mountTimeoutMs))
        return EIO;
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < mountTimeoutMs)
    {
        struct stat info;
        struct statfs filesystem;
        const bool mounted = stat(mountPath_.constData(), &info) == 0
            && statfs(mountPath_.constData(), &filesystem) == 0
            && quint64(info.st_dev) == device && filesystem.f_type == FUSE_SUPER_MAGIC;
        if (mounted)
        {
            mounted_ = true;
            uprintf("NTFS file copying uses ntfs-3g");
            return 0;
        }
        if (IsRufusJobCancelled())
            return ECANCELED;
        if (ntfs3g_.waitForFinished(partitionPollMs))
            return EIO;
    }
    return ETIMEDOUT;
}

int PrepareDeviceTarget::finishNtfs3g()
{
    if (ntfs3g_.state() == QProcess::NotRunning)
        return 0;
    if (!ntfs3g_.waitForFinished(unmountTimeoutMs))
    {
        ntfs3g_.terminate();
        if (!ntfs3g_.waitForFinished(terminateGraceMs))
        {
            ntfs3g_.kill();
            ntfs3g_.waitForFinished();
        }
        return ETIMEDOUT;
    }
    const bool cleanExit = ntfs3g_.exitStatus() == QProcess::NormalExit && ntfs3g_.exitCode() == 0;
    return cleanExit ? 0 : EIO;
}

int PrepareDeviceTarget::mount(quint64 offset, quint64 size)
{
    if (table_ == Table::Raw) return EINVAL;
    const quint32 number = request_.windowsToGo && table_ == Table::Gpt ? 3 : mainPartitionNumber;
    return mountPartition(number, offset, size, request_.fileSystem);
}

int PrepareDeviceTarget::applyWindowsImage(WIMStruct* wim, int image, const PrepareIsoSource& source,
    const GPT_IMAGE_RESULT& layout, WindowsApplyResult& result)
{
    result = {};
    if (!request_.windowsToGo || request_.fileSystem != FS_NTFS || table_ == Table::Raw || table_ == Table::Sfd) return EINVAL;
    if (partition_ >= 0 || directory_ >= 0 || mounted_ || !mountPath_.isEmpty()) return EBUSY;
    GPT_IMAGE_RESULT expected = {};
    int error = PlanFormatPartition(request_, expected);
    if (error == 0 && (expected.offset != layout.offset || expected.size != layout.size)) error = ESTALE;
    if (error == 0) error = check();
    if (error == 0) error = checkSavedPartitionTable();
    if (error == 0) error = source.check();
    quint64 device = 0;
    const int partition = error == 0 ? openPartition(table_ == Table::Gpt ? 3 : 1, layout.offset, layout.size, device) : -error;
    if (error == 0 && partition < 0) error = -partition;
    if (error == 0)
    {
        const QByteArray path = QFile::encodeName(request_.imagePath);
        error = ApplyWindowsImage(wim, image, source.retainedSource(), path.constData(),
            partition, device, layout.size, result);
    }
    if (partition >= 0 && ::close(partition) != 0 && error == 0) error = errno;
    const int checked = check();
    if (error == 0) error = checked;
    if (error == 0) error = checkSavedPartitionTable();
    return error;
}

int PrepareDeviceTarget::formatWindowsEsp(const GPT_IMAGE_RESULT& layout, const char* mkfs, FAT32_FORMAT_RESULT& result)
{
    result = {};
    result.exit_code = -1;
    if (!request_.windowsToGo || table_ != Table::Gpt) return EINVAL;
    GPT_IMAGE_RESULT expected = {};
    int error = PlanFormatPartition(request_, expected);
    if (error == 0 && (expected.esp_offset != layout.esp_offset || expected.esp_size != layout.esp_size)) error = ESTALE;
    const FAT32_FORMAT_OPTIONS options = {layout.esp_offset, layout.esp_size, qMax(quint32(1024), request_.logicalSectorSize), 0, {0}, false,
        request_.logicalSectorSize, request_.physicalSectorSize};
    if (error == 0) error = CheckFat32FormatOptions(&options);
    int partition = -1;
    quint64 device = 0;
    if (error == 0) error = beginFormat(layout.esp_offset, layout.esp_size, partition, device, 1);
    if (error == 0) error = FormatFat32Partition(partition, device, mkfs, &options, &result);
    return finishFormat(partition, error);
}

int PrepareDeviceTarget::mountWindowsEsp(const GPT_IMAGE_RESULT& layout)
{
    if (!request_.windowsToGo || table_ != Table::Gpt) return EINVAL;
    GPT_IMAGE_RESULT expected = {};
    int error = PlanFormatPartition(request_, expected);
    if (error == 0 && (expected.esp_offset != layout.esp_offset || expected.esp_size != layout.esp_size)) error = ESTALE;
    return error == 0 ? mountPartition(1, layout.esp_offset, layout.esp_size, FS_FAT32) : error;
}

int PrepareDeviceTarget::mountFfuPartition(quint32 number, quint64 offset, quint64 size)
{
    if (table_ != Table::Raw || request_.ffuMinimumSize == 0 || number == 0) return EINVAL;
    return mountPartition(number, offset, size, FS_NTFS);
}

int PrepareDeviceTarget::mountPartition(quint32 number, quint64 offset, quint64 size, int fileSystem)
{
    if (partition_ >= 0 || directory_ >= 0 || mounted_ || !mountPath_.isEmpty()) return EBUSY;
    cleanupAttempted_ = false;
    const bool sfd = table_ == Table::Sfd;
    if (sfd && (offset != 0 || size != request_.size)) return EINVAL;
    int error = check();
    if (error != 0)
        return error;
    error = sfd ? checkNoPartitions() : checkSavedPartitionTable();
    if (error != 0) return error;
    quint64 device = sfd ? request_.device : 0;
    partition_ = sfd ? ::open(QFile::encodeName(request_.path).constData(), diskOpenFlags & ~O_EXCL)
        : openPartition(number, offset, size, device);
    if (partition_ < 0)
    {
        error = sfd ? errno : -partition_;
        partition_ = -1;
        return error;
    }
    mountPath_ = "/run/rufus-XXXXXX";
    if (mkdtemp(mountPath_.data()) == nullptr)
    {
        error = errno;
        mountPath_.clear();
        return error;
    }
    const int released = ::close(fd_);
    fd_ = -1;
    if (released != 0)
        return errno;
    error = checkSnapshot();
    if (error == 0)
        error = checkDescriptor(partition_, device, size);
    if (error != 0)
        return error;
    const QByteArray source = QByteArray("/proc/self/fd/") + QByteArray::number(partition_);
    const unsigned long flags = MS_NODEV | MS_NOSUID | MS_NOEXEC;
    long expectedMagic = MSDOS_SUPER_MAGIC;
    if (fileSystem == FS_NTFS)
    {
        expectedMagic = kernelNtfs_ ? ntfsMagic : FUSE_SUPER_MAGIC;
        if (kernelNtfs_)
        {
            if (::mount(source.constData(), mountPath_.constData(), "ntfs", flags,
                "iocharset=utf8,umask=0077,windows_names,nocase,errors=remount-ro") != 0)
                return errno;
            mounted_ = true;
            uprintf("NTFS file copying uses the kernel ntfs driver");
        }
        else
        {
            error = mountNtfs3g(device);
            if (error != 0)
                return error;
        }
    }
    else
    {
        const bool exfat = fileSystem == FS_EXFAT;
        const bool udf = fileSystem == FS_UDF;
        const char* filesystem = udf ? "udf" : exfat ? "exfat" : "vfat";
        const char* options = udf ? "iocharset=utf8" : exfat ? "iocharset=utf8,umask=0077" : "utf8=1,shortname=mixed,umask=0077";
        if (exfat) expectedMagic = EXFAT_SUPER_MAGIC;
        if (udf) expectedMagic = UDF_SUPER_MAGIC;
        if (::mount(source.constData(), mountPath_.constData(), filesystem, flags, options) != 0)
            return errno;
        mounted_ = true;
    }
    directory_ = ::open(mountPath_.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directory_ < 0)
        return errno;
    struct stat info;
    struct statfs filesystem;
    if (fstat(directory_, &info) != 0 || fstatfs(directory_, &filesystem) != 0)
        return errno;
    const bool expectedMount = quint64(info.st_dev) == device && filesystem.f_type == expectedMagic;
    if (!expectedMount)
        return ESTALE;
    if (filesystem.f_flags & ST_RDONLY)
        return EROFS;
    return IsRufusJobCancelled() ? ECANCELED : 0;
}

int PrepareDeviceTarget::finishMount(int& syncError)
{
    cleanupAttempted_ = true;
    int error = 0;
    syncError = 0;
    if (directory_ >= 0)
    {
        // Cancellation still flushes completed writes before unmounting.
        while (syncfs(directory_) != 0)
        {
            if (errno == EINTR)
                continue;
            syncError = errno;
            break;
        }
        if (::close(directory_) != 0)
            error = errno;
        directory_ = -1;
    }
    if (mounted_)
    {
        if (umount(mountPath_.constData()) != 0)
            error = errno;
        else
            mounted_ = false;
    }
    const int childError = finishNtfs3g();
    if (error == 0)
        error = childError;
    // Never recursively remove this path or lazily detach a failed mount.
    if (!mounted_ && !mountPath_.isEmpty())
    {
        if (rmdir(mountPath_.constData()) != 0 && error == 0)
            error = errno;
        mountPath_.clear();
    }
    if (partition_ >= 0)
    {
        if (::close(partition_) != 0 && error == 0)
            error = errno;
        partition_ = -1;
    }
    if (fd_ < 0)
    {
        const int reacquired = reacquire();
        if (error == 0)
            error = reacquired;
    }
    // The mount has flushed its filesystem and released its claim. Flush the
    // reacquired whole disk too, including cancellation and earlier failures.
    if (fd_ >= 0)
    {
        while (fsync(fd_) != 0)
        {
            if (errno == EINTR)
                continue;
            if (syncError == 0)
                syncError = errno;
            break;
        }
    }
    return error;
}
}
