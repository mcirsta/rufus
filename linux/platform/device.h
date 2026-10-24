// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "device_class.h"
#include <QList>
#include <QMetaType>
#include <QString>
#include <QStringList>

namespace rufus
{

// Write access remains the default for target validation; capture callers opt in.
enum class DeviceAccess { Write, Read };

struct Device
{
    // Selection identity, not a persistent ID or permission to write.
    QString id = {};
    QString path = {};
    QString name = {};
    quint64 size = 0;
    bool removable = false; // UDisks Drive.Removable, not permission to erase.
    bool readOnly = false;
    // Native discovery facts, required when preparing a destructive operation.
    QString objectPath = {};
    QString drivePath = {};
    QString serviceOwner = {};
    quint64 number = 0;
    QStringList identity = {};
    // Display-only content metadata; never part of selection identity.
    int partitionCount = 0; // Existing partitions counted for Rufus's extra erase warning.
    int mountedVolumes = 0; // Distinct mounted filesystems, not partitions or aliases.
    QString partitionTableType = {};
    QString volumeLabel = {};
    QString fileSystemType = {};
    QString fileSystemVersion = {};
    // Native fact, separate from Drive.Removable: USB enclosures can contain fixed media.
    DeviceClass deviceClass = DeviceClass::Usb;
    bool mediaRemovable = false;
    // Shared HDD/card filter, separate from physical removal and user opt-in.
    bool defaultVisible = false;
    // Fresh native/UDisks facts determine this, never the score or checkbox.
    bool requiresNativeProtection = true;
    quint32 logicalSectorSize = 512;
    quint32 physicalSectorSize = 512;
};

} // namespace rufus

Q_DECLARE_METATYPE(rufus::Device)
