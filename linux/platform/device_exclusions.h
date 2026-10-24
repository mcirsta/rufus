// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "device_policy.h"
namespace rufus
{
// descriptor >= 0 requires fresh on-device GPT data. Discovery may use udev's
// kernel-device keyed partition-table metadata when raw read access is absent.
int CheckDeviceExclusions(const QString& path, quint64 device, quint64 size,
    const DeviceExclusions& exclusions, int descriptor = -1);
}
