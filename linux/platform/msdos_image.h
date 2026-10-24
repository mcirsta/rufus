// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "msdos_files.h"
#include <QByteArray>
#include <QString>
#include <QVector>
#include <sys/types.h>

namespace rufus
{
struct MsDosFile
{
    const MSDOS_FILE* entry = nullptr;
    QByteArray bytes;
    quint8 attributes = 0;
    quint16 accessDate = 0, writeDate = 0, writeTime = 0;
};
// Fixed SHA256 is checked before any FAT offsets or patches are inspected.
int ReadMsDosFiles(const QByteArray& image, QVector<MsDosFile>& files);
// Caller-owned regular file, no symlinks, exact fixed size and stable fstat.
int ReadMsDosImage(const QString& path, uid_t owner, QByteArray& bytes);
int StoreMsDosImage(const QString& directory, const QByteArray& bytes);
}
