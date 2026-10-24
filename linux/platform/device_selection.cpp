// SPDX-License-Identifier: GPL-3.0-or-later
#include "device_selection.h"
#include "device_exclusions.h"
#include "qt/prepare_image_protocol.h"
#include "rufus_reporting.h"
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusReply>
#include <QFile>
#include <cerrno>
#include <sys/sysmacros.h>

namespace rufus
{
namespace
{
constexpr int snapshotTimeoutMs = 5000;
}

quint64 ReadDiskSequence(quint64 device)
{
    QFile file(QStringLiteral("/sys/dev/block/%1:%2/diskseq")
        .arg(major(dev_t(device))).arg(minor(dev_t(device))));
    if (!file.open(QIODevice::ReadOnly))
        return 0;
    bool valid = false;
    const quint64 value = file.read(128).trimmed().toULongLong(&valid);
    return valid ? value : 0;
}

static int readDeviceSnapshot(const QDBusConnection& connection, const PrepareImageRequest& request,
    UDisksObjects& objects, Device& current, DeviceAccess access)
{
    qDBusRegisterMetaType<UDisksInterfaces>();
    qDBusRegisterMetaType<UDisksObjects>();
    QDBusMessage message = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.DBus"),
        QStringLiteral("/org/freedesktop/DBus"), QStringLiteral("org.freedesktop.DBus"), QStringLiteral("GetNameOwner"));
    message.setArguments({QStringLiteral("org.freedesktop.UDisks2")});
    const QDBusReply<QString> owner = connection.call(message, QDBus::Block, snapshotTimeoutMs);
    if (!owner.isValid())
        return EHOSTDOWN;
    if (owner.value() != request.serviceOwner)
        return ESTALE;
    message = QDBusMessage::createMethodCall(owner.value(), QStringLiteral("/org/freedesktop/UDisks2"),
        QStringLiteral("org.freedesktop.DBus.ObjectManager"), QStringLiteral("GetManagedObjects"));
    const QDBusReply<UDisksObjects> reply = connection.call(message, QDBus::Block, snapshotTimeoutMs);
    if (!reply.isValid())
        return EHOSTDOWN;
    objects = reply.value();
    if (!DescribeUDisksDevice(objects, request.objectPath, current, request.allowFixedUsb, access, DeviceClassMask(request.deviceClass)))
        return ENOTSUP;
    const int excluded = CheckDeviceExclusions(current.path, current.number, current.size, request.exclusions);
    if (excluded != 0) return excluded;
    const bool sameIdentity = current.number == request.device && current.identity == request.identity;
    const bool sameMedia = current.path == request.path && current.size == request.size;
    const bool sameGeometry = request.rawWrite || request.deviceOperation != 0 || access == DeviceAccess::Read
        || (current.logicalSectorSize == request.logicalSectorSize && current.physicalSectorSize == request.physicalSectorSize);
    if (!sameIdentity || !sameMedia || !sameGeometry)
        return ESTALE;
    return IsRufusJobCancelled() ? ECANCELED : 0;
}

int ReadPrepareDeviceSnapshot(const QDBusConnection& connection, const PrepareImageRequest& request,
    UDisksObjects& objects, Device& current)
{
    return readDeviceSnapshot(connection, request, objects, current, DeviceAccess::Write);
}

int ReadSaveDeviceSnapshot(const QDBusConnection& connection, const PrepareImageRequest& request,
    UDisksObjects& objects, Device& current)
{
    return readDeviceSnapshot(connection, request, objects, current, DeviceAccess::Read);
}

int CheckSaveDeviceSelection(const QDBusConnection& connection, const PrepareImageRequest& request)
{
    UDisksObjects objects;
    Device current;
    const int error = ReadSaveDeviceSnapshot(connection, request, objects, current);
    if (error != 0) return error;
    const QString problem = CheckUDisksTargetUse(objects, current, request.imageDevice, request.allowFixedUsb, DeviceAccess::Read);
    if (!problem.isEmpty()) { uprintfs(problem.toUtf8().constData()); return EBUSY; }
    return IsRufusJobCancelled() ? ECANCELED : 0;
}

int CheckPrepareDeviceSelection(const QDBusConnection& connection, const PrepareImageRequest& request, bool allowMounted, int* partitionCount)
{
    UDisksObjects objects;
    Device current;
    const int error = ReadPrepareDeviceSnapshot(connection, request, objects, current);
    if (error != 0)
        return error;
    QStringList mounts;
    const QString problem = allowMounted ? ListUDisksTargetMounts(objects, current, request.imageDevice, mounts, request.allowFixedUsb)
        : CheckUDisksTargetUse(objects, current, request.imageDevice, request.allowFixedUsb);
    if (!problem.isEmpty())
    {
        uprintfs(problem.toUtf8().constData());
        return EBUSY;
    }
    if (partitionCount) *partitionCount = current.partitionCount;
    return IsRufusJobCancelled() ? ECANCELED : 0;
}

}
