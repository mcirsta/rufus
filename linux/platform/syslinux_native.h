// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "syslinux_version.h"

#include "image_report.h"
#include <QByteArrayView>
#include <QString>

namespace rufus
{
constexpr quint16 nativeSyslinuxVersion = 0x0604;
// Win32 displays the embedded major/minor version without the /pre1 suffix.
constexpr char nativeSyslinuxName[] = "Syslinux 6.04";
constexpr char nativeSyslinux4Name[] = "Syslinux 4.07";
constexpr qsizetype nativeSyslinuxModuleBytes = 122656;
// ReactOS ISO creation selects embedded 4.07 when its source has no Syslinux
// config/version. Keep that installer selection separate from source scan facts.
RUFUS_IMG_REPORT SyslinuxSourceReport(const RUFUS_IMG_REPORT& report);
// Native FAT/NTFS provider limits and payload identity, independent of an ISO.
int CheckNativeSyslinuxGeometry(int filesystem, quint32 sectorBytes, quint64 offsetBytes, QString& problem);
int CheckNativeSyslinuxModule(QByteArrayView module, QString& problem);

// Select by the shared version and suffix as Win32 does. An inferred version
// alone is insufficient; callers must then check the source companion as well.
int CheckNativeSyslinuxSelection(const RUFUS_IMG_REPORT& report, int filesystem,
    quint32 sectorBytes, quint64 offsetBytes, QString& problem);

// Check the pinned native 6.04-pre1 installer/payload set.
// report comes from the shared Syslinux selection policy; all strings terminate.
// module contains the image's BIOS ldlinux.c32 for the selected config directory.
// Unknown detection may use the shared inferred v6 choice, but still needs the
// exact companion bytes. The report is never changed. Returns 0 or ENOTSUP with
// a diagnostic; does not open files, launch a host installer or authorize writes.
//
// This checks the pair and FAT16/FAT32/NTFS 512-byte geometry only. The caller must validate
// every module the job would replace, preserve config/search-directory behavior,
// and use the verified executable built with the pinned payloads. A --version
// banner alone does not identify that binary. Other modules, file/layout support,
// retained source/target identity and final sync remain job responsibilities.
int CheckNativeSyslinuxPair(const RUFUS_IMG_REPORT& report, int filesystem,
    quint32 sectorBytes, quint64 offsetBytes, QByteArrayView module, QString& problem);
}
