// SPDX-License-Identifier: GPL-3.0-or-later
#include "image_save_destination.h"
#include "image_compression.h"
#include "iso_export.h"
#include "rufus_reporting.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUuid>
#include <cerrno>
#include <fcntl.h>
#include <cstdio>
#include <unistd.h>

namespace rufus
{
namespace
{
bool sameFile(const struct stat& first, const struct stat& second)
{
    return first.st_dev == second.st_dev && first.st_ino == second.st_ino && first.st_mode == second.st_mode
        && first.st_size == second.st_size && first.st_mtim.tv_sec == second.st_mtim.tv_sec
        && first.st_mtim.tv_nsec == second.st_mtim.tv_nsec && first.st_ctim.tv_sec == second.st_ctim.tv_sec
        && first.st_ctim.tv_nsec == second.st_ctim.tv_nsec;
}

int renameImage(int directory, const QString& from, const QString& to, unsigned flags)
{
    return renameat2(directory, QFile::encodeName(from).constData(), directory,
        QFile::encodeName(to).constData(), flags) == 0 ? 0 : errno;
}
}

int ImageSaveDestination::checkRenamed(const QString& name,const struct stat& expected)
{
    struct stat current;
    if (fstatat(directory_,QFile::encodeName(name).constData(),&current,AT_SYMLINK_NOFOLLOW)!=0) return errno;
    struct stat renamed=expected; renamed.st_ctim=current.st_ctim;
    return sameFile(current,renamed) ? 0 : ESTALE;
}

QString ImageSaveDestination::recoveryPath() const
{
    return backup_.isEmpty() ? QString() : QDir(parent_).filePath(backup_);
}

int ImageSaveDestination::restoreBackup()
{
    const int checked=checkRenamed(backup_,backupInfo_);
    if (checked!=0) return checked;
    const int error=renameImage(directory_,backup_,name_,RENAME_NOREPLACE);
    if (error!=0) return error;
    backup_.clear();
    return fsync(directory_)==0 ? 0 : errno;
}

int ImageSaveDestination::publishWithBackup()
{
    // Native NTFS rejects RENAME_EXCHANGE. Retain the old entry with a synced
    // no-replace rename before publishing; never overwrite a concurrent entry.
    const QString backup=QStringLiteral(".rufus-backup-%1").arg(QUuid::createUuid().toString(QUuid::Id128));
    int error=renameImage(directory_,name_,backup,RENAME_NOREPLACE);
    if (error!=0) return error;
    backup_=backup;
    error=fstatat(directory_,QFile::encodeName(backup_).constData(),&backupInfo_,AT_SYMLINK_NOFOLLOW)==0 ? 0 : errno;
    if (error==0) error=checkRenamed(backup_,original_);
    if (error==0 && fsync(directory_)!=0) error=errno;
    if (error==0 && IsRufusJobCancelled()) error=ECANCELED;
    if (error==0) error=renameImage(directory_,temporary_,name_,RENAME_NOREPLACE);
    if (error!=0) {
        const int restored=restoreBackup();
        return restored!=0 ? restored : error;
    }
    committed_=true;
    error=checkRenamed(name_,staged_);
    if (error==0 && fsync(directory_)!=0) error=errno;
    if (error!=0) {
        // Keep an unexpected concurrent entry intact. Both retained entries
        // remain recoverable when restoring the original name is impossible.
        if (checkRenamed(name_,staged_)!=0) return error;
        const int removed=renameImage(directory_,name_,temporary_,RENAME_NOREPLACE);
        if (removed!=0) return removed;
        committed_=false;
        const int restored=restoreBackup();
        return restored!=0 ? restored : error;
    }
    error=checkRenamed(backup_,original_);
    if (error!=0) return error;
    if (unlinkat(directory_,QFile::encodeName(backup_).constData(),0)!=0) return errno;
    backup_.clear(); temporary_.clear(); temporaryPath_.clear();
    return fsync(directory_)==0 ? 0 : errno;
}

ImageSaveDestination::ImageSaveDestination(const QString& path) : path_(path) {}
ImageSaveDestination::~ImageSaveDestination() { close(); }

int ImageSaveDestination::open()
{
    if (directory_ >= 0 || path_.isEmpty() || path_.contains(QChar(0))) return EINVAL;
    const QFileInfo chosen(path_);
    name_ = chosen.fileName();
    parent_ = QFileInfo(chosen.absolutePath()).canonicalFilePath();
    if (parent_.isEmpty() || name_.isEmpty() || name_ == QLatin1String(".") || name_ == QLatin1String("..")) return EINVAL;
    directory_ = ::open(QFile::encodeName(parent_).constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directory_ < 0) return errno;
    if (fstat(directory_, &directoryInfo_) != 0) return errno;
    if (fstatat(directory_, QFile::encodeName(name_).constData(), &original_, AT_SYMLINK_NOFOLLOW) == 0) {
        existed_ = true;
        if (!S_ISREG(original_.st_mode)) return EINVAL;
    } else if (errno != ENOENT) return errno;
    return 0;
}

int ImageSaveDestination::create(int format)
{
    if (directory_ < 0 || output_ >= 0 || !temporary_.isEmpty()) return EINVAL;
    const char* suffix = format == IMG_COMPRESSION_VHD ? ".vhd" : format == IMG_COMPRESSION_VHDX ? ".vhdx"
        : format == IMG_COMPRESSION_FFU ? ".ffu" : format == ImageSaveIso ? ".iso" : nullptr;
    if (!suffix) return EINVAL;
    temporary_ = QStringLiteral(".rufus-image-%1%2").arg(QUuid::createUuid().toString(QUuid::Id128), QLatin1String(suffix));
    temporaryPath_ = QDir(parent_).filePath(temporary_);
    output_ = openat(directory_, QFile::encodeName(temporary_).constData(), O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC | O_RDWR, 0600);
    if (output_ < 0) return errno;
    if (fstat(output_, &staged_) != 0) return errno;
    return 0;
}

int ImageSaveDestination::checkDestination()
{
    struct stat parent, current;
    if (stat(QFile::encodeName(parent_).constData(), &parent) != 0) return errno;
    if (parent.st_dev != directoryInfo_.st_dev || parent.st_ino != directoryInfo_.st_ino) return ESTALE;
    if (fstatat(directory_, QFile::encodeName(name_).constData(), &current, AT_SYMLINK_NOFOLLOW) != 0)
        return !existed_ && errno == ENOENT ? 0 : errno;
    return existed_ && sameFile(current, original_) ? 0 : ESTALE;
}

int ImageSaveDestination::commit()
{
    if (output_ < 0 || committed_ || directory_ < 0) return EINVAL;
    if (IsRufusJobCancelled()) return ECANCELED;
    if (fsync(output_) != 0) return errno;
    struct stat current;
    if (fstat(output_, &current) != 0) return errno;
    if (current.st_dev != staged_.st_dev || current.st_ino != staged_.st_ino || current.st_nlink != 1) return ESTALE;
    staged_=current;
    const int descriptor = output_;
    output_ = -1;
    if (::close(descriptor) != 0) return errno;
    int error = checkDestination();
    if (error != 0) return error;
    if (fstatat(directory_, QFile::encodeName(temporary_).constData(), &current, AT_SYMLINK_NOFOLLOW) != 0) return errno;
    if (current.st_dev != staged_.st_dev || current.st_ino != staged_.st_ino) return ESTALE;
    if (IsRufusJobCancelled()) return ECANCELED;
    error = renameImage(directory_, temporary_, name_, existed_ ? RENAME_EXCHANGE : RENAME_NOREPLACE);
    const bool exchangeUnavailable=existed_ && (error==EINVAL || error==ENOTSUP || error==ENOSYS);
    if (exchangeUnavailable) return publishWithBackup();
    if (error != 0) return error;
    committed_ = true;
    if (fstatat(directory_, QFile::encodeName(name_).constData(), &current, AT_SYMLINK_NOFOLLOW) != 0) error = errno;
    else if (current.st_dev != staged_.st_dev || current.st_ino != staged_.st_ino) error = ESTALE;
    // Never delete a different file raced into the destination during saving.
    if (existed_ && error == 0) {
        if (fstatat(directory_, QFile::encodeName(temporary_).constData(), &current, AT_SYMLINK_NOFOLLOW) != 0) error = errno;
        else {
            // A rename may update ctime; inode, size and modification time must match.
            struct stat expected = original_;
            expected.st_ctim = current.st_ctim;
            if (!sameFile(current, expected)) error = ESTALE;
        }
    }
    if (error == 0 && fsync(directory_) != 0) error = errno;
    if (error != 0) {
        const int rollback = renameImage(directory_, name_, temporary_, existed_ ? RENAME_EXCHANGE : RENAME_NOREPLACE);
        if (rollback == 0) { committed_ = false; fsync(directory_); }
        return error;
    }
    if (existed_ && unlinkat(directory_, QFile::encodeName(temporary_).constData(), 0) != 0) return errno;
    temporary_.clear();
    temporaryPath_.clear();
    if (fsync(directory_) != 0) return errno;
    return 0;
}

int ImageSaveDestination::close()
{
    if (cleanup_) return 0;
    cleanup_ = true;
    int error = 0;
    if (!backup_.isEmpty()) {
        if (!committed_) error=restoreBackup();
        if (!backup_.isEmpty()) {
            if (error==0) error=ESTALE;
        }
    }
    if (output_ >= 0) {
        const int descriptor = output_; output_ = -1;
        if (::close(descriptor) != 0) error = errno;
    }
    if (directory_ >= 0 && !temporary_.isEmpty() && !committed_) {
        struct stat current;
        if (fstatat(directory_, QFile::encodeName(temporary_).constData(), &current, AT_SYMLINK_NOFOLLOW) == 0) {
            if (current.st_dev != staged_.st_dev || current.st_ino != staged_.st_ino) error = ESTALE;
            else if (unlinkat(directory_, QFile::encodeName(temporary_).constData(), 0) != 0) error = errno;
            else if (fsync(directory_) != 0 && error == 0) error = errno;
        } else if (errno != ENOENT) error = errno;
    }
    if (directory_ >= 0) {
        const int descriptor = directory_; directory_ = -1;
        if (::close(descriptor) != 0 && error == 0) error = errno;
    }
    return error;
}
}
