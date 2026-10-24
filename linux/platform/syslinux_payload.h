// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "syslinux_resources.h"
#include <QByteArray>
#include <QList>
#include <QString>
#include <stdint.h>

namespace rufus
{
constexpr int syslinuxSectorBytes = 512;
constexpr int syslinuxAdvBytes = 1024;
struct SyslinuxBootPayload
{
    int loaderBytes;
    const char* resourcePrefix;
    int paddedBytes = (loaderBytes + syslinuxSectorBytes - 1) / syslinuxSectorBytes * syslinuxSectorBytes;
    int installedBytes = paddedBytes + syslinuxAdvBytes;
};
constexpr SyslinuxBootPayload syslinux4Payload = {36488, ":/rufus/syslinux/v4/"};
constexpr SyslinuxBootPayload syslinux6Payload = {68121, ":/rufus/syslinux/"};

// Serialize with the other callers of the original Rufus patcher globals.
// Only pinned resources become patch templates. The caller validates the sector
// map and reads the actual files/VBR through its filesystem-specific adapter.
int CheckSyslinuxPayload(int bootType, int fileSystem, const QByteArray& beforeBoot,
    const QByteArray& afterBoot, const QByteArray& installed,
    const QList<uint64_t>& sectors, QString& problem, const SyslinuxResources* resources = nullptr);
}
