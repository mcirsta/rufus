#include <QDirIterator>
// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/prepare_device.h"
#include "platform/device_operations.h"
#include "platform/target_mounts.h"
#include "platform/device_exclusions.h"
#include "platform/format_options.h"
#include "platform/udisks_client.h"
#include "platform/syslinux_install.h"
#include "platform/iso_extract.h"
#include "platform/prepare_source.h"
#include "platform/raw_device.h"
#include "platform/native_disk_facts.h"
#include "platform/image_save.h"
#include "platform/media_appearance.h"
#include "platform/grub2_cache.h"
#include "qt/prepare_image_process.h"
#include "unattend.h"
#include <hivex.h>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonArray>
#include <QCoreApplication>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusReply>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QProcess>
#include <QSignalSpy>
#include <QXmlStreamReader>
#include <QtEndian>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <grp.h>
#include <linux/fs.h>
#include "platform/filesystem_magic.h"
#include <linux/msdos_fs.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <unistd.h>
#include <zlib.h>

namespace
{
using Table = rufus::PrepareDeviceTarget::Table;
constexpr quint64 handoffDiskBytes = 64 * 1024 * 1024;
constexpr quint64 alpineDiskBytes = 512 * 1024 * 1024;
quint64 diskBytes = handoffDiskBytes;
constexpr int sectorBytes = 512;
constexpr int diskSignatureOffset = 440;
constexpr int gptBackupBytes = 33 * sectorBytes;
constexpr int processTimeoutMs = 5000;
constexpr int compressedMountedTimeoutMs = 60000; // Full stream preflight plus sanitizer/TCG helper exit.
enum class Fault { None, SnapshotRead, FormatMbr, FormatGpt, FormatBackup, FormatBusy,
    FormatCancel, SfdEntries, SfdBpb, BeforeMount, Mount, Sync, MountedMbr, InstallMbr, InstallCancel, InstallSync };
Fault fault = Fault::None;
bool cancelled = false;
int busyDescriptor = -1;
bool installing = false;
bool sfdHandoff = false;
bool exfatHandoff = false;
bool exfatFormatting = false;
enum class RawFault { None, CancelUnmount, CancelCopy, Sync, Verify };
RawFault rawFault = RawFault::None;
bool rawActive = false;
rufus::UDisksObjects rawObjects;
rufus::PrepareImageRequest rawRequest;
quint64 rawPartition = 0;
int rawUnmounts = 0;
int rawFlushes = 0;

void require(bool valid, const char* message)
{
    if (!valid)
        qFatal("Handoff guest failed: %s (errno=%d)", message, errno);
}

QByteArray readFile(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QByteArray readBytes(int fd, quint64 offset, int size)
{
    QByteArray bytes(size, Qt::Uninitialized);
    require(pread(fd, bytes.data(), size_t(size), off_t(offset)) == size, "read disk bytes");
    return bytes;
}

void corruptDisk(off_t offset)
{
    const int fd = open("/dev/sda", O_RDWR | O_CLOEXEC);
    require(fd >= 0, "fault writer open");
    char byte;
    require(pread(fd, &byte, 1, offset) == 1, "fault byte read");
    byte ^= 1;
    require(pwrite(fd, &byte, 1, offset) == 1 && fsync(fd) == 0, "fault byte write");
    require(close(fd) == 0, "fault writer close");
}

void checkExclusiveClaim()
{
    const int fd = open("/dev/sda", O_RDWR | O_EXCL | O_CLOEXEC);
    const int error = errno;
    if (fd >= 0)
        close(fd);
    require(fd < 0 && error == EBUSY, "exclusive whole-disk claim retained");
}

void checkClaim(const rufus::PrepareDeviceTarget& target)
{
    require(target.descriptor() >= 0, "whole-disk descriptor reacquired");
    checkExclusiveClaim();
}

int descriptorCount()
{
    DIR* directory = opendir("/proc/self/fd");
    require(directory != nullptr, "descriptor inventory");
    int count = 0;
    while (readdir(directory))
        count++;
    closedir(directory);
    return count;
}

rufus::PrepareImageRequest makeRequest(QProcess& daemon, QProcess& fixture)
{
    daemon.setProcessChannelMode(QProcess::ForwardedErrorChannel);
    fixture.setProcessChannelMode(QProcess::ForwardedErrorChannel);
    daemon.start(QStringLiteral("/usr/bin/dbus-daemon"), {QStringLiteral("--nofork"),
        QStringLiteral("--print-address=1"), QStringLiteral("--config-file=/etc/rufus-test-bus.conf")});
    require(daemon.waitForStarted(processTimeoutMs) && daemon.waitForReadyRead(processTimeoutMs), "private bus ready");
    const QByteArray address = daemon.readLine().trimmed();
    require(address.startsWith("unix:"), "private bus address");
    qputenv("DBUS_SYSTEM_BUS_ADDRESS", address);
    fixture.start(QStringLiteral("/usr/bin/udisks_fixture"), {QString::fromUtf8(address)});
    require(fixture.waitForStarted(processTimeoutMs) && fixture.waitForReadyRead(processTimeoutMs), "UDisks fixture ready");
    require(fixture.readLine() == "ready\n", "fixture ready marker");
    qDBusRegisterMetaType<rufus::UDisksInterfaces>();
    qDBusRegisterMetaType<rufus::UDisksObjects>();
    qDBusRegisterMetaType<QList<QByteArray>>();
    const QDBusConnection connection = QDBusConnection::systemBus();
    QString node = QStringLiteral("/dev/sda");
    if (QFile::exists(QStringLiteral("/etc/rufus-geometry-fixture"))) {
        QStringList matches;
        for (const QString& name : QDir(QStringLiteral("/sys/block")).entryList({QStringLiteral("sd*")}, QDir::Dirs)) {
            const QString queue = QStringLiteral("/sys/block/") + name + QStringLiteral("/queue/");
            const QByteArray logical = readFile(queue + QStringLiteral("logical_block_size")).trimmed();
            const QByteArray physical = readFile(queue + QStringLiteral("physical_block_size")).trimmed();
            printf("RUFUS_GEOMETRY_DISCOVERY NODE=%s LOGICAL=%s PHYSICAL=%s\n", qPrintable(name), logical.constData(), physical.constData());
            if (logical == "512" && physical == "512")
                matches.append(QStringLiteral("/dev/") + name);
        }
        fflush(stdout);
        require(matches.size() == 1, "unique geometry baseline stick");
        node = matches.first();
    }
    if (QFile::exists(QStringLiteral("/etc/rufus-device-options-fixture"))) {
        for (const QString& name : QDir(QStringLiteral("/sys/block")).entryList({QStringLiteral("sd*")}, QDir::Dirs))
            if (readFile(QStringLiteral("/sys/block/") + name + QStringLiteral("/device/model")).startsWith("QEMU HARDDISK"))
                node = QStringLiteral("/dev/") + name;
    }
    const QDBusObjectPath disk(QStringLiteral("/org/freedesktop/UDisks2/block_devices/") + node.mid(5));
    const QDBusObjectPath drive(QStringLiteral("/org/freedesktop/UDisks2/drives/fixture"));
    struct stat info;
    require(stat(QFile::encodeName(node).constData(), &info) == 0 && S_ISBLK(info.st_mode), "guest USB node");
    rufus::UDisksObjects objects;
    objects[disk][QStringLiteral("org.freedesktop.UDisks2.Block")] = {
        {QStringLiteral("Drive"), QVariant::fromValue(drive)},
        {QStringLiteral("Device"), QFile::encodeName(node) + '\0'},
        {QStringLiteral("Size"), QVariant::fromValue(diskBytes)},
        {QStringLiteral("DeviceNumber"), QVariant::fromValue(quint64(info.st_rdev))},
        {QStringLiteral("ReadOnly"), false}, {QStringLiteral("HintSystem"), false},
        {QStringLiteral("HintIgnore"), false}, {QStringLiteral("Id"), QStringLiteral("handoff-fixture")}};
    objects[drive][QStringLiteral("org.freedesktop.UDisks2.Drive")] = {
        {QStringLiteral("ConnectionBus"), QStringLiteral("usb")}, {QStringLiteral("Removable"), true},
        {QStringLiteral("MediaRemovable"), true}, {QStringLiteral("MediaAvailable"), true},
        {QStringLiteral("Optical"), false}, {QStringLiteral("Serial"), QStringLiteral("RUFUS-MBR-TEST")},
        {QStringLiteral("TimeDetected"), QVariant::fromValue(quint64(1))},
        {QStringLiteral("TimeMediaDetected"), QVariant::fromValue(quint64(2))}};
    QDBusMessage message = QDBusMessage::createMethodCall(QStringLiteral("org.rufus.TestUDisks"),
        QStringLiteral("/org/freedesktop/UDisks2"), QStringLiteral("org.rufus.TestUDisks"), QStringLiteral("SetObjects"));
    message.setArguments({QVariant::fromValue(objects)});
    require(connection.call(message).type() == QDBusMessage::ReplyMessage, "publish fixture snapshot");
    rufus::Device device;
    require(rufus::DescribeUDisksDevice(objects, disk.path(), device), "describe fixture");
    rufus::PrepareImageRequest request;
    request.path = device.path;
    request.device = device.number;
    request.size = device.size;
    request.logicalSectorSize = device.logicalSectorSize;
    request.physicalSectorSize = device.physicalSectorSize;
    request.objectPath = device.objectPath;
    request.identity = device.identity;
    request.serviceOwner = connection.interface()->serviceOwner(QStringLiteral("org.freedesktop.UDisks2")).value();
    request.diskSequence = rufus::ReadDiskSequence(device.number);
    request.isDevice = true;
    request.volumeId = 0x1234abcd;
    if (QFile::exists(QStringLiteral("/etc/rufus-fat16-fixture")))
    {
        request.fileSystem = FS_FAT16;
        request.clusterSize = 2048;
    }
    if (diskBytes == alpineDiskBytes)
        request.clusterSize = 4096;
    request.label = QStringLiteral("RUFUS TEST");
    const int selectionError = rufus::CheckPrepareDeviceSelection(connection, request);
    if (QFile::exists(QStringLiteral("/etc/rufus-geometry-fixture")))
        printf("RUFUS_GEOMETRY_BASE NODE=%s LOGICAL=%u PHYSICAL=%u ERROR=%d\n", qPrintable(node),
            request.logicalSectorSize, request.physicalSectorSize, selectionError);
    fflush(stdout);
    require(selectionError == 0, "production snapshot check");
    return request;
}

FAT32_FORMAT_OPTIONS prepareDisk(rufus::PrepareDeviceTarget& target, const rufus::PrepareImageRequest& request, Table table, quint64 bootBytes = 0)
{
    quint64 offset, size;
    if (table == Table::Mbr)
    {
        const MBR_IMAGE_OPTIONS options = {diskBytes, sectorBytes, 63, request.clusterSize, 0x78563412, request.device, request.fileSystem, MBR_BOOT_SYSLINUX, bootBytes};
        MBR_IMAGE_RESULT result;
        require(PrepareMbrTarget(target.descriptor(), &options, &result) == 0, "prepare MBR");
        offset = result.offset;
        size = result.size;
    }
    else
    {
        const GPT_IMAGE_OPTIONS options = {diskBytes, sectorBytes, 63, sectorBytes, request.device, 0};
        GPT_IMAGE_RESULT result;
        require(CreateGptImagePartition(target.descriptor(), &options, &result) == 0, "prepare GPT");
        offset = result.offset;
        size = result.size;
    }
    require(ioctl(target.descriptor(), BLKRRPART, 0) == 0, "refresh kernel partitions");
    // This small initramfs has no udev. Supply only its usual /dev/block links;
    // all device facts still come from real kernel sysfs and ioctls.
    require(QDir().mkpath(QStringLiteral("/dev/block")), "partition link directory");
    const int partitions = bootBytes != 0 ? 2 : 1;
    for (int number = 1; number <= partitions; number++)
    {
        struct stat info;
        const QByteArray node = "/dev/sda" + QByteArray::number(number);
        require(stat(node.constData(), &info) == 0 && S_ISBLK(info.st_mode), "partition node");
        const QByteArray link = QByteArray("/dev/block/") + QByteArray::number(major(info.st_rdev)) + ':'
            + QByteArray::number(minor(info.st_rdev));
        const QByteArray relative = "../sda" + QByteArray::number(number);
        if (symlink(relative.constData(), link.constData()) != 0)
            require(errno == EEXIST, "partition link");
    }
    FAT32_FORMAT_OPTIONS format = {offset, size, request.clusterSize, request.volumeId, "RUFUS TEST"};
    return format;
}

bool runExfatRefusals(const rufus::PrepareImageRequest& request)
{
    constexpr int initialPosition = 17;
    constexpr int probeBytes = 64 * 1024;
    rufus::PrepareDeviceTarget target(request);
    const int openError = target.open();
    int sector = 0;
    require(ioctl(target.descriptor(), BLKSSZGET, &sector) == 0, "exFAT device sector");
    if (sector != sectorBytes)
    {
        require(openError == ENOTSUP, "exFAT 4K target refused");
        const rufus::ExfatFormatOptions options = {request.size, request.clusterSize, request.label, 0};
        FORMAT_RESULT result;
        const int error = rufus::FormatExfatDevice(target.descriptor(), request.device,
            QStringLiteral("/usr/bin/mkfs.exfat"), options, result);
        require(error == ENOTSUP && !result.started, "exFAT 4K formatter refused");
        puts("RUFUS_EXFAT_CASE unsupported-sector ERROR=95 STARTED=0");
        puts("RUFUS_EXFAT_DONE");
        return false;
    }
    require(openError == 0, "exFAT target open");
    const FAT32_FORMAT_OPTIONS extent = prepareDisk(target, request, Table::Gpt);
    const rufus::ExfatFormatOptions options = {extent.size, request.clusterSize, request.label, extent.offset};
    struct stat info;
    require(stat("/dev/sda1", &info) == 0, "exFAT partition stat");
    struct DirectCase { const char* name; int error; bool started; };
    const DirectCase cases[] = {
        {"zero-device", EINVAL, false}, {"wrong-device", ESTALE, false},
        {"wrong-capacity", ESTALE, false}, {"wrong-offset", ESTALE, false},
        {"unaligned-offset", EINVAL, false}, {"readonly-fd", EINVAL, false}, {"readonly-device", EROFS, false},
        {"whole-disk", ENOTSUP, false}, {"file-api-refused", ENOTSUP, false},
        {"metadata-space", ENOSPC, false}, {"held-claim", EIO, true}
    };
    for (const DirectCase& test : cases)
    {
        rufus::ExfatFormatOptions changed = options;
        quint64 device = quint64(info.st_rdev);
        const bool readonly = strcmp(test.name, "readonly-fd") == 0;
        const int partition = open("/dev/sda1", (readonly ? O_RDONLY : O_RDWR) | O_CLOEXEC);
        require(partition >= 0, "direct exFAT partition open");
        const QByteArray before = readBytes(partition, 0, probeBytes);
        require(lseek(partition, initialPosition, SEEK_SET) == initialPosition, "direct initial position");
        if (strcmp(test.name, "zero-device") == 0) device = 0;
        if (strcmp(test.name, "wrong-device") == 0) device++;
        if (strcmp(test.name, "wrong-capacity") == 0) changed.size -= sectorBytes;
        if (strcmp(test.name, "wrong-offset") == 0) changed.partitionOffset += 1024 * 1024;
        if (strcmp(test.name, "unaligned-offset") == 0) changed.partitionOffset++;
        if (strcmp(test.name, "metadata-space") == 0) changed.clusterSize = 32 * 1024 * 1024;
        int readOnly = strcmp(test.name, "readonly-device") == 0;
        if (readOnly)
            require(ioctl(partition, BLKROSET, &readOnly) == 0, "set native read-only state");
        FORMAT_RESULT result;
        int error;
        if (strcmp(test.name, "whole-disk") == 0)
        {
            changed.size = request.size;
            changed.partitionOffset = 0;
            error = rufus::FormatExfatDevice(target.descriptor(), request.device,
                QStringLiteral("/usr/bin/mkfs.exfat"), changed, result);
        }
        else if (strcmp(test.name, "file-api-refused") == 0)
            error = rufus::FormatExfatImage(partition, QStringLiteral("/usr/bin/mkfs.exfat"), changed, result);
        else
            error = rufus::FormatExfatDevice(partition, device, QStringLiteral("/usr/bin/mkfs.exfat"), changed, result);
        require(error == test.error && result.started == test.started, test.name);
        if (readOnly)
        {
            readOnly = 0;
            require(ioctl(partition, BLKROSET, &readOnly) == 0, "restore native writable state");
        }
        require(lseek(partition, 0, SEEK_CUR) == initialPosition, "direct position retained");
        require(before == readBytes(partition, 0, before.size()), "refusal left boot region untouched");
        require(close(partition) == 0, "direct partition close");
        checkClaim(target);
        printf("RUFUS_EXFAT_CASE %s ERROR=%d STARTED=%d\n", test.name, error, result.started);
    }
    require(target.checkMountSupport() == ENOTSUP && target.mount(extent.offset, extent.size) == ENOTSUP,
        "exFAT copying stays gated");
    return true;
}

void runExfatCases(rufus::PrepareImageRequest request)
{
    request.fileSystem = FS_EXFAT;
    request.clusterSize = 4096;
    request.label = QString::fromUtf8("Rufus ș😀");
    const int descriptors = descriptorCount();
    if (!runExfatRefusals(request))
        return;
    require(descriptorCount() == descriptors, "exFAT refusal descriptor leak");
    struct HandoffCase { const char* name; Fault fault; int error; };
    const HandoffCase cases[] = {
        {"snapshot-fail", Fault::SnapshotRead, EIO},
        {"gpt-primary-changed", Fault::FormatGpt, EBADMSG},
        {"gpt-backup-changed", Fault::FormatBackup, EBADMSG},
        {"busy-reacquire", Fault::FormatBusy, EBUSY},
        {"cancel-after", Fault::FormatCancel, ECANCELED},
        {"sync-fail", Fault::Sync, EIO}, {"success", Fault::None, 0}
    };
    for (const HandoffCase& test : cases)
    {
        fault = Fault::None;
        cancelled = false;
        {
            rufus::PrepareDeviceTarget target(request);
            require(target.open() == 0, "exFAT handoff open");
            const FAT32_FORMAT_OPTIONS extent = prepareDisk(target, request, Table::Gpt);
            const rufus::ExfatFormatOptions options = {extent.size, request.clusterSize, request.label, extent.offset};
            const QByteArray prefix = readBytes(target.descriptor(), 0, int(extent.offset));
            const quint64 end = extent.offset + extent.size;
            const QByteArray suffix = readBytes(target.descriptor(), end, int(request.size - end));
            fault = test.fault;
            exfatHandoff = true;
            FORMAT_RESULT result;
            const int error = target.formatExfat(options, QStringLiteral("/usr/bin/mkfs.exfat"), result);
            exfatHandoff = false;
            require(error == test.error, test.name);
            require(result.started == (fault != Fault::SnapshotRead), "exFAT started flag");
            if (fault == Fault::FormatBusy)
            {
                require(target.descriptor() < 0 && busyDescriptor >= 0, "exFAT competing claim respected");
                require(close(busyDescriptor) == 0, "release exFAT competing claim");
                busyDescriptor = -1;
            }
            else
                checkClaim(target);
            if (error == 0)
            {
                require(result.filesystem_bytes == extent.size && fsync(target.descriptor()) == 0, "exFAT final disk sync");
                require(prefix == readBytes(target.descriptor(), 0, prefix.size()), "exFAT prefix preserved");
                require(suffix == readBytes(target.descriptor(), end, suffix.size()), "exFAT suffix preserved");
                printf("RUFUS_EXFAT_VOLUME OFFSET=%llu SIZE=%llu\n",
                    (unsigned long long)extent.offset, (unsigned long long)extent.size);
            }
            printf("RUFUS_EXFAT_CASE %s ERROR=%d STARTED=%d SYNC=%d\n", test.name, error, result.started, result.sync_error);
        }
        require(descriptorCount() == descriptors, "exFAT descriptor leak");
    }
    fault = Fault::None;
    cancelled = false;
    puts("RUFUS_EXFAT_DONE");
}

void writeFixture(const char* directory, bool fill)
{
    const QByteArray path = QByteArray(directory) + "/filled.bin";
    const int fd = open(path.constData(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
    require(fd >= 0, "create filesystem file");
    quint64 remaining = sectorBytes;
    if (fill)
    {
        struct statvfs info;
        require(fstatvfs(fd, &info) == 0 && info.f_frsize != 0, "free filesystem space");
        remaining = quint64(info.f_bavail) * info.f_frsize;
        require(remaining > diskBytes / 2, "meaningful full-volume fixture");
        printf("RUFUS_HANDOFF_FILLED_BYTES=%llu\n", static_cast<unsigned long long>(remaining));
    }
    const QByteArray buffer(1024 * 1024, 'K');
    while (remaining != 0)
    {
        const size_t count = size_t(qMin(remaining, quint64(buffer.size())));
        const ssize_t written = write(fd, buffer.constData(), count);
        if (written < 0 && errno == EINTR)
            continue;
        require(written > 0, "write filesystem fixture");
        remaining -= quint64(written);
    }
    require(close(fd) == 0, "close filesystem fixture");
}

struct Case
{
    const char* name;
    Table table;
    Fault fault = Fault::None;
    int formatError = 0;
    int mountError = 0;
    int cleanupError = 0;
    int syncError = 0;
    bool fill = false;
};

void runCase(const Case& test, const rufus::PrepareImageRequest& request)
{
    fault = Fault::None;
    cancelled = false;
    rufus::PrepareDeviceTarget target(request, test.table);
    require(target.open() == 0, "open target through production backend");
    const FAT32_FORMAT_OPTIONS options = prepareDisk(target, request, test.table);
    fault = test.fault;
    FAT32_FORMAT_RESULT formatted = {};
    const int formatError = target.format(options, "/usr/bin/mkfs.fat", formatted);
    require(formatError == test.formatError, "format outcome");
    require(formatted.started == (test.fault != Fault::SnapshotRead), "formatter started flag");
    if (test.fault == Fault::FormatBusy)
    {
        require(target.descriptor() < 0 && busyDescriptor >= 0, "busy claim was not bypassed");
        require(close(busyDescriptor) == 0, "release competing fixture claim");
        busyDescriptor = -1;
    }
    else
        checkClaim(target);
    if (formatError != 0)
    {
        if (test.fault == Fault::SnapshotRead)
        {
            fault = Fault::None;
            require(target.mount(options.offset, options.size) == EINVAL, "failed snapshot cannot authorize mount");
        }
        printf("RUFUS_HANDOFF_CASE %s ERROR=%d\n", test.name, formatError);
        return;
    }
    const QByteArray table = readBytes(target.descriptor(), 0, sectorBytes);
    const QByteArray tail = readBytes(target.descriptor(), diskBytes - gptBackupBytes, gptBackupBytes);
    if (test.fault == Fault::BeforeMount)
        corruptDisk(diskSignatureOffset);
    const int mountError = target.mount(options.offset, options.size);
    require(mountError == test.mountError, "mount outcome");
    const QByteArray mountPath(target.mountPath());
    if (mountError == 0)
    {
        writeFixture(target.mountPath(), test.fill);
        if (test.fault == Fault::MountedMbr)
            corruptDisk(diskSignatureOffset);
    }
    int syncError = 0;
    const int cleanupError = target.finishMount(syncError);
    require(cleanupError == test.cleanupError && syncError == test.syncError, "cleanup outcome");
    checkClaim(target);
    require(mountPath.isEmpty() || access(mountPath.constData(), F_OK) != 0, "private mountpoint removed");
    if (mountError == 0 && cleanupError == 0)
        require(readBytes(target.descriptor(), 0, sectorBytes) == table, "MBR/PMBR unchanged");
    if (test.fill)
    {
        require(readBytes(target.descriptor(), diskBytes - gptBackupBytes, gptBackupBytes) != tail,
            "filesystem writes reached the former GPT backup area");
        puts("RUFUS_HANDOFF_FILESYSTEM_TAIL_CHANGED");
    }
    printf("RUFUS_HANDOFF_CASE %s ERROR=%d CLEANUP=%d SYNC=%d\n", test.name, mountError, cleanupError, syncError);
}

struct SfdCase
{
    const char* name;
    int error;
    bool started;
    Fault fault = Fault::None;
};

void runSfdCase(const SfdCase& test, const rufus::PrepareImageRequest& request)
{
    fault = Fault::None;
    cancelled = false;
    rufus::PrepareDeviceTarget target(request, Table::Sfd);
    require(target.open() == 0, "SFD target open");
    prepareDisk(target, request, Table::Gpt);
    // The kernel still sees the old GPT children until format explicitly
    // refreshes it, even though on-media metadata is now cleared for SFD.
    if (strcmp(test.name, "table-still-present") != 0)
    {
        const SFD_IMAGE_OPTIONS layout = {request.size, sectorBytes, request.device};
        SFD_IMAGE_RESULT cleared;
        require(PrepareSfdTarget(target.descriptor(), &layout, &cleared) == 0, "SFD metadata preparation");
    }
    require(QFile::exists(QStringLiteral("/sys/class/block/sda1/partition")), "stale kernel partition present");
    FAT32_FORMAT_OPTIONS options = {0, request.size, request.clusterSize, request.volumeId, "RUFUS SFD"};
    const QByteArray before = readBytes(target.descriptor(), 0, 64 * 1024);
    if (strcmp(test.name, "wrong-offset") == 0)
        options.offset = sectorBytes;
    if (strcmp(test.name, "wrong-size") == 0)
        options.size -= sectorBytes;
    if (strcmp(test.name, "invalid-cluster") == 0)
        options.cluster_size = 0;
    int child = -1;
    if (strcmp(test.name, "busy-child") == 0)
    {
        child = open("/dev/sda1", O_RDWR | O_CLOEXEC);
        require(child >= 0, "hold old partition open");
    }
    const char* mkfs = "/usr/bin/mkfs.fat";
    if (strcmp(test.name, "formatter-failure") == 0)
    {
        QFile script(QStringLiteral("/run/sfd-formatter"));
        require(script.open(QIODevice::WriteOnly), "SFD formatter fixture");
        require(script.write("#!/usr/bin/bash\nexit 7\n") > 0, "SFD formatter failure script");
        script.close();
        require(script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner), "SFD formatter permissions");
        mkfs = "/run/sfd-formatter";
    }
    if (strcmp(test.name, "cancel-before") == 0)
        cancelled = true;
    fault = test.fault;
    FAT32_FORMAT_RESULT formatted;
    const int error = target.format(options, mkfs, formatted);
    require(error == test.error && formatted.started == test.started, "SFD format outcome");
    if (child >= 0)
        require(close(child) == 0, "release old partition");
    if (test.fault == Fault::FormatBusy)
    {
        require(target.descriptor() < 0 && busyDescriptor >= 0, "SFD competing claim not bypassed");
        require(close(busyDescriptor) == 0, "release SFD competing claim");
        busyDescriptor = -1;
    }
    else
    {
        checkClaim(target);
        if (!test.started)
            require(readBytes(target.descriptor(), 0, before.size()) == before, "SFD refusal preserved media");
    }
    if (test.started)
        require(!QFile::exists(QStringLiteral("/sys/class/block/sda1/partition")), "old kernel partition removed");
    if (error == 0)
    {
        require(target.check() == 0, "SFD final selection check");
        require(formatted.filesystem_bytes <= request.size, "SFD filesystem fits");
        require(target.checkMountSupport() == ENOTSUP && target.mount(0, request.size) == ENOTSUP, "SFD copy route gated");
        require(fsync(target.descriptor()) == 0, "SFD final device sync");
    }
    printf("RUFUS_SFD_CASE %s ERROR=%d STARTED=%d SYNC=%d\n", test.name, error, formatted.started, formatted.sync_error);
}

void runSfdCases(rufus::PrepareImageRequest request)
{
    request.nonBootable = true;
    sfdHandoff = true;
    bool largeSector = false;
    {
        rufus::PrepareDeviceTarget target(request, Table::Sfd);
        const int error = target.open();
        int sector = 0;
        require(ioctl(target.descriptor(), BLKSSZGET, &sector) == 0, "SFD sector size");
        if (sector != sectorBytes)
        {
            require(error == ENOTSUP, "SFD unsupported sector refusal");
            largeSector = true;
        }
        else
            require(error == 0, "SFD supported sector size");
    }
    if (largeSector)
    {
        auto raw = request;
        raw.nonBootable = raw.sfd = false;
        raw.rawWrite = true;
        rufus::PrepareDeviceTarget target(raw, Table::Raw);
        require(target.open() == 0, "4K DD helper acquisition remains supported");
        require(CheckRawBlockTarget(target.descriptor(), raw.device, raw.size, 4096) == 0,
            "4K DD native descriptor validation");
        printf("RUFUS_SFD_CASE unsupported-sector ERROR=%d STARTED=0\n", ENOTSUP);
        puts("RUFUS_RAW_4K_ACQUISITION_OK");
        return;
    }
    rufus::PrepareImageRequest unsupported = request;
    unsupported.nonBootable = false;
    { rufus::PrepareDeviceTarget target(unsupported, Table::Sfd);
      require(target.open() == ENOTSUP, "SFD bootable request refused"); }
    unsupported = request;
    unsupported.fileSystem = FS_UNKNOWN;
    { rufus::PrepareDeviceTarget target(unsupported, Table::Sfd);
      require(target.open() == ENOTSUP, "SFD unknown filesystem refused"); }
    const SfdCase cases[] = {
        {"table-still-present", EBUSY, false}, {"wrong-offset", EINVAL, false},
        {"wrong-size", EINVAL, false}, {"invalid-cluster", EINVAL, false},
        {"busy-child", EBUSY, false}, {"cancel-before", ECANCELED, false},
        {"formatter-failure", EIO, true}, {"sync-fail", EIO, true, Fault::Sync},
        {"busy-reacquire", EBUSY, true, Fault::FormatBusy},
        {"cancel-after", ECANCELED, true, Fault::FormatCancel},
        {"nested-mbr", EBADMSG, true, Fault::SfdEntries},
        {"changed-bpb", EBADMSG, true, Fault::SfdBpb}, {"success", 0, true}
    };
    for (const SfdCase& test : cases)
    {
        const int before = descriptorCount();
        runSfdCase(test, request);
        require(descriptorCount() == before, "SFD descriptor cleanup");
    }
    puts("RUFUS_SFD_DONE");
}

enum class InstallChange { None, FileOnly, Device, Capacity, Sequence, Extent, VolumeId, FileSystem };
struct InstallCase
{
    const char* name;
    InstallChange change;
    int error;
    bool started;
    Fault fault = Fault::None;
};

FAT32_FORMAT_OPTIONS prepareInstallerTarget(rufus::PrepareDeviceTarget& target, const rufus::PrepareImageRequest& request)
{
    require(target.open() == 0, "installer target open");
    const FAT32_FORMAT_OPTIONS format = prepareDisk(target, request, Table::Mbr);
    FAT32_FORMAT_RESULT formatted;
    require(target.format(format, "/usr/bin/mkfs.fat", formatted) == 0, "installer FAT format");
    require(target.mount(format.offset, format.size) == 0, "installer extraction mount");
    const ISO_ANALYSIS_OPTIONS scanOptions = {true, false, {0x0407, 0x0604}};
    const ISO_EXTRACT_OPTIONS extractOptions = {scanOptions, "RUFUS TEST", "/source.iso", false};
    ISO_EXTRACT_RESULT extracted;
    require(ExtractIsoTree("/source.iso", target.mountPath(), &extractOptions, &extracted) == 0, "shared ISO extraction");
    int syncError = 0;
    require(target.finishMount(syncError) == 0 && syncError == 0, "installer extraction unmount");
    checkClaim(target);
    return format;
}

void runInstallCase(const InstallCase& test, const rufus::PrepareImageRequest& request)
{
    fault = Fault::None;
    cancelled = false;
    rufus::PrepareDeviceTarget target(request, Table::Mbr);
    const FAT32_FORMAT_OPTIONS format = prepareInstallerTarget(target, request);
    const ISO_ANALYSIS_OPTIONS scanOptions = {true, false, {0x0407, 0x0604}};
    IMAGE_FILE_SOURCE retained = {};
    require(OpenImageFileSource("/source.iso", &retained) == 0, "installer source open");
    const QByteArray sourcePath = QByteArray("/proc/self/fd/") + QByteArray::number(retained.fd);
    ISO_ANALYSIS_SOURCE source = {};
    ISO_ANALYSIS analysis = {};
    const int scanned = AnalyzeIsoImageWithSource(sourcePath.constData(), uint64_t(retained.info.st_size),
        &scanOptions, &analysis, &source);
    require(scanned == ISO_SCAN_OK || scanned == ISO_SCAN_PARTIAL, "installer source scan");
    MBR_IMAGE_OPTIONS layout = {diskBytes, sectorBytes, 63, request.clusterSize, 0x78563412, request.device, request.fileSystem, MBR_BOOT_SYSLINUX, 0};
    rufus::NativeSyslinuxRequest install;
    install.target = target.descriptor();
    install.format = format;
    install.fileSystem = request.fileSystem;
    install.image = &retained;
    install.imagePath = "/source.iso";
    install.source = &source;
    install.analysis = &analysis;
    install.installer = QStringLiteral("/usr/bin/rufus-syslinux");
    install.toolsDirectory = QStringLiteral("/run/tools");
    install.temporaryRoot = QStringLiteral("/run");
    install.deviceLayout = &layout;
    install.diskSequence = request.diskSequence;
    switch (test.change)
    {
    case InstallChange::FileOnly: install.deviceLayout = nullptr; break;
    case InstallChange::Device: layout.device++; break;
    case InstallChange::Capacity: layout.disk_size += sectorBytes; break;
    case InstallChange::Sequence: install.diskSequence++; break;
    case InstallChange::Extent: install.format.offset += sectorBytes; break;
    case InstallChange::VolumeId: install.format.volume_id ^= 1; break;
    case InstallChange::FileSystem: layout.file_system = request.fileSystem == FS_FAT16 ? FS_FAT32 : FS_FAT16; break;
    case InstallChange::None: break;
    }
    const int flags = fcntl(install.target, F_GETFL), fdFlags = fcntl(install.target, F_GETFD);
    require(lseek(install.target, 17, SEEK_SET) == 17, "installer caller position");
    const QByteArray table = readBytes(install.target, 0, sectorBytes);
    fault = test.fault;
    FORMAT_RESULT result;
    QString problem;
    const int error = rufus::InstallNativeSyslinux(install, result, problem);
    if (error != test.error)
        qFatal("Installer case %s: error=%d expected=%d: %s; %s", test.name, error, test.error,
            qPrintable(problem), result.diagnostics);
    require(result.started == test.started, "installer started flag");
    if (result.started)
    {
        require(strstr(result.diagnostics, "RUFUS_INSTALL_CHILD_CLAIM_OK") != nullptr, "claim checked inside installer child");
        fputs(result.diagnostics, stdout);
    }
    require(result.sync_error == (test.fault == Fault::InstallSync ? EIO : 0), "installer sync outcome");
    require(problem.isEmpty() == (error == 0), "installer failure diagnostic");
    checkClaim(target);
    const bool sameFlags = flags == fcntl(install.target, F_GETFL) && fdFlags == fcntl(install.target, F_GETFD);
    require(sameFlags && lseek(install.target, 0, SEEK_CUR) == 17, "installer descriptor ownership");
    require(QDir(QStringLiteral("/run")).entryList({QStringLiteral("rufus-syslinux-*")}, QDir::Dirs).isEmpty(),
        "installer private directory removed");
    int status;
    const int child = waitpid(-1, &status, WNOHANG);
    // The fixture and bus remain alive; no installer child may still be waitable.
    require(child == 0, "installer descendants reaped");
    if (test.fault != Fault::InstallMbr)
        require(readBytes(install.target, 0, sectorBytes) == table, "installer preserved MBR");
    CloseIsoAnalysisSource(&source);
    CloseImageFileSource(&retained);
    printf("RUFUS_INSTALL_CASE %s ERROR=%d STARTED=%d SYNC=%d\n", test.name, error, result.started, result.sync_error);
}

void runInstallCases(const rufus::PrepareImageRequest& request, qint64 daemonPid, qint64 fixturePid)
{
    require(prctl(PR_SET_CHILD_SUBREAPER, 1) == 0, "installer subreaper");
    const InstallCase cases[] = {
        {"file-only-refused", InstallChange::FileOnly, ENOTSUP, false},
        {"wrong-device", InstallChange::Device, ESTALE, false},
        {"wrong-capacity", InstallChange::Capacity, ESTALE, false},
        {"wrong-sequence", InstallChange::Sequence, ESTALE, false},
        {"wrong-extent", InstallChange::Extent, EINVAL, false},
        {"wrong-volume-id", InstallChange::VolumeId, EBADMSG, false},
        {"wrong-filesystem", InstallChange::FileSystem, EINVAL, false},
        {"mbr-changed", InstallChange::None, EBADMSG, true, Fault::InstallMbr},
        {"cancel-after", InstallChange::None, ECANCELED, true, Fault::InstallCancel},
        {"sync-fail", InstallChange::None, EIO, true, Fault::InstallSync},
        {"success", InstallChange::None, 0, true}
    };
    const int descriptors = descriptorCount();
    for (const InstallCase& test : cases)
    {
        runInstallCase(test, request);
        require(descriptorCount() == descriptors, "installer descriptor leak");
        const QString path = QStringLiteral("/proc/self/task/%1/children").arg(getpid());
        const QList<QByteArray> children = readFile(path).simplified().split(' ');
        const bool onlyFixtures = children.size() == 2 && children.contains(QByteArray::number(daemonPid))
            && children.contains(QByteArray::number(fixturePid));
        require(onlyFixtures, "no live or zombie installer descendants");
    }
    puts("RUFUS_INSTALL_DONE");
}

// These cases run the production helper through its actual pipe protocol.
// Only UDisks discovery is a fixture; partitioning, formatting, extraction,
// mounting, installation and cleanup all operate on the guest USB device.
struct HelperCase
{
    const char* name;
    const char* source;
    int error;
    bool started;
    bool cancel = false;
};

QByteArray diskDigest()
{
    QFile disk(QStringLiteral("/dev/sda"));
    require(disk.open(QIODevice::ReadOnly), "hash disk open");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    require(hash.addData(&disk), "hash disk read");
    return hash.result();
}

void runHelperCase(const HelperCase& test, rufus::PrepareImageRequest request)
{
    require(chown(test.source, 1000, 1000) == 0, "source caller ownership");
    require(rufus::CapturePrepareSource(QString::fromUtf8(test.source), request) == 0, "capture helper source");
    request.syslinux = true;
    const QByteArray before = test.started ? QByteArray() : diskDigest();
    QProcess helper;
    helper.setProcessChannelMode(QProcess::ForwardedErrorChannel);
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("PKEXEC_UID"), QStringLiteral("1000"));
    helper.setProcessEnvironment(environment);
    helper.start(QStringLiteral("/usr/bin/rufus-prepare-helper"), {QStringLiteral("--device")});
    require(helper.waitForStarted(processTimeoutMs), "start production helper");
    QElapsedTimer elapsed;
    elapsed.start();
    QByteArray pending;
    QStringList phases;
    int readyCount = 0, resultCount = 0;
    bool sentCancel = false;
    quint64 previousProgress = 0;
    rufus::PrepareImageOutcome outcome;
    const int helperTimeoutMs = diskBytes > handoffDiskBytes ? 120000 : 30000;
    while (helper.state() != QProcess::NotRunning || helper.bytesAvailable() != 0)
    {
        require(elapsed.elapsed() < helperTimeoutMs, "helper deadline");
        helper.waitForReadyRead(50);
        pending += helper.readAllStandardOutput();
        while (pending.contains('\n'))
        {
            const qsizetype end = pending.indexOf('\n');
            const QByteArray line = pending.first(end);
            pending.remove(0, end + 1);
            require(line.size() < rufus::PrepareMessageBytes, "bounded helper frame");
            printf("RUFUS_HELPER_FRAME %s %s\n", test.name, line.constData());
            const QJsonDocument document = QJsonDocument::fromJson(line);
            require(document.isObject() && resultCount == 0, "valid frame before result");
            const QJsonObject object = document.object();
            const QString event = object.value(QStringLiteral("event")).toString();
            if (event == QStringLiteral("ready"))
            {
                require(++readyCount == 1, "single helper request");
                const QByteArray message = QJsonDocument(rufus::EncodePrepareRequest(request))
                    .toJson(QJsonDocument::Compact) + '\n';
                require(helper.write(message) == message.size(), "send Syslinux request");
            }
            else if (event == QStringLiteral("phase"))
                phases.append(object.value(QStringLiteral("phase")).toString());
            else if (event == QStringLiteral("progress"))
            {
                int operation;
                quint64 processed, total;
                require(rufus::DecodePrepareProgress(object, operation, processed, total), "copy progress frame");
                const bool copyPhase = phases.last() == QStringLiteral("extracting") && operation == OP_FILE_COPY;
                require(copyPhase && processed >= previousProgress && processed <= total, "monotonic copy progress");
                previousProgress = processed;
            }
            else if (event == QStringLiteral("result"))
            {
                require(rufus::DecodePrepareOutcome(object, outcome), "helper result frame");
                resultCount++;
            }
            else
                require(event == QStringLiteral("log"), "known helper event");
        }
        if (test.cancel && !sentCancel && QFile::exists(QStringLiteral("/run/installer-blocked")))
        {
            require(phases.last() == QStringLiteral("finalizing") && resultCount == 0, "installer before completion");
            checkExclusiveClaim();
            helper.closeWriteChannel();
            sentCancel = true;
        }
    }
    const bool cleanExit = helper.exitStatus() == QProcess::NormalExit
        && helper.exitCode() == (test.error == 0 ? 0 : 1);
    require(cleanExit && pending.isEmpty() && readyCount == 1 && resultCount == 1, "single result and joined helper");
    if (outcome.error != test.error)
        qFatal("Helper case %s: error=%d expected=%d", test.name, outcome.error, test.error);
    require(outcome.started == test.started && outcome.syncError == 0 && outcome.cleanupError == 0, "helper cleanup result");
    require(sentCancel == test.cancel, "requested cancellation reached installer");
    const QStringList expected = {QStringLiteral("checking"), QStringLiteral("partitioning"),
        QStringLiteral("formatting"), QStringLiteral("mounting"), QStringLiteral("extracting"), QStringLiteral("finalizing")};
    require(phases == (test.started ? expected : expected.first(1)), "helper phase order");
    if (!test.started)
        require(diskDigest() == before, "preflight refusal leaves entire disk unchanged");
    else
        require(outcome.copiedBytes > 0 && previousProgress > 0, "shared extraction and progress ran");
    const int fd = open("/dev/sda", O_RDWR | O_EXCL | O_CLOEXEC);
    require(fd >= 0 && close(fd) == 0, "helper released claim and mounts before exit");
    require(QDir(QStringLiteral("/run")).entryList({QStringLiteral("rufus-*")}, QDir::Dirs).isEmpty(),
        "helper temporary directories removed");
    printf("RUFUS_HELPER_CASE %s ERROR=%d STARTED=%d SYNC=%d CLEANUP=%d\n", test.name,
        outcome.error, outcome.started, outcome.syncError, outcome.cleanupError);
}

void runHelperCases(const rufus::PrepareImageRequest& request, qint64 daemonPid, qint64 fixturePid, bool alpine)
{
    require(prctl(PR_SET_CHILD_SUBREAPER, 1) == 0, "helper descendant audit");
    {
        // Supply the udev link once in this minimal guest. The helper still
        // replaces the table and refreshes the kernel itself on every job.
        rufus::PrepareDeviceTarget target(request, Table::Mbr);
        require(target.open(1000) == 0, "seed guest partition link");
        prepareDisk(target, request, Table::Mbr);
    }
    QList<HelperCase> cases = {
        {"missing-installer", "/source.iso", ENOENT, false},
        {"incompatible-source", "/incompatible.iso", ENOTSUP, false},
        {"cancel-installer", "/source.iso", ECANCELED, true, true},
        {"known-success", "/source.iso", 0, true},
        {"unknown-success", "/unknown.iso", 0, true}
    };
    if (alpine)
        cases = {{"alpine", "/source.iso", 0, true}};
    if (QFile::exists(QStringLiteral("/etc/rufus-vhdx-qt-fixture"))
        || QFile::exists(QStringLiteral("/etc/rufus-ffu-qt-fixture"))
        || QFile::exists(QStringLiteral("/etc/rufus-save-qt-fixture")))
        cases = {{"unknown-success", "/unknown.iso", 0, true}};
    const QString installer = QStringLiteral(RUFUS_NATIVE_SYSLINUX_PATH);
    const int descriptors = descriptorCount();
    for (const HelperCase& test : cases)
    {
        const bool missing = strcmp(test.name, "missing-installer") == 0;
        if (missing)
            require(QFile::rename(installer, installer + QStringLiteral(".saved")), "hide installer");
        if (test.cancel)
        {
            require(QFile::rename(QStringLiteral("/usr/bin/mcopy"), QStringLiteral("/usr/bin/mcopy.saved")), "hold real mcopy");
            QFile wrapper(QStringLiteral("/usr/bin/mcopy"));
            require(wrapper.open(QIODevice::WriteOnly), "installer cancellation wrapper");
            const QByteArray script = "#!/bin/bash\necho $$ > /run/installer-blocked\nsleep 60\n";
            require(wrapper.write(script) == script.size(), "write cancellation wrapper");
            wrapper.close();
            require(chmod("/usr/bin/mcopy", 0755) == 0, "executable cancellation wrapper");
        }
        runHelperCase(test, request);
        if (missing)
            require(QFile::rename(installer + QStringLiteral(".saved"), installer), "restore installer");
        if (test.cancel)
        {
            require(QFile::remove(QStringLiteral("/usr/bin/mcopy")), "remove cancellation wrapper");
            require(QFile::rename(QStringLiteral("/usr/bin/mcopy.saved"), QStringLiteral("/usr/bin/mcopy")), "restore mcopy");
            require(QFile::remove(QStringLiteral("/run/installer-blocked")), "remove cancellation marker");
        }
        require(descriptorCount() == descriptors, "helper case descriptor leak");
        const QString path = QStringLiteral("/proc/self/task/%1/children").arg(getpid());
        const QList<QByteArray> children = readFile(path).simplified().split(' ');
        const bool onlyFixtures = children.size() == 2 && children.contains(QByteArray::number(daemonPid))
            && children.contains(QByteArray::number(fixturePid));
        require(onlyFixtures, "helper reaped all installer descendants before exit");
    }
    puts("RUFUS_HELPER_DONE");
}

struct FormatHelperCase
{
    const char* name;
    int error;
    bool started;
    int phases;
    bool faultProbe = false;
    bool cancel = false;
};

void writeGuestFile(const QString& path, const QByteArray& bytes, bool executable = false)
{
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "guest fixture file open");
    require(file.write(bytes) == bytes.size(), "guest fixture file write");
    if (executable)
        require(file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner), "guest executable");
}

void releaseGrub4dosFuseExit(bool standalone)
{
    require(readFile(QStringLiteral("/run/grub4dos-fuse-exit-held")).trimmed() == "0",
        "real foreground NTFS-3G exited cleanly after unmount");
    const QList<QByteArray> pids = readFile(QStringLiteral("/run/grub4dos-fuse-processes")).simplified().split(' ');
    require(pids.size() == 3, "NTFS-3G launcher/helper/native PIDs recorded");
    bool valid = false;
    const qlonglong launcher = pids.at(0).toLongLong(&valid);
    require(valid && launcher > 1, "valid owned NTFS-3G launcher PID");
    const qlonglong helper = pids.at(1).toLongLong(&valid);
    require(valid && helper > 1, "valid NTFS-3G helper parent PID");
    const qlonglong native = pids.at(2).toLongLong(&valid);
    require(valid && native > 1, "valid real NTFS-3G PID");
    require(QFile::exists(QStringLiteral("/proc/%1/status").arg(launcher)), "FUSE launcher still owned");
    require(!QFile::exists(QStringLiteral("/proc/%1/status").arg(native)), "real FUSE child reaped");
    const QByteArray mounts = readFile(QStringLiteral("/proc/%1/mountinfo").arg(helper));
    require(!mounts.isEmpty() && !mounts.contains("/run/rufus-"), "private NTFS mount removed before child join");
    const int snapshots = QDir(QStringLiteral("/tmp")).entryList({QStringLiteral("rufus-grub2-*")}, QDir::Dirs).size();
    require(snapshots == int(standalone), "only standalone GRUB4DOS retains a loader snapshot through FUSE exit");
    writeGuestFile(QStringLiteral("/run/grub4dos-fuse-exit-release"), "release\n");
}

void checkGrub4dosFuseJoined()
{
    const QList<QByteArray> pids = readFile(QStringLiteral("/run/grub4dos-fuse-processes")).simplified().split(' ');
    require(pids.size() == 3, "completed FUSE process audit");
    for (const QByteArray& pid : pids)
        require(!QFile::exists(QStringLiteral("/proc/%1/status").arg(QString::fromLatin1(pid))),
            "NTFS-3G launcher/helper/native descendants gone before completion");
    puts("RUFUS_GRUB4DOS_FUSE_JOINED");
}

void runBiosImage(rufus::PrepareImageRequest request, qint64 daemonPid, qint64 fixturePid, bool windows)
{
    require(prctl(PR_SET_CHILD_SUBREAPER, 1) == 0, "image helper descendant audit");
    const bool grub4dosImage = !windows && QFile::exists(QStringLiteral("/etc/rufus-grub4dos-image-fixture"));
    request.mbr = true;
    request.grub2 = !windows && !grub4dosImage;
    request.grub4dos = grub4dosImage;
    request.windowsBios = windows;
    const QList<QByteArray> windowsChoices = readFile(QStringLiteral("/etc/rufus-windows-bios-fixture")).simplified().split(' ');
    if (windows) request.windowsRufusMbr = windowsChoices.value(0) == "rufus";
    constexpr quint32 fat16ImageClusterBytes = 8192; // 512-MiB fixture below the FAT16 cluster-count limit.
    if (request.fileSystem == FS_FAT16 && !grub4dosImage)
        request.clusterSize = fat16ImageClusterBytes;
    const QString ntfsDriver = QString::fromUtf8(readFile(QStringLiteral("/etc/rufus-ntfs-mount-fixture")).trimmed());
    const bool grub4dosFallback = grub4dosImage && ntfsDriver == QStringLiteral("lowntfs-3g");
    if (grub4dosFallback)
    {
        const int context = int(syscall(SYS_fsopen, "ntfs", FSOPEN_CLOEXEC));
        require(context < 0 && errno == ENODEV, "new kernel NTFS support absent in fallback image guest");
    }
    if (!ntfsDriver.isEmpty())
    {
        request.fileSystem = FS_NTFS;
        request.volumeId = 0;
        request.clusterSize = 4096;
    }
    const bool windowsFat = windows && windowsChoices.value(2) == "fat32";
    const bool syslinuxData = windows && windowsChoices.contains("syslinux-data");
    const bool grub4dosSbr = grub4dosImage || (windows && windowsChoices.contains("grub4dos-sbr"));
    if (windowsFat)
    {
        request.fileSystem = FS_FAT32;
        request.clusterSize = sectorBytes; // 64-MiB fixture must retain enough FAT32 clusters.
    }
    request.uefiNtfs = windows ? windowsChoices.value(1) == "dual" && !windowsFat
        : (grub4dosImage && request.fileSystem == FS_NTFS
            && QFile::exists(QStringLiteral("/etc/rufus-grub4dos-image-uefi-fixture")))
            || readFile(QStringLiteral("/etc/rufus-grub2-image-fixture")).trimmed() == "uefi";
    request.enhancedInstall = windows && (windowsFat || request.uefiNtfs || syslinuxData || grub4dosSbr);
    const QByteArray payload = request.uefiNtfs ? readFile(QStringLiteral("/uefi-ntfs.img")) : QByteArray();
    require(!request.uefiNtfs || !payload.isEmpty(), "GRUB UEFI:NTFS fixture payload");
    request.label = QStringLiteral("RUFUS_TEST");
    {
        // This initramfs has no udev. Publish its usual partition link once;
        // the helper still replaces the table and validates native kernel facts.
        rufus::PrepareDeviceTarget target(request, Table::Mbr);
        require(target.open(1000) == 0, "seed GRUB guest partition link");
        prepareDisk(target, request, Table::Mbr, quint64(payload.size()));
    }
    require(chown("/source.iso", 1000, 1000) == 0, "image source caller ownership");
    require(rufus::CapturePrepareSource(QStringLiteral("/source.iso"), request) == 0, "retain BIOS image identity");
    if (grub4dosImage)
    {
        require(!request.isSourceFree() && !request.hasGrub2Core(), "GRUB4DOS ISO owns its loader");
        require(!QFile::exists(QStringLiteral("/selected-loader")) && !QFile::exists(QStringLiteral("/selected-core.img")),
            "GRUB4DOS image guest has no standalone payload");
        if (QFile::exists(QStringLiteral("/etc/rufus-grub4dos-image-uefi-fixture")))
        {
            const bool fat = request.fileSystem == FS_FAT16 || request.fileSystem == FS_FAT32;
            const bool pairedNtfs = request.fileSystem == FS_NTFS && request.usesUefiNtfs();
            const bool directFat = fat && !request.usesUefiNtfs();
            require((directFat || pairedNtfs) && !request.enhancedInstall,
                "combined image uses direct FAT or paired NTFS");
            puts("RUFUS_GRUB4DOS_IMAGE_DUAL_REQUEST_DONE");
        }
    }
    const QByteArray sourceHash = QCryptographicHash::hash(readFile(request.imagePath), QCryptographicHash::Sha256);
    if (syslinuxData)
    {
        const bool legacyBios = request.enhancedInstall && !request.uefiNtfs && windowsChoices.contains("winpe");
        require(legacyBios && !request.syslinux && !request.grub2,
            "mixed WinPE follows the Windows Alt+E request");
        for (int boot : {BT_SYSLINUX_V4, BT_SYSLINUX_V6})
        {
            const char* installer = rufus::NativeSyslinuxInstaller(boot, FS_FAT32);
            require(installer != nullptr && !QFile::exists(QString::fromLocal8Bit(installer)),
                "mixed guest contains no Syslinux installer");
        }
        if (!windowsFat)
        {
            const char* installer = rufus::NativeSyslinuxInstaller(BT_SYSLINUX_V6, FS_NTFS);
            require(installer != nullptr && !QFile::exists(QString::fromLocal8Bit(installer)),
                "mixed NTFS guest contains no Extlinux installer");
        }
    }
    const QByteArray core = windows || grub4dosImage ? QByteArray() : readFile(QStringLiteral("/selected-core.img"));
    require(!request.grub2 || !core.isEmpty(), "selected GRUB core fixture");
    const int descriptors = descriptorCount();
    const bool winPeFailure = windows && QFile::exists(QStringLiteral("/etc/rufus-winpe-failure-fixture"));
    const QString imageHelper = winPeFailure ? QStringLiteral("/usr/bin/prepare_extract_probe")
        : QStringLiteral("/usr/bin/rufus-prepare-helper");
    rufus::PrepareImageProcess process(imageHelper);
    QSignalSpy completed(&process, &rufus::PrepareImageProcess::finished);
    QSignalSpy phases(&process, &rufus::PrepareImageProcess::phaseChanged);
    QSignalSpy progress(&process, &rufus::PrepareImageProcess::progressChanged);
    QSignalSpy logs(&process, &rufus::PrepareImageProcess::logMessage);
    require(process.start(request, core), "start production BIOS ISO client");
    QElapsedTimer timer; timer.start();
    bool waitedForExit = false, fuseHeld = false;
    constexpr int imageDeadlineMs = 60000;
    while (completed.isEmpty())
    {
        QCoreApplication::processEvents();
        require(timer.elapsed() < imageDeadlineMs, "BIOS image helper deadline");
        if (grub4dosFallback && !fuseHeld && QFile::exists(QStringLiteral("/run/grub4dos-fuse-exit-held")))
        {
            require(process.isActive() && completed.isEmpty(), "GRUB4DOS ISO waits for owned FUSE launcher exit");
            releaseGrub4dosFuseExit(false);
            fuseHeld = true;
        }
        if (completed.isEmpty() && QFile::exists(QStringLiteral("/run/format-exit-held")))
        {
            require(process.isActive(), "BIOS image waits for helper exit");
            if (grub4dosImage)
                require(QDir(QStringLiteral("/run")).entryList({QStringLiteral("rufus-*")}, QDir::Dirs).isEmpty(),
                    "GRUB4DOS ISO private mount removed before held launcher exit");
            waitedForExit = true;
            writeGuestFile(QStringLiteral("/run/format-exit-release"), "release\n");
        }
        usleep(1000);
    }
    const char* logPrefix = windows ? "RUFUS_WINDOWS_IMAGE_LOG"
        : grub4dosImage ? "RUFUS_GRUB4DOS_IMAGE_LOG" : "RUFUS_GRUB2_IMAGE_LOG";
    for (const QList<QVariant>& entry : logs)
        printf("%s %s\n", logPrefix, qPrintable(entry.first().toString()));
    const auto& outcome = process.outcome();
    const auto expectedState = winPeFailure ? rufus::PrepareImageProcess::State::Failed
        : rufus::PrepareImageProcess::State::Complete;
    if (process.state() != expectedState)
        qFatal("BIOS image failed: error=%d sync=%d cleanup=%d problem=%s", outcome.error,
            outcome.syncError, outcome.cleanupError, qPrintable(process.problem()));
    require(outcome.error == (winPeFailure ? EIO : 0), "BIOS image expected job result");
    if (windows && windowsChoices.value(3) == "repair")
    {
        bool repaired = false;
        for (const QList<QVariant>& entry : logs)
            repaired |= entry.first().toString() == QStringLiteral("Setting up the Windows EFI fallback loader.");
        require(repaired, "native helper performed the required Windows EFI repair");
    }
    if (!ntfsDriver.isEmpty())
    {
        const QString expectedLog = ntfsDriver == QStringLiteral("ntfs")
            ? QStringLiteral("NTFS file copying uses the kernel ntfs driver") : QStringLiteral("NTFS file copying uses ntfs-3g");
        bool selectedDriver = false;
        for (const auto& entry : logs)
            selectedDriver |= entry.first().toString() == expectedLog;
        require(selectedDriver, "selected BIOS image NTFS driver");
    }
    if (windows && windowsChoices.contains("winpe"))
    {
        bool prepared = false;
        for (const QList<QVariant>& entry : logs)
            prepared |= entry.first().toString() == QStringLiteral("Setting up legacy WinPE boot files.");
        require(prepared, "helper performed legacy root preparation");
    }
    if (syslinuxData)
    {
        bool unknown = false, installed = false;
        for (const QList<QVariant>& entry : logs)
        {
            const QString message = entry.first().toString();
            unknown |= message.contains(QStringLiteral("Could not detect Isolinux version - No fallback version available"));
            installed |= message.startsWith(QStringLiteral("Installing Syslinux"));
        }
        require(unknown && !installed, "mixed data keeps unknown version without Syslinux installation");
        printf("RUFUS_WINPE_SYSLINUX_DATA_DONE\n");
    }
    if (grub4dosSbr)
    {
        bool installed = false, otherProvider = false;
        for (const QList<QVariant>& entry : logs)
        {
            const QString message = entry.first().toString();
            installed |= message == QStringLiteral("Installing GRUB4DOS boot records.");
            otherProvider |= message == QStringLiteral("Installing GRUB2 boot records.")
                || message.startsWith(QStringLiteral("Installing Syslinux"));
        }
        require(installed && !otherProvider && request.grub2CorePath.isEmpty(), "native bundled GRUB4DOS dispatch");
        puts(grub4dosImage ? "RUFUS_IMAGE_GRUB4DOS_SBR_DONE" : "RUFUS_WINPE_GRUB4DOS_SBR_DONE");
    }
    require(completed.size() == 1 && waitedForExit, "BIOS image completion after helper exit");
    require(outcome.started && outcome.copiedBytes > 0, "BIOS image writes and copies data");
    require(outcome.syncError == 0 && outcome.cleanupError == 0, "BIOS image clean finalization");
    GPT_IMAGE_RESULT layout = {};
    require(rufus::PlanFormatPartition(request, layout) == 0, "BIOS image expected native layout");
    require(outcome.offset == layout.offset && outcome.size == layout.size, "BIOS image confirmed extent");
    if (request.fileSystem == FS_NTFS)
        require(outcome.filesystemBytes == layout.size, "complete image NTFS formatting");
    const bool deferredWinPeBoot = windowsFat && windowsChoices.contains("winpe");
    const QStringList expected = {QStringLiteral("checking"), QStringLiteral("partitioning"),
        QStringLiteral("formatting"), QStringLiteral("bootloader"), QStringLiteral("mounting"),
        QStringLiteral("extracting"), QStringLiteral("finalizing")};
    if (deferredWinPeBoot)
    {
        bool installed = false;
        for (const QList<QVariant>& entry : logs)
            installed |= entry.first().toString() == QStringLiteral("Installing legacy WinPE FAT32 boot records.");
        require(installed == !winPeFailure, "FAT32 records installed only after successful copy/cleanup");
    }
    require(phases.size() == expected.size(), "BIOS ISO phase count");
    for (int i = 0; i < expected.size(); ++i)
        require(phases.at(i).first().toString() == expected.at(i), "BIOS ISO phase order");
    quint64 previous = 0, total = 0;
    for (const QList<QVariant>& entry : progress)
    {
        const quint64 done = entry.at(1).toULongLong(), denominator = entry.at(2).toULongLong();
        const bool valid = entry.first().toInt() == OP_FILE_COPY && done >= previous && done <= denominator;
        require(valid && denominator != 0, "image copy progress range and order");
        require(total == 0 || total == denominator, "stable image copy progress denominator");
        previous = done; total = denominator;
    }
    require(total > 0 && previous == total, "image copy progress finishes");
    require(readFile(QStringLiteral("/run/format-launches")).count("launch\n") == 1, "one image job authorization");
    require(QCryptographicHash::hash(readFile(request.imagePath), QCryptographicHash::Sha256) == sourceHash,
        "image source unchanged");
    const int disk = open("/dev/sda", O_RDWR | O_EXCL | O_CLOEXEC);
    require(disk >= 0, "image helper released disk before completion");
    if (request.grub2) require(readBytes(disk, sectorBytes, core.size()) == core, "selected core on native USB");
    if (request.uefiNtfs)
        require(readBytes(disk, layout.uefi_ntfs_offset, payload.size()) == payload, "complete native image UEFI:NTFS payload");
    require(close(disk) == 0, "GRUB readback disk close");
    require(QDir(QStringLiteral("/tmp")).entryList({QStringLiteral("rufus-grub2-*")}, QDir::Dirs).isEmpty(),
        "private core staging removed");
    require(QDir(QStringLiteral("/run")).entryList({QStringLiteral("rufus-*")}, QDir::Dirs).isEmpty(),
        "private image mount removed");
    require(!readFile(QStringLiteral("/proc/self/mountinfo")).contains("/dev/sda"), "no public target mount");
    require(descriptorCount() == descriptors, "BIOS image descriptor cleanup");
    const QList<QByteArray> children = readFile(QStringLiteral("/proc/self/task/%1/children").arg(getpid())).simplified().split(' ');
    const bool onlyFixtures = children.size() == 2 && children.contains(QByteArray::number(daemonPid))
        && children.contains(QByteArray::number(fixturePid));
    require(onlyFixtures, "BIOS image helper children reaped");
    if (grub4dosFallback)
    {
        require(fuseHeld, "GRUB4DOS ISO observed and joined held FUSE exit");
        checkGrub4dosFuseJoined();
    }
    printf("%s ERROR=%d OFFSET=%llu SIZE=%llu BYTES=%llu COPIED=%llu FAT=%d CLUSTER=%u\n",
        windows ? "RUFUS_WINDOWS_IMAGE_DONE" : grub4dosImage ? "RUFUS_GRUB4DOS_IMAGE_DONE" : "RUFUS_GRUB2_IMAGE_DONE",
        outcome.error, outcome.offset, outcome.size, outcome.filesystemBytes, outcome.copiedBytes,
        request.fileSystem == FS_NTFS ? 0 : request.fileSystem == FS_FAT16 ? 16 : 32, request.clusterSize);
}

void runGrub4dosHelper(rufus::PrepareImageRequest request)
{
    request.grub4dos = request.mbr = true;
    const QString driverPath = QStringLiteral("/etc/rufus-ntfs-mount-fixture");
    const QByteArray ntfsDriver = QFile::exists(driverPath) ? readFile(driverPath).trimmed() : QByteArray();
    const bool fallback = ntfsDriver == "lowntfs-3g";
    if (fallback)
    {
        const int context = int(syscall(SYS_fsopen, "ntfs", FSOPEN_CLOEXEC));
        require(context < 0 && errno == ENODEV, "new kernel NTFS support absent in fallback guest");
    }
    if (QFile::exists(QStringLiteral("/etc/rufus-grub4dos-ntfs-fixture")))
    {
        request.fileSystem = FS_NTFS;
        request.clusterSize = 4096;
        request.volumeId = 0;
    }
    if (request.fileSystem == FS_FAT32)
        request.clusterSize = sectorBytes;
    const QByteArray loader = readFile(QStringLiteral("/selected-loader"));
    require(loader.size() == 322965, "original standalone loader fixture");
    {
        // Minimal initramfs has no udev; publish its ordinary /dev/block link.
        // Production helper still replaces GPT and checks real kernel facts.
        rufus::PrepareDeviceTarget seed(request, Table::Gpt);
        require(seed.open() == 0, "seed GRUB4DOS guest partition link");
        prepareDisk(seed, request, Table::Gpt);
    }
    const int descriptors = descriptorCount();
    const QString childrenPath = QStringLiteral("/proc/self/task/%1/children").arg(getpid());
    const QByteArray children = readFile(childrenPath).simplified();
    rufus::PrepareImageProcess process(QStringLiteral("/usr/bin/rufus-prepare-helper"));
    QSignalSpy done(&process, &rufus::PrepareImageProcess::finished);
    QSignalSpy phases(&process, &rufus::PrepareImageProcess::phaseChanged);
    QSignalSpy logs(&process, &rufus::PrepareImageProcess::logMessage);
    require(process.start(request, loader), "start standalone GRUB4DOS helper");
    QElapsedTimer timer;
    timer.start();
    bool held = false, fuseHeld = false;
    while (done.isEmpty())
    {
        QCoreApplication::processEvents();
        require(timer.elapsed() < 30000, "standalone GRUB4DOS helper deadline");
        if (fallback && !fuseHeld && QFile::exists(QStringLiteral("/run/grub4dos-fuse-exit-held")))
        {
            require(process.isActive() && done.isEmpty(), "GRUB4DOS waits for owned FUSE launcher exit");
            releaseGrub4dosFuseExit(true);
            fuseHeld = true;
        }
        if (done.isEmpty() && QFile::exists(QStringLiteral("/run/format-exit-held")))
        {
            require(process.isActive(), "GRUB4DOS waits for launcher exit");
            require(QDir(QStringLiteral("/tmp")).entryList({QStringLiteral("rufus-grub2-*")}, QDir::Dirs).size() == 1,
                "GRUB4DOS snapshot held through launcher exit");
            writeGuestFile(QStringLiteral("/run/format-exit-release"), "release\n");
            held = true;
        }
        usleep(1000);
    }
    for (const QList<QVariant>& entry : logs)
        printf("RUFUS_GRUB4DOS_LOG %s\n", qPrintable(entry.first().toString()));
    const rufus::PrepareImageOutcome& outcome = process.outcome();
    if (outcome.error != 0)
        qFatal("GRUB4DOS helper: error=%d problem=%s", outcome.error, qPrintable(process.problem()));
    require(process.state() == rufus::PrepareImageProcess::State::Complete && done.size() == 1 && held,
        "GRUB4DOS successful joined completion");
    require(outcome.started && outcome.syncError == 0 && outcome.cleanupError == 0,
        "GRUB4DOS sync/cleanup before READY");
    require(outcome.copiedBytes == quint64(loader.size()), "complete retained loader copied");
    const QStringList expected = {"checking", "partitioning", "formatting", "bootloader", "mounting", "extracting", "finalizing"};
    require(phases.size() == expected.size(), "GRUB4DOS native phase count");
    for (qsizetype i = 0; i < phases.size(); ++i)
        require(phases.at(i).first().toString() == expected.at(i), "GRUB4DOS native phase order");
    require(readFile(QStringLiteral("/run/format-launches")) == "launch\n", "one GRUB4DOS authorization");
    require(QDir(QStringLiteral("/run")).entryList({QStringLiteral("rufus-*")}, QDir::Dirs).isEmpty(), "GRUB4DOS private mount removed");
    require(QDir(QStringLiteral("/tmp")).entryList({QStringLiteral("rufus-grub2-*")}, QDir::Dirs).isEmpty(), "GRUB4DOS staging removed");
    const int fd = open("/dev/sda", O_RDWR | O_EXCL | O_CLOEXEC);
    require(fd >= 0, "GRUB4DOS released disk before completion");
    require(close(fd) == 0, "GRUB4DOS claim audit closed");
    require(descriptorCount() == descriptors, "GRUB4DOS descriptors released");
    require(readFile(childrenPath).simplified() == children, "GRUB4DOS descendants joined");
    if (fallback)
    {
        require(fuseHeld, "GRUB4DOS observed and joined held FUSE exit");
        checkGrub4dosFuseJoined();
    }
    printf("RUFUS_GRUB4DOS_DONE ERROR=0 OFFSET=%llu SIZE=%llu BYTES=%llu COPIED=%llu\n",
        outcome.offset, outcome.size, outcome.filesystemBytes, outcome.copiedBytes);
}

void runFormatHelperCase(const FormatHelperCase& test, rufus::PrepareImageRequest request)
{
    const QByteArray name(test.name);
    const bool standalone = !request.nonBootable && QFile::exists(QStringLiteral("/etc/rufus-uefi-ntfs-helper-fixture"));
    const QByteArray reportName = standalone ? QByteArray("uefi-") + name : name;
    if (name == "bad-label")
    {
        if (request.fileSystem == FS_NTFS)
            request.label = QString(33, QLatin1Char('x'));
        else if (request.fileSystem == FS_EXT2 || request.fileSystem == FS_EXT3)
            request.label = QString(17, QLatin1Char('x'));
        else
            request.label = QStringLiteral("invalid/label");
    }
    if (name == "invalid-cluster")
        request.clusterSize = 128 * 1024;
    if (name == "metadata-space")
        request.clusterSize = 32 * 1024 * 1024;
    if (name == "wrong-sequence")
        request.diskSequence++;
    const QByteArray before = test.started ? QByteArray() : diskDigest();
    const QString helper = test.faultProbe ? QStringLiteral("/usr/bin/prepare_exfat_probe")
        : QStringLiteral("/usr/bin/rufus-prepare-helper");
    rufus::PrepareImageProcess process(helper);
    QSignalSpy completed(&process, &rufus::PrepareImageProcess::finished);
    QSignalSpy phases(&process, &rufus::PrepareImageProcess::phaseChanged);
    QSignalSpy steps(&process, &rufus::PrepareImageProcess::stepChanged);
    QSignalSpy progress(&process, &rufus::PrepareImageProcess::progressChanged);
    QSignalSpy logs(&process, &rufus::PrepareImageProcess::logMessage);
    require(process.start(request), "start production format client");
    QElapsedTimer timer;
    timer.start();
    bool cancelled = false, waitedForExit = false;
    while (completed.isEmpty())
    {
        QCoreApplication::processEvents();
        require(timer.elapsed() < (request.quickFormat ? 30000 : 240000), "format helper deadline");
        const bool formatterBlocked = QFile::exists(QStringLiteral("/run/format-formatter-blocked"));
        const bool bootBlocked = QFile::exists(QStringLiteral("/run/format-boot-blocked"));
        const bool syncBlocked = QFile::exists(QStringLiteral("/run/format-final-sync"));
        // The worker can publish its marker before the queued phase signal
        // reaches the client. Observe both before requesting cancellation.
        const bool phaseReached = phases.size() == test.phases;
        if (test.cancel && !cancelled && phaseReached && (formatterBlocked || syncBlocked || bootBlocked))
        {
            if (syncBlocked || bootBlocked || request.fileSystem == FS_NTFS)
                checkExclusiveClaim();
            process.cancel();
            cancelled = true;
        }
        // The guest launcher deliberately stays alive briefly after the real
        // helper exits. A result alone must never complete the client.
        const bool launcherHolding = QFile::exists(QStringLiteral("/run/format-exit-held")) && completed.isEmpty();
        if (launcherHolding)
        {
            require(process.isActive(), "client waits for process exit after result");
            waitedForExit = true;
            if (request.grub2) writeGuestFile(QStringLiteral("/run/format-exit-release"), "release\n");
        }
        usleep(1000);
    }
    for (const QList<QVariant>& entry : logs)
        printf("RUFUS_FORMAT_HELPER_LOG %s %s\n", test.name, qPrintable(entry.first().toString()));
    const rufus::PrepareImageOutcome& outcome = process.outcome();
    if (outcome.error != test.error)
        qFatal("format helper %s: error=%d expected=%d phases=%lld bytes=%llu problem=%s", test.name,
            outcome.error, test.error, static_cast<long long>(phases.size()), outcome.filesystemBytes, qPrintable(process.problem()));
    const auto expectedState = test.cancel ? rufus::PrepareImageProcess::State::Cancelled :
        test.error ? rufus::PrepareImageProcess::State::Failed : rufus::PrepareImageProcess::State::Complete;
    require(process.state() == expectedState && completed.size() == 1 && waitedForExit, "joined client outcome");
    require(outcome.started == test.started && outcome.copiedBytes == 0 && outcome.cleanupError == 0, "source-free outcome");
    require(outcome.syncError == (name.endsWith("sync-fail") ? EIO : 0), "final sync error propagation");
    if (request.grub2) require(steps.isEmpty() && progress.isEmpty(), "standalone GRUB2 emits no copy progress");
    require(cancelled == test.cancel, "cancellation sent");
    QStringList expected = {QStringLiteral("checking"), QStringLiteral("partitioning"), QStringLiteral("formatting")};
    if (!request.nonBootable)
        expected.append(QStringLiteral("bootloader"));
    expected.append(QStringLiteral("finalizing"));
    require(phases.size() == test.phases, "format phase count");
    for (int i = 0; i < phases.size(); ++i)
        require(phases.at(i).first().toString() == expected.at(i), "format phase order");
    if (!test.started)
        require(diskDigest() == before, "preflight preserves entire disk");
    if (test.error == 0)
    {
        require(outcome.filesystemBytes <= outcome.size, "filesystem fits partition");
        const quint64 unused = outcome.size - outcome.filesystemBytes;
        const bool wholeVolume = request.fileSystem == FS_EXFAT || request.fileSystem == FS_NTFS;
        const bool validTail = wholeVolume ? unused == 0 : unused < 63 * sectorBytes;
        require(outcome.filesystemBytes > 0 && validTail, "native formatted extent");
        if (request.nonBootable || request.grub2)
            printf("RUFUS_FORMAT_VOLUME OFFSET=%llu SIZE=%llu BYTES=%llu\n", outcome.offset, outcome.size, outcome.filesystemBytes);
    }
    const int fd = open("/dev/sda", O_RDWR | O_EXCL | O_CLOEXEC);
    require(fd >= 0, "format helper released disk before completion");
    if ((request.mbr || request.sfd) && name == "formatter-failure")
    {
        // This is the first writing job after the GPT seed and preflight refusals.
        // Its fake formatter writes nothing, so both old headers must be zero.
        unsigned char sector[sectorBytes];
        const unsigned char zeroes[sectorBytes] = {};
        for (quint64 offset : {quint64(sectorBytes), request.size - sectorBytes})
        {
            require(pread(fd, sector, sizeof(sector), off_t(offset)) == sizeof(sector), "old GPT header readback");
            require(memcmp(sector, zeroes, sizeof(sector)) == 0, "old GPT header cleared before formatting");
        }
        puts("RUFUS_FORMAT_MBR_GPT_CLEARED");
    }
    if (request.sfd && test.started)
        require(!QFile::exists(QStringLiteral("/sys/class/block/sda1/partition")), "SFD helper removed stale kernel partition");
    if (request.sfd && test.error == 0)
        require(outcome.offset == 0 && outcome.size == request.size, "SFD helper whole-disk outcome");
    require(close(fd) == 0, "format helper claim audit closed");
    require(QDir(QStringLiteral("/run")).entryList({QStringLiteral("rufus-*")}, QDir::Dirs).isEmpty(), "no temporary mounts");
    require(readFile(QStringLiteral("/run/format-launches")) == "launch\n", "one authorization per job");
    printf("RUFUS_FORMAT_HELPER_CASE %s ERROR=%d STARTED=%d SYNC=%d CLEANUP=%d\n", reportName.constData(),
        outcome.error, outcome.started, outcome.syncError, outcome.cleanupError);
}

void runSyslinuxCases(rufus::PrepareImageRequest request, int bootType)
{
    const bool image = QFile::exists(QStringLiteral("/etc/rufus-syslinux-image-fixture"));
    const bool uefi = image && readFile(QStringLiteral("/etc/rufus-syslinux-image-fixture")).trimmed() == "uefi";
    request.syslinux = true;
    request.syslinuxSelection = bootType;
    const bool ntfs = QFile::exists(QStringLiteral("/etc/rufus-syslinux-ntfs-fixture"));
    if (ntfs)
    {
        request.fileSystem = FS_NTFS;
        request.clusterSize = 4096;
        request.volumeId = 0;
    }
    if (image)
        request.label = QStringLiteral("RUFUS_EXT");
    {
        rufus::PrepareDeviceTarget target(request, Table::Mbr);
        require(target.open(1000) == 0, "seed standalone guest partition link");
        const quint64 bootBytes = uefi ? quint64(readFile(QStringLiteral("/uefi-ntfs.img")).size()) : 0;
        prepareDisk(target, request, Table::Mbr, bootBytes);
    }
    if (image)
    {
        request.syslinuxSelection = BT_IMAGE;
        request.uefiNtfs = uefi;
    }
    struct Case { const char* name; int error; bool started; int steps; };
    QList<Case> cases = {{"wrong-sequence", ESTALE, false, 0}, {"missing-installer", ENOENT, false, 0},
        {"pause", ECANCELED, true, 0}, {"failure", EIO, true, 0}, {"no-op", ENOENT, true, 0},
        {"flush", EIO, true, 0}, {"read", EIO, true, 0}, {"corrupt", EBADMSG, true, 0},
        {"cleanup", EIO, true, 0}, {"final-flush", EIO, true, 2}, {"success", 0, true, 2}};
    if (ntfs)
    {
        // Keep success last for independent host readback and boot.
        cases.insert(cases.size() - 1, {"mount-flush", EIO, true, 0});
        cases.insert(cases.size() - 1, {"attributes", EIO, true, 0});
        cases.insert(cases.size() - 1, {"unmount", EBUSY, true, 2});
        cases.insert(cases.size() - 1, {"missing-formatter", ENOENT, false, 0});
        if (readFile(QStringLiteral("/etc/rufus-syslinux-ntfs-fixture")).trimmed() == "ntfs")
            cases.insert(cases.size() - 1, {"mount", EPERM, true, 0});
    }
    if (image)
    {
        const Case sourceCases[] = {{"stale-source", ESTALE, false, 0},
            {"incompatible-source", ENOTSUP, false, 0}, {"conflicting-config", ENOTSUP, false, 0},
            {"extra-partition", ENOTSUP, false, 0}, {"extract-failure", EIO, true, 0},
            {"extract-pause", ECANCELED, true, 0}, {"source-after-copy", ESTALE, true, 0},
            {"source-after-install", ESTALE, true, 0}, {"unknown-success", 0, true, 0}};
        for (const Case& test : sourceCases) cases.insert(cases.size() - 1, test);
    }
    if (uefi)
    {
        const Case bootCases[] = {{"unneeded-partition", ENOTSUP, false, 0}, {"bad-efi", EBADMSG, false, 0},
            {"uefi-failure", EIO, true, 0}, {"uefi-pause", ECANCELED, true, 0}, {"uefi-flush", EIO, true, 0}};
        for (const Case& test : bootCases) cases.insert(cases.size() - 1, test);
    }
    const int descriptors = descriptorCount();
    const QString childList = QStringLiteral("/proc/self/task/%1/children").arg(getpid());
    const QByteArray children = readFile(childList).simplified();
    for (const Case& test : cases)
    {
        const QByteArray name(test.name);
        for (const char* marker : {"/run/format-launches", "/run/format-exit-held", "/run/format-exit-release", "/run/syslinux-paused"})
            QFile::remove(QString::fromLatin1(marker));
        writeGuestFile(QStringLiteral("/home/rufus/syslinux-fault"), name);
        auto selected = request;
        if (name == "wrong-sequence") selected.diskSequence++;
        QByteArray sourceDigest;
        if (image)
        {
            QString sourcePath = QStringLiteral("/source.iso");
            if (name == "unknown-success") sourcePath = QStringLiteral("/unknown.iso");
            if (name == "incompatible-source") sourcePath = QStringLiteral("/incompatible.iso");
            if (name == "conflicting-config") sourcePath = QStringLiteral("/conflict.iso");
            if (name == "extra-partition")
            {
                if (uefi) selected.uefiNtfs = false;
                else sourcePath = QStringLiteral("/efi.iso");
            }
            if (name == "unneeded-partition") sourcePath = QStringLiteral("/bios.iso");
            if (name == "bad-efi") sourcePath = QStringLiteral("/efi.iso");
            require(chown(QFile::encodeName(sourcePath).constData(), 1000, 1000) == 0, "image caller ownership");
            require(rufus::CapturePrepareSource(sourcePath, selected) == 0, "capture NTFS helper source");
            sourceDigest = QCryptographicHash::hash(readFile(sourcePath), QCryptographicHash::Sha256);
            if (name == "stale-source") selected.imageInode++;
        }
        const bool missing = name == "missing-installer" || name == "missing-formatter";
        const QString installer = name == "missing-formatter" ? QStringLiteral(RUFUS_MKNTFS_PATH)
            : QString::fromLocal8Bit(rufus::NativeSyslinuxInstaller(bootType, request.fileSystem));
        if (missing)
            require(QFile::rename(installer, installer + QStringLiteral(".held")), "hide guest installer before preflight");
        const QByteArray before = test.started ? QByteArray() : diskDigest();
        const bool production = name == "success" || name == "unknown-success" || name == "pause" || !test.started;
        const QString helper = production ? QStringLiteral("/usr/bin/rufus-prepare-helper")
            : QStringLiteral("/usr/bin/prepare_syslinux_probe");
        rufus::PrepareImageProcess process(helper);
        QSignalSpy completed(&process, &rufus::PrepareImageProcess::finished);
        QSignalSpy phases(&process, &rufus::PrepareImageProcess::phaseChanged);
        QSignalSpy steps(&process, &rufus::PrepareImageProcess::stepChanged);
        QSignalSpy counts(&process, &rufus::PrepareImageProcess::progressChanged);
        QSignalSpy logs(&process, &rufus::PrepareImageProcess::logMessage);
        require(process.start(selected), "start standalone Syslinux client");
        QElapsedTimer timer;
        timer.start();
        bool cancelled = false, waitedForExit = false;
        while (completed.isEmpty())
        {
            QCoreApplication::processEvents();
            require(timer.elapsed() < 30000, "standalone Syslinux helper deadline");
            const bool pause = name == "pause" || name == "extract-pause" || name == "uefi-pause";
            if (pause && !cancelled && QFile::exists(QStringLiteral("/run/syslinux-paused")))
            {
                process.cancel();
                cancelled = true;
            }
            if (QFile::exists(QStringLiteral("/run/format-exit-held")) && completed.isEmpty())
            {
                require(process.isActive(), "standalone completion waits for helper exit");
                waitedForExit = true;
                writeGuestFile(QStringLiteral("/run/format-exit-release"), "release\n");
            }
            usleep(1000);
        }
        if (missing)
            require(QFile::rename(installer + QStringLiteral(".held"), installer), "restore guest installer");
        const auto& outcome = process.outcome();
        for (const auto& entry : logs)
            printf("RUFUS_SYSLINUX_LOG %s %s\n", test.name, qPrintable(entry.first().toString()));
        if (outcome.error != test.error)
            qFatal("Standalone %s: error=%d expected=%d problem=%s", test.name, outcome.error, test.error, qPrintable(process.problem()));
        const auto state = cancelled ? rufus::PrepareImageProcess::State::Cancelled : test.error != 0
            ? rufus::PrepareImageProcess::State::Failed : rufus::PrepareImageProcess::State::Complete;
        require(process.state() == state && completed.size() == 1 && waitedForExit, "standalone completion state");
        const int expectedSteps = image ? 0 : ntfs && name == "cleanup" ? 2 : test.steps;
        require(outcome.started == test.started && steps.size() == expectedSteps, "Syslinux copy steps");
        const int cleanupError = ntfs ? name == "cleanup" ? EIO : name == "unmount" ? EBUSY : 0 : 0;
        require(outcome.cleanupError == cleanupError, "Syslinux cleanup result");
        const bool bootFailure = name == "uefi-failure" || name == "uefi-pause" || name == "uefi-flush";
        if (image)
        {
            const bool copied = test.started && name != "mount" && !bootFailure;
            require(copied ? outcome.copiedBytes > 0 && !counts.isEmpty() : outcome.copiedBytes == 0 && counts.isEmpty(),
                "NTFS image copy counters");
            quint64 previous = 0, total = 0;
            for (const auto& update : counts)
            {
                const quint64 processed = update[1].toULongLong(), expectedTotal = update[2].toULongLong();
                require(update[0].toInt() == OP_FILE_COPY && processed >= previous && processed <= expectedTotal,
                    "NTFS image progress range and order");
                require(expectedTotal != 0 && (total == 0 || total == expectedTotal), "stable image progress denominator");
                previous = processed;
                total = expectedTotal;
            }
            require(QCryptographicHash::hash(readFile(selected.imagePath), QCryptographicHash::Sha256) == sourceDigest,
                "source image bytes unchanged");
        }
        else
            require(outcome.copiedBytes == 0 && counts.isEmpty(), "standalone discrete progress only");
        for (const auto& step : steps)
            require(step.first().toInt() == OP_FILE_COPY, "standalone copy operation");
        const bool syncFailure = name == "flush" || name == "final-flush" || name == "mount-flush" || name == "uefi-flush";
        require(outcome.syncError == (syncFailure ? EIO : 0), "standalone sync result");
        QStringList expected = {QStringLiteral("checking")};
        if (test.started)
            expected += {QStringLiteral("partitioning"), QStringLiteral("formatting"),
                image ? QStringLiteral("extracting") : QStringLiteral("bootloader")};
        if (ntfs && test.started)
        {
            expected.insert(3, QStringLiteral("mounting"));
            if (name == "mount") expected.removeLast();
        }
        if (uefi && test.started)
        {
            expected.insert(3, QStringLiteral("bootloader"));
            if (bootFailure)
                expected = expected.mid(0, 4);
        }
        if ((ntfs && test.started && !bootFailure) || name == "success" || name == "final-flush")
            expected.append(QStringLiteral("finalizing"));
        require(phases.size() == expected.size(), "standalone phase count");
        for (int i = 0; i < phases.size(); ++i)
            require(phases[i].first().toString() == expected[i], "standalone phase order");
        if (!test.started)
            require(diskDigest() == before, "standalone preflight preserves whole disk");
        if (test.error == 0 && ntfs)
        {
            const QString driver = QString::fromUtf8(readFile(QStringLiteral("/etc/rufus-syslinux-ntfs-fixture")).trimmed());
            const QString expectedLog = driver == QStringLiteral("ntfs")
                ? QStringLiteral("NTFS file copying uses the kernel ntfs driver") : QStringLiteral("NTFS file copying uses ntfs-3g");
            bool selectedDriver = false;
            for (const auto& entry : logs) selectedDriver |= entry.first().toString() == expectedLog;
            require(selectedDriver, "selected native NTFS driver");
        }
        if (test.error == 0)
            printf("RUFUS_SYSLINUX_VOLUME OFFSET=%llu SIZE=%llu BYTES=%llu\n",
                outcome.offset, outcome.size, outcome.filesystemBytes);
        const int fd = open("/dev/sda", O_RDWR | O_EXCL | O_CLOEXEC);
        require(fd >= 0 && close(fd) == 0, "standalone helper released disk claim");
        const QStringList directories = QDir(QStringLiteral("/run")).entryList({QStringLiteral("rufus-*")}, QDir::Dirs);
        if (name == "cleanup" || name == "unmount")
        {
            require(directories.size() == 1 && directories.first().startsWith(ntfs ? QStringLiteral("rufus-") : QStringLiteral("rufus-syslinux-")), "one failed-cleanup directory");
            require(QDir().rmdir(QStringLiteral("/run/") + directories.first()), "remove empty fault directory");
        }
        else require(directories.isEmpty(), "standalone private directories removed");
        require(readFile(QStringLiteral("/run/format-launches")) == "launch\n", "one standalone authorization launch");
        require(descriptorCount() == descriptors, "standalone client descriptor audit");
        require(readFile(childList).simplified() == children, "standalone helper descendants exited");
        printf("RUFUS_SYSLINUX_CASE %s ERROR=%d STARTED=%d SYNC=%d STEPS=%lld\n",
            test.name, outcome.error, outcome.started, outcome.syncError, static_cast<long long>(steps.size()));
    }
    puts("RUFUS_SYSLINUX_DONE");
}

// Complete FreeDOS jobs use the same private-bus/virtual-USB fixture as ISO jobs.
void runFreeDosHelperCases(rufus::PrepareImageRequest request)
{
    request.freeDos = request.mbr = true;
    request.dosRufusMbr = !QFile::exists(QStringLiteral("/etc/rufus-freedos-win7-fixture"));
    request.dosKeyboard = request.dosRufusMbr ? QStringLiteral("ro") : QStringLiteral("us");
    request.dosCodepage = request.dosRufusMbr ? 852 : 437;
    {
        rufus::PrepareDeviceTarget target(request, Table::Mbr);
        require(target.open(1000) == 0, "seed FreeDOS guest partition link");
        prepareDisk(target, request, Table::Mbr);
    }
    struct Case { const char* name; int error; bool started; int steps; };
    const Case cases[] = {{"wrong-sequence", ESTALE, false, 0}, {"mount", EIO, true, 0},
        {"pause", ECANCELED, true, 1}, {"flush", EIO, true, 6},
        {"unmount", EBUSY, true, 6}, {"success", 0, true, 6}};
    const int descriptors = descriptorCount();
    const QString childList = QStringLiteral("/proc/self/task/%1/children").arg(getpid());
    const QByteArray children = readFile(childList).simplified();
    for (const Case& test : cases)
    {
        const QByteArray name(test.name);
        for (const char* marker : {"/run/format-launches", "/run/format-exit-held", "/run/rufus-extractor.pid"})
            QFile::remove(QString::fromLatin1(marker));
        writeGuestFile(QStringLiteral("/home/rufus/extract-fault"), name);
        auto selected = request;
        if (!test.started)
            selected.diskSequence++;
        const QByteArray before = test.started ? QByteArray() : diskDigest();
        const QString helper = name == "success" || !test.started ? QStringLiteral("/usr/bin/rufus-prepare-helper")
            : QStringLiteral("/usr/bin/prepare_extract_probe");
        rufus::PrepareImageProcess process(helper);
        QSignalSpy completed(&process, &rufus::PrepareImageProcess::finished);
        QSignalSpy phases(&process, &rufus::PrepareImageProcess::phaseChanged);
        QSignalSpy steps(&process, &rufus::PrepareImageProcess::stepChanged);
        QSignalSpy counts(&process, &rufus::PrepareImageProcess::progressChanged);
        QSignalSpy logs(&process, &rufus::PrepareImageProcess::logMessage);
        require(process.start(selected), "start FreeDOS client");
        QElapsedTimer timer;
        timer.start();
        bool cancelled = false, waitedForExit = false;
        while (completed.isEmpty())
        {
            QCoreApplication::processEvents();
            require(timer.elapsed() < 30000, "FreeDOS helper deadline");
            if (name == "pause" && !cancelled && QFile::exists(QStringLiteral("/run/rufus-extractor.pid")))
            {
                process.cancel();
                cancelled = true;
            }
            if (QFile::exists(QStringLiteral("/run/format-exit-held")) && completed.isEmpty())
            {
                require(process.isActive(), "FreeDOS completion waits for helper exit");
                waitedForExit = true;
            }
            usleep(1000);
        }
        const auto& outcome = process.outcome();
        for (const auto& entry : logs)
            printf("RUFUS_FREEDOS_LOG %s %s\n", test.name, qPrintable(entry.first().toString()));
        if (outcome.error != test.error)
            qFatal("FreeDOS %s: error=%d expected=%d problem=%s", test.name, outcome.error, test.error, qPrintable(process.problem()));
        const auto state = cancelled ? rufus::PrepareImageProcess::State::Cancelled : test.error != 0
            ? rufus::PrepareImageProcess::State::Failed : rufus::PrepareImageProcess::State::Complete;
        require(process.state() == state && completed.size() == 1 && waitedForExit, "FreeDOS completion state");
        require(outcome.started == test.started && steps.size() == test.steps && counts.isEmpty(), "FreeDOS discrete progress");
        for (const auto& step : steps)
            require(step.first().toInt() == OP_FILE_COPY, "FreeDOS copy operation");
        require(outcome.syncError == (name == "flush" ? EIO : 0), "FreeDOS flush error");
        require(outcome.cleanupError == (name == "unmount" ? EBUSY : 0), "FreeDOS cleanup error");
        QStringList expected = {QStringLiteral("checking")};
        if (test.started)
        {
            expected += {QStringLiteral("partitioning"), QStringLiteral("formatting"), QStringLiteral("bootloader"), QStringLiteral("mounting")};
            if (name != "mount") expected.append(QStringLiteral("extracting"));
            expected.append(QStringLiteral("finalizing"));
        }
        require(phases.size() == expected.size(), "FreeDOS phase count");
        for (int i = 0; i < phases.size(); i++)
            require(phases[i].first().toString() == expected[i], "FreeDOS phase order");
        if (!test.started)
            require(diskDigest() == before, "FreeDOS preflight preserves whole disk");
        if (test.error == 0)
        {
            require(outcome.copiedBytes > 400604 && outcome.copiedBytes < outcome.filesystemBytes, "FreeDOS payload plus startup bytes");
            printf("RUFUS_FREEDOS_VOLUME OFFSET=%llu SIZE=%llu BYTES=%llu COPIED=%llu\n",
                outcome.offset, outcome.size, outcome.filesystemBytes, outcome.copiedBytes);
        }
        const int fd = open("/dev/sda", O_RDWR | O_EXCL | O_CLOEXEC);
        require(fd >= 0 && close(fd) == 0, "FreeDOS helper released claim/mount before completion");
        const QStringList directories = QDir(QStringLiteral("/run")).entryList({QStringLiteral("rufus-*")}, QDir::Dirs);
        if (name == "unmount")
        {
            // The injected failure leaves its empty directory. Exiting the
            // private namespace releases the failed mount; never detach it.
            require(directories.size() == 1, "one failed-unmount directory");
            require(QDir().rmdir(QStringLiteral("/run/") + directories.first()), "remove empty fault mountpoint");
        }
        else require(directories.isEmpty(), "FreeDOS removed private mountpoint");
        require(readFile(QStringLiteral("/run/format-launches")) == "launch\n", "one FreeDOS authorization launch");
        require(descriptorCount() == descriptors, "FreeDOS client descriptor audit");
        require(readFile(childList).simplified() == children, "FreeDOS helper descendants exited");
        printf("RUFUS_FREEDOS_CASE %s ERROR=%d STARTED=%d SYNC=%d CLEANUP=%d STEPS=%lld\n",
            test.name, outcome.error, outcome.started, outcome.syncError, outcome.cleanupError, static_cast<long long>(steps.size()));
    }
    puts("RUFUS_FREEDOS_DONE");
}

QDBusMessage mountFixtureControl(const QString& method, const QList<QVariant>& arguments = {})
{
    QDBusMessage message = QDBusMessage::createMethodCall(QStringLiteral("org.rufus.TestUDisks"),
        QStringLiteral("/org/freedesktop/UDisks2"), QStringLiteral("org.rufus.TestUDisks"), method);
    message.setArguments(arguments);
    const QDBusMessage reply = QDBusConnection::systemBus().call(message);
    require(reply.type() == QDBusMessage::ReplyMessage, "mount fixture control");
    return reply;
}

void publishTargetMount(rufus::UDisksObjects objects, const rufus::PrepareImageRequest& request,
    const QByteArray& path, quint64 partitionDevice = 0)
{
    QList<QByteArray> paths;
    if (!path.isEmpty())
        paths.append(path + '\0');
    QDBusObjectPath member(request.objectPath);
    if (partitionDevice != 0)
    {
        const QString block = QStringLiteral("org.freedesktop.UDisks2.Block");
        const QVariant drive = objects[member][block].value(QStringLiteral("Drive"));
        member = QDBusObjectPath(request.objectPath + QStringLiteral("1"));
        objects[member][block] = {{QStringLiteral("Drive"), drive}, {QStringLiteral("DeviceNumber"), partitionDevice},
            {QStringLiteral("HintSystem"), false}, {QStringLiteral("HintIgnore"), false}, {QStringLiteral("ReadOnly"), false}};
        objects[member][QStringLiteral("org.freedesktop.UDisks2.Partition")][QStringLiteral("Table")] =
            QVariant::fromValue(QDBusObjectPath(request.objectPath));
    }
    objects[member][QStringLiteral("org.freedesktop.UDisks2.Filesystem")]
        [QStringLiteral("MountPoints")] = QVariant::fromValue(paths);
    mountFixtureControl(QStringLiteral("SetObjects"), {QVariant::fromValue(objects)});
}

bool targetMounted(quint64 device)
{
    // Poll the inventory without walking the mount. Repeated stat() calls can
    // themselves hold a FUSE path busy while the helper tries to unmount it.
    const QByteArray number = QByteArray::number(major(dev_t(device))) + ':' + QByteArray::number(minor(dev_t(device)));
    const QByteArray inventory = readFile(QStringLiteral("/proc/thread-self/mountinfo"));
    require(!inventory.isEmpty(), "target mount inventory");
    for (const QByteArray& line : inventory.split('\n'))
    {
        const QList<QByteArray> fields = line.split(' ');
        if (fields.size() >= 6 && fields[2] == number && fields[4] == "/mnt/target")
            return true;
    }
    return false;
}

struct MountedHelperCase
{
    const char* name;
    int error;
    bool unmounted;
    bool mountInitially = true;
};

void mountDesktopNtfs(QProcess& daemon, const char* driver, const char* node, quint64 device, bool readOnly)
{
    const QString access = readOnly ? QStringLiteral("ro") : QStringLiteral("rw");
    daemon.setProcessChannelMode(QProcess::ForwardedChannels);
    daemon.setStandardInputFile(QProcess::nullDevice());
    daemon.start(QStringLiteral("/usr/bin/") + QString::fromLatin1(driver),
        {QString::fromLatin1(node), QStringLiteral("/mnt/target"), QStringLiteral("-o"),
         access + QStringLiteral(",uid=1000,no_detach,no_def_opts,norecover,nodev,nosuid,noexec")});
    require(daemon.waitForStarted(processTimeoutMs), "desktop NTFS-3G started");
    QElapsedTimer timer;
    timer.start();
    while (!targetMounted(device))
    {
        require(timer.elapsed() < processTimeoutMs, "desktop NTFS-3G mount deadline");
        require(!daemon.waitForFinished(25), "desktop NTFS-3G remains alive");
    }
    struct stat info;
    require(stat("/mnt/target", &info) == 0 && quint64(info.st_dev) == device, "desktop NTFS-3G partition identity");
    struct statfs filesystem;
    require(statfs("/mnt/target", &filesystem) == 0 && filesystem.f_type == FUSE_SUPER_MAGIC,
        "real desktop FUSE mount with partition dev_t");
    require(readFile(QStringLiteral("/proc/thread-self/mountinfo")).contains(" - fuseblk "),
        "desktop NTFS-3G uses block mount identity");
}

void runMountedHelperCase(const MountedHelperCase& test, rufus::PrepareImageRequest request,
    const rufus::UDisksObjects& objects, quint64 partitionDevice = 0, const char* mountDriver = nullptr,
    const char* reportPrefix = nullptr)
{
    const QByteArray name(test.name);
    const char* prefix = reportPrefix ? reportPrefix : mountDriver;
    const QByteArray reportName = prefix ? QByteArray(prefix) + '-' + name : name;
    const bool copying = !request.nonBootable;
    const bool success = test.error == 0;
    const bool cancel = name.endsWith("cancel-after");
    const quint64 mountedDevice = partitionDevice != 0 ? partitionDevice : request.device;
    const char* mountedNode = partitionDevice != 0 ? "/dev/sda1" : "/dev/sda";
    const QByteArray home = name == "home-target" ? "/mnt/target" : "/run/caller-home";
    writeGuestFile(QStringLiteral("/etc/passwd"), "root:x:0:0:root:/root:/bin/bash\ncaller:x:1000:1000:Caller:" + home + ":/bin/bash\n");
    const unsigned long flags = success ? 0UL : static_cast<unsigned long>(MS_RDONLY);
    QProcess desktopDaemon;
    if (mountDriver)
        mountDesktopNtfs(desktopDaemon, mountDriver, mountedNode, mountedDevice, !success);
    else if (test.mountInitially)
        require(mount(mountedNode, "/mnt/target", "vfat", flags | MS_NODEV | MS_NOSUID | MS_NOEXEC, "uid=1000") == 0, "desktop mount");
    QByteArray metadataPath = test.mountInitially ? QByteArray("/mnt/target") : QByteArray();
    if (name == "mount-mismatch") metadataPath = "/mnt/other";
    if (name == "metadata-missing") metadataPath.clear();
    publishTargetMount(objects, request, metadataPath, partitionDevice);
    if (name == "wrong-sequence") request.diskSequence++;
    if (name == "wrong-owner") request.serviceOwner = QStringLiteral(":1.9999");
    if (name.endsWith("source-changed"))
    {
        QFile source(request.imagePath);
        require(source.open(QIODevice::WriteOnly | QIODevice::Append) && source.write("x", 1) == 1, "change captured ISO source");
        source.close();
    }
    if (name == "source-target")
    {
        request.sfd = request.nonBootable = false;
        request.syslinux = true;
        require(rufus::CapturePrepareSource(QStringLiteral("/mnt/target/source.iso"), request) == 0, "capture target source");
    }
    const int held = name == "busy" ? open("/mnt/target", O_RDONLY | O_DIRECTORY | O_CLOEXEC) : -1;
    if (name == "busy") require(held >= 0, "retain busy desktop directory");
    const QByteArray before = diskDigest();
    rufus::PrepareImageProcess process(QStringLiteral("/usr/bin/rufus-prepare-helper"));
    QSignalSpy completed(&process, &rufus::PrepareImageProcess::finished);
    QSignalSpy phases(&process, &rufus::PrepareImageProcess::phaseChanged);
    QSignalSpy logs(&process, &rufus::PrepareImageProcess::logMessage);
    QSignalSpy rawProgress(&process, &rufus::PrepareImageProcess::rawProgressChanged);
    require(process.start(request), "start mounted helper client");
    QElapsedTimer timer, settled;
    timer.start();
    bool removed = false, acted = false, waitedForExit = false, reportedSlow = false;
    const int deadlineMs = request.hasCompressedImage() ? compressedMountedTimeoutMs : 15000;
    const int diagnosticMs = qMin(15000, deadlineMs - 1000);
    while (completed.isEmpty())
    {
        QCoreApplication::processEvents();
        if (!reportedSlow && timer.elapsed() >= diagnosticMs)
        {
            reportedSlow = true;
            for (const QList<QVariant>& log : logs)
                printf("RUFUS_MOUNT_HELPER_WAIT_LOG %s %s\n", reportName.constData(), qPrintable(log.first().toString()));
            const QList<QVariant> last = rawProgress.isEmpty() ? QList<QVariant>() : rawProgress.last();
            printf("RUFUS_MOUNT_HELPER_WAIT %s STATE=%d REMOVED=%d HELD=%d PHASE=%d PROCESSED=%llu\n",
                reportName.constData(), int(process.state()), removed, QFile::exists(QStringLiteral("/run/format-exit-held")),
                last.isEmpty() ? -1 : last[0].toInt(), last.isEmpty() ? 0ULL : last[1].toULongLong());
            fflush(stdout);
        }
        require(timer.elapsed() < deadlineMs, "mounted helper deadline");
        if (test.mountInitially && !removed && !targetMounted(mountedDevice))
        {
            removed = true;
            settled.start();
        }
        // Simulate UDisks's delayed mount notification without delaying native
        // unmount. The helper must wait, and must still handle cancel/service loss.
        if (removed && !acted && settled.elapsed() >= 250)
        {
            acted = true;
            if (cancel) process.cancel();
            else if (name == "service-gone") mountFixtureControl(QStringLiteral("ReleaseName"));
            else if (name != "stale-cache")
            {
                if (name == "remounted")
                    require(mount("/dev/sda", "/mnt/target", "vfat", MS_RDONLY, "") == 0, "competing desktop remount");
                publishTargetMount(objects, request, {}, partitionDevice);
            }
        }
        if (QFile::exists(QStringLiteral("/run/format-exit-held")) && completed.isEmpty())
        {
            require(process.isActive(), "mounted client waits for launcher exit");
            waitedForExit = true;
        }
        usleep(1000);
    }
    const qint64 helperMs = timer.elapsed();
    for (const QList<QVariant>& log : logs)
        printf("RUFUS_MOUNT_HELPER_LOG %s %s\n", reportName.constData(), qPrintable(log.first().toString()));
    const auto outcome = process.outcome();
    if (outcome.error != test.error)
        qFatal("mounted helper %s: error=%d expected=%d problem=%s", reportName.constData(), outcome.error, test.error, qPrintable(process.problem()));
    const auto state = cancel ? rufus::PrepareImageProcess::State::Cancelled :
        success ? rufus::PrepareImageProcess::State::Complete : rufus::PrepareImageProcess::State::Failed;
    require(process.state() == state && completed.size() == 1 && waitedForExit, "mounted client joined outcome");
    require(outcome.started == success && outcome.cleanupError == 0 && outcome.syncError == 0, "mounted outcome before writes or clean completion");
    const int expectedPhases = request.rawWrite ? 1 : success ? (copying ? 6 : 4) : 1;
    require(phases.size() == expectedPhases, "mounted preparation phase count");
    if (success && request.rawWrite)
    {
        require(outcome.rawWrite && outcome.raw.phase == RAW_WRITE_DONE, "raw helper result type and phase");
        const quint64 writtenBytes = rufus::PrepareRawWriteBytes(request);
        require(outcome.raw.bytes_written == writtenBytes && outcome.raw.bytes_verified == writtenBytes,
            "raw helper full copy and readback");
        require(rawProgress.size() >= 5, "raw helper copy/sync/verify progress");
    }
    else if (success && copying) require(outcome.copiedBytes > 0, "mounted ISO copied files");
    require(removed == test.unmounted, "expected unmount before completion");
    if (!success) require(diskDigest() == before, "refusal or cancellation preserves whole disk");
    require(readFile(QStringLiteral("/run/format-launches")) == "launch\n", "one mounted-job authorization");
    if (held >= 0) close(held);
    if (targetMounted(mountedDevice)) require(umount("/mnt/target") == 0, "test desktop mount cleanup");
    if (name == "service-gone") mountFixtureControl(QStringLiteral("AcquireName"));
    publishTargetMount(objects, request, {}, partitionDevice);
    const int claim = open("/dev/sda", O_RDWR | O_EXCL | O_CLOEXEC);
    require(claim >= 0, "mounted helper released all device claims");
    close(claim);
    if (mountDriver)
    {
        const bool exited = desktopDaemon.state() == QProcess::NotRunning || desktopDaemon.waitForFinished(processTimeoutMs);
        require(exited, "desktop NTFS-3G exits after normal unmount");
        const bool cleanExit = desktopDaemon.exitStatus() == QProcess::NormalExit && desktopDaemon.exitCode() == 0;
        require(cleanExit, "desktop NTFS-3G exits without being killed or adopted by helper");
    }
    require(QFile::remove(QStringLiteral("/run/format-exit-held")), "clear mounted exit marker");
    require(QFile::remove(QStringLiteral("/run/format-launches")), "clear mounted launch count");
    printf("RUFUS_MOUNT_HELPER_CASE %s ERROR=%d UNMOUNTED=%d STARTED=%d ELAPSED_MS=%lld\n",
        reportName.constData(), outcome.error, removed, outcome.started, static_cast<long long>(helperMs));
}

void runMountedHelperCases(rufus::PrepareImageRequest request, qint64 daemonPid, qint64 fixturePid)
{
    request.sfd = request.nonBootable = true;
    request.label = QStringLiteral("RUFUS SFD");
    require(QDir().mkpath(QStringLiteral("/mnt/target")) && QDir().mkpath(QStringLiteral("/run/caller-home")), "mounted fixture directories");
    require(mount("tmpfs", "/run/caller-home", "tmpfs", 0, "size=1m") == 0, "independent caller home");
    require(mount("/dev/sda", "/mnt/target", "vfat", 0, "uid=1000") == 0, "seed source on target");
    writeGuestFile(QStringLiteral("/mnt/target/source.iso"), QByteArray(4096, 's'));
    require(umount("/mnt/target") == 0, "sync source fixture");
    const QDBusMessage message = QDBusMessage::createMethodCall(request.serviceOwner,
        QStringLiteral("/org/freedesktop/UDisks2"), QStringLiteral("org.freedesktop.DBus.ObjectManager"), QStringLiteral("GetManagedObjects"));
    const QDBusReply<rufus::UDisksObjects> snapshot = QDBusConnection::systemBus().call(message);
    require(snapshot.isValid(), "mounted baseline metadata");
    const MountedHelperCase cases[] = {
        {"metadata-missing", EBUSY, false}, {"mount-mismatch", ESTALE, false}, {"home-target", EBUSY, false},
        {"busy", EBUSY, false}, {"wrong-sequence", ESTALE, false}, {"wrong-owner", ESTALE, false},
        {"source-target", EBUSY, false}, {"cancel-after", ECANCELED, true}, {"stale-cache", ETIMEDOUT, true},
        {"service-gone", EHOSTDOWN, true}, {"remounted", EBUSY, true}, {"success", 0, true}};
    const int descriptors = descriptorCount();
    for (const MountedHelperCase& test : cases)
    {
        runMountedHelperCase(test, request, snapshot.value());
        require(descriptorCount() == descriptors, "mounted client descriptor leak");
        const QList<QByteArray> children = readFile(QStringLiteral("/proc/self/task/%1/children").arg(getpid())).simplified().split(' ');
        const bool onlyFixtures = children.size() == 2 && children.contains(QByteArray::number(daemonPid))
            && children.contains(QByteArray::number(fixturePid));
        require(onlyFixtures, "mounted helper descendants joined");
    }
    require(umount("/run/caller-home") == 0, "caller home descriptor released");
    const QVariantMap stats = qdbus_cast<QVariantMap>(mountFixtureControl(QStringLiteral("Stats")).arguments().first());
    require(stats.value(QStringLiteral("opens")).toInt() == 0 && stats.value(QStringLiteral("unexpected")).toInt() == 0,
        "no UDisks authorization or unmount request");
    puts("RUFUS_MOUNT_HELPER_DONE");
}

void runMountedIsoHelperCases(rufus::PrepareImageRequest request, qint64 daemonPid, qint64 fixturePid)
{
    request.syslinux = true;
    require(QDir().mkpath(QStringLiteral("/mnt/target")) && QDir().mkpath(QStringLiteral("/run/caller-home")), "mounted ISO directories");
    require(mount("tmpfs", "/run/caller-home", "tmpfs", 0, "size=32m") == 0, "independent source and home");
    const QDBusMessage message = QDBusMessage::createMethodCall(request.serviceOwner,
        QStringLiteral("/org/freedesktop/UDisks2"), QStringLiteral("org.freedesktop.DBus.ObjectManager"), QStringLiteral("GetManagedObjects"));
    const QDBusReply<rufus::UDisksObjects> snapshot = QDBusConnection::systemBus().call(message);
    require(snapshot.isValid(), "mounted ISO baseline metadata");
    struct stat partition;
    require(stat("/dev/sda1", &partition) == 0 && S_ISBLK(partition.st_mode), "existing boot partition");
    const QString source = QStringLiteral("/run/caller-home/source.iso");
    const MountedHelperCase cases[] = {{"iso-source-changed", ESTALE, false},
        {"iso-cancel-after", ECANCELED, true}, {"iso-success", 0, true}};
    const int descriptors = descriptorCount();
    for (const MountedHelperCase& test : cases)
    {
        QFile::remove(source);
        require(QFile::copy(QStringLiteral("/unknown.iso"), source), "copy independent ISO fixture");
        require(chown(QFile::encodeName(source).constData(), 1000, 1000) == 0, "ISO caller ownership");
        require(rufus::CapturePrepareSource(source, request) == 0, "retain mounted job source identity");
        runMountedHelperCase(test, request, snapshot.value(), quint64(partition.st_rdev));
        require(descriptorCount() == descriptors, "mounted ISO client descriptors released");
        const QList<QByteArray> children = readFile(QStringLiteral("/proc/self/task/%1/children").arg(getpid())).simplified().split(' ');
        const bool onlyFixtures = children.size() == 2 && children.contains(QByteArray::number(daemonPid))
            && children.contains(QByteArray::number(fixturePid));
        require(onlyFixtures, "mounted ISO helper descendants joined");
    }
    require(umount("/run/caller-home") == 0, "mounted ISO source and home descriptors released");
    const QVariantMap stats = qdbus_cast<QVariantMap>(mountFixtureControl(QStringLiteral("Stats")).arguments().first());
    require(stats.value(QStringLiteral("opens")).toInt() == 0 && stats.value(QStringLiteral("unexpected")).toInt() == 0,
        "mounted ISO uses only its single helper authorization");
    puts("RUFUS_MOUNT_ISO_HELPER_DONE");
}

void runFixedUsbAcquisitionCases(rufus::PrepareImageRequest& request)
{
    const QString driveInterface = QStringLiteral("org.freedesktop.UDisks2.Drive");
    for (auto object = rawObjects.begin(); object != rawObjects.end(); ++object)
        if (object->contains(driveInterface))
            (*object)[driveInterface][QStringLiteral("MediaRemovable")] = false;
    rufus::Device classified;
    require(rufus::DescribeUDisksDevice(rawObjects, request.objectPath, classified), "default-visible native fixed USB");
    require(classified.defaultVisible && classified.requiresNativeProtection, "score acceptance retains native protection");
    request.identity = classified.identity;
    request.allowFixedUsb = false;
    publishTargetMount(rawObjects, request, {}, rawPartition);
    require(QDir().mkpath(QStringLiteral("/boot")), "fixed system mountpoint");
    require(QProcess::execute(QStringLiteral("/usr/bin/bash"), {QStringLiteral("-c"), QStringLiteral("modprobe dm_mod")}) == 0,
        "fixed holder module");
    const QByteArray before = diskDigest();
    const int descriptors = descriptorCount();
    const char* cases[] = {"hidden-mount", "home-target", "source-target", "system-target", "holder", "success"};
    for (bool optIn : {false, true})
    for (const char* value : cases)
    {
        const QByteArray name(value);
        auto selected = request;
        selected.allowFixedUsb = optIn;
        const bool mountTarget = name == "hidden-mount" || name == "home-target" || name == "source-target";
        const bool system = name == "system-target";
        const QByteArray mountpoint = system ? QByteArray("/boot") : QByteArray("/mnt/target");
        if (mountTarget || system)
            require(mount("/dev/sda1", mountpoint.constData(), "vfat", MS_RDONLY | MS_NODEV | MS_NOSUID, "uid=1000") == 0,
                "fixed hidden native mount");
        const QByteArray home = name == "home-target" ? "/mnt/target" : "/run/caller-home";
        writeGuestFile(QStringLiteral("/etc/passwd"), "root:x:0:0:root:/root:/bin/bash\ncaller:x:1000:1000:Caller:" + home + ":/bin/bash\n");
        if (name == "holder")
            require(QProcess::execute(QStringLiteral("/usr/bin/dmsetup"), {QStringLiteral("--noudevsync"), QStringLiteral("create"),
                QStringLiteral("rufus-fixed-holder"), QStringLiteral("--table"), QStringLiteral("0 2048 linear /dev/sda1 0")}) == 0,
                "fixed partition holder");
        if (name == "source-target")
            require(rufus::CapturePrepareSource(QStringLiteral("/mnt/target/ldlinux.sys"), selected) == 0, "fixed target source");
        int error;
        {
            rufus::PrepareIsoSource source(selected);
            const bool withSource = !selected.imagePath.isEmpty();
            if (withSource) require(source.open(1000) == 0, "fixed source descriptor");
            rufus::PrepareDeviceTarget target(selected, rufus::PrepareDeviceTarget::Table::Raw);
            error = target.open(1000, withSource ? &source : nullptr);
            if (error == 0) checkClaim(target);
        }
        const int expected = name == "success" ? 0 : name == "hidden-mount" ? ESTALE : EBUSY;
        if (error != expected) qFatal("fixed acquisition %s error=%d expected=%d", value, error, expected);
        if (mountTarget || system) require(umount(mountpoint.constData()) == 0, "fixed native mount cleanup");
        if (name == "holder")
            require(QProcess::execute(QStringLiteral("/usr/bin/dmsetup"), {QStringLiteral("--noudevsync"), QStringLiteral("remove"),
                QStringLiteral("rufus-fixed-holder")}) == 0, "fixed holder cleanup");
        require(diskDigest() == before && descriptorCount() == descriptors, "fixed checks preserve disk and release descriptors");
        printf("RUFUS_FIXED_USB_CASE %s-%s ERROR=%d\n", optIn ? "opt-in" : "default", value, error);
    }
}

void runNtfsMountedHelperCases(rufus::PrepareImageRequest request, const QString& path, const QByteArray& expectedDisk)
{
    const int descriptors = descriptorCount();
    const QString childList = QStringLiteral("/proc/self/task/%1/children").arg(getpid());
    const QByteArray children = readFile(childList).simplified();
    const MountedHelperCase cases[] = {{"raw-source-changed", ESTALE, false}, {"busy", EBUSY, false},
        {"home-target", EBUSY, false}, {"mount-mismatch", ESTALE, false}, {"raw-cancel-after", ECANCELED, true},
        {"format-success", 0, true}, {"raw-success", 0, true}};
    for (const char* driver : {"ntfs-3g", "lowntfs-3g"})
    for (const MountedHelperCase& test : cases)
    {
        QProcess formatter;
        formatter.start(QStringLiteral("/usr/bin/mkntfs"),
            {QStringLiteral("-F"), QStringLiteral("-Q"), QStringLiteral("/dev/sda1")});
        require(formatter.waitForFinished(processTimeoutMs), "seed NTFS partition before mounted helper job");
        const bool formatted = formatter.exitStatus() == QProcess::NormalExit && formatter.exitCode() == 0;
        require(formatted, "NTFS fixture formatted");
        QFile source(path);
        require(source.open(QIODevice::ReadWrite) && source.resize(qint64(request.size)), "restore NTFS helper raw source size");
        source.close();
        require(rufus::CapturePrepareSource(path, request) == 0, "capture NTFS helper source");
        auto selected = request;
        const bool format = QByteArray(test.name) == "format-success";
        if (format)
        {
            selected.rawWrite = false;
            selected.nonBootable = selected.mbr = true;
            selected.imagePath.clear();
            selected.label = QStringLiteral("FROM NTFS");
        }
        runMountedHelperCase(test, selected, rawObjects, rawPartition, driver);
        if (format)
        {
            const int partition = open("/dev/sda1", O_RDONLY | O_CLOEXEC);
            require(partition >= 0, "formatted NTFS target readback");
            require(readBytes(partition, 82, 8) == "FAT32   ", "NTFS target replaced with FAT32");
            close(partition);
        }
        else if (test.error == 0)
            require(diskDigest() == expectedDisk, "NTFS target replaced with complete source image");
        require(descriptorCount() == descriptors, "NTFS helper descriptors released");
        require(readFile(childList).simplified() == children, "NTFS helper and desktop daemon descendants joined");
    }
    const QVariantMap stats = qdbus_cast<QVariantMap>(mountFixtureControl(QStringLiteral("Stats")).arguments().first());
    require(stats.value(QStringLiteral("opens")).toInt() == 0 && stats.value(QStringLiteral("unexpected")).toInt() == 0,
        "NTFS mounted jobs need no UDisks authorization or unmount request");
    puts("RUFUS_NTFS_MOUNT_HELPER_DONE");
}

#include "compressed_helper_cases.inc"
#include "vtsi_helper_cases.inc"
#include "vhd_helper_cases.inc"
#include "vhdx_helper_cases.inc"
#include "virtual_disk_cases.inc"
#include "ffu_helper_cases.inc"
#include "ffu_store_cases.inc"
#include "ffu_optimized_cases.inc"
#include "image_entry_cases.inc"
#include "zero_device_cases.inc"
#include "full_format_cases.inc"
#include "badblocks_cases.inc"
#include "media_appearance_cases.inc"
#include "native_format_cases.inc"
#include "runtime_validation_cases.inc"
#include "iso_options_cases.inc"
#include "iso_resources_cases.inc"
#include "legacy_iso_cases.inc"
#include "geometry_cases.inc"
#include "ntfs_options_cases.inc"
int sourceBatchRecord(void*, const QString&, bool) { return 0; }
#include "device_source_cases.inc"
#include "device_options_cases.inc"
#include "legacy_boot_cases.inc"
#include "old_bios_cases.inc"
#include "archive_cases.inc"
#include "windows_togo_cases.inc"
#include "image_save_cases.inc"

void runRawHelperCases(rufus::PrepareImageRequest request, const QString& path, const QByteArray& expectedDisk)
{
    QByteArray finalExpected = expectedDisk;
    QFile source(path);
    const int descriptors = descriptorCount();
    const bool fixedUsb = QFile::exists(QStringLiteral("/etc/rufus-fixed-usb-fixture"));
    request.rawWrite = true;
    request.volumeId = 0;
    request.label.clear();
    const QByteArray children = readFile(QStringLiteral("/proc/self/task/%1/children").arg(getpid())).simplified();
    const MountedHelperCase cases[] = {{"raw-source-changed", ESTALE, false}, {"busy", EBUSY, false},
        {"raw-cancel-after", ECANCELED, true}, {"raw-success", 0, true},
        {"raw-unmounted", 0, false, false}};
    for (const MountedHelperCase& test : cases)
    {
        if (QFile::exists(QStringLiteral("/etc/rufus-vhdx-qt-fixture"))
            || QFile::exists(QStringLiteral("/etc/rufus-ffu-qt-fixture"))
            || QFile::exists(QStringLiteral("/etc/rufus-save-qt-fixture")))
            break; // Existing raw claim/fault jobs are qualified; this batch tests the new provider.
        require(source.open(QIODevice::ReadWrite) && source.resize(qint64(request.size)), "restore raw helper source size");
        source.close();
        require(rufus::CapturePrepareSource(path, request) == 0, "capture raw helper source");
        runMountedHelperCase(test, request, rawObjects, rawPartition);
        require(descriptorCount() == descriptors, "raw helper descriptors released");
        require(readFile(QStringLiteral("/proc/self/task/%1/children").arg(getpid())).simplified() == children,
            "raw helper descendants joined");
    }
    if (fixedUsb)
    {
        request.allowFixedUsb = true;
        auto format = request;
        format.rawWrite = false;
        format.nonBootable = format.mbr = true;
        format.imagePath.clear();
        format.label = QStringLiteral("FIXED USB");
        runMountedHelperCase({"fixed-format-unmounted", 0, false, false}, format, rawObjects, rawPartition);
        require(rufus::CapturePrepareSource(path, request) == 0, "fixed raw source");
        runMountedHelperCase({"fixed-raw-unmounted", 0, false, false}, request, rawObjects, rawPartition);
        require(descriptorCount() == descriptors, "fixed helper descriptors released");
        require(readFile(QStringLiteral("/proc/self/task/%1/children").arg(getpid())).simplified() == children,
            "fixed helper descendants joined");
    }
    if (QFile::exists(QStringLiteral("/etc/rufus-ntfs-mounted-fixture")))
        runNtfsMountedHelperCases(request, path, expectedDisk);
    if (QFile::exists(QStringLiteral("/etc/rufus-compressed-helper-fixture")))
        runCompressedHelperCases(request, path, expectedDisk);
    if (QFile::exists(QStringLiteral("/etc/rufus-xz-helper-fixture")))
        runCompressedHelperCases(request, path, expectedDisk, BLED_COMPRESSION_XZ);
    if (QFile::exists(QStringLiteral("/etc/rufus-bzip2-helper-fixture")))
        runCompressedHelperCases(request, path, expectedDisk, BLED_COMPRESSION_BZIP2);
    if (QFile::exists(QStringLiteral("/etc/rufus-lzma-helper-fixture")))
        runCompressedHelperCases(request, path, expectedDisk, BLED_COMPRESSION_LZMA);
    if (QFile::exists(QStringLiteral("/etc/rufus-lzw-helper-fixture")))
        runCompressedHelperCases(request, path, expectedDisk, BLED_COMPRESSION_LZW);
    if (QFile::exists(QStringLiteral("/etc/rufus-zip-helper-fixture")))
        runCompressedHelperCases(request, path, expectedDisk, BLED_COMPRESSION_ZIP);
    if (QFile::exists(QStringLiteral("/etc/rufus-zstd-helper-fixture")))
        runCompressedHelperCases(request, path, expectedDisk, BLED_COMPRESSION_ZSTD);
    if (QFile::exists(QStringLiteral("/etc/rufus-vtsi-helper-fixture")))
        runVtsiHelperCases(request, path, expectedDisk);
    if (QFile::exists(QStringLiteral("/etc/rufus-vhd-helper-fixture")))
        runVhdHelperCases(request, path, expectedDisk);
    if (QFile::exists(QStringLiteral("/etc/rufus-vhdx-qt-fixture")))
    {
        if (QFile::exists(QStringLiteral("/etc/rufus-virtual-disk-fixture")))
            runVirtualDiskQtCases(request, path, expectedDisk);
        else
            runVhdxQtCases(request, path, expectedDisk);
    }
    if (QFile::exists(QStringLiteral("/etc/rufus-ffu-qt-fixture")))
    {
        if (QFile::exists(QStringLiteral("/etc/rufus-ffu-optimized-fixture")))
            runFfuOptimizedQtCases(request, path, expectedDisk);
        else if (QFile::exists(QStringLiteral("/etc/rufus-ffu-stores-fixture")))
            runFfuStoreQtCases(request, path, expectedDisk);
        else if (QFile::exists(QStringLiteral("/etc/rufus-old-bios-fixture")))
            runOldBiosCases(request, path, expectedDisk);
        else if (QFile::exists(QStringLiteral("/etc/rufus-media-appearance-fixture")))
            runMediaAppearanceCases(request, path, expectedDisk);
        else if (QFile::exists(QStringLiteral("/etc/rufus-badblocks-fixture")))
            runBadBlocksCases(request, path, expectedDisk);
        else if (QFile::exists(QStringLiteral("/etc/rufus-full-format-fixture")))
            runFullFormatCases(request, path, expectedDisk);
        else if (QFile::exists(QStringLiteral("/etc/rufus-zero-device-fixture")))
            runZeroDeviceQtCases(request, path, expectedDisk);
        else if (QFile::exists(QStringLiteral("/etc/rufus-image-entry-fixture")))
            runImageEntryQtCases(request, path, expectedDisk);
        else if (QFile::exists(QStringLiteral("/etc/rufus-ffu-partial-fixture")))
            finalExpected = runFfuPartialQtCases(request, path, expectedDisk);
        else if (QFile::exists(QStringLiteral("/etc/rufus-ffu-geometry-fixture")))
            runFfuGeometryQtCases(request, path, expectedDisk);
        else if (QFile::exists(QStringLiteral("/etc/rufus-ffu-variants-fixture")))
            runFfuVariantQtCases(request, path, expectedDisk);
        else
            runFfuQtCases(request, path, expectedDisk);
    }
    if (QFile::exists(QStringLiteral("/etc/rufus-save-read-only-fixture")))
        runReadOnlySaveCases(request, expectedDisk);
    else if (QFile::exists(QStringLiteral("/etc/rufus-save-export-fixture")))
        runImageExportCases(request, expectedDisk);
    else if (QFile::exists(QStringLiteral("/etc/rufus-save-qt-fixture")))
        runImageSaveCases(request, expectedDisk);
    if (QFile::exists(QStringLiteral("/etc/rufus-bzip2-qt-fixture")))
        runCompressedQtRestore(request, path, expectedDisk, BLED_COMPRESSION_BZIP2);
    if (QFile::exists(QStringLiteral("/etc/rufus-lzma-qt-fixture")))
        runCompressedQtRestore(request, path, expectedDisk, BLED_COMPRESSION_LZMA);
    if (QFile::exists(QStringLiteral("/etc/rufus-lzw-qt-fixture")))
        runCompressedQtRestore(request, path, expectedDisk, BLED_COMPRESSION_LZW);
    if (QFile::exists(QStringLiteral("/etc/rufus-zip-qt-fixture")))
        runCompressedQtRestore(request, path, expectedDisk, BLED_COMPRESSION_ZIP);
    if (QFile::exists(QStringLiteral("/etc/rufus-zstd-qt-fixture")))
        runCompressedQtRestore(request, path, expectedDisk, BLED_COMPRESSION_ZSTD);
    require(diskDigest() == finalExpected, "raw helper whole-disk equality");
    require(umount("/run/caller-home") == 0, "raw helper source/home released");
    puts("RUFUS_RAW_HELPER_DONE");
}

void runMountedRawCases(rufus::PrepareImageRequest request, bool helper = false)
{
    require(QDir().mkpath(QStringLiteral("/mnt/target")) && QDir().mkpath(QStringLiteral("/run/caller-home")), "raw directories");
    const bool containerFixture = QFile::exists(QStringLiteral("/etc/rufus-vhdx-qt-fixture"))
        || QFile::exists(QStringLiteral("/etc/rufus-ffu-qt-fixture"))
        || QFile::exists(QStringLiteral("/etc/rufus-save-qt-fixture"));
    const char* sourceCapacity = containerFixture ? "size=192m" : "size=128m";
    require(mount("tmpfs", "/run/caller-home", "tmpfs", 0, sourceCapacity) == 0, "independent raw source/home");
    writeGuestFile(QStringLiteral("/etc/passwd"), "root:x:0:0:root:/root:/bin/bash\ncaller:x:1000:1000:Caller:/run/caller-home:/bin/bash\n");
    const QDBusMessage message = QDBusMessage::createMethodCall(request.serviceOwner,
        QStringLiteral("/org/freedesktop/UDisks2"), QStringLiteral("org.freedesktop.DBus.ObjectManager"), QStringLiteral("GetManagedObjects"));
    const QDBusReply<rufus::UDisksObjects> snapshot = QDBusConnection::systemBus().call(message);
    require(snapshot.isValid(), "raw baseline metadata");
    rawObjects = snapshot.value();
    struct stat partition;
    require(stat("/dev/sda1", &partition) == 0 && S_ISBLK(partition.st_mode), "raw mounted partition");
    rawPartition = quint64(partition.st_rdev);
    if (QFile::exists(QStringLiteral("/etc/rufus-fixed-usb-fixture")))
        runFixedUsbAcquisitionCases(request);

    const QString path = QStringLiteral("/run/caller-home/source.img");
    QFile source(path), disk(QStringLiteral("/dev/sda"));
    require(source.open(QIODevice::ReadWrite) && disk.open(QIODevice::ReadOnly), "raw fixture files");
    constexpr int copyBufferBytes = 1024 * 1024;
    for (quint64 offset = 0; offset < request.size; offset += copyBufferBytes)
    {
        const QByteArray bytes = disk.read(copyBufferBytes);
        require(bytes.size() == copyBufferBytes && source.write(bytes) == bytes.size(), "raw fixture copy");
    }
    require(source.flush() && source.seek(0), "raw source header");
    const QByteArray header = source.read(sectorBytes);
    source.close();
    disk.close();
    require(chown(QFile::encodeName(path).constData(), 1000, 1000) == 0, "raw caller ownership");
    const QByteArray expectedDisk = diskDigest();
    const int descriptors = descriptorCount();
    if (helper)
    {
        runRawHelperCases(request, path, expectedDisk);
        return;
    }
    const char* cases[] = {"source-changed", "wrong-owner", "bad-marker", "wim", "partial-sector", "too-large",
        "wrong-sequence", "busy", "cancel-before", "cancel-unmount", "cancel-copy", "sync-fail", "verify-fail", "success"};
    for (const char* value : cases)
    {
        const QByteArray name(value);
        require(source.open(QIODevice::ReadWrite) && source.resize(qint64(request.size)) && source.seek(0), "restore raw source");
        require(source.write(header) == header.size() && source.flush(), "restore raw header");
        if (name == "bad-marker")
        {
            require(source.seek(sectorBytes - 2) && source.write("xx", 2) == 2, "invalid raw marker");
        }
        if (name == "wim")
            require(source.seek(0) && source.write("MSWIM\0\0\0", 8) == 8, "WIM signature");
        if (name == "partial-sector") require(source.resize(qint64(request.size) - 1), "partial source sector");
        if (name == "too-large") require(source.resize(qint64(request.size) + sectorBytes), "oversized source");
        source.close();
        require(chown(QFile::encodeName(path).constData(), name == "wrong-owner" ? 0 : 1000, 1000) == 0, "raw test owner");
        require(rufus::CapturePrepareSource(path, request) == 0, "capture raw source");
        if (name == "source-changed")
        {
            require(source.open(QIODevice::Append) && source.write("x", 1) == 1, "change raw source");
            source.close();
        }
        rawRequest = request;
        if (name == "wrong-sequence") rawRequest.diskSequence++;
        require(mount("/dev/sda1", "/mnt/target", "vfat", MS_RDONLY | MS_NODEV | MS_NOSUID | MS_NOEXEC, "uid=1000") == 0, "raw desktop mount");
        publishTargetMount(rawObjects, request, "/mnt/target", rawPartition);
        const int held = name == "busy" ? open("/mnt/target", O_RDONLY | O_DIRECTORY | O_CLOEXEC) : -1;
        if (name == "busy") require(held >= 0, "busy raw mount");
        const QByteArray before = diskDigest();
        rawFault = RawFault::None;
        if (name == "cancel-unmount") rawFault = RawFault::CancelUnmount;
        if (name == "cancel-copy") rawFault = RawFault::CancelCopy;
        if (name == "sync-fail") rawFault = RawFault::Sync;
        if (name == "verify-fail") rawFault = RawFault::Verify;
        cancelled = name == "cancel-before";
        rawUnmounts = rawFlushes = 0;
        rawActive = true;
        RAW_WRITE_RESULT result;
        const int error = rufus::WriteRawDevice(rawRequest, 1000, result);
        rawActive = false;
        cancelled = false;
        int expected = 0;
        if (name == "source-changed" || name == "wrong-sequence") expected = ESTALE;
        if (name == "wrong-owner") expected = EACCES;
        if (name == "bad-marker" || name == "wim" || name == "partial-sector") expected = ENOTSUP;
        if (name == "too-large") expected = ENOSPC;
        if (name == "busy") expected = EBUSY;
        if (name.startsWith("cancel-")) expected = ECANCELED;
        if (name == "sync-fail") expected = EIO;
        if (name == "verify-fail") expected = EBADMSG;
        if (error != expected) qFatal("raw %s: error=%d expected=%d", value, error, expected);
        const bool wrote = name == "cancel-copy" || name == "sync-fail" || name == "verify-fail" || name == "success";
        const bool unmounted = wrote || name == "cancel-unmount";
        require(rawUnmounts == int(unmounted), "raw unmount count");
        require((result.bytes_written != 0) == wrote, "raw write boundary");
        if (!wrote) require(diskDigest() == before, "raw refusal preserves disk");
        if (wrote) require(rawFlushes > 0, "raw writes flushed even on cancellation");
        if (name == "cancel-copy") require(result.bytes_written < request.imageSize, "raw partial cancellation");
        require(result.sync_error == (name == "sync-fail" ? EIO : 0), "raw sync error retained");
        if (name == "success")
        {
            require(result.phase == RAW_WRITE_DONE && result.bytes_written == request.size
                && result.bytes_verified == request.size, "raw complete readback");
            require(diskDigest() == expectedDisk, "raw whole-disk equality");
        }
        if (held >= 0) close(held);
        if (targetMounted(rawPartition)) require(umount("/mnt/target") == 0, "raw fixture unmount");
        publishTargetMount(rawObjects, request, {}, rawPartition);
        const int claim = open("/dev/sda", O_RDWR | O_EXCL | O_CLOEXEC);
        require(claim >= 0 && close(claim) == 0, "raw claim released");
        require(descriptorCount() == descriptors, "raw descriptors released");
        printf("RUFUS_RAW_DEVICE_CASE %s ERROR=%d UNMOUNTED=%d WRITTEN=%llu VERIFIED=%llu\n", value, error,
            rawUnmounts, (unsigned long long)result.bytes_written, (unsigned long long)result.bytes_verified);
    }
    require(umount("/run/caller-home") == 0, "raw source and home released");
    const QVariantMap stats = qdbus_cast<QVariantMap>(mountFixtureControl(QStringLiteral("Stats")).arguments().first());
    require(stats.value(QStringLiteral("opens")).toInt() == 0 && stats.value(QStringLiteral("unexpected")).toInt() == 0,
        "raw native backend makes no authorization calls");
    puts("RUFUS_RAW_DEVICE_DONE");
}

void checkUefiNtfsPreparation(rufus::PrepareImageRequest request)
{
    const bool standalone = QFile::exists(QStringLiteral("/etc/rufus-uefi-ntfs-helper-fixture"));
    const bool mbr = request.mbr;
    request.mbr = mbr && standalone; // Older format-only fixtures retain their GPT boot regression.
    request.sfd = false;
    const Table table = request.mbr ? Table::Mbr : Table::Gpt;
    const QString payloadPath = QStringLiteral("/uefi-ntfs.img");
    const QByteArray payload = readFile(payloadPath);
    require(!payload.isEmpty(), "bundled boot payload fixture");
    const GPT_IMAGE_OPTIONS options = {request.size, sectorBytes, 63,
        request.clusterSize, request.device, quint64(payload.size())};
    GPT_IMAGE_RESULT layout = {};
    rufus::PrepareImageRequest bootRequest = request;
    bootRequest.nonBootable = false;
    bootRequest.uefiNtfs = true;
    require(rufus::PlanFormatPartition(bootRequest, layout) == 0, "standalone UEFI:NTFS plan");
    if (request.fileSystem == FS_NTFS)
    {
        const rufus::NtfsFormatOptions format = {layout.size, layout.offset, request.clusterSize, request.label};
        rufus::NtfsFormatResult formatted;
        {
            rufus::PrepareDeviceTarget target(request);
            require(target.formatNtfs(layout, format, QStringLiteral(RUFUS_MKNTFS_PATH), formatted) == EINVAL,
                "non-bootable NTFS refuses a boot partition before access");
        }
        if (mbr)
        {
            rufus::PrepareDeviceTarget target(request, Table::Mbr);
            require(target.formatNtfs(layout, format, QStringLiteral(RUFUS_MKNTFS_PATH), formatted) == EINVAL,
                "non-bootable MBR/NTFS refuses a boot partition before access");
            rufus::PrepareImageRequest bootable = request;
            bootable.nonBootable = false;
            rufus::PrepareDeviceTarget bootTarget(bootable, Table::Mbr);
            require(bootTarget.open() == ENOTSUP, "implicit bootable MBR/NTFS remains unsupported");
            require(bootTarget.formatNtfs(layout, format, QStringLiteral(RUFUS_MKNTFS_PATH), formatted) == ENOTSUP,
                "bootable MBR/NTFS cannot format through the adapter");
        }
    }
    request.nonBootable = false;
    request.uefiNtfs = true;
    {
        rufus::PrepareDeviceTarget target(request, table);
        if (request.fileSystem == FS_NTFS)
        {
            const rufus::NtfsFormatOptions format = {layout.size, layout.offset, request.clusterSize, request.label};
            rufus::NtfsFormatResult formatted;
            GPT_IMAGE_RESULT withoutBoot = layout;
            withoutBoot.uefi_ntfs_offset = withoutBoot.uefi_ntfs_size = 0;
            require(target.formatNtfs(withoutBoot, format, QStringLiteral(RUFUS_MKNTFS_PATH), formatted) == EINVAL,
                "bootable NTFS requires its boot partition before access");
        }
        require(target.open() == 0, "seed standalone UEFI:NTFS target");
        if (request.mbr)
        {
            const MBR_IMAGE_OPTIONS mbrOptions = {request.size, sectorBytes, 63, request.clusterSize,
                0, request.device, request.fileSystem, MBR_BOOT_ZERO, quint64(payload.size())};
            MBR_IMAGE_RESULT prepared;
            require(PrepareMbrTarget(target.descriptor(), &mbrOptions, &prepared) == 0, "seed MBR boot partition");
        }
        else
            require(CreateGptImagePartition(target.descriptor(), &options, &layout) == 0, "seed GPT boot partition");
        require(ioctl(target.descriptor(), BLKRRPART, 0) == 0, "refresh boot partition");
        struct stat info;
        require(stat("/dev/sda2", &info) == 0 && S_ISBLK(info.st_mode), "boot partition node");
        const QByteArray link = QByteArray("/dev/block/") + QByteArray::number(major(info.st_rdev)) + ':'
            + QByteArray::number(minor(info.st_rdev));
        require(symlink("../sda2", link.constData()) == 0, "boot partition udev fixture link");
    }
    const FormatHelperCase cases[] = {{"sync-fail", EIO, true, 5, true},
        {"cancel-sync", ECANCELED, true, 5, true, true}, {"sync-eintr", 0, true, 5, true},
        {"bootable-regression", 0, true, 5}};
    const int descriptors = descriptorCount();
    const QString childList = QStringLiteral("/proc/self/task/%1/children").arg(getpid());
    const QByteArray children = readFile(childList).simplified();
    for (const FormatHelperCase& test : cases)
    {
        if (!standalone && test.faultProbe)
            continue;
        writeGuestFile(QStringLiteral("/run/format-fault"), test.name);
        runFormatHelperCase(test, request);
        const int fd = open("/dev/sda", O_RDONLY | O_EXCL | O_CLOEXEC);
        require(fd >= 0, "bootable readback claim");
        QByteArray actual(payload.size(), '\0');
        require(pread(fd, actual.data(), size_t(actual.size()), off_t(layout.uefi_ntfs_offset)) == actual.size(), "boot payload readback");
        require(actual == payload, "complete UEFI:NTFS payload precedes final sync, even on failure/cancellation");
        require(close(fd) == 0, "close bootable readback");
        for (const char* marker : {"format-exit-held", "format-launches", "format-final-sync", "format-fault"})
            QFile::remove(QStringLiteral("/run/") + QString::fromLatin1(marker));
        require(descriptorCount() == descriptors, "standalone helper descriptors released");
        require(readFile(childList).simplified() == children, "standalone helper descendants joined");
    }
    puts("RUFUS_NTFS_BOOTABLE_PAYLOAD_OK");
}

void runFormatHelperCases(rufus::PrepareImageRequest request, qint64 daemonPid, qint64 fixturePid, int fileSystem)
{
    const bool exfat = fileSystem == FS_EXFAT;
    const bool ntfs = fileSystem == FS_NTFS;
    const bool standalone = QFile::exists(QStringLiteral("/etc/rufus-uefi-ntfs-helper-fixture"));
    require(prctl(PR_SET_CHILD_SUBREAPER, 1) == 0, "format descendant audit");
    request.grub2 = QFile::exists(QStringLiteral("/etc/rufus-grub2-fixture"));
    request.nonBootable = !request.grub2;
    request.mbr = request.grub2 || QFile::exists(QStringLiteral("/etc/rufus-nonboot-mbr-fixture"));
    request.sfd = QFile::exists(QStringLiteral("/etc/rufus-sfd-helper-fixture"));
    if (request.sfd)
        request.label = QStringLiteral("RUFUS SFD");
    request.fileSystem = fileSystem;
    if (ntfs)
    {
        request.volumeId = 0;
        request.clusterSize = 4096;
        request.label = QStringLiteral("Rufus ș😀");
    }
    if (exfat)
    {
        request.fileSystem = FS_EXFAT;
        request.volumeId = 0;
        request.clusterSize = 128 * 1024; // Exercise exFAT clusters above the FAT/NTFS range.
        request.label = QStringLiteral("Rufus ș😀");
    }
    {
        rufus::PrepareDeviceTarget target(request);
        require(target.open() == 0, "seed format guest udev link");
        prepareDisk(target, request, Table::Gpt);
    }
    if (ntfs && request.sfd)
    {
        rufus::PrepareImageRequest bootable = request;
        bootable.nonBootable = false;
        rufus::PrepareDeviceTarget refused(bootable, Table::Sfd);
        require(refused.open() == ENOTSUP, "SFD/NTFS bootable request refused");
        rufus::PrepareDeviceTarget target(request, Table::Sfd);
        require(target.open() == 0, "SFD/NTFS preflight claim");
        const QByteArray before = diskDigest();
        GPT_IMAGE_RESULT layout = {};
        layout.size = request.size;
        rufus::NtfsFormatResult formatted;
        rufus::NtfsFormatOptions options = {request.size, 0, request.clusterSize, request.label};
        require(target.formatNtfs(layout, options, QStringLiteral(RUFUS_MKNTFS_PATH), formatted) == EBUSY,
            "SFD/NTFS refuses existing GPT children before clearing");
        layout.offset = options.partitionOffset = sectorBytes;
        require(target.formatNtfs(layout, options, QStringLiteral(RUFUS_MKNTFS_PATH), formatted) == EINVAL, "SFD/NTFS rejects shifted extent");
        layout.offset = options.partitionOffset = 0;
        layout.size = options.size = request.size - sectorBytes;
        require(target.formatNtfs(layout, options, QStringLiteral(RUFUS_MKNTFS_PATH), formatted) == EINVAL, "SFD/NTFS rejects shortened extent");
        layout.size = options.size = request.size;
        layout.uefi_ntfs_size = sectorBytes;
        require(target.formatNtfs(layout, options, QStringLiteral(RUFUS_MKNTFS_PATH), formatted) == EINVAL, "SFD/NTFS rejects boot payload");
        require(before == diskDigest(), "SFD/NTFS adapter preflight preserves media");
        checkClaim(target);
        puts("RUFUS_SFD_NTFS_PREFLIGHT_OK");
    }
    if (ntfs && !standalone && !request.grub2)
        checkUefiNtfsPreparation(request);
    if (exfat && request.sfd)
    {
        rufus::PrepareImageRequest bootable = request;
        bootable.nonBootable = false;
        rufus::PrepareDeviceTarget refused(bootable, Table::Sfd);
        require(refused.open() == ENOTSUP, "SFD/exFAT bootable request refused");
        rufus::PrepareDeviceTarget target(request, Table::Sfd);
        require(target.open() == 0, "SFD/exFAT preflight claim");
        const QByteArray before = diskDigest();
        FORMAT_RESULT formatted;
        rufus::ExfatFormatOptions options = {request.size, request.clusterSize, request.label, 0};
        const QString mkfs = QStringLiteral(RUFUS_MKFS_EXFAT_PATH);
        require(target.formatExfat(options, mkfs, formatted) == EBUSY, "SFD/exFAT refuses existing GPT children");
        options.partitionOffset = 1024 * 1024;
        require(target.formatExfat(options, mkfs, formatted) == EINVAL, "SFD/exFAT refuses shifted extent");
        options.partitionOffset = 0;
        options.size -= sectorBytes;
        require(target.formatExfat(options, mkfs, formatted) == EINVAL, "SFD/exFAT refuses shortened extent");
        options.size = request.size;
        require(rufus::FormatExfatDevice(target.descriptor(), request.device, mkfs, options, formatted, true) == EIO,
            "native exFAT cannot format a whole disk while its exclusive claim is held");
        require(formatted.started && formatted.exit_code != 0, "native formatter refused the competing claim");
        require(before == diskDigest(), "SFD/exFAT adapter preflight preserves media");
        checkClaim(target);
        puts("RUFUS_SFD_EXFAT_PREFLIGHT_OK");
    }
    if (exfat && request.mbr)
    {
        rufus::PrepareImageRequest bootable = request;
        bootable.nonBootable = false;
        rufus::PrepareDeviceTarget target(bootable, Table::Mbr);
        require(target.open() == ENOTSUP, "bootable MBR/exFAT remains unsupported");
        const rufus::ExfatFormatOptions format = {request.size, request.clusterSize, request.label, 0};
        FORMAT_RESULT result = {};
        require(target.formatExfat(format, QStringLiteral(RUFUS_MKFS_EXFAT_PATH), result) == ENOTSUP,
            "bootable MBR/exFAT cannot format through the adapter");
    }
    QList<FormatHelperCase> cases = {
        {"namespace-create", EPERM, false, 1, true}, {"namespace-private", EIO, false, 1, true},
        {"missing-formatter", ENOENT, false, 1}, {"old-formatter", ENOTSUP, false, 1},
        {"bad-label", EINVAL, false, 1}, {"metadata-space", ENOSPC, false, 1},
        {"invalid-cluster", EINVAL, false, 1},
        {"wrong-sequence", ESTALE, false, 1}, {"formatter-failure", EIO, true, 3},
        {"cancel-formatter", ECANCELED, true, 3, false, true},
        {"sync-fail", EIO, true, 4, true}, {"cancel-sync", ECANCELED, true, 4, true, true},
        {"mbr-changed", EBADMSG, true, 3, true}, {"sfd-table", EBADMSG, true, 3, true}, {"busy-reacquire", EBUSY, true, 3, true},
        {"sync-eintr", 0, true, 4, true}, {"success", 0, true, 4}
    };
    if (request.grub2)
    {
        for (FormatHelperCase& test : cases)
            if (test.phases == 4) ++test.phases; // Boot records precede finalization.
        const FormatHelperCase bootCases[] = {
            {"core-write-fail", EIO, true, 4, true}, {"core-sync-fail", EIO, true, 4, true},
            {"core-corrupt", EBADMSG, true, 4, true}, {"core-cancel", ECANCELED, true, 4, true, true},
            {"pbr-write-fail", EIO, true, 4, true}, {"pbr-sync-fail", EIO, true, 4, true},
            {"pbr-corrupt", EBADMSG, true, 4, true}, {"pbr-cancel", ECANCELED, true, 4, true, true}
        };
        for (const FormatHelperCase& test : bootCases) cases.insert(cases.size() - 1, test);
    }
    if (ntfs && request.grub2)
    {
        // The FAT matrix already qualifies common formatter/claim failures.
        // Exercise both Rufus boot-record writers on the native NTFS partition.
        cases = {
            {"core-write-fail", EIO, true, 4, true}, {"core-sync-fail", EIO, true, 4, true},
            {"core-corrupt", EBADMSG, true, 4, true}, {"core-cancel", ECANCELED, true, 4, true, true},
            {"pbr-write-fail", EIO, true, 4, true}, {"pbr-sync-fail", EIO, true, 4, true},
            {"pbr-corrupt", EBADMSG, true, 4, true}, {"pbr-cancel", ECANCELED, true, 4, true, true},
            {"success", 0, true, 5}
        };
    }
    const QString formatter = ntfs ? QStringLiteral(RUFUS_MKNTFS_PATH) :
        exfat ? QStringLiteral(RUFUS_MKFS_EXFAT_PATH) : QStringLiteral(RUFUS_MKFS_FAT_PATH);
    const int descriptors = descriptorCount();
    for (const FormatHelperCase& test : cases)
    {
        const QByteArray name(test.name);
        if (name == "mbr-changed" && !request.mbr)
            continue;
        if (name == "sfd-table" && !request.sfd)
            continue;
        if (name == "busy-reacquire" && !request.mbr && !request.sfd)
            continue;
        if (ntfs && name == "busy-reacquire")
            continue; // NTFS retains its claim throughout formatting; there is no handoff.
        if (exfat && name == "invalid-cluster")
            continue;
        if (!exfat && (name == "old-formatter" || name == "metadata-space"))
            continue;
        const bool missing = name == "missing-formatter", old = name == "old-formatter";
        const bool failed = name == "formatter-failure", cancelFormatter = name == "cancel-formatter";
        const bool replaceTool = ntfs || missing || old || failed || cancelFormatter;
        if (replaceTool)
            require(QFile::rename(formatter, formatter + QStringLiteral(".saved")), "hold native format tool");
        if (old)
            writeGuestFile(formatter, "#!/bin/bash\necho 'Usage: mkfs.exfat --cluster-size'\nexit 1\n", true);
        if (failed || cancelFormatter || (ntfs && !missing))
        {
            QByteArray script = "#!/bin/bash\n";
            if (ntfs)
            {
                script += request.sfd ? "[[ $1 == -F ]] || exit 64\n" : "[[ $1 == -Q ]] || exit 64\n";
                script += "/usr/bin/test_mbr_handoff_guest --claim || exit $?\n";
            }
            if (exfat)
                script += "if [[ $1 == --help ]]; then exec /usr/bin/mkfs.exfat.saved --help; fi\n";
            if (failed)
                script += "exit 7\n";
            else if (cancelFormatter)
                script += "echo blocked > /run/format-formatter-blocked\nexec sleep 60\n";
            else
                script += "exec /usr/bin/mkntfs.saved \"$@\"\n";
            writeGuestFile(formatter, script, true);
        }
        writeGuestFile(QStringLiteral("/run/format-fault"), name);
        runFormatHelperCase(test, request);
        if (replaceTool)
        {
            if (!missing)
                require(QFile::remove(formatter), "remove format wrapper");
            require(QFile::rename(formatter + QStringLiteral(".saved"), formatter), "restore format tool");
        }
        for (const QString& marker : {QStringLiteral("format-formatter-blocked"), QStringLiteral("format-final-sync"),
            QStringLiteral("format-boot-blocked"), QStringLiteral("format-exit-release"),
            QStringLiteral("format-exit-held"), QStringLiteral("format-launches"), QStringLiteral("format-fault")})
            QFile::remove(QStringLiteral("/run/") + marker);
        require(descriptorCount() == descriptors, "format client descriptor cleanup");
        const QString path = QStringLiteral("/proc/self/task/%1/children").arg(getpid());
        const QList<QByteArray> children = readFile(path).simplified().split(' ');
        const bool onlyFixtures = children.size() == 2 && children.contains(QByteArray::number(daemonPid))
            && children.contains(QByteArray::number(fixturePid));
        require(onlyFixtures, "format helper reaped all descendants");
    }
    if (standalone)
        checkUefiNtfsPreparation(request);
    if (ntfs && request.grub2)
        puts("RUFUS_GRUB2_NTFS_DONE");
    puts("RUFUS_FORMAT_HELPER_DONE");
}

void runExtHelperCases(rufus::PrepareImageRequest request, int fileSystem)
{
    require(fileSystem == FS_EXT2 || fileSystem == FS_EXT3, "ext helper fixture type");
    require(prctl(PR_SET_CHILD_SUBREAPER, 1) == 0, "ext descendant audit");
    {
        rufus::PrepareDeviceTarget seed(request);
        require(seed.open() == 0, "seed ext guest disk");
        prepareDisk(seed, request, Table::Gpt);
    }
    request.fileSystem = fileSystem;
    request.clusterSize = RUFUS_EXT_DEFAULT_CLUSTER;
    request.volumeId = 0;
    request.label = QStringLiteral("RufusExt");
    request.grub2 = request.mbr = true;
    const QString formatter = QStringLiteral(RUFUS_MKE2FS_PATH);
    QList<FormatHelperCase> cases;
    if (fileSystem == FS_EXT3)
        cases = {{"wrong-sequence", ESTALE, false, 1}, {"missing-formatter", ENOENT, false, 1},
            {"bad-label", EINVAL, false, 1}, {"formatter-failure", EIO, true, 3},
            {"cancel-formatter", ECANCELED, true, 3, false, true},
            {"mbr-changed", EBADMSG, true, 3, true}, {"busy-reacquire", EBUSY, true, 3, true},
            {"sync-fail", EIO, true, 5, true}, {"cancel-sync", ECANCELED, true, 5, true, true},
            {"core-write-fail", EIO, true, 4, true}, {"core-corrupt", EBADMSG, true, 4, true},
            {"core-cancel", ECANCELED, true, 4, true, true}};
    cases.append({"success", 0, true, 5});
    const int descriptors = descriptorCount();
    for (const FormatHelperCase& test : cases)
    {
        const QByteArray name(test.name);
        const bool missing = name == "missing-formatter";
        const bool failed = name == "formatter-failure";
        const bool cancel = name == "cancel-formatter";
        const bool replaceTool = missing || failed || cancel;
        if (replaceTool)
            require(QFile::rename(formatter, formatter + QStringLiteral(".saved")), "hold native ext formatter");
        if (failed || cancel)
        {
            QByteArray script = "#!/bin/bash\n";
            script += failed ? "exit 7\n" : "echo blocked > /run/format-formatter-blocked\nexec sleep 60\n";
            writeGuestFile(formatter, script, true);
        }
        writeGuestFile(QStringLiteral("/run/format-fault"), name);
        runFormatHelperCase(test, request);
        if (replaceTool)
        {
            if (!missing)
                require(QFile::remove(formatter), "remove ext formatter wrapper");
            require(QFile::rename(formatter + QStringLiteral(".saved"), formatter), "restore ext formatter");
        }
        for (const QString& marker : {QStringLiteral("format-formatter-blocked"), QStringLiteral("format-final-sync"),
            QStringLiteral("format-boot-blocked"), QStringLiteral("format-exit-release"),
            QStringLiteral("format-exit-held"), QStringLiteral("format-launches"), QStringLiteral("format-fault")})
            QFile::remove(QStringLiteral("/run/") + marker);
        require(descriptorCount() == descriptors, "ext client descriptors released");
    }
    puts(fileSystem == FS_EXT2 ? "RUFUS_GRUB2_EXT2_DONE" : "RUFUS_GRUB2_EXT3_DONE");
}

void runHandoffCases(const rufus::PrepareImageRequest& request)
{
    rufus::PrepareImageRequest unsupported = request;
    unsupported.fileSystem = FS_NTFS;
    rufus::PrepareDeviceTarget unsupportedTarget(unsupported, Table::Mbr);
    require(unsupportedTarget.open() == ENOTSUP && unsupportedTarget.descriptor() < 0, "bootable MBR/NTFS refused");
    puts("RUFUS_HANDOFF_CASE ntfs-refused ERROR=95");
    const Case cases[] = {
        {"snapshot-fail", Table::Mbr, Fault::SnapshotRead, EIO},
        {"format-mbr-changed", Table::Mbr, Fault::FormatMbr, EBADMSG},
        {"format-busy", Table::Mbr, Fault::FormatBusy, EBUSY},
        {"format-cancel", Table::Mbr, Fault::FormatCancel, ECANCELED},
        {"gpt-primary-changed", Table::Gpt, Fault::FormatGpt, EBADMSG},
        {"gpt-backup-changed", Table::Gpt, Fault::FormatBackup, EBADMSG},
        {"gpt-success", Table::Gpt},
        {"before-mount-changed", Table::Mbr, Fault::BeforeMount, 0, EBADMSG},
        {"mount-fail", Table::Mbr, Fault::Mount, 0, EIO},
        {"sync-fail", Table::Mbr, Fault::Sync, 0, 0, 0, EIO},
        {"mounted-mbr-changed", Table::Mbr, Fault::MountedMbr, 0, 0, EBADMSG},
        {"mbr-fill", Table::Mbr, Fault::None, 0, 0, 0, 0, true}
    };
    const int descriptors = descriptorCount();
    for (const Case& test : cases)
    {
        runCase(test, request);
        require(descriptorCount() == descriptors, "case descriptor leak");
    }
}
}

extern "C" int IsRufusJobCancelled(void) { return cancelled; }
extern "C" int IsIsoScanCancelled(void) { return cancelled; }
extern "C" void UpdateProgress(int, float) {}
extern "C" void _UpdateProgressWithInfo(int operation, int, uint64_t processed, uint64_t, int)
{
    if (!rawActive)
        return;
    checkExclusiveClaim();
    if (rawFault == RawFault::CancelCopy && operation == RAW_WRITE_COPY && processed != 0)
        cancelled = true;
    if (rawFault == RawFault::Verify && operation == RAW_WRITE_VERIFY && processed == 0)
        corruptDisk(off_t(diskBytes - 1));
}
extern "C" void uprintfs(const char* text) { fprintf(stderr, "%s\n", text); }
extern "C" void uprintf(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputc('\n', stderr);
}

static int afterFormat(int error)
{
    if (error != 0)
        return error;
    if (fault == Fault::SfdEntries)
        corruptDisk(446); // First partition entry, after formatter readback.
    if (fault == Fault::SfdBpb)
        corruptDisk(11); // BPB bytes per sector, after formatter readback.
    if (fault == Fault::FormatMbr)
        corruptDisk(diskSignatureOffset);
    if (fault == Fault::FormatGpt)
        corruptDisk(sectorBytes);
    if (fault == Fault::FormatBackup)
        corruptDisk(diskBytes - sectorBytes);
    if (fault == Fault::FormatBusy)
    {
        busyDescriptor = open("/dev/sda", O_RDWR | O_EXCL | O_CLOEXEC);
        require(busyDescriptor >= 0, "competing claim during formatter handoff");
    }
    if (fault == Fault::FormatCancel)
    {
        cancelled = true;
        return ECANCELED;
    }
    return 0;
}

extern "C" int __real_FormatFat32Partition(int, uint64_t, const char*, const FAT_FORMAT_OPTIONS*, FORMAT_RESULT*);
extern "C" int __wrap_FormatFat32Partition(int fd, uint64_t device, const char* mkfs,
    const FAT_FORMAT_OPTIONS* options, FORMAT_RESULT* result)
{
    return afterFormat(__real_FormatFat32Partition(fd, device, mkfs, options, result));
}

extern "C" int __real_FormatFat16Partition(int, uint64_t, const char*, const FAT_FORMAT_OPTIONS*, FORMAT_RESULT*);
extern "C" int __wrap_FormatFat16Partition(int fd, uint64_t device, const char* mkfs,
    const FAT_FORMAT_OPTIONS* options, FORMAT_RESULT* result)
{
    return afterFormat(__real_FormatFat16Partition(fd, device, mkfs, options, result));
}

extern "C" ssize_t __real_pread64(int, void*, size_t, off_t);
extern "C" ssize_t __wrap_pread64(int fd, void* data, size_t size, off_t offset)
{
    if (fault == Fault::SnapshotRead)
    {
        errno = EIO;
        return -1;
    }
    return __real_pread64(fd, data, size, offset);
}

extern "C" int __real_mount(const char*, const char*, const char*, unsigned long, const void*);
extern "C" int __wrap_mount(const char* source, const char* target, const char* type, unsigned long flags, const void* data)
{
    if (fault == Fault::Mount && type != nullptr && strcmp(type, "vfat") == 0)
    {
        errno = EIO;
        return -1;
    }
    return __real_mount(source, target, type, flags, data);
}

extern "C" int __real_syncfs(int);
extern "C" int __wrap_syncfs(int fd)
{
    if (fault == Fault::Sync)
    {
        errno = EIO;
        return -1;
    }
    return __real_syncfs(fd);
}

extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd)
{
    if (rawActive)
    {
        rawFlushes++;
        if (rawFault == RawFault::Sync)
        {
            errno = EIO;
            return -1;
        }
    }
    if ((installing && fault == Fault::InstallSync) || ((exfatFormatting || sfdHandoff) && fault == Fault::Sync))
    {
        errno = EIO;
        return -1;
    }
    return __real_fsync(fd);
}

extern "C" int __real_umount2(const char*, int);
extern "C" int __wrap_umount2(const char* path, int flags)
{
    const int result = __real_umount2(path, flags);
    if (result == 0 && rawActive)
    {
        rawUnmounts++;
        publishTargetMount(rawObjects, rawRequest, {}, rawPartition);
        if (rawFault == RawFault::CancelUnmount)
            cancelled = true;
    }
    return result;
}

extern "C" int __real_RunFormatCommand(int, char* const*, FORMAT_RESULT*);
extern "C" int __wrap_RunFormatCommand(int fd, char* const* arguments, FORMAT_RESULT* result)
{
    if (exfatHandoff || sfdHandoff)
    {
        const int claim = open("/dev/sda", O_RDWR | O_EXCL | O_CLOEXEC);
        require(claim >= 0 && close(claim) == 0, "disk claim released for exFAT");
    }
    exfatFormatting = exfatHandoff;
    const int error = __real_RunFormatCommand(fd, arguments, result);
    exfatFormatting = false;
    return exfatHandoff ? afterFormat(error) : error;
}

extern "C" int __real_RunInstallerCommand(int, char* const*, char* const*, const char*, FORMAT_RESULT*);
extern "C" int __wrap_RunInstallerCommand(int fd, char* const* arguments, char* const* environment,
    const char* directory, FORMAT_RESULT* result)
{
    checkExclusiveClaim();
    installing = true;
    const int error = __real_RunInstallerCommand(fd, arguments, environment, directory, result);
    installing = false;
    checkExclusiveClaim();
    if (error == 0 && fault == Fault::InstallMbr)
        corruptDisk(diskSignatureOffset);
    if (error == 0 && fault == Fault::InstallCancel)
        cancelled = true;
    return error;
}

int main(int argc, char** argv)
{
    const bool root = getuid() == 0 && geteuid() == 0;
    if (!root)
        return 77;
    const bool fixture = readFile(QStringLiteral("/etc/rufus-mbr-fixture")) == "RUFUS-MBR-TEST\n"
        && readFile(QStringLiteral("/sys/class/dmi/id/product_name")) == "RufusMBRFixture\n";
    if (!fixture)
        return 77;
    if (argc == 2 && strcmp(argv[1], "--claim") == 0)
    {
        checkExclusiveClaim();
        puts("RUFUS_INSTALL_CHILD_CLAIM_OK");
        return 0;
    }
    if (getppid() != 1)
        return 77;
    require(unshare(CLONE_NEWNS) == 0, "private mount namespace");
    require(mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) == 0, "private mount propagation");
    const bool alpine = argc == 2 && strcmp(argv[1], "--alpine") == 0;
    const bool grubImage = argc == 2 && strcmp(argv[1], "--grub2-image") == 0;
    const bool grub4dosImage = argc == 2 && strcmp(argv[1], "--grub4dos-image") == 0;
    const bool windowsImage = argc == 2 && strcmp(argv[1], "--windows-bios") == 0;
    const bool legacyBoot = argc == 2 && strcmp(argv[1], "--legacy-boot") == 0;
    const bool nativeFormats = argc == 2 && strcmp(argv[1], "--native-formats") == 0;
    const bool geometryBatch = argc == 2 && strcmp(argv[1], "--geometry-batch") == 0;
    const bool runtimeValidation = argc == 2 && strcmp(argv[1], "--runtime-validation") == 0;
    const bool isoResources = argc == 2 && strcmp(argv[1], "--iso-resources") == 0;
    const bool legacyIso = argc == 2 && strcmp(argv[1], "--legacy-iso") == 0;
    const bool isoOptions = argc == 2 && strcmp(argv[1], "--iso-options") == 0;
    const bool ntfsOptions = argc == 2 && strcmp(argv[1], "--ntfs-options") == 0;
    if (nativeFormats || geometryBatch || runtimeValidation || legacyBoot || isoOptions || ntfsOptions || isoResources || legacyIso) diskBytes = 320 * 1024 * 1024;
    const bool windowsToGo = argc == 2 && strcmp(argv[1], "--windows-togo") == 0;
    if (windowsToGo) diskBytes = 1024 * 1024 * 1024;
    if (windowsImage || grub4dosImage) diskBytes = 64 * 1024 * 1024;
    if (alpine || grubImage)
        diskBytes = alpineDiskBytes;
    const bool ext2 = argc == 2 && strcmp(argv[1], "--grub2-ext2-helper") == 0;
    const bool ext3 = argc == 2 && strcmp(argv[1], "--grub2-ext3-helper") == 0;
    if (ext2 || ext3)
        diskBytes = 320 * 1024 * 1024;
    QCoreApplication application(argc, argv);
    QProcess daemon, fixtureProcess;
    const rufus::PrepareImageRequest request = makeRequest(daemon, fixtureProcess);
    const bool syslinux4 = argc == 2 && strcmp(argv[1], "--syslinux4-helper") == 0;
    const bool syslinux6 = argc == 2 && strcmp(argv[1], "--syslinux6-helper") == 0;
    if (geometryBatch)
        runGeometryCases(request);
    else if (QFile::exists(QStringLiteral("/etc/rufus-device-source-fixture")))
        runDeviceSourceCases(request);
    else if (argc == 2 && strcmp(argv[1], "--device-options") == 0)
        runDeviceOptionsCases(request);
    else if (isoResources)
        runIsoResourcesCases(request);
    else if (legacyIso)
        runLegacyIsoCases(request);
    else if (ntfsOptions)
        runNtfsOptionsCases(request);
    else if (isoOptions)
        runIsoOptionsCases(request);
    else if (legacyBoot)
        runLegacyBootCases(request);
    else if (runtimeValidation)
        runRuntimeValidationCases(request);
    else if (nativeFormats)
        runNativeFormatCases(request);
    else if (grubImage || grub4dosImage || windowsImage)
        runBiosImage(request, daemon.processId(), fixtureProcess.processId(), windowsImage);
    else if (syslinux4 || syslinux6)
        runSyslinuxCases(request, syslinux4 ? BT_SYSLINUX_V4 : BT_SYSLINUX_V6);
    else if (argc == 2 && strcmp(argv[1], "--grub4dos-helper") == 0)
        runGrub4dosHelper(request);
    else if (argc == 2 && strcmp(argv[1], "--freedos-helper") == 0)
        runFreeDosHelperCases(request);
    else if (argc == 2 && strcmp(argv[1], "--sfd") == 0)
        runSfdCases(request);
    else if (argc == 2 && strcmp(argv[1], "--exfat-helper") == 0)
        runFormatHelperCases(request, daemon.processId(), fixtureProcess.processId(), FS_EXFAT);
    else if (argc == 2 && strcmp(argv[1], "--fat-helper") == 0)
    {
        runFormatHelperCases(request, daemon.processId(), fixtureProcess.processId(), request.fileSystem);
        if (QFile::exists(QStringLiteral("/etc/rufus-mounted-helper-fixture")))
            runMountedHelperCases(request, daemon.processId(), fixtureProcess.processId());
    }
    else if (argc == 2 && strcmp(argv[1], "--ntfs-helper") == 0)
        runFormatHelperCases(request, daemon.processId(), fixtureProcess.processId(), FS_NTFS);
    else if (ext2 || ext3)
        runExtHelperCases(request, ext2 ? FS_EXT2 : FS_EXT3);
    else if (argc == 2 && strcmp(argv[1], "--exfat") == 0)
        runExfatCases(request);
    else if (argc == 2 && strcmp(argv[1], "--zip-supplement") == 0)
        runArchiveCases(request);
    else if (windowsToGo)
        runWindowsToGoCases(request);
    else if (alpine || (argc == 2 && strcmp(argv[1], "--helper") == 0))
    {
        runHelperCases(request, daemon.processId(), fixtureProcess.processId(), alpine);
        if (QFile::exists(QStringLiteral("/etc/rufus-mounted-iso-helper-fixture")))
            runMountedIsoHelperCases(request, daemon.processId(), fixtureProcess.processId());
        if (QFile::exists(QStringLiteral("/etc/rufus-mounted-raw-fixture")))
            runMountedRawCases(request);
        if (QFile::exists(QStringLiteral("/etc/rufus-mounted-raw-helper-fixture")))
            runMountedRawCases(request, true);
    }
    else if (argc == 2 && strcmp(argv[1], "--install") == 0)
        runInstallCases(request, daemon.processId(), fixtureProcess.processId());
    else
        runHandoffCases(request);
    const QDBusMessage quit = QDBusMessage::createMethodCall(QStringLiteral("org.rufus.TestUDisks"),
        QStringLiteral("/org/freedesktop/UDisks2"), QStringLiteral("org.rufus.TestUDisks"), QStringLiteral("Quit"));
    require(QDBusConnection::systemBus().call(quit).type() == QDBusMessage::ReplyMessage, "stop fixture");
    require(fixtureProcess.waitForFinished(processTimeoutMs), "fixture exit");
    const bool cleanExit = fixtureProcess.exitStatus() == QProcess::NormalExit && fixtureProcess.exitCode() == 0;
    require(cleanExit, "fixture clean exit");
    daemon.terminate();
    require(daemon.waitForFinished(processTimeoutMs), "bus exit");
    puts("RUFUS_HANDOFF_DONE");
    return 0;
}
