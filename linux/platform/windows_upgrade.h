// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>
#include <QStringList>

namespace rufus
{
// Rufus's appraiser placeholder and (build >= 26000) setup.exe wrapper. The
// extracted target must be private and stable, as for ApplyWindowsCustomization.
// Return errno; preserve originals in appraiserres.bak / setup.dll. An I/O error
// fails the creation job instead of reporting partially customized media ready.
int PrepareWindowsUpgradeFiles(const char* directory, uint32_t build, QStringList* modified = nullptr);
}
