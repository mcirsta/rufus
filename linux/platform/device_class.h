// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QtGlobal>

namespace rufus
{
enum class DeviceClass : quint32 { Usb = 1, Virtual = 2, Vmware = 4, NonUsbRemovable = 8, Card = 16, Optical = 32 };
constexpr quint32 DeviceClassMask(DeviceClass kind) { return quint32(kind); }
constexpr quint32 DefaultDeviceClasses = DeviceClassMask(DeviceClass::Usb)
    | DeviceClassMask(DeviceClass::Virtual) | DeviceClassMask(DeviceClass::Card);
constexpr quint32 AllDeviceClasses = DefaultDeviceClasses | DeviceClassMask(DeviceClass::Vmware)
    | DeviceClassMask(DeviceClass::NonUsbRemovable) | DeviceClassMask(DeviceClass::Optical);
constexpr bool ValidDeviceClass(DeviceClass kind)
{
    const quint32 value = DeviceClassMask(kind);
    return value != 0 && (value & (value - 1)) == 0 && (value & AllDeviceClasses) != 0;
}
}
