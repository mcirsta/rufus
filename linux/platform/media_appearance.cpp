// SPDX-License-Identifier: GPL-3.0-or-later
#include "media_appearance.h"
#include "rufus_reporting.h"
#include "rufus_build_info.h"
#include <QFile>
#include <QFileInfo>
#include <QResource>
#include <QTemporaryFile>
#include <QtEndian>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include "filesystem_magic.h"
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace rufus
{
namespace
{
int checkDirectory(int directory, bool& udf)
{
    const int flags = fcntl(directory, F_GETFL);
    if (flags < 0) return errno;
    if (flags & O_PATH) return EINVAL;
    struct stat info;
    struct statfs filesystem;
    if (fstat(directory, &info) != 0 || fstatfs(directory, &filesystem) != 0) return errno;
    if (!S_ISDIR(info.st_mode)) return ENOTDIR;
    constexpr long ntfsMagic = 0x5346544e;
    const bool supported = filesystem.f_type == MSDOS_SUPER_MAGIC || filesystem.f_type == EXFAT_SUPER_MAGIC
        || filesystem.f_type == ntfsMagic || filesystem.f_type == FUSE_SUPER_MAGIC || filesystem.f_type == UDF_SUPER_MAGIC;
    if (!supported) return ENOTSUP;
    udf = filesystem.f_type == UDF_SUPER_MAGIC;
    return filesystem.f_flags & ST_RDONLY ? EROFS : 0;
}

int syncFile(int fd, MediaAppearanceResult& result)
{
    while (fsync(fd) != 0)
    {
        if (errno == EINTR) continue;
        result.syncError = errno;
        return errno;
    }
    return 0;
}

int writeVerified(int fd, const QByteArray& bytes, MediaAppearanceResult& result)
{
    size_t done = 0;
    int error = 0;
    while (done < size_t(bytes.size()))
    {
        if (IsRufusJobCancelled()) { error = ECANCELED; break; }
        const ssize_t count = pwrite(fd, bytes.constData() + done, size_t(bytes.size()) - done, off_t(done));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { error = count < 0 ? errno : EIO; break; }
        done += size_t(count); result.bytesWritten += quint64(count);
    }
    const int sync = syncFile(fd, result); // Partial writes are flushed even on cancellation.
    if (error == 0) error = sync;
    QByteArray actual(bytes.size(), '\0'); done = 0;
    while (error == 0 && done < size_t(actual.size()))
    {
        if (IsRufusJobCancelled()) { error = ECANCELED; break; }
        const ssize_t count = pread(fd, actual.data() + done, size_t(actual.size()) - done, off_t(done));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { error = count < 0 ? errno : EIO; break; }
        done += size_t(count);
    }
    if (error == 0 && actual != bytes) error = EBADMSG;
    return error;
}

QByteArray autorunBytes(const QString& label)
{
    const QString text = QStringLiteral("; Created by Rufus %1\r\n; https://rufus.ie\r\n[autorun]\r\nicon  = autorun.ico\r\nlabel = %2\r\n")
        .arg(QLatin1String(RUFUS_LINUX_VERSION), label);
    QByteArray bytes(2 + text.size() * 2, '\0');
    bytes[0] = char(0xff); bytes[1] = char(0xfe);
    for (qsizetype i = 0; i < text.size(); ++i)
        qToLittleEndian<quint16>(text[i].unicode(), bytes.data() + 2 + i * 2);
    return bytes;
}
}

int WriteMediaAppearance(int directory, const QString& label, MediaAppearanceResult& result)
{
    result = {};
    if (!ValidMediaLabel(label)) return EINVAL;
    if (IsRufusJobCancelled()) return ECANCELED;
    bool udf = false;
    int error = checkDirectory(directory, udf);
    if (error != 0) return error;
    struct stat existing;
    if (fstatat(directory, "autorun.inf", &existing, AT_SYMLINK_NOFOLLOW) == 0)
    {
        result.preserved = true;
        return 0;
    }
    if (errno != ENOENT) return errno;
    QByteArray icon;
    error = ReadMediaIcon(icon);
    if (error != 0) return error;
    // Stage the icon before creating metadata. Rename replaces an old icon's
    // directory entry, so image-controlled symlinks/hardlinks are never followed.
    QTemporaryFile staged(QStringLiteral("/proc/self/fd/%1/.rufus-autorun-XXXXXX").arg(directory));
    if (!staged.open()) return EIO;
    // UDF records Unix modes; its metadata must remain readable after the
    // root helper exits. FAT/NTFS/exFAT keep their established mount rules.
    if (udf && fchmod(staged.handle(), 0644) != 0) error = errno;
    if (error == 0) error = writeVerified(staged.handle(), icon, result);
    const QByteArray temporary = QFile::encodeName(QFileInfo(staged.fileName()).fileName());
    int inf = -1;
    struct stat created = {};
    bool identified = false;
    if (error == 0)
    {
        inf = openat(directory, "autorun.inf", O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (inf < 0 && errno == EEXIST) result.preserved = true;
        else if (inf < 0) error = errno;
        else if (fstat(inf, &created) != 0) error = errno;
        else {
            identified = true;
            if (udf && fchmod(inf, 0644) != 0) error = errno;
            if (error == 0) error = writeVerified(inf, autorunBytes(label), result);
        }
    }
    if (error == 0 && IsRufusJobCancelled()) error = ECANCELED;
    if (inf >= 0 && close(inf) != 0 && error == 0) error = errno;
    if (inf >= 0 && error == 0)
    {
        if (renameat(directory, temporary.constData(), directory, "autorun.ico") != 0) error = errno;
        else { staged.setAutoRemove(false); result.created = true; }
    }
    if (inf >= 0 && identified && error != 0)
    {
        struct stat current;
        if (fstatat(directory, "autorun.inf", &current, AT_SYMLINK_NOFOLLOW) == 0
            && current.st_ino == created.st_ino && current.st_dev == created.st_dev)
            unlinkat(directory, "autorun.inf", 0);
    }
    if (!result.created && !staged.remove() && error == 0) error = EIO;
    while (syncfs(directory) != 0)
    {
        if (errno == EINTR) continue;
        result.syncError = errno;
        if (error == 0) error = errno;
        break;
    }
    return error;
}
}
