// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "qt/prepare_image_protocol.h"
namespace rufus
{
enum DeviceOperation { NoDeviceOperation, ResetUsbPort, ToggleEspType };
using EspRecord = int (*)(void*, const QString&, bool);
int ToggleDeviceEsp(int descriptor, const QStringList& records, EspRecord record,
    void* context, bool& started);
int RunDeviceOperation(const PrepareImageRequest& request, uid_t caller,
    EspRecord record, void* context, PrepareImageOutcome& outcome);
}
