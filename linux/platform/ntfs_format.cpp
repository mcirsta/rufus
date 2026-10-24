// SPDX-License-Identifier: GPL-3.0-or-later
#include "ntfs_format.h"
#include "target_io.h"
#include "rufus_reporting.h"
#include "format_policy.h"
#include "syslinux/libinstaller/syslxint.h"
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QElapsedTimer>
#include <QtEndian>
#include <cerrno>
#include <cstring>
#include <unistd.h>

namespace rufus
{
namespace
{
constexpr quint32 bootHeaderBytes = 512;
constexpr quint32 heads = 255;
constexpr quint32 sectorsPerTrack = 63;
constexpr quint64 minimumVolumeBytes = 16 * 1024 * 1024;
constexpr quint16 bootSignature = 0xaa55;
static_assert(sizeof(ntfs_boot_sector) == bootHeaderBytes);

int fullFormatOption(const QString& mkfs, QByteArray& option)
{
    // ntfsprogs-plus defaults to quick and offers --full; NTFS-3G defaults
    // to full and needs the quick switch removed. Inspect capabilities rather
    // than assuming that two programs named mkntfs share the same default.
    constexpr int helpBytes = 16384, timeoutMs = 3000;
    QProcess process;
    QProcessEnvironment environment;
    environment.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    process.setProcessEnvironment(environment);
    process.setProcessChannelMode(QProcess::MergedChannels);
    QElapsedTimer timer; timer.start();
    process.start(mkfs, {QStringLiteral("--help")});
    QByteArray help;
    int error = 0;
    while (process.state() != QProcess::NotRunning)
    {
        process.waitForReadyRead(50);
        help += process.read(helpBytes + 1 - help.size());
        if (IsRufusJobCancelled()) error = ECANCELED;
        else if (timer.elapsed() >= timeoutMs) error = ETIMEDOUT;
        else if (help.size() > helpBytes) error = EOVERFLOW;
        if (error != 0) { process.kill(); process.waitForFinished(); return error; }
    }
    if (process.error() == QProcess::FailedToStart) return EIO;
    const bool normalHelp = process.exitStatus() == QProcess::NormalExit && process.exitCode() <= 1;
    if (!normalHelp || !help.contains("--quick")) return ENOTSUP;
    if (help.contains("--full")) option = "--full";
    else
    {
        const qsizetype start = help.indexOf("--quick");
        const qsizetype end = help.indexOf('\n', start);
        if (help.mid(start, end < 0 ? -1 : end - start).contains("default")) return ENOTSUP;
        option.clear();
    }
    return 0;
}

int readSector(int fd, quint64 offset, void* output)
{
    ssize_t count;
    do {
        count = pread(fd, output, bootHeaderBytes, off_t(offset));
    } while (count < 0 && errno == EINTR);
    if (count < 0)
        return errno;
    return count == bootHeaderBytes ? 0 : EIO;
}

int checkBoot(int fd, const NtfsFormatOptions& options, NtfsFormatResult& result)
{
    const quint32 sectorBytes = options.sectorSize;
    unsigned char primary[4096], backup[4096];
    ssize_t count;
    do { count = pread(fd, primary, sectorBytes, 0); } while (count < 0 && errno == EINTR);
    if (count < 0) return errno;
    if (count != sectorBytes) return EIO;
    ntfs_boot_sector boot;
    memcpy(&boot, primary, sizeof(boot));
    const bool ntfsSignature = memcmp(boot.bsOemName, "NTFS    ", sizeof(boot.bsOemName)) == 0;
    const bool bootSignatureMatches = qFromLittleEndian(boot.bsSignature) == bootSignature;
    const bool sectorMatches = qFromLittleEndian(boot.bsBytesPerSec) == sectorBytes;
    const bool clusterMatches = boot.bsSecPerClust * sectorBytes == options.clusterSize;
    if (!ntfsSignature || !bootSignatureMatches || !ntfs_check_zero_fields(&boot))
        return EBADMSG;
    if (!sectorMatches || !clusterMatches)
        return EBADMSG;
    // These fields are named "Unused" in the bundled Syslinux NTFS structure.
    const bool trackMatches = qFromLittleEndian(boot.bsUnused_0) == sectorsPerTrack;
    const bool headsMatch = qFromLittleEndian(boot.bsUnused_1) == heads;
    const bool offsetMatches = qFromLittleEndian(boot.bsUnused_2) == options.partitionOffset / sectorBytes;
    const quint64 sectors = qFromLittleEndian(boot.bsTotalSectors);
    const bool sizeMatches = sectors == options.size / sectorBytes - 1; // Backup sector is outside this count.
    if (!trackMatches || !headsMatch || !offsetMatches || !sizeMatches)
        return EBADMSG;
    // Compare the complete logical boot sector, including bytes beyond the
    // fixed 512-byte NTFS BPB on a native 4-KiB volume.
    do { count = pread(fd, backup, sectorBytes, off_t(options.size - sectorBytes)); } while (count < 0 && errno == EINTR);
    if (count < 0) return errno;
    if (count != sectorBytes) return EIO;
    if (memcmp(primary, backup, sectorBytes) != 0) return EBADMSG;

    const quint64 mft = qFromLittleEndian(boot.bsMFTLogicalClustNr);
    const quint64 mirror = qFromLittleEndian(boot.bsMFTMirrLogicalClustNr);
    const quint64 clusters = (options.size - sectorBytes) / options.clusterSize;
    const bool mftFits = mft != 0 && mft < clusters;
    const bool mirrorFits = mirror != 0 && mirror < clusters && mirror != mft;
    if (!mftFits || !mirrorFits)
        return EBADMSG;
    char first[bootHeaderBytes], second[bootHeaderBytes];
    int error = readSector(fd, mft * options.clusterSize, first);
    if (error == 0)
        error = readSector(fd, mirror * options.clusterSize, second);
    if (error != 0)
        return error;
    const bool mftHeader = memcmp(first, "FILE", 4) == 0;
    const bool mirrorMatches = memcmp(first, second, sizeof(first)) == 0;
    if (!mftHeader || !mirrorMatches)
        return EBADMSG;
    result.volumeId = qFromLittleEndian(boot.bsVolSerialNr);
    result.format.filesystem_bytes = options.size;
    return 0;
}
}

int CheckNtfsFormatOptions(const NtfsFormatOptions& options)
{
    const quint32 sectorBytes = options.sectorSize;
    const bool validSector = sectorBytes >= 512 && sectorBytes <= 4096 && (sectorBytes & (sectorBytes - 1)) == 0;
    const bool validPhysical = options.physicalSectorSize >= sectorBytes && options.physicalSectorSize <= 65536 &&
        (options.physicalSectorSize & (options.physicalSectorSize - 1)) == 0;
    if (!validSector || !validPhysical) return EINVAL;
    const bool sectorAligned = options.size % sectorBytes == 0 && options.partitionOffset % sectorBytes == 0;
    const bool supportedSize = options.size >= minimumVolumeBytes && options.size <= INT64_MAX;
    const bool offsetFits = options.partitionOffset / sectorBytes <= UINT32_MAX;
    if (!sectorAligned || !supportedSize || !offsetFits)
        return EINVAL;
    const bool singleCluster = options.clusterSize != 0 && (options.clusterSize & (options.clusterSize - 1)) == 0;
    const RUFUS_CLUSTER_SIZES sizes = GetNtfsClusterSizes(options.size, sectorBytes);
    if (!singleCluster || (sizes.Allowed & options.clusterSize) == 0)
        return EINVAL;
    const bool clusterCountFits = (options.size - sectorBytes) / options.clusterSize <= UINT32_MAX;
    if (!clusterCountFits)
        return ENOTSUP;
    const bool labelLengthFits = options.label.size() < VOLUME_LABEL_CAPACITY;
    const bool labelTerminated = !options.label.contains(QChar(u'\0'));
    const bool validUtf16 = QString::fromUtf8(options.label.toUtf8()) == options.label;
    if (!labelLengthFits || !labelTerminated || !validUtf16)
        return EINVAL;
    return 0;
}

static int checkTarget(int fd, quint64 device, const QString& mkfs, const NtfsFormatOptions& options)
{
    int error = CheckNtfsFormatOptions(options);
    if (error != 0)
        return error;
    if (!mkfs.startsWith(QChar(u'/')) || mkfs.contains(QChar(u'\0')))
        return EINVAL;
    WRITABLE_TARGET target;
    error = InspectWritableTarget(fd, device, &target);
    if (error != 0)
        return error;
    if (device != 0 && (target.sector_size != options.sectorSize || target.physical_sector_size != options.physicalSectorSize))
        return ENOTSUP;
    if (target.size != options.size)
        return ESTALE;
    return IsRufusJobCancelled() ? ECANCELED : 0;
}

static int formatNtfs(int fd, quint64 device, const QString& mkfs,
    const NtfsFormatOptions& options, NtfsFormatResult& result, bool wholeDevice = false)
{
    result = {};
    result.format.exit_code = -1;
    if (wholeDevice && options.partitionOffset != 0)
        return EINVAL;
    int error = checkTarget(fd, device, mkfs, options);
    if (error != 0)
        return error;
    QByteArray program = QFile::encodeName(mkfs);
    QByteArray label = options.label.toUtf8();
    QByteArray cluster = QByteArray::number(options.clusterSize);
    QByteArray sector = QByteArray::number(options.sectorSize);
    QByteArray start = QByteArray::number(options.partitionOffset / options.sectorSize);
    QByteArray headCount = QByteArray::number(heads);
    QByteArray tracks = QByteArray::number(sectorsPerTrack);
    QByteArray size = QByteArray::number(options.size / options.sectorSize);
    QByteArray target = QByteArray("/proc/self/fd/") + QByteArray::number(FORMAT_TARGET_FD);
    QByteArray fullOption;
    if (!options.quickFormat)
    {
        error = fullFormatOption(mkfs, fullOption);
        if (error != 0) return error;
    }
    // The trusted native formatter chooses its own volume serial number.
    const char* arguments[] = {program.constData(), "-Q", "-s", sector.constData(),
        "-c", cluster.constData(), "-p", start.constData(), "-H", headCount.constData(),
        "-S", tracks.constData(), "-L", label.constData(), target.constData(), size.constData(), nullptr, nullptr, nullptr, nullptr};
    if (device == 0 || wholeDevice)
    {
        // mkntfs requires force for files and whole disks. The device caller
        // validates the SFD extent and retains its exclusive claim throughout.
        const size_t shiftedArguments = sizeof(arguments) / sizeof(arguments[0]) - 2;
        memmove(arguments + 2, arguments + 1, shiftedArguments * sizeof(*arguments));
        arguments[1] = "-F";
    }
    if (!options.quickFormat)
    {
        size_t index = 1;
        while (arguments[index] != nullptr && strcmp(arguments[index], "-Q") != 0) index++;
        if (arguments[index] != nullptr)
        {
            if (!fullOption.isEmpty()) arguments[index] = fullOption.constData();
            else
            {
                size_t end = index; while (arguments[end] != nullptr) end++;
                memmove(arguments + index, arguments + index + 1, (end - index) * sizeof(*arguments));
            }
        }
    }
    size_t last = 0; while (arguments[last] != nullptr) last++;
    if (!options.indexing) arguments[last++] = "-I";
    const bool compression = options.compression && options.clusterSize <= NtfsCompressionClusterBytes;
    if (compression) arguments[last++] = "-C";
    else if (options.compression) uprintf("NTFS compression requires clusters no larger than 4096 bytes; using an uncompressed volume.");
    error = options.quickFormat ? RunFormatCommand(fd, const_cast<char* const*>(arguments), &result.format)
        : RunFullFormatCommand(fd, const_cast<char* const*>(arguments), &result.format);
    if (result.format.diagnostics[0])
        uprintf("mkntfs: %s", result.format.diagnostics);
    if (error == 0)
        error = checkTarget(fd, device, mkfs, options);
    if (error == 0)
        error = checkBoot(fd, options, result);
    return error;
}

int CheckNtfsImageRequest(int fd, const QString& mkfs, const NtfsFormatOptions& options)
{
    return checkTarget(fd, 0, mkfs, options);
}

int CheckNtfsFullFormatTools(const QString& mkfs)
{
    if (!mkfs.startsWith('/') || mkfs.contains(QChar::Null)) return EINVAL;
    if (access(QFile::encodeName(mkfs).constData(), X_OK) != 0) return errno;
    QByteArray option;
    return fullFormatOption(mkfs, option);
}

int FormatNtfsImage(int fd, const QString& mkfs, const NtfsFormatOptions& options, NtfsFormatResult& result)
{
    return formatNtfs(fd, 0, mkfs, options, result);
}

int CheckNtfsFormattedVolume(int fd, quint64 device, const NtfsFormatOptions& options)
{
    int error = CheckNtfsFormatOptions(options);
    if (error != 0)
        return error;
    WRITABLE_TARGET target;
    error = InspectWritableTarget(fd, device, &target);
    if (error != 0)
        return error;
    if (device != 0 && (target.sector_size != options.sectorSize || target.physical_sector_size != options.physicalSectorSize))
        return ENOTSUP;
    if (target.size != options.size)
        return ESTALE;
    NtfsFormatResult formatted;
    error = checkBoot(fd, options, formatted);
    if (error != 0)
        return error;
    return IsRufusJobCancelled() ? ECANCELED : 0;
}

int FormatNtfsDevice(int fd, quint64 device, const QString& mkfs,
    const NtfsFormatOptions& options, NtfsFormatResult& result, bool wholeDevice)
{
    if (device == 0)
    {
        result = {};
        result.format.exit_code = -1;
        return EINVAL;
    }
    return formatNtfs(fd, device, mkfs, options, result, wholeDevice);
}

}
