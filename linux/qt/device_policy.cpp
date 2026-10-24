// SPDX-License-Identifier: GPL-3.0-or-later
#include "device_policy.h"
#include "application_storage.h"
#include "prepare_image_protocol.h"
#include <QDebug>
#include <atomic>
#include <limits>
namespace rufus
{
namespace { std::atomic_bool lockDevice{true}; }
bool DeviceLocking() { return lockDevice.load(); }
void SetDeviceLocking(bool enabled) { lockDevice.store(enabled); }
DeviceExclusions ConfiguredDeviceExclusions()
{
    ApplicationSettings settings;
    DeviceExclusions result;
    for (int slot = 1; slot <= 8; ++slot) {
        const QString usbKey = QStringLiteral("IgnoreUsb%1").arg(slot, 2, 10, QLatin1Char('0'));
        const QString diskKey = QStringLiteral("IgnoreDisk%1").arg(slot, 2, 10, QLatin1Char('0'));
        const QString usb = settings.value(usbKey).toString().trimmed();
        bool valid = false;
        const quint64 number = usb.toULongLong(&valid, usb.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive) ? 16 : 10);
        if (!usb.isEmpty() && (!valid || number > std::numeric_limits<quint32>::max()))
            qWarning().noquote() << "Ignoring malformed Rufus setting:" << usbKey;
        else if (number != 0) result.usb.append(quint32(number));
        const QString disk = settings.value(diskKey).toString().trimmed();
        if (!disk.isEmpty()) {
            const QUuid uuid(disk);
            const bool decorated = disk.size() == 38 && disk.front() == u'{' && disk.back() == u'}';
            if (!decorated || uuid.isNull()) qWarning().noquote() << "Ignoring malformed Rufus setting:" << diskKey;
            else result.disks.append(uuid.toString());
        }
    }
    return result;
}
void FreezeDevicePolicy(PrepareImageRequest& request)
{
    request.lockDevice = DeviceLocking();
    request.exclusions = ConfiguredDeviceExclusions();
}
}
