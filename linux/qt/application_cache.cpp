// SPDX-License-Identifier: GPL-3.0-or-later
#include "application_storage.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace rufus
{
namespace
{
bool ownedFile(const QByteArray& name, bool nested)
{
    const QByteArray base = name.endsWith(".sig") ? name.first(name.size()-4) : name;
    if (nested) return base == "core.img" || base == "grldr" || base == "ldlinux.sys" || base == "ldlinux.bss"
        || base == "ldlinux.c32" || base == "menu.c32" || base == "vesamenu.c32";
    if (base == "diskcopy.dll" || base == "sbat_level.txt" || base == "sb_active.txt" || base == "sb_revoked.txt") return true;
    static const QRegularExpression dbx(QStringLiteral("^dbx_(ia32|x64|arm|aa64|ia64|riscv64|loongarch64|ebc)\\.json$"));
    return dbx.match(QString::fromLatin1(base)).hasMatch();
}

bool ownedDirectory(const QByteArray& name)
{
    static const QRegularExpression version(QStringLiteral("^(grub|grub4dos|syslinux)-[A-Za-z0-9._+/-]{1,100}$"));
    return !name.contains('/') && version.match(QString::fromLatin1(name)).hasMatch();
}

int clearDirectory(int descriptor, bool nested, const QSet<QPair<quint64,quint64>>& protectedFiles)
{
    struct stat parent = {};
    if (fstat(descriptor,&parent) != 0) return errno;
    const int copy = dup(descriptor);
    if (copy < 0) return errno;
    DIR* directory = fdopendir(copy);
    if (directory == nullptr) { const int error = errno; close(copy); return error; }
    int error = 0;
    errno = 0;
    while (dirent* entry = readdir(directory)) {
        const QByteArray name(entry->d_name);
        if (name == "." || name == "..") continue;
        const bool ownedSubdirectory = !nested && ownedDirectory(name);
        if (!ownedSubdirectory && !ownedFile(name,nested)) continue;
        struct stat info = {};
        if (fstatat(descriptor,entry->d_name,&info,AT_SYMLINK_NOFOLLOW) != 0) { error = errno; break; }
        if (info.st_uid != geteuid() || info.st_dev != parent.st_dev) { error = EPERM; break; }
        if (protectedFiles.contains({quint64(info.st_dev),quint64(info.st_ino)})) continue;
        if (ownedSubdirectory && S_ISDIR(info.st_mode)) {
            const int child = openat(descriptor,entry->d_name,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
            if (child < 0) { error = errno; break; }
            error = clearDirectory(child,true,protectedFiles);
            close(child);
            if (error != 0) break;
            if (unlinkat(descriptor,entry->d_name,AT_REMOVEDIR) != 0 && errno != ENOTEMPTY) { error = errno; break; }
        } else if (S_ISREG(info.st_mode) || S_ISLNK(info.st_mode)) {
            if (unlinkat(descriptor,entry->d_name,0) != 0) { error = errno; break; }
        }
        errno = 0;
    }
    if (error == 0 && errno != 0) error = errno;
    closedir(directory);
    return error;
}
}

int ClearApplicationResources(const QStringList& protectedPaths, QString& problem)
{
    problem.clear();
    const QByteArray path = QFile::encodeName(ApplicationFilesDirectory());
    const int descriptor = open(path.constData(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    if (descriptor < 0) return errno == ENOENT ? 0 : errno;
    struct stat info = {};
    int error = fstat(descriptor,&info) == 0 ? 0 : errno;
    if (error == 0 && info.st_uid != geteuid()) error = EPERM;
    QSet<QPair<quint64,quint64>> protectedFiles;
    for (const QString& source : protectedPaths) {
        struct stat sourceInfo = {};
        if (!source.isEmpty() && stat(QFile::encodeName(source).constData(),&sourceInfo) == 0)
            protectedFiles.insert({quint64(sourceInfo.st_dev),quint64(sourceInfo.st_ino)});
    }
    if (error == 0) error = clearDirectory(descriptor,false,protectedFiles);
    close(descriptor);
    if (error != 0) problem = QString::fromLocal8Bit(strerror(error));
    return error;
}
}
