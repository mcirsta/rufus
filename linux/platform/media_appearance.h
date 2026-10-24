// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QByteArray>
#include <QString>

namespace rufus
{
constexpr qsizetype MediaLabelUnits = 127; // SetAutorun's 128-wide-character buffer includes NUL.
QString ExtendedMediaLabel(const QString& text);
bool ValidMediaLabel(const QString& text);
int ReadMediaIcon(QByteArray& icon);
struct MediaAppearanceResult
{
    quint64 bytesWritten = 0;
    int syncError = 0;
    bool created = false, preserved = false;
};
// The job owns this checked private FAT/NTFS/exFAT mount. Keep existing
// autorun.inf entries; create UTF-16LE metadata and the original Rufus icon.
// Do not follow image-controlled links or change the borrowed directory offset.
int WriteMediaAppearance(int directory, const QString& label, MediaAppearanceResult& result);
}
