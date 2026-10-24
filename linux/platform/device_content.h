// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QByteArray>
#include <QString>

namespace rufus
{
constexpr qsizetype DriveLabelFileBytes = 64 * 1024;
// Original ccs=UNICODE file mode: UTF-16LE by default or UTF-8 with BOM.
// Uses the shared first nonempty label token; no autorun action is executed.
int ParseDriveLabel(const QByteArray& bytes, QString& label);
// Borrow a checked mount-root descriptor. Scan case-insensitively, refuse links,
// nonregular/oversized/changed files, and preserve the caller's directory offset.
int ReadMountedDriveLabel(int directory, quint64 filesystemDevice, QString& label);
}
