// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "platform/device_policy.h"
namespace rufus
{
struct PrepareImageRequest;
DeviceExclusions ConfiguredDeviceExclusions();
bool DeviceLocking();
void SetDeviceLocking(bool enabled);
void FreezeDevicePolicy(PrepareImageRequest& request);
}
