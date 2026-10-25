// SPDX-License-Identifier: GPL-3.0-or-later
#include "raw_write_plan.h"
#include "prepare_source.h"
#include "native_disk_facts.h"

#include <QFile>
#include <QFileInfo>

#include <cerrno>
#include <cstring>

namespace rufus
{
namespace
{

constexpr qint64 rawSectorBytes = 512;

QString systemError(const char* operation, int error)
{
    return QStringLiteral("%1: %2").arg(QLatin1String(operation), QString::fromLocal8Bit(strerror(error)));
}

} // namespace

QString CheckRawImageSource(const IMAGE_FILE_SOURCE& source, const QString& path, bool ignoreBootMarker)
{
    if (GetImageCompressionType(QFile::encodeName(path).constData()) != BLED_COMPRESSION_NONE)
        return QStringLiteral("Select this compressed or virtual-disk image for container analysis before DD writing.");
    const bool completeSectors = source.info.st_size >= rawSectorBytes && source.info.st_size % rawSectorBytes == 0;
    const bool supportedInput = !source.is_wim && completeSectors;
    if (!supportedInput)
        return QStringLiteral("Choose an uncompressed disk image containing complete 512-byte sectors.");
    if (!source.has_boot_marker && !ignoreBootMarker)
        return QStringLiteral("The raw image has no Rufus boot marker (55 AA).");
    return {};
}

QString CheckRawImageFile(const QString& path, bool ignoreBootMarker)
{
    IMAGE_FILE_SOURCE source = {-1, {}, false, false, nullptr, 0};
    const QByteArray name = QFile::encodeName(path);
    int error = OpenImageFileSource(name.constData(), &source);
    if (error != 0)
        return systemError("Cannot open the raw image", error);
    QString problem = CheckRawImageSource(source, path, ignoreBootMarker);
    if (problem.isEmpty())
    {
        error = CheckImageFileSource(&source, name.constData());
        if (error != 0)
            problem = systemError("The raw image changed", error);
    }
    CloseImageFileSource(&source);
    return problem;
}

RawWritePlan::~RawWritePlan()
{
    CloseImageFileSource(&source_);
}

QString RawWritePlan::prepare(const QString& path, const Device& target, const PrepareImageRequest* expected, bool allowFixedUsb)
{
    CloseImageFileSource(&source_);
    path_.clear();
    target_ = {};
    compressed_ = {};
    if (expected != nullptr)
    {
        if (expected->badBlocksPattern < -1 || expected->badBlocksPattern >= BADLOCKS_PATTERN_TYPES)
            return QStringLiteral("Invalid bad-block test pattern.");
        compressed_.badBlocksPattern = expected->badBlocksPattern;
        compressed_.detectFakes = expected->detectFakes;
    }
    writeBytes_ = diskSequence_ = 0;
    allowFixedUsb_ = allowFixedUsb;
    compressed_.deviceClass = target.deviceClass;
    zeroSectorSize_ = 0;
    const bool selectedClass = target.defaultVisible || allowFixedUsb_;
    const bool writableMedia = selectedClass && !target.readOnly;
    const bool identified = target.number != 0 && !target.identity.isEmpty();
    const bool nativeDiscovery = target.serviceOwner.startsWith(QLatin1Char(':'))
        && target.objectPath.startsWith(QStringLiteral("/org/freedesktop/UDisks2/block_devices/"));
    if (!writableMedia || !identified || !nativeDiscovery)
        return QStringLiteral("Select a device from native UDisks discovery.");
    if (expected != nullptr && expected->zeroMode != ZERO_NONE)
    {
        const bool validMode = expected->zeroMode == ZERO_FULL || expected->zeroMode == ZERO_FAST;
        if (!validMode || !path.isEmpty()) return QStringLiteral("Invalid device clearing plan.");
        NativeDiskFacts facts;
        const int geometry = ReadNativeDiskFacts(target.number, target.size, facts);
        const unsigned sector = facts.logicalSectorSize;
        const bool supportedSector = sector >= 512 && sector <= 65536 && (sector & (sector - 1)) == 0;
        if (geometry != 0 || !supportedSector || target.size % sector != 0 || facts.diskSequence == 0)
            return QStringLiteral("Cannot freeze the selected device geometry for clearing.");
        compressed_.zeroMode = expected->zeroMode;
        zeroSectorSize_ = sector;
        writeBytes_ = target.size;
        diskSequence_ = facts.diskSequence;
        target_ = target;
        return {};
    }
    const QString absolute = QFileInfo(path).absoluteFilePath();
    const int error = OpenImageFileSource(QFile::encodeName(absolute).constData(), &source_);
    if (error != 0)
        return systemError("Cannot retain the source image", error);
    const bool compressed = expected != nullptr && expected->hasCompressedImage();
    QString problem = compressed ? QString() : CheckRawImageSource(source_, absolute, expected != nullptr && expected->ignoreBootMarker);
    if (problem.isEmpty() && expected != nullptr)
    {
        const bool sameSource = expected->imagePath == absolute && CheckPrepareSourceIdentity(*expected, source_) == 0;
        if (!sameSource)
            problem = QStringLiteral("The analyzed image changed. Select it again before choosing DD mode.");
    }
    if (problem.isEmpty() && compressed)
    {
        NativeDiskFacts facts;
        const int geometryError = ReadNativeDiskFacts(target.number, target.size, facts);
        if (geometryError != 0 || facts.logicalSectorSize == 0)
            problem = QStringLiteral("Cannot read the selected target's logical sector size. Select the device again.");
        else
        {
            PrepareImageRequest checked = *expected;
            checked.rawWrite = checked.isDevice = true;
            checked.size = target.size;
            checked.imageSectorSize = facts.logicalSectorSize;
            if (!ValidatePrepareCompressedImage(checked))
                problem = QStringLiteral("The decoded image or its final sector does not fit the selected target.");
            else
            {
                compressed_.imageCompression = checked.imageCompression;
                compressed_.decodedImageSize = checked.decodedImageSize;
                compressed_.imageSectorSize = checked.imageSectorSize;
                compressed_.decodedImageSha256 = checked.decodedImageSha256;
                compressed_.imageLayoutSha256 = checked.imageLayoutSha256;
                compressed_.ffuStore = checked.ffuStore;
                compressed_.ffuMinimumSize = checked.ffuMinimumSize;
                source_.ffu_store = checked.ffuStore;
                compressed_.ignoreBootMarker = checked.ignoreBootMarker;
                writeBytes_ = PrepareRawWriteBytes(checked);
                diskSequence_ = facts.diskSequence;
            }
        }
    }
    if (problem.isEmpty() && !compressed && quint64(source_.info.st_size) > target.size)
        problem = QStringLiteral("The image is larger than the selected target.");
    if (problem.isEmpty())
    {
        path_ = absolute;
        target_ = target;
        if (!compressed)
        {
            writeBytes_ = quint64(source_.info.st_size);
            compressed_.ignoreBootMarker = expected != nullptr && expected->ignoreBootMarker;
        }
        problem = checkSource();
    }
    if (!problem.isEmpty())
    {
        CloseImageFileSource(&source_);
        path_.clear();
        target_ = {};
        compressed_ = {};
        writeBytes_ = diskSequence_ = 0;
    }
    return problem;
}

QString RawWritePlan::checkSource() const
{
    if (compressed_.zeroMode != ZERO_NONE) return {};
    const int error = CheckImageFileSource(&source_, QFile::encodeName(path_).constData());
    return error == 0 ? QString() : systemError("The selected source is no longer valid", error);
}

QString RawWritePlan::checkSnapshot(const UDisksObjects& objects, const QString& owner, QStringList* mounts, int* partitionCount) const
{
    QString problem = checkSource();
    if (!problem.isEmpty())
        return problem;
    Device current;
    const bool targetFound = owner == target_.serviceOwner
        && DescribeUDisksDevice(objects, target_.objectPath, current, allowFixedUsb_, DeviceAccess::Write, DeviceClassMask(target_.deviceClass));
    const bool sameIdentity = targetFound && current.identity == target_.identity && current.number == target_.number;
    const bool sameMedia = targetFound && current.size == target_.size && current.path == target_.path;
    if (!sameIdentity || !sameMedia)
        return QStringLiteral("The selected target or UDisks service changed. Prepare a new write plan.");
    if (compressed_.hasCompressedImage() || compressed_.zeroMode != ZERO_NONE)
    {
        NativeDiskFacts facts;
        const int error = ReadNativeDiskFacts(target_.number, target_.size, facts);
        const bool sameGeometry = error == 0 && facts.diskSequence == diskSequence_
            && facts.logicalSectorSize == (compressed_.zeroMode != ZERO_NONE ? zeroSectorSize_ : compressed_.imageSectorSize);
        if (!sameGeometry)
            return QStringLiteral("The target geometry changed. Prepare a new write plan.");
    }
    if (partitionCount) *partitionCount = current.partitionCount;
    if (mounts)
        return ListUDisksTargetMounts(objects, target_, quint64(source_.info.st_dev), *mounts, allowFixedUsb_);
    return CheckUDisksTargetUse(objects, target_, quint64(source_.info.st_dev), allowFixedUsb_);
}

} // namespace rufus
