// SPDX-License-Identifier: GPL-3.0-or-later
#include "zip_extract.h"
#include "compressed_zip.h"
#include "rufus_reporting.h"
#include "progress_policy.h"
#include <QFile>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iconv.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace rufus
{
namespace
{
constexpr quint64 maximumZipEntries = 262144, maximumZipNames = 64 * 1024 * 1024;
constexpr qsizetype maximumZipDepth = 128, maximumZipPath = 4095;
constexpr quint64 allocationSectorBytes = 512;

int syncZipOutput(int descriptor, ZipExtractResult& result)
{
    while (fsync(descriptor) != 0) {
        if (errno == EINTR) continue;
        result.syncError = errno;
        return errno;
    }
    return 0;
}

int zipEntryPath(const ZIP_ARCHIVE_ENTRY& entry, QByteArray& path)
{
    const QByteArray raw(entry.name, qsizetype(entry.name_bytes));
    if (raw.contains('\0')) return EBADMSG;
    QString text = QString::fromUtf8(raw);
    if (text.toUtf8() != raw) {
        if (entry.flags & 0x800) return EILSEQ;
        // ZIP's unflagged legacy names use CP437. Valid UTF-8 names are kept
        // even when their producer omitted the Unicode flag, as Windows does.
        iconv_t converter = iconv_open("UTF-8", "CP437");
        if (converter == iconv_t(-1)) return errno;
        QByteArray converted(raw.size() * 4, '\0');
        char* input = const_cast<char*>(raw.constData());
        char* output = converted.data();
        size_t remaining = size_t(raw.size()), available = size_t(converted.size());
        const size_t status = iconv(converter, &input, &remaining, &output, &available);
        int error = status == size_t(-1) || remaining != 0 ? EILSEQ : 0;
        if (iconv_close(converter) != 0 && error == 0) error = errno;
        if (error != 0) return error;
        converted.resize(converted.size() - qsizetype(available));
        text = QString::fromUtf8(converted);
    }
    text.replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (text.startsWith(QLatin1Char('/')) || text.contains(QLatin1Char(':'))) return EPERM;
    const QStringList components = text.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    QStringList normalized;
    for (const QString& component : components) {
        if (component == QLatin1String("..")) return EPERM;
        if (component != QLatin1String(".")) normalized.append(component);
    }
    if (normalized.size() > maximumZipDepth) return ENAMETOOLONG;
    path = normalized.join(QLatin1Char('/')).toUtf8();
    if (path.size() > maximumZipPath) return ENAMETOOLONG;
    return path.isEmpty() && !entry.directory ? EBADMSG : 0;
}

int openZipDirectory(int parent, const QByteArray& name, dev_t device, ZipExtractResult& result)
{
    bool created = mkdirat(parent, name.constData(), 0755) == 0;
    if (!created && errno != EEXIST) return -1;
    int descriptor = openat(parent, name.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (descriptor < 0) return -1;
    struct stat info;
    int error = fstat(descriptor, &info) == 0 ? 0 : errno;
    if (error == 0 && info.st_dev != device) error = EXDEV;
    if (error == 0 && created) error = syncZipOutput(parent, result);
    if (error != 0) { close(descriptor); errno = error; return -1; }
    return descriptor;
}

int openZipParent(int root, const QByteArray& path, bool directory, dev_t device,
    QByteArray& name, ZipExtractResult& result)
{
    const QList<QByteArray> components = path.split('/');
    name = components.last();
    int descriptor = openat(root, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (descriptor < 0) return -1;
    const qsizetype count = directory ? components.size() : components.size() - 1;
    for (qsizetype index = 0; index < count; index++) {
        const int child = openZipDirectory(descriptor, components[index], device, result);
        const int error = child < 0 ? errno : 0;
        const int closed = close(descriptor);
        if (error != 0 || closed != 0) {
            const int failure = error != 0 ? error : errno;
            if (child >= 0) close(child);
            errno = failure; return -1;
        }
        descriptor = child;
    }
    return descriptor;
}

int openZipOutput(int parent, const QByteArray& name, dev_t device, quint64 bytes)
{
    constexpr int flags = O_WRONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK;
    int descriptor = openat(parent, name.constData(), flags | O_CREAT | O_EXCL, 0644);
    const bool created = descriptor >= 0;
    if (descriptor < 0 && errno == EEXIST) descriptor = openat(parent, name.constData(), flags);
    if (descriptor < 0) return -1;
    struct stat info;
    int error = fstat(descriptor, &info) == 0 ? 0 : errno;
    if (error == 0 && !S_ISREG(info.st_mode)) error = ENOTSUP;
    if (error == 0 && info.st_nlink != 1) error = EMLINK;
    if (error == 0 && info.st_dev != device) error = EXDEV;
    struct statvfs space;
    if (error == 0 && fstatvfs(descriptor, &space) != 0) error = errno;
    if (error == 0 && (space.f_frsize < allocationSectorBytes || space.f_frsize % allocationSectorBytes != 0
        || info.st_blocks < 0)) error = EIO;
    if (error == 0) {
        const quint64 needed = bytes / space.f_frsize + (bytes % space.f_frsize != 0);
        const quint64 previous = quint64(info.st_blocks) / (space.f_frsize / allocationSectorBytes);
        // Replacement can reclaim its old content. Check each member against
        // actual free space, allowing large overlays and repeated ZIP names.
        if (needed > space.f_bavail && needed - space.f_bavail > previous) error = ENOSPC;
    }
    if (error == 0 && IsRufusJobCancelled()) error = ECANCELED;
    if (error == 0 && ftruncate(descriptor, 0) != 0) error = errno;
    if (error != 0) {
        close(descriptor);
        if (created) unlinkat(parent, name.constData(), 0);
        errno = error; return -1;
    }
    return descriptor;
}

struct ZipCopy
{
    const IMAGE_FILE_SOURCE* source;
    const QByteArray* path;
    int output;
    quint64 position, previous, total;
};

int copyZipBytes(void* opaque, const uint8_t* bytes, size_t size, uint64_t offset)
{
    ZipCopy& copy = *static_cast<ZipCopy*>(opaque);
    if (offset != copy.position) return EIO;
    int error = CheckImageFileSource(copy.source, copy.path->constData());
    if (error != 0) return error;
    size_t position = 0;
    while (position < size) {
        if (IsRufusJobCancelled()) return ECANCELED;
        const ssize_t written = write(copy.output, bytes + position, size - position);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return written < 0 ? errno : EIO;
        position += size_t(written);
    }
    copy.position += size;
    _UpdateProgressWithInfo(OP_EXTRACT_ZIP, 0, copy.previous + copy.position, copy.total, 0);
    return 0;
}

int processZip(const IMAGE_FILE_SOURCE& source, const QString& path, int root,
    quint64 maximumFile, quint64 maximumTotal, quint64 total, ZipExtractResult& result)
{
    result = {};
    if (maximumTotal == 0 || maximumTotal > quint64(INT64_MAX)) return EINVAL;
    if (maximumFile == 0 || maximumFile > maximumTotal) maximumFile = maximumTotal;
    const QByteArray sourcePath = QFile::encodeName(path);
    int error = CheckImageFileSource(&source, sourcePath.constData());
    struct stat rootInfo = {};
    if (error == 0 && root >= 0 && fstat(root, &rootInfo) != 0) error = errno;
    if (error == 0 && root >= 0 && !S_ISDIR(rootInfo.st_mode)) error = ENOTDIR;
    ZIP_ARCHIVE* archive = nullptr;
    if (error == 0) error = OpenZipArchive(&source, &archive);
    quint64 names = 0;
    ZIP_ARCHIVE_ENTRY entry;
    while (error == 0) {
        error = NextZipArchiveEntry(archive, &entry);
        if (error == ENOENT) { error = 0; break; }
        if (error != 0) break;
        if (result.files + result.directories == maximumZipEntries) { error = E2BIG; break; }
        if (entry.name_bytes > maximumZipNames - names) { error = E2BIG; break; }
        names += entry.name_bytes;
        QByteArray outputPath;
        error = zipEntryPath(entry, outputPath);
        result.path = QString::fromUtf8(outputPath);
        const bool fits = entry.size <= maximumFile && entry.size <= maximumTotal - result.bytes;
        if (error == 0 && !fits) error = EFBIG;
        if (error == 0 && entry.directory && entry.size != 0) error = EBADMSG;
        if (error != 0) break;
        int parent = -1, output = -1;
        QByteArray name;
        if (root >= 0 && !outputPath.isEmpty()) {
            parent = openZipParent(root, outputPath, entry.directory, rootInfo.st_dev, name, result);
            if (parent < 0) error = errno;
            if (error == 0 && !entry.directory) {
                output = openZipOutput(parent, name, rootInfo.st_dev, entry.size);
                if (output < 0) error = errno;
            }
        }
        ZipCopy copy{&source, &sourcePath, output, 0, result.bytes, total};
        COMPRESSED_IMAGE_RESULT decoded = {};
        if (error == 0) error = ReadZipArchiveEntry(archive, entry.size == 0 ? 1 : entry.size,
            output >= 0 ? copyZipBytes : nullptr, &copy, &decoded);
        if (error == 0) error = CheckImageFileSource(&source, sourcePath.constData());
        if (error == 0 && output >= 0) error = syncZipOutput(output, result);
        if (output >= 0 && close(output) != 0 && error == 0) error = errno;
        if (error != 0 && output >= 0) unlinkat(parent, name.constData(), 0);
        if (error == 0 && parent >= 0) error = syncZipOutput(parent, result);
        if (parent >= 0 && close(parent) != 0 && error == 0) error = errno;
        if (error != 0) break;
        if (entry.directory) result.directories++; else result.files++;
        result.bytes += decoded.decoded_bytes;
    }
    CloseZipArchive(archive);
    if (error == 0) error = CheckImageFileSource(&source, sourcePath.constData());
    if (error == 0 && root >= 0) error = syncZipOutput(root, result);
    if (error == 0) result.path.clear();
    return error;
}
}

int CheckZipSupplement(const IMAGE_FILE_SOURCE& source, const QString& path,
    quint64 maximumFile, quint64 maximumTotal, ZipExtractResult& result)
{
    return processZip(source, path, -1, maximumFile, maximumTotal, 0, result);
}

int ExtractZipSupplement(const IMAGE_FILE_SOURCE& source, const QString& path, int directory,
    quint64 maximumFile, quint64 maximumTotal, ZipExtractResult& result)
{
    result = {};
    if (directory < 0) return EBADF;
    ZipExtractResult planned;
    int error = CheckZipSupplement(source, path, maximumFile, maximumTotal, planned);
    if (error != 0) { result = planned; return error; }
    error = processZip(source, path, directory, maximumFile, maximumTotal, planned.bytes, result);
    if (error == 0) UpdateProgress(OP_EXTRACT_ZIP, 100.0f);
    return error;
}
}
