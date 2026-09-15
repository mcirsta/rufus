// SPDX-License-Identifier: GPL-3.0-or-later
#include "raw_write_plan.h"
#include "raw_write.h"

#include <QFile>
#include <QFileInfo>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace rufus
{
namespace
{

constexpr qint64 rawSectorBytes = 512;

QString systemError(const char* operation, int error)
{
    return QStringLiteral("%1: %2").arg(QLatin1String(operation), QString::fromLocal8Bit(strerror(error)));
}

QString checkRawImage(const IMAGE_FILE_SOURCE& source, const QString& path)
{
    const bool rawExtension = QFileInfo(path).suffix().compare(QStringLiteral("img"), Qt::CaseInsensitive) == 0;
    const bool completeSectors = source.info.st_size >= rawSectorBytes && source.info.st_size % rawSectorBytes == 0;
    if (!rawExtension || source.is_wim || !completeSectors)
        return QStringLiteral("Choose an uncompressed .img file containing complete 512-byte sectors.");
    if (!source.has_boot_marker)
        return QStringLiteral("The raw image has no Rufus boot marker (55 AA).");
    return {};
}

} // namespace

QString CheckRawImageFile(const QString& path)
{
    IMAGE_FILE_SOURCE source = {-1, {}, false, false};
    const QByteArray name = QFile::encodeName(path);
    int error = OpenImageFileSource(name.constData(), &source);
    if (error != 0)
        return systemError("Cannot open the raw image", error);
    QString problem = checkRawImage(source, path);
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

QString RawWritePlan::prepare(const QString& path, const Device& target)
{
    CloseImageFileSource(&source_);
    path_.clear();
    target_ = {};
    const bool writableMedia = target.removable && !target.readOnly;
    const bool identified = target.number != 0 && !target.identity.isEmpty();
    const bool nativeDiscovery = target.serviceOwner.startsWith(QLatin1Char(':'))
        && target.objectPath.startsWith(QStringLiteral("/org/freedesktop/UDisks2/block_devices/"));
    if (!writableMedia || !identified || !nativeDiscovery)
        return QStringLiteral("Select a device from native UDisks discovery.");
    const QString absolute = QFileInfo(path).absoluteFilePath();
    const int error = OpenImageFileSource(QFile::encodeName(absolute).constData(), &source_);
    if (error != 0)
        return systemError("Cannot retain the source image", error);
    QString problem = checkRawImage(source_, absolute);
    if (problem.isEmpty() && quint64(source_.info.st_size) > target.size)
        problem = QStringLiteral("The image is larger than the selected target.");
    if (problem.isEmpty())
    {
        path_ = absolute;
        target_ = target;
        problem = checkSource();
    }
    if (!problem.isEmpty())
    {
        CloseImageFileSource(&source_);
        path_.clear();
        target_ = {};
    }
    return problem;
}

QString RawWritePlan::checkSource() const
{
    const int error = CheckImageFileSource(&source_, QFile::encodeName(path_).constData());
    return error == 0 ? QString() : systemError("The selected source is no longer valid", error);
}

QString RawWritePlan::checkSnapshot(const UDisksObjects& objects, const QString& owner) const
{
    QString problem = checkSource();
    if (!problem.isEmpty())
        return problem;
    Device current;
    const bool targetFound = owner == target_.serviceOwner
        && DescribeUDisksDevice(objects, target_.objectPath, current);
    const bool sameIdentity = targetFound && current.identity == target_.identity && current.number == target_.number;
    const bool sameMedia = targetFound && current.size == target_.size && current.path == target_.path;
    if (!sameIdentity || !sameMedia)
        return QStringLiteral("The selected target or UDisks service changed. Prepare a new write plan.");
    return CheckUDisksTargetUse(objects, target_, quint64(source_.info.st_dev));
}

QString CheckRawTargetDescriptor(int fd, const RawWritePlan& plan)
{
    const int error = CheckRawBlockTarget(fd, plan.target().number, plan.target().size,
        quint64(plan.source().info.st_size));
    if (error != 0)
        return systemError("The authorized device failed validation", error);
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) != 0)
        return systemError("Cannot protect descriptor inheritance", errno);
    return plan.checkSource();
}

} // namespace rufus
