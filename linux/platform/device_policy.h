// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QStringList>
#include <QList>
#include <QUuid>

namespace rufus
{
struct DeviceExclusions
{
    QList<quint32> usb;
    QStringList disks;
    bool empty() const { return usb.isEmpty() && disks.isEmpty(); }
    bool valid() const
    {
        if (usb.size() > 8 || disks.size() > 8) return false;
        for (quint32 id : usb) if (id == 0) return false;
        for (const QString& id : disks)
            if (QUuid(id).isNull() || QUuid(id).toString() != id) return false;
        return true;
    }
};
}
