// SPDX-License-Identifier: GPL-3.0-or-later
// Disposable-directory adapter; never a device test or installed executable.
#include "platform/media_appearance.h"
#include <QCoreApplication>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <cerrno>
#include <cstdarg>
#include <cstring>
#include <fcntl.h>
#include <linux/magic.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace
{
QByteArray fault;
int writes = 0, syncs = 0;
bool mode(const char* name) { return fault == name; }
}
extern "C" int IsRufusJobCancelled()
{
    return mode("cancel-before") || (mode("cancel-icon") && writes >= 1) || (mode("cancel-inf") && writes >= 2);
}
extern "C" int __real_fstatfs64(int, struct statfs64*);
extern "C" int __wrap_fstatfs64(int fd, struct statfs64* info)
{
    const int error = __real_fstatfs64(fd, info);
    if (error != 0 || mode("unsupported")) return error;
    info->f_type = mode("ntfs") ? 0x5346544e : mode("exfat") ? EXFAT_SUPER_MAGIC
        : mode("fuse") ? FUSE_SUPER_MAGIC : MSDOS_SUPER_MAGIC;
    if (mode("readonly")) info->f_flags |= ST_RDONLY;
    return 0;
}
extern "C" ssize_t __real_pwrite64(int, const void*, size_t, off_t);
extern "C" ssize_t __wrap_pwrite64(int fd, const void* data, size_t size, off_t offset)
{
    if (mode("eintr") && writes == 0) { ++writes; errno = EINTR; return -1; }
    if (mode("write-inf") && writes >= 1) { errno = ENOSPC; return -1; }
    if (mode("short") && size > 17) size = 17;
    const ssize_t count = __real_pwrite64(fd, data, size, offset);
    if (count > 0) ++writes;
    return count;
}
extern "C" ssize_t __real_pread64(int, void*, size_t, off_t);
extern "C" ssize_t __wrap_pread64(int fd, void* data, size_t size, off_t offset)
{
    const ssize_t count = __real_pread64(fd, data, size, offset);
    if (count > 0 && mode("corrupt")) static_cast<char*>(data)[0] ^= 1;
    return count;
}
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd)
{
    ++syncs;
    if (mode("sync")) { errno = EIO; return -1; }
    return __real_fsync(fd);
}
extern "C" int __real_syncfs(int);
extern "C" int __wrap_syncfs(int fd)
{
    if (mode("directory-sync")) { errno = EIO; return -1; }
    return __real_syncfs(fd);
}
extern "C" int __real_renameat(int, const char*, int, const char*);
extern "C" int __wrap_renameat(int first, const char* source, int second, const char* destination)
{
    if (mode("rename")) { errno = EACCES; return -1; }
    return __real_renameat(first, source, second, destination);
}
extern "C" int __real_openat64(int, const char*, int, ...);
extern "C" int __wrap_openat64(int directory, const char* path, int flags, ...)
{
    mode_t permissions = 0;
    if (flags & O_CREAT)
    {
        va_list arguments; va_start(arguments, flags); permissions = mode_t(va_arg(arguments, int)); va_end(arguments);
    }
    if (mode("race") && strcmp(path, "autorun.inf") == 0 && (flags & O_CREAT))
    {
        const int fd = __real_openat64(directory, path, flags, permissions);
        if (fd < 0 || write(fd, "racer", 5) != 5 || close(fd) != 0) _exit(3);
        errno = EEXIST; return -1;
    }
    return __real_openat64(directory, path, flags, permissions);
}
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (argc != 4) return 2;
    const QString path = QString::fromLocal8Bit(argv[1]);
    if (!QDir(path).dirName().startsWith(QStringLiteral("test_media_appearance-"))) return 77;
    fault = argv[3];
    QString label = QString::fromUtf8(argv[2]);
    if (mode("surrogate")) label = QChar(0xd800);
    if (mode("nul")) label = QChar(0);
    const int fd = open(argv[1], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0 || lseek(fd, 17, SEEK_SET) != 17) return 2;
    rufus::MediaAppearanceResult result;
    const int error = rufus::WriteMediaAppearance(fd, label, result);
    if (lseek(fd, 0, SEEK_CUR) != 17 || close(fd) != 0) return 3;
    const QJsonObject object = {{"error", error}, {"sync", result.syncError}, {"created", result.created},
        {"preserved", result.preserved}, {"written", QString::number(result.bytesWritten)}, {"writes", writes}, {"syncs", syncs}};
    puts(QJsonDocument(object).toJson(QJsonDocument::Compact).constData());
    return 0;
}
