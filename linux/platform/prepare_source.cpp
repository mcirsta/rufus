// SPDX-License-Identifier: GPL-3.0-or-later
#include "prepare_source.h"
#include "virtual_disk.h"

#include <QFile>
#include <QCryptographicHash>
#include "rufus_reporting.h"
#include <algorithm>
#include <utility>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

namespace rufus
{
namespace
{
constexpr qsizetype coreReadChunkBytes = 64 * 1024;

QString timestamp(const timespec& value)
{
    return QString::number(value.tv_sec) + QLatin1Char(':') + QString::number(value.tv_nsec);
}
}

int CapturePrepareSource(const QString& path, PrepareImageRequest& request)
{
    IMAGE_FILE_SOURCE source;
    const int error = OpenImageFileSource(QFile::encodeName(path).constData(), &source);
    if (error != 0)
        return error;
    request.imagePath = path;
    request.imageDevice = quint64(source.info.st_dev);
    request.imageInode = quint64(source.info.st_ino);
    request.imageSize = quint64(source.info.st_size);
    request.imageModified = timestamp(source.info.st_mtim);
    request.imageChanged = timestamp(source.info.st_ctim);
    CloseImageFileSource(&source);
    return 0;
}

int CapturePrepareArchive(const QString& path, PrepareArchiveSource& archive)
{
    archive = {};
    PrepareImageRequest source;
    const int error = CapturePrepareSource(path, source);
    if (error != 0) return error;
    if (source.imageSize < 22) return EBADMSG;
    archive = {source.imagePath, source.imageDevice, source.imageInode, source.imageSize,
        source.imageModified, source.imageChanged};
    return 0;
}

PrepareImageRequest PrepareArchiveRequest(const PrepareArchiveSource& archive)
{
    PrepareImageRequest source;
    source.imagePath = archive.path;
    source.imageDevice = archive.device;
    source.imageInode = archive.inode;
    source.imageSize = archive.size;
    source.imageModified = archive.modified;
    source.imageChanged = archive.changed;
    return source;
}

PrepareIsoSource::~PrepareIsoSource()
{
    close();
}

int PrepareIsoSource::close()
{
    const int parentsClosed = CloseVirtualDiskSource(source_.virtual_disk);
    source_.virtual_disk = nullptr;
    const int descriptor = source_.fd;
    source_ = {-1, {}, false, false, nullptr, 0};
    retainedPath_.clear();
    if (descriptor >= 0 && ::close(descriptor) != 0)
        return errno;
    return parentsClosed;
}

int CheckPrepareSourceIdentity(const PrepareImageRequest& request, const IMAGE_FILE_SOURCE& source)
{
    const struct stat& info = source.info;
    const bool sameFile = quint64(info.st_dev) == request.imageDevice && quint64(info.st_ino) == request.imageInode;
    const bool sameContent = info.st_size >= 0 && quint64(info.st_size) == request.imageSize
        && timestamp(info.st_mtim) == request.imageModified && timestamp(info.st_ctim) == request.imageChanged;
    return sameFile && sameContent ? 0 : ESTALE;
}

int PrepareIsoSource::open(uid_t caller)
{
    if (source_.fd >= 0)
        return EALREADY;
    source_.fd = ::open(QFile::encodeName(request_.imagePath).constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (source_.fd < 0)
        return errno;
    if (fstat(source_.fd, &source_.info) != 0)
        return errno;
    const struct stat& info = source_.info;
    if (!S_ISREG(info.st_mode) || info.st_size < 0)
        return EINVAL;
    // This first device extraction path accepts caller-owned readable images.
    // Check before reading any source bytes with the helper's credentials.
    if (info.st_uid != caller || (info.st_mode & S_IRUSR) == 0)
        return EACCES;
    const int error = CheckPrepareSourceIdentity(request_, source_);
    if (error != 0)
        return error;
    retainedPath_ = QByteArray("/proc/self/fd/") + QByteArray::number(source_.fd);
    source_.ffu_store = request_.ffuStore;
    const bool virtualDisk = request_.imageCompression == IMG_COMPRESSION_VHD || request_.imageCompression == IMG_COMPRESSION_VHDX;
    if (virtualDisk) {
        const int opened = OpenVirtualDiskSource(&source_, request_.imageCompression, request_.decodedImageSize, &source_.virtual_disk);
        if (opened != 0) return opened;
    }
    return check();
}

int PrepareIsoSource::check() const
{
    const int error = CheckImageFileSource(&source_, QFile::encodeName(request_.imagePath).constData());
    return error == 0 && source_.virtual_disk ? CheckVirtualDiskSource(source_.virtual_disk) : error;
}

bool PrepareIsoSource::hasParentSources() const
{
    return source_.virtual_disk && source_.virtual_disk->parent;
}


int ReadPrepareCoreSource(IMAGE_FILE_SOURCE& source, const QByteArray& path, quint64 maximumBytes,
    QByteArray& bytes)
{
    bytes.clear();
    if (source.fd < 0)
        return EBADF;
    if (maximumBytes > PrepareCoreMaximumBytes)
        return EINVAL;
    if (!S_ISREG(source.info.st_mode) || source.info.st_size < 0)
        return EINVAL;
    const quint64 size = quint64(source.info.st_size);
    if (size < PrepareCoreSectorBytes)
        return EBADMSG;
    if (size > maximumBytes)
        return ENOSPC;
    int error = CheckImageFileSource(&source, path.constData());
    if (error != 0)
        return error;
    QByteArray data(qsizetype(size), Qt::Uninitialized);
    qsizetype position = 0;
    while (position < data.size())
    {
        if (IsIsoScanCancelled())
            return ECANCELED;
        const qsizetype chunk = std::min(coreReadChunkBytes, data.size() - position);
        const ssize_t count = pread(source.fd, data.data() + position, size_t(chunk), off_t(position));
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
        {
            error = count < 0 ? errno : EIO;
            break;
        }
        position += count;
    }
    if (IsIsoScanCancelled())
        return ECANCELED;
    const int changed = CheckImageFileSource(&source, path.constData());
    if (changed != 0)
        return changed;
    if (error == 0)
        bytes = std::move(data);
    return error;
}

namespace
{
int loadPrepareBootFile(const PrepareImageRequest& request, uid_t caller,
    quint64 maximumBytes, QByteArray& bytes)
{
    if (IsIsoScanCancelled())
        return ECANCELED;
    const QByteArray path = QFile::encodeName(request.grub2CorePath);
    IMAGE_FILE_SOURCE source = {-1, {}, false, false, nullptr, 0};
    source.fd = ::open(path.constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (source.fd < 0)
        return errno;
    int error = fstat(source.fd, &source.info) == 0 ? 0 : errno;
    if (error == 0)
    {
        const bool ordinaryFile = S_ISREG(source.info.st_mode) && source.info.st_nlink == 1;
        const bool ownedReadable = source.info.st_uid == caller && (source.info.st_mode & S_IRUSR) != 0;
        const bool sameSize = source.info.st_size >= 0 && quint64(source.info.st_size) == request.grub2CoreSize;
        if (!ordinaryFile)
            error = EINVAL;
        else if (!ownedReadable)
            error = EACCES;
        else if (!sameSize)
            error = ESTALE;
    }
    QByteArray core;
    if (error == 0)
        error = ReadPrepareCoreSource(source, path, maximumBytes, core);
    CloseImageFileSource(&source);
    if (IsIsoScanCancelled())
        error = ECANCELED;
    else if (error == ENOENT || error == ENOTDIR)
        error = ESTALE;
    if (error == 0)
    {
        const bool sameBytes = QCryptographicHash::hash(core, QCryptographicHash::Sha256) == request.grub2CoreSha256;
        if (!sameBytes)
            return ESTALE;
        bytes = std::move(core);
    }
    return error;
}
}

int LoadPrepareGrub2Core(const PrepareImageRequest& request, uid_t caller,
    quint64 partitionOffset, QByteArray& bytes)
{
    bytes.clear();
    if (request.grub4dos || !request.hasGrub2Core() || !ValidatePrepareGrub2Core(request))
        return EINVAL;
    const bool validGap = partitionOffset > PrepareCoreSectorBytes
        && partitionOffset <= PrepareCoreMaximumBytes + PrepareCoreSectorBytes
        && partitionOffset % PrepareCoreSectorBytes == 0;
    if (!validGap)
        return EINVAL;
    if (request.grub2CoreSize > partitionOffset - PrepareCoreSectorBytes)
        return ENOSPC;
    return loadPrepareBootFile(request, caller, partitionOffset - PrepareCoreSectorBytes, bytes);
}

int LoadPrepareSyslinuxResources(const PrepareImageRequest& request, uid_t caller, QByteArray& bytes)
{
    bytes.clear();
    if (!request.hasSyslinuxResources() || EncodePrepareRequest(request).isEmpty()) return EINVAL;
    PrepareImageRequest file = request;
    file.grub2CorePath = request.syslinuxResourcesPath; file.grub2CoreSize = request.syslinuxResourcesSize; file.grub2CoreSha256 = request.syslinuxResourcesSha256;
    return loadPrepareBootFile(file,caller,PrepareCoreMaximumBytes,bytes);
}

int LoadPrepareGrub4dosLoader(const PrepareImageRequest& request, uid_t caller, QByteArray& bytes)
{
    bytes.clear();
    if (!request.grub4dos || EncodePrepareRequest(request).isEmpty())
        return EINVAL;
    // The loader is copied into the filesystem; its size is not an SBR gap bound.
    return loadPrepareBootFile(request, caller, PrepareCoreMaximumBytes, bytes);
}

}
