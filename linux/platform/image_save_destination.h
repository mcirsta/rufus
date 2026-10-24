// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QString>
#include <sys/stat.h>

namespace rufus
{
// Unprivileged destination transaction. Keep the original file until the new
// image is verified. A held directory anchors creation, rename and cleanup.
class ImageSaveDestination
{
public:
    explicit ImageSaveDestination(const QString& path);
    ~ImageSaveDestination();
    int open();
    int create(int format);
    int commit();
    int close();
    int descriptor() const { return output_; }
    quint64 originalInode() const { return existed_ ? quint64(original_.st_ino) : 0; }
    quint64 filesystemDevice() const { return quint64(directoryInfo_.st_dev); }
    int directoryDescriptor() const { return directory_; }
    const QString& temporaryPath() const { return temporaryPath_; }
    bool committed() const { return committed_; }
    QString recoveryPath() const;

private:
    int checkDestination();
    int checkRenamed(const QString& name,const struct stat& expected);
    int restoreBackup();
    int publishWithBackup();
    const QString path_;
    QString parent_, name_, temporary_, temporaryPath_, backup_;
    int directory_ = -1, output_ = -1;
    struct stat directoryInfo_ = {}, original_ = {}, staged_ = {}, backupInfo_ = {};
    bool existed_ = false, committed_ = false, cleanup_ = false;
};
}
