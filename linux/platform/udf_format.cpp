// SPDX-License-Identifier: GPL-3.0-or-later
#include "udf_format.h"
#include "target_io.h"
#include "zero_write.h"
#include "rufus_reporting.h"
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryFile>
#include <QtEndian>
#include <array>
#include <cerrno>
#include <cstring>
#include <unistd.h>

namespace rufus
{
namespace
{
constexpr quint32 maximumBlockBytes = 4096;
constexpr quint32 maximumPhysicalSectorBytes = 65536;
constexpr quint64 minimumVolumeBytes = 1024 * 1024;
constexpr quint32 anchorBlock = 256;
constexpr quint32 minimumVolumeBlocks = anchorBlock + 2;
constexpr quint32 threeAnchorMinimumBlocks = 3072; // mkudffs random-access layout.
constexpr quint32 maximumSequenceBytes = 64 * 1024;
constexpr int maximumLabelUnits = 32; // Rufus's non-FAT label limit.
using Block = std::array<unsigned char, maximumBlockBytes>;

quint32 blockBytes(const UdfFormatOptions& options)
{
    return options.blockSize == 0 ? options.sectorSize : options.blockSize;
}

quint16 crc16(const unsigned char* data, size_t bytes)
{
    quint16 crc = 0;
    for (size_t index = 0; index < bytes; ++index) {
        crc ^= quint16(data[index]) << 8;
        for (unsigned bit = 0; bit < 8; ++bit) crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
    }
    return crc;
}

int readAt(int fd, void* data, size_t size, quint64 offset)
{
    size_t done = 0;
    while (done < size) {
        const ssize_t count = pread(fd, static_cast<char*>(data) + done, size - done, off_t(offset + done));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return count < 0 ? errno : EIO;
        done += size_t(count);
    }
    return 0;
}

bool validTag(const Block& block, quint32 blockSize, quint16 identifier, quint32 location)
{
    const quint16 version = qFromLittleEndian<quint16>(block.data() + 2);
    const quint16 length = qFromLittleEndian<quint16>(block.data() + 10);
    const unsigned minimumBody = identifier == 2 || identifier == 5 || identifier == 256 ? 496
        : identifier == 6 ? 430 : identifier == 9 ? 118 : identifier == 261 ? 160 : identifier == 266 ? 200 : 0;
    const bool validHeader = qFromLittleEndian<quint16>(block.data()) == identifier
        && (version == 2 || version == 3) && length >= minimumBody && length <= blockSize - 16
        && qFromLittleEndian<quint32>(block.data() + 12) == location;
    if (!validHeader) return false;
    unsigned char checksum = 0;
    for (size_t index = 0; index < 16; ++index) if (index != 4) checksum += block[index];
    return checksum == block[4] && crc16(block.data() + 16, length) == qFromLittleEndian<quint16>(block.data() + 8);
}

bool matchingLabel(const unsigned char* label, const QString& expected)
{
    // ECMA-167 dstring: CS0 compressed Unicode followed by a length byte.
    const unsigned length = label[127];
    if (length == 0) return expected.isEmpty();
    if (length < 1 || length > 127) return false;
    QString decoded;
    if (label[0] == 8) {
        for (unsigned index = 1; index < length; ++index) decoded += QChar(label[index]);
    } else if (label[0] == 16 && length % 2 == 1) {
        for (unsigned index = 1; index + 1 < length; index += 2)
            decoded += QChar(qFromBigEndian<quint16>(label + index));
    } else return false;
    return decoded == expected;
}

struct VolumeLayout
{
    quint32 partitionStart = 0, partitionBlocks = 0;
    quint32 fileSet = 0, integrity = 0;
};

int checkSequence(int fd, quint32 start, quint32 length, quint32 blocks,
    const UdfFormatOptions& options, VolumeLayout& layout)
{
    const quint32 blockSize = blockBytes(options);
    const bool bounded = length >= blockSize && length <= maximumSequenceBytes && length % blockSize == 0;
    if (!bounded || start >= blocks || length / blockSize > blocks - start) return EBADMSG;
    bool partitionSeen = false, logicalSeen = false;
    quint16 partitionNumber = 0, mappedPartition = 0;
    for (quint32 index = 0; index < length / blockSize; ++index) {
        Block descriptor;
        int error = readAt(fd, descriptor.data(), blockSize, quint64(start + index) * blockSize);
        if (error != 0) return error;
        const quint16 tag = qFromLittleEndian<quint16>(descriptor.data());
        if (!validTag(descriptor, blockSize, tag, start + index)) return EBADMSG;
        if (tag == 8) return partitionSeen && logicalSeen && partitionNumber == mappedPartition ? 0 : EBADMSG;
        if (tag == 5) {
            if (partitionSeen) return ENOTSUP;
            partitionSeen = true;
            partitionNumber = qFromLittleEndian<quint16>(descriptor.data() + 22);
            layout.partitionStart = qFromLittleEndian<quint32>(descriptor.data() + 188);
            layout.partitionBlocks = qFromLittleEndian<quint32>(descriptor.data() + 192);
            const bool physicalExtent = layout.partitionStart < blocks && layout.partitionBlocks > 0
                && layout.partitionBlocks <= blocks - layout.partitionStart;
            if (!physicalExtent || qFromLittleEndian<quint32>(descriptor.data() + 184) != 4) return EBADMSG;
        } else if (tag == 6) {
            if (logicalSeen) return ENOTSUP;
            logicalSeen = true;
            const bool nativeLayout = qFromLittleEndian<quint32>(descriptor.data() + 212) == blockSize
                && qFromLittleEndian<quint32>(descriptor.data() + 264) == 6
                && qFromLittleEndian<quint32>(descriptor.data() + 268) == 1
                && descriptor[440] == 1 && descriptor[441] == 6;
            if (!nativeLayout) return ENOTSUP;
            mappedPartition = qFromLittleEndian<quint16>(descriptor.data() + 444);
            if (qFromLittleEndian<quint16>(descriptor.data() + 240) != options.revision
                || !matchingLabel(descriptor.data() + 84, options.label)) return EBADMSG;
            if (qFromLittleEndian<quint16>(descriptor.data() + 256) != 0) return ENOTSUP;
            layout.fileSet = qFromLittleEndian<quint32>(descriptor.data() + 252);
            layout.integrity = qFromLittleEndian<quint32>(descriptor.data() + 436);
        }
    }
    return EBADMSG;
}

QStringList arguments(const UdfFormatOptions& options)
{
    return {QStringLiteral("--utf8"), QStringLiteral("--blocksize=%1").arg(blockBytes(options)), QStringLiteral("--media-type=hd"),
        QStringLiteral("--udfrev=0x%1").arg(options.revision, 4, 16, QLatin1Char('0')),
        QStringLiteral("--label=%1").arg(options.label), QStringLiteral("--uid=-1"), QStringLiteral("--gid=-1"),
        QStringLiteral("--mode=0777"), QStringLiteral("--bootarea=erase")};
}

int checkTarget(int fd, quint64 device, const QString& mkfs, const UdfFormatOptions& options)
{
    int error = CheckUdfFormatOptions(options);
    if (error != 0) return error;
    const bool validProgram = mkfs.startsWith('/') && !mkfs.contains(QChar::Null);
    if (!validProgram) return EINVAL;
    WRITABLE_TARGET target;
    error = InspectWritableTarget(fd, device, &target);
    if (error != 0) return error;
    const bool sameGeometry = target.sector_size == options.sectorSize
        && target.physical_sector_size == options.physicalSectorSize;
    if (device != 0 && !sameGeometry) return ESTALE;
    if (target.size != options.size) return ESTALE;
    return IsRufusJobCancelled() ? ECANCELED : 0;
}
}

int CheckUdfFormatOptions(const UdfFormatOptions& options)
{
    const bool supportedRevision = options.revision == 0x0102 || options.revision == 0x0150
        || options.revision == 0x0200 || options.revision == 0x0201;
    if (!supportedRevision) return ENOTSUP;
    const bool supportedSector = options.sectorSize == 512 || options.sectorSize == 1024
        || options.sectorSize == 2048 || options.sectorSize == 4096;
    const bool physicalGeometry = options.physicalSectorSize >= options.sectorSize
        && options.physicalSectorSize <= maximumPhysicalSectorBytes
        && (options.physicalSectorSize & (options.physicalSectorSize - 1)) == 0;
    if (!supportedSector || !physicalGeometry) return EINVAL;
    const quint32 blockSize = blockBytes(options);
    // The filesystem logical block and device logical sector have distinct
    // roles, but this provider's portable writable media require equal sizes.
    if (blockSize != options.sectorSize) return ENOTSUP;
    const bool supportedSize = options.size >= minimumVolumeBytes && options.size % blockSize == 0
        && options.size / blockSize >= minimumVolumeBlocks && options.size / blockSize <= UINT32_MAX;
    const bool validLabel = options.label.size() <= maximumLabelUnits && !options.label.contains(QChar::Null)
        && QString::fromUtf8(options.label.toUtf8()) == options.label;
    return supportedSize && validLabel ? 0 : EINVAL;
}

int CheckUdfFormatTools(const QString& mkfs, const UdfFormatOptions& options, QString& problem)
{
    problem = QStringLiteral("UDF needs udftools mkudffs with UTF-8 labels and random-access media support.");
    int error = CheckUdfFormatOptions(options);
    if (error != 0) return error;
    if (!mkfs.startsWith('/') || mkfs.contains(QChar::Null)) return EINVAL;
    if (access(QFile::encodeName(mkfs).constData(), X_OK) != 0) return errno;
    // Exercise the actual revision/label/options before the real target is
    // touched. A private tiny dry-run file avoids opening any host device.
    QTemporaryFile probe(getuid() == 0 ? QStringLiteral("/run/rufus-udf-probe-XXXXXX")
        : QStringLiteral("/tmp/rufus-udf-probe-XXXXXX"));
    const quint64 probeBytes = qMax(minimumVolumeBytes, quint64(minimumVolumeBlocks) * blockBytes(options));
    if (!probe.open() || !probe.resize(probeBytes)) return EIO;
    QStringList selected = arguments(options);
    selected << QStringLiteral("--no-write") << probe.fileName();
    QProcess process;
    QProcessEnvironment environment;
    environment.insert(QStringLiteral("LC_ALL"), QStringLiteral("C.UTF-8"));
    process.setProcessEnvironment(environment);
    process.setProcessChannelMode(QProcess::MergedChannels);
    QElapsedTimer timer; timer.start();
    process.start(mkfs, selected);
    constexpr int maximumOutputBytes = 16 * 1024;
    constexpr int probePollMs = 50, probeTimeoutMs = 3000;
    QByteArray output;
    while (process.state() != QProcess::NotRunning) {
        process.waitForReadyRead(probePollMs);
        output += process.read(maximumOutputBytes + 1 - output.size());
        if (IsRufusJobCancelled()) error = ECANCELED;
        else if (timer.elapsed() >= probeTimeoutMs) error = ETIMEDOUT;
        else if (output.size() > maximumOutputBytes) error = EOVERFLOW;
        if (error != 0) { process.kill(); process.waitForFinished(); return error; }
    }
    output += process.read(maximumOutputBytes + 1 - output.size());
    if (process.error() == QProcess::FailedToStart) return EIO;
    const bool accepted = process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0
        && output.size() <= maximumOutputBytes && output.contains("Note: Not writing to device");
    if (!accepted) { problem += QLatin1Char('\n') + QString::fromUtf8(output); return ENOTSUP; }
    problem.clear();
    return 0;
}

int CheckUdfVolume(int fd, const UdfFormatOptions& options)
{
    int error = CheckUdfFormatOptions(options);
    if (error != 0) return error;
    const quint32 blockSize = blockBytes(options);
    const quint32 blocks = quint32(options.size / blockSize);
    Block anchor;
    error = readAt(fd, anchor.data(), blockSize, quint64(anchorBlock) * blockSize);
    if (error != 0) return error;
    if (!validTag(anchor, blockSize, 2, anchorBlock)) return EBADMSG;
    for (quint32 location : {blocks - 1, blocks - 1 - anchorBlock}) {
        // The provider deliberately uses only the first and final anchors on
        // volumes below 3072 logical blocks. Never probe that absent anchor.
        if (location != blocks - 1 && blocks < threeAnchorMinimumBlocks) continue;
        Block backup;
        error = readAt(fd, backup.data(), blockSize, quint64(location) * blockSize);
        if (error != 0) return error;
        if (!validTag(backup, blockSize, 2, location) || memcmp(anchor.data() + 16, backup.data() + 16, blockSize - 16) != 0)
            return EBADMSG;
    }
    const char* nsr = options.revision < 0x0200 ? "NSR02" : "NSR03";
    const quint32 recognitionStride = qMax(quint32(2048), blockSize);
    const char* identifiers[] = {"BEA01", nsr, "TEA01"};
    for (quint32 index = 0; index < 3; ++index) {
        std::array<unsigned char, 2048> recognition;
        error = readAt(fd, recognition.data(), recognition.size(), 32768 + quint64(index) * recognitionStride);
        if (error != 0) return error;
        if (memcmp(recognition.data() + 1, identifiers[index], 5) != 0) return EBADMSG;
    }
    VolumeLayout main, backup;
    error = checkSequence(fd, qFromLittleEndian<quint32>(anchor.data() + 20),
        qFromLittleEndian<quint32>(anchor.data() + 16), blocks, options, main);
    if (error == 0) error = checkSequence(fd, qFromLittleEndian<quint32>(anchor.data() + 28),
        qFromLittleEndian<quint32>(anchor.data() + 24), blocks, options, backup);
    if (error != 0) return error;
    const bool sameLayout = main.partitionStart == backup.partitionStart && main.partitionBlocks == backup.partitionBlocks
        && main.fileSet == backup.fileSet && main.integrity == backup.integrity;
    if (!sameLayout || main.fileSet >= main.partitionBlocks || main.integrity >= blocks) return EBADMSG;
    Block integrity;
    error = readAt(fd, integrity.data(), blockSize, quint64(main.integrity) * blockSize);
    if (error != 0) return error;
    const bool closed = validTag(integrity, blockSize, 9, main.integrity) && qFromLittleEndian<quint32>(integrity.data() + 28) == 1
        && qFromLittleEndian<quint32>(integrity.data() + 72) == 1
        && qFromLittleEndian<quint32>(integrity.data() + 84) == main.partitionBlocks;
    if (!closed) return EBADMSG;
    Block fileSet;
    error = readAt(fd, fileSet.data(), blockSize, quint64(main.partitionStart + main.fileSet) * blockSize);
    if (error != 0) return error;
    if (!validTag(fileSet, blockSize, 256, main.fileSet) || qFromLittleEndian<quint16>(fileSet.data() + 408) != 0) return EBADMSG;
    const quint32 root = qFromLittleEndian<quint32>(fileSet.data() + 404);
    if (root >= main.partitionBlocks) return EBADMSG;
    Block entry;
    error = readAt(fd, entry.data(), blockSize, quint64(main.partitionStart + root) * blockSize);
    if (error != 0) return error;
    const quint16 type = qFromLittleEndian<quint16>(entry.data());
    return (type == 261 || type == 266) && validTag(entry, blockSize, type, root) && entry[27] == 4 ? 0 : EBADMSG;
}

static int formatUdf(int fd, quint64 device, const QString& mkfs, const UdfFormatOptions& options, FORMAT_RESULT& result)
{
    result = {}; result.exit_code = -1;
    int error = checkTarget(fd, device, mkfs, options);
    QString problem;
    if (error == 0) error = CheckUdfFormatTools(mkfs, options, problem);
    if (error != 0) return error;
    bool cleared = false;
    if (!options.quickFormat) {
        RAW_WRITE_RESULT zero = {};
        error = ZeroFormatExtent(fd, device, 0, options.size, &zero);
        cleared = zero.bytes_written != 0;
        result.started = cleared; result.sync_error = zero.sync_error;
        if (error != 0) return error;
    }
    error = checkTarget(fd, device, mkfs, options);
    if (error != 0) return error;
    const QStringList selected = QStringList{mkfs} + arguments(options)
        + QStringList{QStringLiteral("/proc/self/fd/%1").arg(FORMAT_TARGET_FD)};
    QList<QByteArray> encoded;
    for (const QString& argument : selected) encoded.append(argument.toUtf8());
    QList<char*> argv;
    for (QByteArray& argument : encoded) argv.append(argument.data());
    argv.append(nullptr);
    error = RunFormatCommand(fd, argv.data(), &result);
    result.started = result.started || cleared;
    if (result.diagnostics[0]) uprintf("mkudffs: %s", result.diagnostics);
    if (error == 0) error = checkTarget(fd, device, mkfs, options);
    if (error == 0) error = CheckUdfVolume(fd, options);
    if (error == 0) result.filesystem_bytes = options.size;
    return error;
}

int FormatUdfImage(int fd, const QString& mkfs, const UdfFormatOptions& options, FORMAT_RESULT& result)
{
    return formatUdf(fd, 0, mkfs, options, result);
}

int FormatUdfDevice(int fd, quint64 device, const QString& mkfs, const UdfFormatOptions& options, FORMAT_RESULT& result)
{
    if (device == 0) { result = {}; result.exit_code = -1; return EINVAL; }
    return formatUdf(fd, device, mkfs, options, result);
}
}
