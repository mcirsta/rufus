// SPDX-License-Identifier: GPL-3.0-or-later
#include "grub2_cache.h"
#include "prepare_source.h"
#include "grub_inspect.h"
#include "image_source.h"
#include "rufus_reporting.h"
#include "../../res/grub2/grub2_version.h"
#include "../../res/grub/grub_version.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <utility>

static bool initializeGrub2Resources()
{
    Q_INIT_RESOURCE(grub2_resources);
    return true;
}

namespace rufus
{
namespace
{
constexpr quint64 sectorBytes = PrepareCoreSectorBytes;
constexpr quint64 maximumPartitionOffset = PrepareCoreMaximumBytes + sectorBytes;
constexpr qsizetype writeChunkBytes = 64 * 1024;

bool validVersionName(const QByteArray& version)
{
    // GetGrubVersion sanitizes these names. Validate again before using an
    // externally supplied report as a single directory component.
    for (unsigned char c : version)
    {
        const bool lowerCase = c >= 'a' && c <= 'z';
        const bool digit = c >= '0' && c <= '9';
        const bool separator = c == '.' || c == '-';
        if (!lowerCase && !digit && !separator)
            return false;
    }
    return !version.isEmpty();
}

int coreCachePath(const QString& filesDirectory, const RUFUS_IMG_REPORT& report,
    quint64 partitionOffset, QString& path)
{
    const QByteArray version(report.grub2_version);
    if (!validVersionName(version))
        return EINVAL;
    const bool absoluteDirectory = QDir::isAbsolutePath(filesDirectory) && !filesDirectory.contains(QChar(u'\0'));
    if (!absoluteDirectory)
        return EINVAL;
    const bool validOffset = partitionOffset > sectorBytes && partitionOffset <= maximumPartitionOffset
        && partitionOffset % sectorBytes == 0;
    if (!validOffset)
        return EINVAL;
    path = QDir(filesDirectory).filePath(QStringLiteral("grub-%1/core.img").arg(QString::fromLatin1(version)));
    return 0;
}

int cacheWriteError(QFileDevice::FileError error)
{
    if (error == QFileDevice::PermissionsError)
        return EACCES;
    if (error == QFileDevice::ResourceError)
        return ENOSPC;
    return EIO;
}
}

static int readEmbeddedSbr(const QString& path, quint64 partitionOffset, bool skipMbr, QByteArray& core)
{
    static const bool initialized = initializeGrub2Resources();
    Q_UNUSED(initialized);
    core.clear();
    QFile payload(path);
    if (!payload.open(QIODevice::ReadOnly))
        return ENOENT;
    const QByteArray bytes = payload.readAll();
    if (payload.error() != QFileDevice::NoError)
        return EIO;
    const bool hasGap = partitionOffset >= sectorBytes;
    const bool validSize = skipMbr ? bytes.size() > qsizetype(sectorBytes) : bytes.size() >= qsizetype(sectorBytes);
    if (!validSize)
        return EBADMSG;
    const quint64 sbrBytes = quint64(bytes.size()) - (skipMbr ? sectorBytes : 0);
    const bool fitsGap = hasGap && sbrBytes <= partitionOffset - sectorBytes;
    if (!fitsGap)
        return EBADMSG;
    core = bytes;
    return 0;
}

int ReadEmbeddedGrub2Core(quint64 partitionOffset, QByteArray& core)
{
    return readEmbeddedSbr(QStringLiteral(":/rufus/grub2/core.img"), partitionOffset, false, core);
}

int ReadEmbeddedGrub4dosSbr(quint64 partitionOffset, QByteArray& resource)
{
    return readEmbeddedSbr(QStringLiteral(":/rufus/grub/grldr.mbr"), partitionOffset, true, resource);
}

static int readCachedBootFile(quint64 maximumBytes, Grub2CachedCore& core)
{
    const QByteArray path = QFile::encodeName(core.path);
    IMAGE_FILE_SOURCE source = {-1, {}, false, false, nullptr, 0};
    source.fd = ::open(path.constData(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (source.fd < 0)
    {
        const int error = errno;
        core.state = Grub2CacheState::Missing;
        return IsIsoScanCancelled() ? ECANCELED : error;
    }
    int error = fstat(source.fd, &source.info) == 0
        ? ReadPrepareCoreSource(source, path, maximumBytes, core.bytes) : errno;
    // An opened cache disappearing is an input change, not a missing-cache
    // download decision. Reuse the source checker without changing ISO callers.
    const bool disappeared = error == ENOENT || error == ENOTDIR;
    if (IsIsoScanCancelled())
        error = ECANCELED;
    else if (disappeared)
        error = ESTALE;
    CloseImageFileSource(&source);
    if (error == 0)
    {
        core.sha256 = QCryptographicHash::hash(core.bytes, QCryptographicHash::Sha256);
        core.state = Grub2CacheState::Ready;
    }
    else
        core.bytes.clear();
    return error;
}

int ReadGrub2CoreCache(const QString& filesDirectory, const RUFUS_IMG_REPORT& report,
    bool mbr, bool bios, quint64 partitionOffset, Grub2CachedCore& core)
{
    core = {};
    if (!NeedsGrub2CoreLookup(&report, mbr, bios, GRUB2_PACKAGE_VERSION))
        return 0;
    core.state = Grub2CacheState::Unusable;
    if (IsIsoScanCancelled())
        return ECANCELED;
    const int pathError = coreCachePath(filesDirectory, report, partitionOffset, core.path);
    if (pathError != 0)
        return pathError;
    return readCachedBootFile(partitionOffset - sectorBytes, core);
}

static int storeCachedBootFile(const QString& path, quint64 maximumBytes, const QByteArray& verifiedBytes)
{
    if (verifiedBytes.size() < qsizetype(sectorBytes))
        return EBADMSG;
    if (quint64(verifiedBytes.size()) > maximumBytes)
        return ENOSPC;
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return EIO;
    if (IsIsoScanCancelled())
        return ECANCELED;
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Unbuffered))
        return cacheWriteError(file.error());
    qsizetype position = 0;
    while (position < verifiedBytes.size())
    {
        if (IsIsoScanCancelled())
            return ECANCELED;
        const qsizetype chunk = std::min(writeChunkBytes, verifiedBytes.size() - position);
        const qint64 written = file.write(verifiedBytes.constData() + position, chunk);
        if (IsIsoScanCancelled())
            return ECANCELED;
        if (written != chunk)
            return cacheWriteError(file.error());
        position += chunk;
    }
    // Cancellation observed before this atomic replacement leaves the old file.
    if (IsIsoScanCancelled())
        return ECANCELED;
    if (!file.commit())
        return cacheWriteError(file.error());
    return 0;
}

int StoreGrub2CoreCache(const QString& filesDirectory, const RUFUS_IMG_REPORT& report,
    bool mbr, bool bios, quint64 partitionOffset, const QByteArray& verifiedBytes)
{
    if (!NeedsGrub2CoreLookup(&report, mbr, bios, GRUB2_PACKAGE_VERSION))
        return EINVAL;
    if (IsIsoScanCancelled())
        return ECANCELED;
    QString path;
    const int pathError = coreCachePath(filesDirectory, report, partitionOffset, path);
    if (pathError != 0)
        return pathError;
    return storeCachedBootFile(path, partitionOffset - sectorBytes, verifiedBytes);
}

static int grub4dosCachePath(const QString& filesDirectory, QString& path)
{
    const bool absoluteDirectory = QDir::isAbsolutePath(filesDirectory) && !filesDirectory.contains(QChar(u'\0'));
    if (!absoluteDirectory)
        return EINVAL;
    path = QDir(filesDirectory).filePath(QStringLiteral("grub4dos-" GRUB4DOS_VERSION "/grldr"));
    return 0;
}

int ReadGrub4dosLoaderCache(const QString& filesDirectory, Grub2CachedCore& loader)
{
    loader = {};
    loader.state = Grub2CacheState::Unusable;
    if (IsIsoScanCancelled())
        return ECANCELED;
    const int error = grub4dosCachePath(filesDirectory, loader.path);
    if (error != 0)
        return error;
    return readCachedBootFile(PrepareCoreMaximumBytes, loader);
}

int StoreGrub4dosLoaderCache(const QString& filesDirectory, const QByteArray& verifiedBytes)
{
    if (IsIsoScanCancelled())
        return ECANCELED;
    QString path;
    const int error = grub4dosCachePath(filesDirectory, path);
    if (error != 0)
        return error;
    return storeCachedBootFile(path, PrepareCoreMaximumBytes, verifiedBytes);
}

}
