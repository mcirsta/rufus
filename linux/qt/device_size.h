// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QString>
namespace rufus {
struct Device;
QString DeviceSizeLabel(quint64 size, bool properUnits = false);
QString DeviceCaption(const Device& device, bool properUnits = false);
}
