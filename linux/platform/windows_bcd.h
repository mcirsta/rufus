// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QString>
#include <QUuid>

namespace rufus
{
struct WindowsBcdPartition
{
    bool gpt = false;
    quint32 mbrSignature = 0;
    quint64 offset = 0;
    QUuid diskGuid, partitionGuid;
};

struct WindowsBcdOptions
{
    WindowsBcdPartition windows, system;
    bool uefi = true;
    QString description = QStringLiteral("Windows");
    QString locale = QStringLiteral("en-US");
};

struct WindowsBcdResult
{
    QByteArray bytes;
    QUuid loader;
};

// Create an installation BCD from the selected image's BCD-Template. Borrow
// the regular-file descriptor without changing its contents or seek position.
// Select a BIOS/EFI OS loader by role/path, excluding PE/ramdisk templates;
// retain its inherited settings and bind manager, loader, resume and memory
// diagnostics to the new partitions. Disable automatic recovery as Rufus does.
// Never reads host boot configuration, executes Windows tools or accesses a
// block device. Output is owned bytes only after commit/sync/close and private
// temporary cleanup. Errors/cancellation clear the result. Return errno.
int PrepareWindowsToGoBcd(int templateFile, const WindowsBcdOptions& options,
    WindowsBcdResult& result);
}
