// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QSet>
#include <QByteArray>
#include <QString>
#include <QStringList>

namespace rufus
{
// Read-only sysfs ancestry: partition parents, slaves and file-backed loops.
// Alternate root supports filesystem fixtures, never a user/job setting.
int NativeBlockBackings(quint64 device, QSet<quint64>& devices,
    const QString& sysfsRoot = QStringLiteral("/sys"));
// Positive file-backed loop/NBD snapshot; paths and inode facts are revalidated.
int NativeFileDiskSources(const QString& sysfsPath, QStringList& paths, QStringList& identity, bool metadataOnly = false);
// Kernel network mounts require a numeric server address that is not local.
int CheckNativeRemoteBacking(const QByteArray& mountOptions);
// Direct Btrfs members, kept separate from their partition/container ancestors.
int NativeBtrfsMembers(int descriptor, QSet<quint64>& devices);
// Complete backing set for a retained file/directory. An unresolved layer is an error.
int NativeFileBackings(int descriptor, QSet<quint64>& devices);
// Capture the running executable and its directory, including Btrfs members.
// A partial set still protects every positively identified device. Unknown
// anonymous filesystems do not imply that an unrelated USB is the app disk.
QSet<quint64> NativeApplicationDevices();
}
