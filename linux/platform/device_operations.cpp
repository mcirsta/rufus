// SPDX-License-Identifier: GPL-3.0-or-later
#include "device_operations.h"
#include "prepare_device.h"
#include "native_disk_facts.h"
#include "rufus_reporting.h"
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QtEndian>
#include <libfdisk/libfdisk.h>
#include <cerrno>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <linux/usbdevice_fs.h>
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
const QString espType = QStringLiteral("c12a7328-f81f-11d2-ba4b-00a0c93ec93b");
const QString dataType = QStringLiteral("ebd0a0a2-b9e5-4433-87c0-68b6b72699c7");
struct Label
{
    fdisk_context* disk = fdisk_new_context();
    fdisk_table* table = nullptr;
    ~Label() { fdisk_unref_table(table); fdisk_unref_context(disk); }
};
struct TableBytes { quint64 offset; QByteArray bytes; };
int readTable(fdisk_context* disk, int fd, QList<TableBytes>& regions)
{
    for (int index = 0; index < 64; ++index) {
        const char* name = nullptr; uint64_t offset = 0; size_t bytes = 0;
        const int found = fdisk_locate_disklabel(disk, index, &name, &offset, &bytes);
        if (found == 1) return regions.isEmpty() ? EBADMSG : 0;
        if (found < 0 || bytes == 0 || bytes > 16 * 1024 * 1024) return EBADMSG;
        QByteArray data(qsizetype(bytes), Qt::Uninitialized);
        if (pread(fd, data.data(), bytes, off_t(offset)) != ssize_t(bytes)) return EIO;
        regions.append({offset, data});
    }
    return E2BIG;
}
int readRegion(int fd, quint64 offset, size_t size, TableBytes& region)
{
    region.offset = offset; region.bytes.resize(qsizetype(size));
    ssize_t count;
    do { count = pread(fd, region.bytes.data(), size, off_t(offset)); } while (count < 0 && errno == EINTR);
    return count == ssize_t(size) ? 0 : count < 0 ? errno : EIO;
}
quint32 tableCrc(const QByteArray& bytes) { return quint32(crc32(0, reinterpret_cast<const Bytef*>(bytes.constData()), uInt(bytes.size()))); }
int validateGptHeader(QByteArray& bytes, quint64 current, quint64 alternate)
{
    const auto* data = reinterpret_cast<const uchar*>(bytes.constData());
    const quint32 length = qFromLittleEndian<quint32>(data + 12), expected = qFromLittleEndian<quint32>(data + 16);
    const bool format = memcmp(data, "EFI PART", 8) == 0 && qFromLittleEndian<quint32>(data + 8) == 0x10000
        && qFromLittleEndian<quint32>(data + 20) == 0 && length >= 92 && length <= quint32(bytes.size());
    if (!format || qFromLittleEndian<quint64>(data + 24) != current || qFromLittleEndian<quint64>(data + 32) != alternate) return EBADMSG;
    qToLittleEndian<quint32>(0, reinterpret_cast<uchar*>(bytes.data() + 16));
    const quint32 actual = quint32(crc32(0, reinterpret_cast<const Bytef*>(bytes.constData()), length));
    qToLittleEndian(expected, reinterpret_cast<uchar*>(bytes.data() + 16));
    return actual == expected ? 0 : EBADMSG;
}
void updateGptCrc(QByteArray& header, quint32 entries)
{
    auto* bytes = reinterpret_cast<uchar*>(header.data());
    qToLittleEndian(entries, bytes + 88); qToLittleEndian<quint32>(0, bytes + 16);
    const quint32 length = qFromLittleEndian<quint32>(bytes + 12);
    qToLittleEndian(quint32(crc32(0, bytes, length)), bytes + 16);
}
int planGptType(int fd, fdisk_context* disk, size_t index, const QString& type,
    QList<TableBytes>& original, QList<TableBytes>& changed)
{
    constexpr quint64 maximumTableBytes = 16 * 1024 * 1024;
    const quint64 sector = fdisk_get_sector_size(disk), sectors = fdisk_get_nsectors(disk);
    if (sector < 512 || sector > 65536 || (sector & (sector - 1)) != 0 || sectors < 6) return ENOTSUP;
    TableBytes primary, backup;
    int error = readRegion(fd, sector, size_t(sector), primary);
    if (error == 0) error = readRegion(fd, (sectors - 1) * sector, size_t(sector), backup);
    if (error == 0) error = validateGptHeader(primary.bytes, 1, sectors - 1);
    if (error == 0) error = validateGptHeader(backup.bytes, sectors - 1, 1);
    if (error != 0) return error;
    const auto* a = reinterpret_cast<const uchar*>(primary.bytes.constData());
    const auto* b = reinterpret_cast<const uchar*>(backup.bytes.constData());
    const quint64 first = qFromLittleEndian<quint64>(a + 40), last = qFromLittleEndian<quint64>(a + 48);
    const quint64 count = qFromLittleEndian<quint32>(a + 80), entry = qFromLittleEndian<quint32>(a + 84);
    const bool dimensions = entry >= 128 && (entry & (entry - 1)) == 0 && entry <= maximumTableBytes
        && count != 0 && count <= maximumTableBytes / entry && index < count;
    if (!dimensions || first > last || last >= sectors - 1 || memcmp(a + 40, b + 40, 32) != 0 || memcmp(a + 80, b + 80, 12) != 0) return EBADMSG;
    const quint64 bytes = count * entry, span = (bytes + sector - 1) / sector;
    const quint64 primaryLba = qFromLittleEndian<quint64>(a + 72), backupLba = qFromLittleEndian<quint64>(b + 72);
    const bool primaryFits = primaryLba >= 2 && primaryLba < first && span <= first - primaryLba;
    const bool backupFits = backupLba > last && backupLba < sectors - 1 && span <= sectors - 1 - backupLba;
    if (!primaryFits || !backupFits) return EBADMSG;
    TableBytes firstTable, lastTable;
    error = readRegion(fd, primaryLba * sector, size_t(bytes), firstTable);
    if (error == 0) error = readRegion(fd, backupLba * sector, size_t(bytes), lastTable);
    if (error != 0) return error;
    if (firstTable.bytes != lastTable.bytes || tableCrc(firstTable.bytes) != qFromLittleEndian<quint32>(a + 88)) return EBADMSG;
    original = {lastTable, backup, firstTable, primary};
    const QUuid guid(type);
    QByteArray encoded = guid.toRfc4122();
    std::reverse(encoded.begin(), encoded.begin() + 4); std::reverse(encoded.begin() + 4, encoded.begin() + 6);
    std::reverse(encoded.begin() + 6, encoded.begin() + 8);
    memcpy(firstTable.bytes.data() + index * size_t(entry), encoded.constData(), 16);
    lastTable.bytes = firstTable.bytes;
    const quint32 checksum = tableCrc(firstTable.bytes);
    updateGptCrc(primary.bytes, checksum); updateGptCrc(backup.bytes, checksum);
    changed = {lastTable, backup, firstTable, primary};
    return 0;
}
int planDosType(fdisk_context* disk, fdisk_partition* partition, uint code,
    const QList<TableBytes>& original, QList<TableBytes>& changed)
{
    const size_t index = fdisk_partition_get_partno(partition);
    const quint64 sector = fdisk_get_sector_size(disk), start = fdisk_partition_get_start(partition);
    for (const TableBytes& region : original) {
        if (region.bytes.size() < 512 || region.offset % sector != 0) return EBADMSG;
        if ((index < 4) != (region.offset == 0)) continue;
        const size_t entry = 446 + (index < 4 ? index * 16 : 0);
        const auto* bytes = reinterpret_cast<const uchar*>(region.bytes.constData());
        if (bytes[510] != 0x55 || bytes[511] != 0xaa) return EBADMSG;
        const quint64 relative = qFromLittleEndian<quint32>(bytes + entry + 8);
        if (region.offset / sector + relative != start) continue;
        if (qFromLittleEndian<quint32>(bytes + entry + 12) != fdisk_partition_get_size(partition)) return ESTALE;
        TableBytes updated = region; updated.bytes[qsizetype(entry + 4)] = char(code); changed = {updated}; return 0;
    }
    return ENOTSUP;
}
int writeRegions(int fd, const QList<TableBytes>& regions)
{
    // Finish a started table update without interrupting between its redundant
    // copies. Cancellation is checked before the first write; errors stay failed.
    for (const TableBytes& region : regions) {
        size_t done = 0;
        while (done < size_t(region.bytes.size())) {
            const ssize_t count = pwrite(fd, region.bytes.constData() + done, size_t(region.bytes.size()) - done, off_t(region.offset + done));
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) return count < 0 ? errno : EIO;
            done += size_t(count);
        }
    }
    return 0;
}
QString partitionId(fdisk_context* disk, fdisk_partition* partition, bool gpt)
{
    if (gpt) return QUuid(QString::fromLatin1(fdisk_partition_get_uuid(partition))).toString();
    char* text = nullptr;
    if (fdisk_get_disklabel_id(disk, &text) != 0) return {};
    bool valid = false; const quint32 signature = QString::fromLatin1(text).toUInt(&valid, 0); free(text);
    if (!valid) return {};
    const quint64 offset = fdisk_partition_get_start(partition) * fdisk_get_sector_size(disk);
    unsigned char bytes[8]; qToLittleEndian(offset, bytes);
    return QUuid(signature, 0, 0, bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7]).toString();
}
int usbHandle(const PrepareImageRequest& request, int& descriptor)
{
    descriptor = -1;
    NativeDiskFacts facts;
    int error = ReadNativeDiskFacts(request.device, request.size, facts);
    if (error != 0) return error;
    if (!facts.usbTransport || facts.deviceClass != DeviceClass::Usb) return ENOTSUP;
    QString parent = facts.sysfsPath;
    while (parent.startsWith(QStringLiteral("/sys/devices/"))) {
        if (QFile::exists(parent + QStringLiteral("/idVendor")) && QFile::exists(parent + QStringLiteral("/busnum"))) break;
        parent = QFileInfo(parent).path();
    }
    if (!parent.startsWith(QStringLiteral("/sys/devices/"))) return ENODEV;
    // A composite USB device or multi-disk reader cannot be reset without
    // affecting another function. Refuse it before issuing the ioctl.
    int disks = 0;
    QDirIterator entries(parent, QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (entries.hasNext()) {
        const QString path = entries.next();
        QFile kind(path + QStringLiteral("/bInterfaceClass"));
        if (kind.open(QIODevice::ReadOnly) && kind.read(32).trimmed() != "08") return ENOTSUP;
        if (QFile::exists(path + QStringLiteral("/diskseq")) && !QFile::exists(path + QStringLiteral("/partition"))) ++disks;
    }
    if (disks != 1) return ENOTSUP;
    QFile bus(parent + QStringLiteral("/busnum")), device(parent + QStringLiteral("/devnum"));
    if (!bus.open(QIODevice::ReadOnly) || !device.open(QIODevice::ReadOnly)) return EIO;
    bool busValid = false, deviceValid = false;
    const uint busNumber = bus.read(32).trimmed().toUInt(&busValid);
    const uint deviceNumber = device.read(32).trimmed().toUInt(&deviceValid);
    if (!busValid || !deviceValid || busNumber == 0 || deviceNumber == 0 || deviceNumber > 127) return EBADMSG;
    const QString path = QStringLiteral("/dev/bus/usb/%1/%2").arg(busNumber, 3, 10, QLatin1Char('0')).arg(deviceNumber, 3, 10, QLatin1Char('0'));
    const int fd = open(QFile::encodeName(path).constData(), O_WRONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return errno;
    struct stat info = {};
    if (fstat(fd, &info) != 0) error = errno;
    else if (!S_ISCHR(info.st_mode) || info.st_rdev != makedev(189, (busNumber - 1) * 128 + deviceNumber - 1)) error = ESTALE;
    NativeDiskFacts current;
    if (error == 0) error = ReadNativeDiskFacts(request.device, request.size, current);
    if (error == 0 && (current.diskSequence != request.diskSequence || current.sysfsPath != facts.sysfsPath)) error = ESTALE;
    if (error != 0) close(fd); else descriptor = fd;
    return error;
}
}
int ToggleDeviceEsp(int fd, const QStringList& records, EspRecord record, void* context, bool& started)
{
    started = false;
    if (!record || records.size() > 8) return EINVAL;
    Label label;
    if (!label.disk) return ENOMEM;
    fdisk_disable_dialogs(label.disk, 1);
    if (fdisk_assign_device_by_fd(label.disk, fd, "Rufus selected device", 0) != 0) return EIO;
    const bool gpt = fdisk_is_label(label.disk, GPT);
    if (!gpt && !fdisk_is_label(label.disk, DOS)) return ENOTSUP;
    if (fdisk_get_partitions(label.disk, &label.table) != 0 || fdisk_verify_disklabel(label.disk) != 0) return EBADMSG;
    QList<TableBytes> regions;
    int error = readTable(label.disk, fd, regions);
    if (error != 0) return error;
    fdisk_partition* selected = nullptr;
    bool restore = false;
    const size_t count = fdisk_table_get_nents(label.table);
    for (size_t index = 0; index < count && !selected; ++index) {
        fdisk_partition* partition = fdisk_table_get_partition(label.table, index);
        const fdisk_parttype* type = fdisk_partition_get_type(partition);
        const bool esp = gpt ? QString::fromLatin1(fdisk_parttype_get_string(type)).compare(espType, Qt::CaseInsensitive) == 0
            : fdisk_parttype_get_code(type) == 0xef;
        if (esp) selected = partition;
    }
    for (const QString& guid : records) {
        if (selected) break;
        if (QUuid(guid).isNull()) return EINVAL;
        for (size_t index = 0; index < count; ++index) {
            fdisk_partition* partition = fdisk_table_get_partition(label.table, index);
            if (partitionId(label.disk, partition, gpt) != guid) continue;
            const fdisk_parttype* type = fdisk_partition_get_type(partition);
            const bool data = gpt ? QString::fromLatin1(fdisk_parttype_get_string(type)).compare(dataType, Qt::CaseInsensitive) == 0
                : fdisk_parttype_get_code(type) == 0x01 || fdisk_parttype_get_code(type) == 0x0b || fdisk_parttype_get_code(type) == 0x0c || fdisk_parttype_get_code(type) == 0x0e;
            if (!data) return ESTALE;
            selected = partition; restore = true; break;
        }
    }
    if (!selected) return ENOENT;
    const QString guid = partitionId(label.disk, selected, gpt);
    if (QUuid(guid).isNull()) return EBADMSG;
    QString newType = restore ? espType : dataType;
    if (!gpt) {
        uint code = restore ? 0xef : 0x0b;
        if (!restore) {
            char boot[512];
            const quint64 offset = fdisk_partition_get_start(selected) * fdisk_get_sector_size(label.disk);
            if (pread(fd, boot, sizeof(boot), off_t(offset)) != ssize_t(sizeof(boot))) return EIO;
            for (int field : {0x36, 0x52}) {
                if (memcmp(boot + field, "FAT12   ", 8) == 0) code = 0x01;
                else if (memcmp(boot + field, "FAT16   ", 8) == 0) code = 0x0e;
                else if (memcmp(boot + field, "FAT32   ", 8) == 0) code = 0x0c;
                else if (memcmp(boot + field, "FAT     ", 8) == 0) code = 0x0b;
            }
        }
        newType = QString::number(code, 16);
    }
    QList<TableBytes> changed;
    if (gpt) error = planGptType(fd, label.disk, fdisk_partition_get_partno(selected), newType, regions, changed);
    else {
        bool valid = false; const uint code = newType.toUInt(&valid, 16);
        error = valid ? planDosType(label.disk, selected, code, regions, changed) : EINVAL;
    }
    if (error == 0) error = record(context, guid, restore);
    for (const TableBytes& region : regions) {
        if (error != 0) break;
        QByteArray current(region.bytes.size(), Qt::Uninitialized);
        if (pread(fd, current.data(), size_t(current.size()), off_t(region.offset)) != current.size()) error = EIO;
        else if (current != region.bytes) error = ESTALE;
    }
    if (error == 0 && IsRufusJobCancelled()) error = ECANCELED;
    if (error == 0) {
        started = true;
        error = writeRegions(fd, changed);
    }
    return error;
}
int RunDeviceOperation(const PrepareImageRequest& request, uid_t caller, EspRecord record, void* context, PrepareImageOutcome& outcome)
{
    PrepareDeviceTarget target(request, PrepareDeviceTarget::Table::Raw);
    int error = target.open(caller);
    int usb = -1;
    if (error == 0 && request.deviceOperation == ResetUsbPort) error = usbHandle(request, usb);
    if (error == 0 && request.deviceOperation == ToggleEspType) error = ToggleDeviceEsp(target.descriptor(), request.espRecords, record, context, outcome.started);
    if ((error == 0 || outcome.started) && fsync(target.descriptor()) != 0) {
        outcome.syncError = errno;
        if (error == 0) error = outcome.syncError;
    }
    if (error == 0 && request.deviceOperation == ResetUsbPort && IsRufusJobCancelled()) error = ECANCELED;
    if (error == 0 && request.deviceOperation == ResetUsbPort) {
        outcome.started = true;
        if (ioctl(usb, USBDEVFS_RESET, nullptr) != 0) error = errno;
    }
    if (error == 0 && request.deviceOperation == ToggleEspType && ioctl(target.descriptor(), BLKRRPART) != 0) error = errno;
    if (usb >= 0 && close(usb) != 0) outcome.cleanupError = errno;
    const int closed = target.finishRaw();
    if (outcome.cleanupError == 0) outcome.cleanupError = closed;
    if (error == 0) error = outcome.cleanupError;
    outcome.error = error;
    return error;
}
}
