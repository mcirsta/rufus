// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "iso_analyze.h"
#include "syslinux_version.h"
#include <QByteArray>
namespace rufus {
struct SyslinuxResources { QByteArray loader, boot, module, oldModules[2]; };
int ReadEmbeddedSyslinuxResources(const RUFUS_IMG_REPORT& report, SyslinuxResources& resources);
int ReadSyslinuxCompanion(const ISO_ANALYSIS_SOURCE& source, const RUFUS_IMG_REPORT& report, SyslinuxResources& resources);
int ValidateSyslinuxResources(const RUFUS_IMG_REPORT& report, const SyslinuxResources& resources);
QByteArray EncodeSyslinuxResources(const SyslinuxResources& resources);
int DecodeSyslinuxResources(const QByteArray& bytes, SyslinuxResources& resources);
}
