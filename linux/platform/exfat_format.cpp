// SPDX-License-Identifier: GPL-3.0-or-later
#include "exfat_format.h"
#include "target_io.h"
#include "rufus_reporting.h"
#include <QFile>
#include <QElapsedTimer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QtEndian>
#include <blkid/blkid.h>
#include <cerrno>
#include <cstring>
#include <sys/sysmacros.h>
#include <unistd.h>

namespace rufus
{
namespace
{
constexpr quint32 sysfsSectorBytes = 512;
constexpr quint64 minimumVolumeBytes = 16 * 1024 * 1024;
constexpr quint64 alignmentBytes = 1024 * 1024;
constexpr quint64 fatEntryBytes = 4;
constexpr quint64 reservedFatEntries = 2;
constexpr quint64 maximumClusterCount = 0xfffffff5; // Exclusive exfatprogs limit.
constexpr quint64 upcaseTableBytes = 5836; // Native standard compressed table.
constexpr int labelUnits = 11;
constexpr int bootRegionSectors = 12;
// exFAT's fixed boot-sector fields; libblkid supplies filesystem validation.
constexpr int partitionOffsetField = 64;
constexpr int volumeLengthField = 72;
constexpr int sectorShiftField = 108;
constexpr int clusterShiftField = 109;
constexpr int maximumClusterShift = 16;

int checkGeometry(const ExfatFormatOptions& options)
{
    // exfatprogs 1.4.3 exfat_build_mkfs_info: one FAT, unpacked bitmap,
    // standard upcase table, MiB-aligned start and explicit 1-MiB alignment.
    // Range checks below must precede this arithmetic. Nothing is allocated.
    const quint32 sectorBytes = options.sectorSize;
    const quint64 cluster = options.clusterSize;
    const quint64 fatOffset = alignmentBytes;
    const quint64 available = options.size - fatOffset - reservedFatEntries * fatEntryBytes;
    const quint64 estimatedClusters = (available - 1) / (cluster + fatEntryBytes) + 1;
    const quint64 fatBytes = (estimatedClusters + reservedFatEntries) * fatEntryBytes;
    const quint64 fatSectors = (fatBytes + sectorBytes - 1) / sectorBytes;
    if (fatSectors > UINT32_MAX)
        return EINVAL;
    const quint64 fatEnd = fatOffset + fatSectors * sectorBytes;
    const quint64 heapOffset = (fatEnd + alignmentBytes - 1) / alignmentBytes * alignmentBytes;
    if (heapOffset >= options.size)
        return ENOSPC;
    const quint64 clusters = (options.size - heapOffset) / cluster;
    if (clusters >= maximumClusterCount)
        return EINVAL;
    const quint64 bitmapBytes = (clusters + 7) / 8;
    const quint64 bitmapClusters = (bitmapBytes + cluster - 1) / cluster;
    const quint64 upcaseClusters = (upcaseTableBytes + cluster - 1) / cluster;
    const quint64 metadataClusters = bitmapClusters + upcaseClusters + 1; // Root directory.
    return clusters < metadataClusters ? ENOSPC : 0;
}

int checkDevice(quint64 device, const ExfatFormatOptions& options, bool wholeDevice)
{
    const QString sysfs = QStringLiteral("/sys/dev/block/%1:%2/").arg(major(dev_t(device))).arg(minor(dev_t(device)));
    if (!QFile::exists(sysfs))
        return ESTALE;
    const bool partition = QFile::exists(sysfs + QStringLiteral("partition"));
    if (wholeDevice)
        return !partition && options.partitionOffset == 0 ? 0 : EINVAL;
    if (!partition)
        return ENOTSUP;
    QFile start(sysfs + QStringLiteral("start"));
    if (!start.open(QIODevice::ReadOnly))
        return EIO;
    bool valid = false;
    const quint64 sectors = start.read(128).trimmed().toULongLong(&valid);
    return valid && sectors == options.partitionOffset / sysfsSectorBytes ? 0 : ESTALE;
}

int checkTarget(int fd, quint64 device, const QString& mkfs,
    const ExfatFormatOptions& options, bool wholeDevice = false)
{
    int error = CheckExfatFormatOptions(options);
    if (error != 0)
        return error;
    // Older libblkid identifies exFAT but does not check its boot checksum.
    if (blkid_get_library_version(nullptr, nullptr) < blkid_parse_version_string("2.39.0"))
        return ENOTSUP;
    if (!mkfs.startsWith('/') || mkfs.contains(QChar::Null))
        return EINVAL;
    WRITABLE_TARGET target;
    error = InspectWritableTarget(fd, device, &target);
    if (error != 0)
        return error;
    if (target.size != options.size)
        return ESTALE;
    if (device != 0)
    {
        if ((target.sector_size != options.sectorSize || target.physical_sector_size != options.physicalSectorSize))
            return ENOTSUP;
        error = checkDevice(device, options, wholeDevice);
        if (error != 0)
            return error;
    }
    else if (options.partitionOffset != 0)
        return EINVAL;
    return IsRufusJobCancelled() ? ECANCELED : 0;
}

int readBootRegions(int fd, const ExfatFormatOptions& options)
{
    const quint32 sectorBytes = options.sectorSize;
    const quint32 bootRegionBytes = bootRegionSectors * sectorBytes;
    unsigned char regions[2 * bootRegionSectors * 4096];
    const size_t regionBytes = size_t(2) * bootRegionBytes;
    ssize_t count;
    do {
        count = pread(fd, regions, regionBytes, 0);
    } while (count < 0 && errno == EINTR);
    if (count < 0)
        return errno;
    if (count != ssize_t(regionBytes))
        return EIO;
    if (memcmp(regions, regions + bootRegionBytes, bootRegionBytes) != 0)
        return EBADMSG;
    const unsigned encodedSectorShift = regions[sectorShiftField];
    const bool sectorMatches = encodedSectorShift >= 9 && encodedSectorShift <= 12 &&
        (1u << encodedSectorShift) == sectorBytes;
    const unsigned int clusterShift = regions[clusterShiftField];
    if (!sectorMatches || clusterShift > maximumClusterShift)
        return EBADMSG;
    const bool clusterMatches = (sectorBytes << clusterShift) == options.clusterSize;
    const bool sizeMatches = qFromLittleEndian<quint64>(regions + volumeLengthField) == options.size / sectorBytes;
    const bool offsetMatches = qFromLittleEndian<quint64>(regions + partitionOffsetField) == options.partitionOffset / sectorBytes;
    if (!clusterMatches || !sizeMatches || !offsetMatches)
        return EBADMSG;
    return 0;
}

int checkProbe(blkid_probe probe, int fd, const ExfatFormatOptions& options)
{
    int error = blkid_probe_set_device(probe, fd, 0, options.size);
    if (error == 0)
        error = blkid_probe_enable_partitions(probe, 0);
    if (error == 0)
        error = blkid_probe_set_superblocks_flags(probe, BLKID_SUBLKS_TYPE | BLKID_SUBLKS_LABELRAW);
    if (error != 0)
        return EIO;
    if (blkid_do_safeprobe(probe) != 0)
        return EBADMSG;
    const char* type = nullptr;
    size_t length = 0;
    if (blkid_probe_lookup_value(probe, "TYPE", &type, &length) != 0 || strcmp(type, "exfat") != 0)
        return EBADMSG;
    const char* label = nullptr;
    const bool hasLabel = blkid_probe_lookup_value(probe, "LABEL_RAW", &label, &length) == 0;
    if (options.label.isEmpty())
        return !hasLabel || length == 0 ? 0 : EBADMSG;
    const size_t labelBytes = size_t(options.label.size()) * sizeof(quint16);
    if (!hasLabel || length != labelBytes)
        return EBADMSG;
    for (qsizetype i = 0; i < options.label.size(); ++i)
        if (qFromLittleEndian<quint16>(label + i * sizeof(quint16)) != options.label[i].unicode())
            return EBADMSG;
    return 0;
}

}

int CheckExfatVolume(int fd, const ExfatFormatOptions& options)
{
    int error = CheckExfatFormatOptions(options);
    if (error != 0) return error;
    error = readBootRegions(fd, options);
    if (error != 0)
        return error;
    // libblkid may seek. Reopen our retained inode read-only with an independent
    // file position; neither the original user path nor a probe cache is used.
    QFile reader(QStringLiteral("/proc/self/fd/%1").arg(fd));
    if (!reader.open(QIODevice::ReadOnly))
        return EIO;
    blkid_probe probe = blkid_new_probe();
    if (probe == nullptr)
        return ENOMEM;
    error = checkProbe(probe, reader.handle(), options);
    blkid_free_probe(probe);
    return error;
}

int CheckExfatFormatOptions(const ExfatFormatOptions& options)
{
    const quint32 sectorBytes = options.sectorSize;
    const bool validSector = sectorBytes >= 512 && sectorBytes <= 4096 && (sectorBytes & (sectorBytes - 1)) == 0;
    const bool validPhysical = options.physicalSectorSize >= sectorBytes && options.physicalSectorSize <= 65536 &&
        (options.physicalSectorSize & (options.physicalSectorSize - 1)) == 0;
    if (!validSector || !validPhysical) return EINVAL;
    const RUFUS_CLUSTER_SIZES sizes = GetExfatClusterSizes(options.size, sectorBytes);
    const bool supportedSize = options.size >= minimumVolumeBytes && sizes.Allowed != 0;
    const bool sectorAligned = options.size % sectorBytes == 0;
    if (!supportedSize || !sectorAligned)
        return EINVAL;
    // Whole-MiB starts preserve the file-only layout calculation. Native mkfs
    // obtains the actual start from sysfs and writes the boot field itself.
    const bool alignedStart = options.partitionOffset % alignmentBytes == 0;
    const bool extentFits = options.partitionOffset <= quint64(INT64_MAX) - options.size;
    if (!alignedStart || !extentFits)
        return EINVAL;
    const bool powerOfTwo = options.clusterSize != 0 && (options.clusterSize & (options.clusterSize - 1)) == 0;
    const bool supportedCluster = (options.clusterSize & sizes.Allowed) != 0;
    if (!powerOfTwo || !supportedCluster)
        return EINVAL;
    const bool labelFits = options.label.size() <= labelUnits;
    const bool validUtf16 = QString::fromUtf8(options.label.toUtf8()) == options.label;
    if (!labelFits || !validUtf16)
        return EINVAL;
    const QString forbidden = QStringLiteral("\"*/:<>?\\|");
    for (QChar character : options.label)
        if (character.unicode() < 32 || forbidden.contains(character))
            return EINVAL;
    return checkGeometry(options);
}

RUFUS_CLUSTER_SIZES NativeExfatClusterSizes(quint64 diskSize, quint64 volumeSize, quint32 sectorSize, quint32 physicalSectorSize)
{
    if (volumeSize > diskSize)
        return {};
    RUFUS_CLUSTER_SIZES sizes = GetExfatClusterSizes(diskSize, sectorSize);
    quint32 candidates = sizes.Allowed;
    while (candidates != 0)
    {
        const quint32 cluster = candidates & (0u - candidates);
        candidates &= ~cluster;
        const ExfatFormatOptions options = {volumeSize, cluster, {}, 0, true, sectorSize, physicalSectorSize};
        if (CheckExfatFormatOptions(options) != 0)
            sizes.Allowed &= ~cluster;
    }
    if ((sizes.Default & sizes.Allowed) == 0)
        sizes.Default = sizes.Allowed & (0u - sizes.Allowed);
    return sizes;
}

int CheckExfatImageRequest(int fd, const QString& mkfs, const ExfatFormatOptions& options)
{
    return checkTarget(fd, 0, mkfs, options);
}

int CheckExfatFormatTools(const QString& mkfs, QString& problem)
{
    constexpr int timeoutMs = 3000;
    constexpr int pollMs = 50;
    constexpr int helpBytes = 16 * 1024;
    problem = QStringLiteral("exFAT needs exfatprogs with metadata verification, no-discard and partition-table options, and libblkid 2.39 or newer.");
    if (blkid_get_library_version(nullptr, nullptr) < blkid_parse_version_string("2.39.0"))
        return ENOTSUP;
    if (mkfs.isEmpty())
        return ENOENT;
    if (!mkfs.startsWith('/') || mkfs.contains(QChar::Null))
        return EINVAL;
    if (access(QFile::encodeName(mkfs).constData(), X_OK) != 0)
        return errno;
    if (IsRufusJobCancelled())
        return ECANCELED;
    QProcess process;
    QProcessEnvironment environment;
    environment.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    process.setProcessEnvironment(environment);
    process.setProcessChannelMode(QProcess::MergedChannels);
    QElapsedTimer timer;
    timer.start();
    process.start(mkfs, {QStringLiteral("--help")});
    QByteArray help;
    int error = 0;
    while (process.state() != QProcess::NotRunning)
    {
        process.waitForReadyRead(pollMs);
        help += process.read(helpBytes + 1 - help.size());
        if (IsRufusJobCancelled())
            error = ECANCELED;
        else if (timer.elapsed() >= timeoutMs)
            error = ETIMEDOUT;
        else if (help.size() > helpBytes)
            error = EOVERFLOW;
        if (error != 0)
        {
            process.kill();
            process.waitForFinished();
            return error;
        }
    }
    if (process.error() == QProcess::FailedToStart)
        return EIO;
    // exfatprogs prints valid help with exit status 1.
    const bool normalHelp = process.exitStatus() == QProcess::NormalExit && process.exitCode() <= 1;
    if (!normalHelp)
        return ENOTSUP;
    const char* const required[] = {"--check-written", "--no-discard", "--partition-table=auto|none|mbr|gpt",
        "--sector-size", "--cluster-size", "--boundary-align", "--volume-label"};
    for (const char* option : required)
    {
        if (!help.contains(option))
            return ENOTSUP;
    }
    problem.clear();
    return 0;
}

static int formatExfat(int fd, quint64 device, const QString& mkfs,
    const ExfatFormatOptions& options, FORMAT_RESULT& result, bool wholeDevice = false)
{
    result = {};
    result.exit_code = -1;
    int error = checkTarget(fd, device, mkfs, options, wholeDevice);
    if (error != 0)
        return error;
    QByteArray program = QFile::encodeName(mkfs);
    QByteArray label = options.label.toUtf8();
    QByteArray cluster = QByteArray::number(options.clusterSize);
    QByteArray sector = QByteArray::number(options.sectorSize);
    QByteArray alignment = QByteArray::number(alignmentBytes);
    QByteArray target = QByteArray("/proc/self/fd/") + QByteArray::number(FORMAT_TARGET_FD);
    // Keep partition creation with Rufus and preserve free data on quick format.
    // -C verifies native writes; -K disables discard; -f initializes the full volume.
    const char* arguments[] = {program.constData(), "-C", "-K", "-P", "none", "-s", sector.constData(),
        "-b", alignment.constData(), "-c", cluster.constData(), "-L", label.constData(), target.constData(), nullptr, nullptr};
    if (!options.quickFormat)
    {
        size_t end = 0; while (arguments[end] != nullptr) end++;
        memmove(arguments + 2, arguments + 1, end * sizeof(*arguments));
        arguments[1] = "-f";
    }
    error = options.quickFormat ? RunFormatCommand(fd, const_cast<char* const*>(arguments), &result)
        : RunFullFormatCommand(fd, const_cast<char* const*>(arguments), &result);
    if (result.diagnostics[0])
        uprintf("mkfs.exfat: %s", result.diagnostics);
    if (error == 0)
        error = checkTarget(fd, device, mkfs, options, wholeDevice);
    if (error == 0)
        error = CheckExfatVolume(fd, options);
    if (error == 0)
        result.filesystem_bytes = options.size;
    return error;
}

int FormatExfatImage(int fd, const QString& mkfs, const ExfatFormatOptions& options, FORMAT_RESULT& result)
{
    return formatExfat(fd, 0, mkfs, options, result);
}

int FormatExfatDevice(int fd, quint64 device, const QString& mkfs,
    const ExfatFormatOptions& options, FORMAT_RESULT& result, bool wholeDevice)
{
    if (device == 0)
    {
        result = {};
        result.exit_code = -1;
        return EINVAL;
    }
    return formatExfat(fd, device, mkfs, options, result, wholeDevice);
}
}
