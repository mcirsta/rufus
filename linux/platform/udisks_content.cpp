// SPDX-License-Identifier: GPL-3.0-or-later
#include "udisks_client.h"
#include "device_content.h"
#include <QDBusArgument>
#include <QFile>
#include <algorithm>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace rufus
{
namespace
{
const QString blockInterface = QStringLiteral("org.freedesktop.UDisks2.Block");
const QString partitionInterface = QStringLiteral("org.freedesktop.UDisks2.Partition");
const QString filesystemInterface = QStringLiteral("org.freedesktop.UDisks2.Filesystem");
struct MountedVolume
{
    quint64 offset = 0, number = 0;
    QString label;
    QList<QByteArray> mounts;
};
bool volumeOrder(const MountedVolume& first,const MountedVolume& second) { return first.offset < second.offset; }
QByteArray mountPath(QByteArray path)
{
    if (!path.endsWith('\0')) return {};
    path.chop(1);
    const auto components = path.split('/');
    const bool absolute = path.startsWith('/') && !path.contains('\0');
    const bool clean = !components.contains(".") && !components.contains("..") && !path.contains("//");
    return absolute && clean ? path : QByteArray();
}
bool toGoVolume(int directory)
{
    // Windows removes drive letters containing EFI\Rufus\ntfs_x64.efi.
    // Open each component without following a link outside the mounted volume.
    int current = openat(directory,"EFI",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    if (current < 0) return false;
    const int rufus = openat(current,"Rufus",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    close(current);
    if (rufus < 0) return false;
    struct stat marker = {};
    const bool found = fstatat(rufus,"ntfs_x64.efi",&marker,AT_SYMLINK_NOFOLLOW)==0 && S_ISREG(marker.st_mode);
    close(rufus);
    return found;
}
}
QList<Device> ReadUDisksDeviceContent(const UDisksObjects& objects,QList<Device> devices)
{
    for (Device& device : devices)
    {
        QList<MountedVolume> volumes;
        for (auto object = objects.cbegin(); object != objects.cend(); ++object)
        {
            const auto partition = object->value(partitionInterface);
            const bool whole = object.key().path()==device.objectPath;
            const bool child = qdbus_cast<QDBusObjectPath>(partition.value(QStringLiteral("Table"))).path()==device.objectPath;
            if ((!whole && !child) || partition.value(QStringLiteral("IsContainer")).toBool()) continue;
            const auto block = object->value(blockInterface);
            const bool protectedVolume = block.value(QStringLiteral("HintSystem"),true).toBool()
                || block.value(QStringLiteral("HintIgnore"),true).toBool();
            if (protectedVolume || block.value(QStringLiteral("IdUsage")).toString()!=QStringLiteral("filesystem")) continue;
            MountedVolume volume;
            volume.offset = partition.value(QStringLiteral("Offset")).toULongLong();
            volume.number = block.value(QStringLiteral("DeviceNumber")).toULongLong();
            if (volume.number==0) continue;
            volume.label = block.value(QStringLiteral("IdLabel")).toString();
            const auto paths = qdbus_cast<QList<QByteArray>>(object->value(filesystemInterface).value(QStringLiteral("MountPoints")));
            for (const QByteArray& raw : paths)
            {
                const QByteArray path = mountPath(raw);
                if (!path.isEmpty()) volume.mounts.append(path);
            }
            if (!volume.mounts.isEmpty()) volumes.append(volume);
        }
        std::stable_sort(volumes.begin(),volumes.end(),volumeOrder);
        device.mountedVolumes = 0;
        QSet<quint64> counted;
        for (const MountedVolume& volume : volumes)
        {
            if (counted.contains(volume.number)) continue;
            bool auxiliary = false;
            QString label = volume.label;
            for (const QByteArray& path : volume.mounts)
            {
                const int directory = open(path.constData(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
                if (directory < 0) continue;
                struct stat root = {};
                const bool matched = fstat(directory,&root)==0 && quint64(root.st_dev)==volume.number;
                if (matched) auxiliary = toGoVolume(directory);
                if (matched && !auxiliary && device.mountedVolumes==0)
                {
                    QString extended;
                    if (ReadMountedDriveLabel(directory,volume.number,extended)==0) label = extended;
                }
                close(directory);
                if (matched) break;
            }
            if (auxiliary) continue;
            counted.insert(volume.number);
            if (device.mountedVolumes++==0) device.volumeLabel = label;
        }
    }
    return devices;
}
}
