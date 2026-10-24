// SPDX-License-Identifier: GPL-3.0-or-later
#include "ffu_finalize.h"
#include "prepare_device.h"
#include "format_command.h"
#include "target_io.h"
#include "rufus_reporting.h"
#include <QFile>
#include <QRegularExpression>
#include <QUuid>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>
#include <QtEndian>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace rufus
{
namespace
{
constexpr qsizetype maximumXmlBytes = 1024 * 1024;
constexpr int directoryFlags = O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW;
const uint8_t recoveryType[16] = {0xa4,0xbb,0x94,0xde,0xd1,0x06,0x40,0x4d,0xa1,0x6a,0xbf,0xd5,0x01,0x79,0xd6,0xac};

QUuid diskGuid(const uint8_t id[16])
{
    return QUuid(qFromLittleEndian<quint32>(id), qFromLittleEndian<quint16>(id + 4),
        qFromLittleEndian<quint16>(id + 6), id[8], id[9], id[10], id[11], id[12], id[13], id[14], id[15]);
}

int updateLocation(QXmlStreamAttributes& attributes, const QUuid& disk, quint64 offset)
{
    qsizetype offsetIndex = -1, guidIndex = -1;
    for (qsizetype index = 0; index < attributes.size(); index++)
    {
        const QStringView name = attributes[index].name();
        if (name.compare(QLatin1String("offset"), Qt::CaseInsensitive) == 0) offsetIndex = index;
        if (name.compare(QLatin1String("guid"), Qt::CaseInsensitive) == 0) guidIndex = index;
    }
    if (offsetIndex < 0 || attributes[offsetIndex].value() == QLatin1String("0")) return 0;
    if (guidIndex < 0) return EBADMSG;
    QString guid = attributes[guidIndex].value().toString();
    if (guid.size() == 38 && guid.front() == QLatin1Char('{') && guid.back() == QLatin1Char('}'))
        guid = guid.mid(1, 36);
    static const QRegularExpression canonical(QStringLiteral("^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$"));
    if (!canonical.match(guid).hasMatch()) return EBADMSG;
    if (QUuid::fromString(guid) == disk)
        attributes[offsetIndex] = QXmlStreamAttribute(attributes[offsetIndex].namespaceUri().toString(),
            attributes[offsetIndex].name().toString(), QString::number(offset));
    return 0;
}

int runResizeCommand(int fd, const QString& program, const char* operation,
    const QByteArray& size, bool force)
{
    const QByteArray executable = QFile::encodeName(program);
    QByteArray path = QByteArray("/proc/self/fd/") + QByteArray::number(FORMAT_TARGET_FD);
    const char* arguments[9] = {executable.constData()};
    unsigned count = 1;
    if (force) arguments[count++] = "--force";
    if (strcmp(operation, "grow") != 0) arguments[count++] = "--no-action";
    if (strcmp(operation, "info") == 0) arguments[count++] = "--info";
    else { arguments[count++] = "--size"; arguments[count++] = size.constData(); }
    arguments[count++] = path.constData(); arguments[count] = nullptr;
    FORMAT_RESULT result = {};
    const int error = RunFullFormatCommand(fd, const_cast<char* const*>(arguments), &result);
    if (result.diagnostics[0]) uprintf("ntfsresize: %s", result.diagnostics);
    return error;
}

int readBoot(int fd, quint64 offset, QByteArray& bytes)
{
    ssize_t count;
    do { count = pread(fd, bytes.data(), size_t(bytes.size()), off_t(offset)); } while (count < 0 && errno == EINTR);
    if (count < 0) return errno;
    return count == bytes.size() ? 0 : EIO;
}

int recoveryIndex(const QVector<FFU_DEPLOYMENT_PARTITION>& partitions, qsizetype& found)
{
    found = -1;
    for (qsizetype index = 0; index < partitions.size(); index++)
        if (memcmp(partitions[index].type, recoveryType, 16) == 0)
        {
            if (found >= 0) return EBADMSG;
            found = index;
        }
    return 0;
}

// Walk fixed image-internal paths through directory descriptors. Reject symlinks,
// special files and crossings to another filesystem at every component.
int openRecoveryFile(int root, const char* path, bool writable)
{
    struct stat volume;
    if (fstat(root, &volume) != 0) return -errno;
    const QList<QByteArray> parts = QByteArray(path).split('/');
    int parent = fcntl(root, F_DUPFD_CLOEXEC, 0);
    if (parent < 0) return -errno;
    int error = 0, result = -1;
    for (qsizetype index = 0; index < parts.size(); index++)
    {
        const bool leaf = index + 1 == parts.size();
        const int flags = leaf ? (writable ? O_RDWR : O_RDONLY) | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK : directoryFlags;
        const int next = openat(parent, parts[index].constData(), flags);
        if (next < 0) { error = errno; break; }
        struct stat info;
        if (fstat(next, &info) != 0) error = errno;
        else if (info.st_dev != volume.st_dev || (leaf ? !S_ISREG(info.st_mode) : !S_ISDIR(info.st_mode))) error = EBADMSG;
        if (error != 0) { close(next); break; }
        if (close(parent) != 0) { error = errno; close(next); parent = -1; break; }
        parent = next;
        if (leaf) { result = parent; parent = -1; }
    }
    if (parent >= 0 && close(parent) != 0 && error == 0) error = errno;
    return error == 0 ? result : -error;
}

int readXml(int fd, QByteArray& xml)
{
    struct stat info;
    if (fstat(fd, &info) != 0) return errno;
    if (info.st_size <= 0 || info.st_size > maximumXmlBytes) return EFBIG;
    xml.resize(info.st_size);
    return readBoot(fd, 0, xml);
}

int writeXml(int fd, const QByteArray& xml)
{
    int error = 0;
    for (qsizetype position = 0; position < xml.size();)
    {
        if (IsRufusJobCancelled()) return ECANCELED;
        const ssize_t count = pwrite(fd, xml.constData() + position, size_t(xml.size() - position), position);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { error = count < 0 ? errno : EIO; break; }
        position += count;
    }
    if (error == 0 && ftruncate(fd, xml.size()) != 0) error = errno;
    if (fsync(fd) != 0 && error == 0) error = errno;
    QByteArray readback(xml.size(), Qt::Uninitialized);
    if (error == 0) error = readBoot(fd, 0, readback);
    if (error == 0 && readback != xml) error = EIO;
    return error;
}

int finishPrivateMount(PrepareDeviceTarget& target, int error)
{
    int syncError = 0;
    const int cleanup = target.finishMount(syncError);
    if (cleanup != 0) return cleanup;
    return syncError != 0 ? syncError : error;
}

int checkCleanPartition(PrepareDeviceTarget& target, const FFU_DEPLOYMENT_PARTITION& part)
{
    quint64 device = 0;
    const int fd = target.openFfuPartition(part.number, part.offset, part.bytes, device);
    if (fd < 0) return -fd;
    int error = runResizeCommand(fd, QStringLiteral(RUFUS_NTFSRESIZE_PATH), "info", {}, false);
    if (close(fd) != 0 && error == 0) error = errno;
    return error;
}

int updateRecovery(PrepareDeviceTarget& target, const FFU_DEPLOYMENT_INFO& info,
    const QVector<FFU_DEPLOYMENT_PARTITION>& partitions, qsizetype recovery)
{
    QByteArray updated;
    bool foundOs = false;
    int error = 0;
    for (const FFU_DEPLOYMENT_PARTITION& part : partitions)
    {
        // OSVolumeFinder requires Windows/System32/ntoskrnl.exe. Inspect only
        // restored NTFS volumes belonging to this selected FFU store.
        QByteArray boot(512, Qt::Uninitialized);
        error = readBoot(target.descriptor(), part.offset, boot);
        if (error != 0) return error;
        if (memcmp(boot.constData() + 3, "NTFS    ", 8) != 0) continue;
        error = checkCleanPartition(target, part);
        if (error != 0) return error;
        error = target.mountFfuPartition(part.number, part.offset, part.bytes);
        if (error == 0)
        {
            const int marker = openRecoveryFile(target.mountDescriptor(), "Windows/System32/ntoskrnl.exe", false);
            if (marker >= 0)
            {
                foundOs = true;
                if (close(marker) != 0) error = errno;
                const int xml = error == 0 ? openRecoveryFile(target.mountDescriptor(), "Windows/System32/Recovery/ReAgent.xml", true) : -error;
                if (xml < 0) error = -xml;
                else
                {
                    QByteArray original;
                    error = readXml(xml, original);
                    if (error == 0) error = UpdateFfuRecoveryXml(original, info.disk_id, partitions[recovery].offset, updated);
                    if (error == 0) error = writeXml(xml, updated);
                    if (close(xml) != 0 && error == 0) error = errno;
                }
            }
            else if (marker != -ENOENT) error = -marker;
        }
        error = finishPrivateMount(target, error);
        if (error != 0) return error;
        if (foundOs) break;
    }
    if (!foundOs) return ENOENT;
    const FFU_DEPLOYMENT_PARTITION& part = partitions[recovery];
    error = checkCleanPartition(target, part);
    if (error != 0) return error;
    error = target.mountFfuPartition(part.number, part.offset, part.bytes);
    if (error == 0)
    {
        const int xml = openRecoveryFile(target.mountDescriptor(), "Recovery/WindowsRE/ReAgent.xml", true);
        if (xml >= 0)
        {
            error = writeXml(xml, updated);
            if (close(xml) != 0 && error == 0) error = errno;
        }
        else if (xml != -ENOENT) error = -xml;
    }
    return finishPrivateMount(target, error);
}
}

int UpdateFfuRecoveryXml(const QByteArray& input, const uint8_t diskId[16], quint64 recoveryOffset, QByteArray& output)
{
    output.clear();
    if (diskId == nullptr || recoveryOffset == 0 || input.isEmpty() || input.size() > maximumXmlBytes) return EINVAL;
    QXmlStreamReader reader(input);
    QByteArray rewritten;
    QXmlStreamWriter writer(&rewritten);
    const QUuid disk = diskGuid(diskId);
    unsigned depth = 0;
    bool root = false;
    int error = 0;
    while (!reader.atEnd() && error == 0)
    {
        if (IsRufusJobCancelled()) return ECANCELED;
        const QXmlStreamReader::TokenType token = reader.readNext();
        if (reader.hasError()) return EBADMSG;
        if (token == QXmlStreamReader::DTD || token == QXmlStreamReader::EntityReference) return EBADMSG;
        if (token == QXmlStreamReader::StartElement)
        {
            depth++;
            QXmlStreamAttributes attributes = reader.attributes();
            if (depth == 1)
            {
                if (root || reader.name().compare(QLatin1String("WindowsRE"), Qt::CaseInsensitive) != 0) return EBADMSG;
                root = true;
                for (const QXmlStreamAttribute& attribute : attributes)
                    if (attribute.name().compare(QLatin1String("version"), Qt::CaseInsensitive) == 0
                        && attribute.value() != QLatin1String("2.0")) return ENOTSUP;
            }
            else if (depth == 2 && (reader.name().compare(QLatin1String("WinreLocation"), Qt::CaseInsensitive) == 0
                || reader.name().compare(QLatin1String("ImageLocation"), Qt::CaseInsensitive) == 0))
                error = updateLocation(attributes, disk, recoveryOffset);
            writer.writeStartElement(reader.qualifiedName().toString());
            for (const QXmlStreamNamespaceDeclaration& declaration : reader.namespaceDeclarations())
                writer.writeNamespace(declaration.namespaceUri().toString(), declaration.prefix().toString());
            writer.writeAttributes(attributes);
        }
        else
        {
            if (token == QXmlStreamReader::EndElement) depth--;
            writer.writeCurrentToken(reader);
        }
        if (rewritten.size() > maximumXmlBytes) return EFBIG;
    }
    if (error == 0 && (reader.hasError() || writer.hasError() || !root || depth != 0)) error = EBADMSG;
    if (error == 0) output = rewritten;
    return error;
}

int GrowFfuNtfs(int fd, quint64 device, quint64 capturedBytes, quint64 partitionBytes,
    unsigned sector, const QString& program)
{
    const bool sectorSupported = sector == 512 || sector == 4096;
    if (!sectorSupported || capturedBytes < sector || partitionBytes < capturedBytes
        || partitionBytes % sector || !program.startsWith(QLatin1Char('/')) || program.contains(QChar(u'\0'))) return EINVAL;
    WRITABLE_TARGET initial;
    int error = InspectWritableTarget(fd, device, &initial);
    if (error != 0) return error;
    if (initial.size != partitionBytes || (device != 0 && initial.sector_size != sector)) return ESTALE;
    QByteArray before(sector, Qt::Uninitialized), after(sector, Qt::Uninitialized), backup(sector, Qt::Uninitialized);
    error = readBoot(fd, 0, before);
    if (error != 0) return error;
    const auto* boot = reinterpret_cast<const uchar*>(before.constData());
    const quint64 total = qFromLittleEndian<quint64>(boot + 40);
    const bool ntfs = memcmp(boot + 3, "NTFS    ", 8) == 0 && qFromLittleEndian<quint16>(boot + 11) == sector
        && boot[510] == 0x55 && boot[511] == 0xaa;
    if (!ntfs || total != capturedBytes / sector - 1) return EBADMSG;
    const unsigned sectorsPerCluster = boot[13];
    quint64 cluster = 0;
    if (sectorsPerCluster != 0 && sectorsPerCluster <= 128 && (sectorsPerCluster & (sectorsPerCluster - 1)) == 0)
        cluster = quint64(sector) * sectorsPerCluster;
    else if (sectorsPerCluster > 128 && 256 - sectorsPerCluster < 32)
        cluster = quint64(1) << (256 - sectorsPerCluster);
    if (cluster < sector || cluster > 32 * 1024 * 1024) return ENOTSUP;
    const QByteArray size = QByteArray::number(partitionBytes);
    // The first read-only check must reject an already dirty/corrupt capture.
    // One force suppresses the authorized grow's interactive confirmation. The
    // postcheck permits only the dirty flag our own successful resize creates.
    error = runResizeCommand(fd, program, "info", size, false);
    if (error == 0) error = runResizeCommand(fd, program, "dry-run", size, false);
    if (error == 0) error = runResizeCommand(fd, program, "grow", size, true);
    if (error == 0) error = runResizeCommand(fd, program, "info", size, true);
    if (error == 0) error = readBoot(fd, 0, after);
    if (error == 0) error = readBoot(fd, partitionBytes - sector, backup);
    if (error == 0)
    {
        const quint64 sectors = qFromLittleEndian<quint64>(after.constData() + 40);
        const quint64 maximum = (partitionBytes - sector) / cluster * (cluster / sector);
        const bool preservedIdentity = before.first(40) == after.first(40) && before.mid(72, 8) == after.mid(72, 8);
        const bool fillsPartition = sectors >= maximum && sectors <= maximum + cluster / sector;
        if (!preservedIdentity || !fillsPartition || sectors >= partitionBytes / sector || after != backup) error = EBADMSG;
    }
    WRITABLE_TARGET current;
    if (error == 0) error = InspectWritableTarget(fd, device, &current);
    if (error == 0 && (current.size != initial.size || current.info.st_dev != initial.info.st_dev
        || current.info.st_ino != initial.info.st_ino)) error = ESTALE;
    return error;
}

int CheckFfuFinalization(PrepareDeviceTarget& target, const FFU_DEPLOYMENT_INFO& info,
    const QVector<FFU_DEPLOYMENT_PARTITION>& partitions)
{
    if (!info.optimized) return 0;
    if (access(RUFUS_NTFSRESIZE_PATH, X_OK) != 0) return errno;
    qsizetype recovery;
    int error = recoveryIndex(partitions, recovery);
    if (error == 0 && recovery >= 0) error = target.checkFfuMountSupport();
    return error;
}

int FinishFfuDeployment(PrepareDeviceTarget& target, const FFU_DEPLOYMENT_INFO& info,
    const QVector<FFU_DEPLOYMENT_PARTITION>& partitions, unsigned sector)
{
    if (!info.optimized) return 0;
    int error = target.prepareFfuPartitions();
    qsizetype recovery;
    if (error == 0) error = recoveryIndex(partitions, recovery);
    // Update clean restored filesystems before ntfsresize sets its first-boot
    // consistency-check flag. Every mount uses the existing private handoff.
    if (error == 0 && recovery >= 0) error = updateRecovery(target, info, partitions, recovery);
    quint64 device = 0;
    const int partition = error == 0 ? target.openFfuPartition(info.partition, info.partition_offset, info.partition_bytes, device) : -error;
    if (partition < 0) return -partition;
    uprintf("Growing the optimized FFU NTFS partition to %llu bytes", static_cast<unsigned long long>(info.partition_bytes));
    error = GrowFfuNtfs(partition, device, info.filesystem_bytes, info.partition_bytes, sector, QStringLiteral(RUFUS_NTFSRESIZE_PATH));
    if (close(partition) != 0 && error == 0) error = errno;
    if (fsync(target.descriptor()) != 0 && error == 0) error = errno;
    if (error == 0) error = target.checkFfuPartitionTable();
    if (error == 0) error = target.check();
    return error;
}
}
