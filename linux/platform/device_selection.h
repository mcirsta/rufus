// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "udisks_client.h"

namespace rufus
{
struct PrepareImageRequest;
// Read-only sysfs identity, captured by the ordinary caller before authorization.
quint64 ReadDiskSequence(quint64 device);
// Read-only identity/use check shared by the preflight worker and root helper.
// Preflight may accept eligible mounted candidates; only the root target object
// can validate/unmount them. Post-acquisition checks always require no mounts.
int CheckPrepareDeviceSelection(const QDBusConnection& connection, const PrepareImageRequest& request,
    bool allowMounted = false, int* partitionCount = nullptr);

// Fresh identity snapshot; does not authorize or acquire the device.
int ReadPrepareDeviceSnapshot(const QDBusConnection& connection, const PrepareImageRequest& request,
    UDisksObjects& objects, Device& current);
// Capture checks share physical identity/protected-use checks, allowing known
// read-only media. These functions never permit a destructive operation.
int ReadSaveDeviceSnapshot(const QDBusConnection& connection, const PrepareImageRequest& request,
    UDisksObjects& objects, Device& current);
int CheckSaveDeviceSelection(const QDBusConnection& connection, const PrepareImageRequest& request);
}
