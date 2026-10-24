// SPDX-License-Identifier: GPL-3.0-or-later
#include "freedos_copy.h"
#include "ntfs_attributes.h"
#include "target_io.h"
#include "rufus_reporting.h"
#include "freedos_files.h"
#include "dos_locale_policy.h"
#include "progress_policy.h"
#include "qt/prepare_image_protocol.h"
#include <QResource>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <iterator>
#include <ctime>
#include <fcntl.h>
#include "filesystem_magic.h"
#include <linux/msdos_fs.h>
#include <sys/ioctl.h>
#include <sys/statfs.h>
#include <unistd.h>

static bool registerFreeDosResources()
{
    Q_INIT_RESOURCE(freedos);
    return true;
}

namespace rufus
{
namespace
{
constexpr int directoryFlags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
constexpr size_t bufferBytes = 64 * 1024;
constexpr long ntfsMagic = 0x5346544e;

int checkCopyDirectory(int root, bool allowNtfs = false)
{
    if (IsRufusJobCancelled())
        return ECANCELED;
    const int flags = fcntl(root, F_GETFL);
    if (flags < 0)
        return errno;
    if ((flags & O_PATH) != 0)
        return EINVAL;
    struct statfs filesystem;
    if (fstatfs(root, &filesystem) != 0)
        return errno;
    // The mount owner has already verified FUSE's NTFS provider and device.
    const bool ntfs = allowNtfs && (filesystem.f_type == ntfsMagic || filesystem.f_type == FUSE_SUPER_MAGIC);
    const bool supported = filesystem.f_type == MSDOS_SUPER_MAGIC || ntfs;
    if (!supported)
        return ENOTSUP;
    struct stat info;
    if (fstat(root, &info) != 0)
        return errno;
    return S_ISDIR(info.st_mode) ? 0 : ENOTDIR;
}

int syncOutput(int fd, FreeDosCopyResult& result)
{
    while (fsync(fd) != 0)
    {
        if (errno == EINTR)
            continue;
        const int error = errno;
        if (result.syncError == 0)
            result.syncError = error;
        return error;
    }
    return 0;
}

int setSystemAttributes(int fd)
{
    uint32_t attributes = 0;
    if (ioctl(fd, FAT_IOCTL_GET_ATTRIBUTES, &attributes) != 0)
        return errno;
    const uint32_t systemAttributes = ATTR_HIDDEN | ATTR_SYS;
    attributes |= systemAttributes;
    if (ioctl(fd, FAT_IOCTL_SET_ATTRIBUTES, &attributes) != 0)
        return errno;
    uint32_t actual = 0;
    if (ioctl(fd, FAT_IOCTL_GET_ATTRIBUTES, &actual) != 0)
        return errno;
    return actual == attributes ? 0 : EBADMSG;
}

int transferPayload(int fd, const QByteArray& data, bool verify, FreeDosCopyResult& result)
{
    char actual[bufferBytes];
    size_t done = 0;
    const size_t size = size_t(data.size());
    while (done < size)
    {
        if (IsRufusJobCancelled())
            return ECANCELED;
        const size_t count = qMin(bufferBytes, size - done);
        const ssize_t transferred = verify ? pread(fd, actual, count, off_t(done)) :
            write(fd, data.constData() + done, count);
        if (transferred < 0 && errno == EINTR)
            continue;
        if (transferred <= 0)
            return transferred == 0 ? EIO : errno;
        if (verify)
        {
            if (memcmp(actual, data.constData() + done, size_t(transferred)) != 0)
                return EBADMSG;
        }
        else
            result.bytesWritten += quint64(transferred);
        done += size_t(transferred);
    }
    return 0;
}

int setMsDosMetadata(int fd, const MsDosFile& source)
{
    uint32_t attributes = source.attributes & (ATTR_RO | ATTR_HIDDEN | ATTR_SYS | ATTR_ARCH);
    if (ioctl(fd,FAT_IOCTL_SET_ATTRIBUTES,&attributes) != 0) return errno;
    uint32_t actual = 0;
    if (ioctl(fd,FAT_IOCTL_GET_ATTRIBUTES,&actual) != 0) return errno;
    if (actual != attributes) return EBADMSG;
    struct tm time = {};
    time.tm_year = int(source.writeDate >> 9) + 80;
    time.tm_mon = int((source.writeDate >> 5) & 15) - 1;
    time.tm_mday = source.writeDate & 31;
    time.tm_hour = source.writeTime >> 11;
    time.tm_min = (source.writeTime >> 5) & 63;
    time.tm_sec = (source.writeTime & 31) * 2;
    time.tm_isdst = -1;
    const time_t modified = mktime(&time);
    if (modified < 0) return EBADMSG;
    time.tm_year = int(source.accessDate >> 9) + 80;
    time.tm_mon = int((source.accessDate >> 5) & 15) - 1;
    time.tm_mday = source.accessDate & 31;
    time.tm_hour = time.tm_min = time.tm_sec = 0; time.tm_isdst = -1;
    const time_t accessed = source.accessDate != 0 ? mktime(&time) : modified;
    const struct timespec times[] = {{accessed,0},{modified,0}};
    return futimens(fd,times) == 0 ? 0 : errno;
}

int writeFile(int directory, const char* name, const QByteArray& data, bool system, FreeDosCopyResult& result, const MsDosFile* original = nullptr)
{
    const int fd = openat(directory, name, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0)
        return errno;
    int error = transferPayload(fd, data, false, result);
    if (error == 0 && system)
        error = setSystemAttributes(fd);
    const int syncError = syncOutput(fd, result);
    if (error == 0)
        error = syncError;
    if (error == 0)
        error = transferPayload(fd, data, true, result);
    if (error == 0 && original != nullptr) {
        error = setMsDosMetadata(fd,*original);
        const int metadataSync = syncOutput(fd,result);
        if (error == 0) error = metadataSync;
    }
    if (close(fd) != 0 && error == 0)
        error = errno;
    return error;
}

int copyPayload(int directory, const FREEDOS_FILE& entry, FreeDosCopyResult& result)
{
    const QResource resource(QStringLiteral(":/rufus/freedos/") + QString::fromLatin1(entry.name));
    if (!resource.isValid())
        return ENOENT;
    const QByteArray data = resource.uncompressedData();
    if (data.isEmpty() || data.size() != resource.uncompressedSize())
        return EIO;
    return writeFile(directory, entry.name, data, entry.root, result);
}

int startupContent(const DOS_LOCALE& locale, bool config, QByteArray& data)
{
    char* output = nullptr;
    size_t length = 0;
    FILE* stream = open_memstream(&output, &length);
    if (stream == nullptr)
        return errno;
    // Only the shared selection's static strings enter these writers. The
    // memory stream preserves their DOS bytes, including CRLF and OEM borders.
    errno = 0;
    const bool written = config ? WriteDOSConfig(stream, &locale) : WriteDOSAutoexec(stream, &locale);
    int error = written ? 0 : (errno != 0 ? errno : EIO);
    if (fclose(stream) != 0 && error == 0)
        error = errno != 0 ? errno : EIO;
    if (error == 0)
        data = QByteArray(output, qsizetype(length));
    free(output);
    return error;
}
}

int CopyFreeDosFiles(int root, FreeDosCopyResult& result)
{
    result = {};
    int error = checkCopyDirectory(root);
    if (error != 0)
        return error;
    error = CheckEmptyTargetDirectory(root);
    if (error != 0)
        return error;
    if (IsRufusJobCancelled())
        return ECANCELED;
    static const bool resourcesReady = registerFreeDosResources();
    Q_UNUSED(resourcesReady);
    if (mkdirat(root, "LOCALE", 0700) != 0)
        return errno;
    const int locale = openat(root, "LOCALE", directoryFlags);
    error = locale < 0 ? errno : 0;
    for (size_t i = 0; i < freedos_file_count && error == 0; i++)
    {
        const FREEDOS_FILE& entry = freedos_files[i];
        result.path = (entry.root ? QString() : QStringLiteral("LOCALE/")) + QString::fromLatin1(entry.name);
        if (IsRufusJobCancelled())
        {
            error = ECANCELED;
            break;
        }
        error = copyPayload(entry.root ? root : locale, entry, result);
        if (error != 0)
            break;
        result.files++;
        if (entry.progress)
            UpdateProgress(OP_FILE_COPY, -1.0f);
    }
    if (locale >= 0)
    {
        syncOutput(locale, result);
        if (close(locale) != 0 && error == 0)
            error = errno;
    }
    syncOutput(root, result);
    if (error == 0)
        error = result.syncError;
    if (error == 0 && IsRufusJobCancelled())
        error = ECANCELED;
    if (error == 0)
        result.path.clear();
    return error;
}

int CopyMsDosFiles(int root, const QVector<MsDosFile>& files, FreeDosCopyResult& result)
{
    result = {};
    int error = checkCopyDirectory(root);
    if (error == 0) error = CheckEmptyTargetDirectory(root);
    if (error != 0) return error;
    if (files.size() != 13) return EBADMSG;
    if (mkdirat(root,"LOCALE",0700) != 0) return errno;
    const int locale = openat(root,"LOCALE",directoryFlags);
    error = locale < 0 ? errno : 0;
    for (qsizetype index = 0; index < files.size() && error == 0; ++index) {
        const auto& source = files[index];
        const auto* entry = GetMsDosFile(size_t(index));
        if (source.entry != entry) { error = EBADMSG; break; }
        result.path = (entry->root ? QString() : QStringLiteral("LOCALE/")) + QString::fromLatin1(entry->name);
        error = writeFile(entry->root ? root : locale,entry->name,source.bytes,false,result,&source);
        if (error != 0) break;
        ++result.files;
        if (index == 2 || index == 7 || index == 12) UpdateProgress(OP_FILE_COPY,-1.0f);
    }
    if (locale >= 0) {
        syncOutput(locale,result);
        if (close(locale) != 0 && error == 0) error = errno;
    }
    syncOutput(root,result);
    if (error == 0) error = result.syncError;
    if (error == 0 && IsRufusJobCancelled()) error = ECANCELED;
    if (error == 0) result.path.clear();
    return error;
}

int CopyGrub4dosLoader(int root, const QByteArray& loader, FreeDosCopyResult& result, bool ntfsCompression)
{
    result = {};
    const bool bounded = loader.size() >= qsizetype(PrepareCoreSectorBytes)
        && quint64(loader.size()) <= PrepareCoreMaximumBytes;
    if (!bounded)
        return EINVAL;
    int error = checkCopyDirectory(root, true);
    if (error == 0)
        error = CheckEmptyTargetDirectory(root);
    if (error != 0)
        return error;
    result.path = QStringLiteral("grldr");
    bool restoreCompression = false;
    if (ntfsCompression) error = PrepareNtfsUncompressedCreation(root, &restoreCompression);
    if (error == 0) error = writeFile(root, "grldr", loader, false, result);
    if (restoreCompression) {
        const int restored = SetNtfsDirectoryCompression(root, true);
        if (error == 0) error = restored;
    }
    if (error == 0)
        result.files = 1;
    syncOutput(root, result);
    if (error == 0)
        error = result.syncError;
    if (error == 0 && IsRufusJobCancelled())
        error = ECANCELED;
    if (error == 0)
        result.path.clear();
    return error;
}

int WriteDosStartupFiles(int root, const char* keyboard, uint32_t codepage, bool freedos, FreeDosCopyResult& result)
{
    result = {};
    int error = checkCopyDirectory(root);
    if (error != 0)
        return error;
    const char* names[] = { "CONFIG.SYS", "AUTOEXEC.BAT" };
    // Refuse either existing startup file, even when CONFIG is omitted. An old
    // CONFIG could change the US/437 boot, and AUTOEXEC must not be overwritten.
    for (const char* name : names)
    {
        result.path = QString::fromLatin1(name);
        struct stat info;
        if (fstatat(root, name, &info, AT_SYMLINK_NOFOLLOW) == 0)
            return EEXIST;
        if (errno != ENOENT)
            return errno;
    }
    const DOS_LOCALE locale = SelectDOSLocale(keyboard, codepage, freedos);
    for (size_t index = 0; index < std::size(names); index++)
    {
        const char* name = names[index];
        const bool config = index == 0;
        if (config && !locale.use_menu)
            continue;
        result.path = QString::fromLatin1(name);
        if (IsRufusJobCancelled())
        {
            error = ECANCELED;
            break;
        }
        QByteArray data;
        error = startupContent(locale, config, data);
        if (error == 0)
            error = writeFile(root, name, data, false, result);
        if (error != 0)
            break;
        result.files++;
    }
    syncOutput(root, result);
    if (error == 0)
        error = result.syncError;
    if (error == 0 && IsRufusJobCancelled())
        error = ECANCELED;
    if (error == 0)
        result.path.clear();
    return error;
}
int WriteFreeDosStartupFiles(int root, const char* keyboard, uint32_t codepage, FreeDosCopyResult& result)
{
    return WriteDosStartupFiles(root,keyboard,codepage,true,result);
}
}
