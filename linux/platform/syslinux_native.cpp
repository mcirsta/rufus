// SPDX-License-Identifier: GPL-3.0-or-later
#include "syslinux_native.h"
#include "format_policy.h"

#include <QCryptographicHash>
#include <cerrno>
#include <cstring>

namespace rufus
{
RUFUS_IMG_REPORT SyslinuxSourceReport(const RUFUS_IMG_REPORT& report)
{
    RUFUS_IMG_REPORT selected = report;
    if (HAS_REACTOS(report) && !HAS_SYSLINUX(report))
    {
        selected.has_syslinux = 1;
        selected.sl_version = 0x0407;
        selected.sl_version_inferred = false;
        strcpy(selected.sl_version_str,"4.07");
        selected.sl_version_ext[0] = 0;
        strcpy(selected.cfg_path,"/syslinux.cfg");
    }
    return selected;
}

int CheckNativeSyslinuxGeometry(int filesystem, quint32 sectorBytes, quint64 offsetBytes, QString& problem)
{
    constexpr quint32 supportedSectorBytes = 512;
    const bool fat = filesystem == FS_FAT16 || filesystem == FS_FAT32;
    const bool supportedFilesystem = fat || filesystem == FS_NTFS;
    // mtools takes a 32-bit byte offset; NTFS records 32-bit hidden sectors.
    const quint64 maximumOffset = fat ? UINT32_MAX : quint64(UINT32_MAX) * supportedSectorBytes;
    problem.clear();
    if (!supportedFilesystem)
        problem = QStringLiteral("The native Syslinux installer requires FAT16, FAT32 or NTFS.");
    else if (sectorBytes != supportedSectorBytes)
        problem = QStringLiteral("The native Syslinux installer requires 512-byte sectors.");
    else if (offsetBytes > maximumOffset)
        problem = QStringLiteral("The native Syslinux installer cannot represent this volume offset.");
    else if (offsetBytes % supportedSectorBytes != 0)
        problem = QStringLiteral("The Syslinux volume offset must be sector aligned.");
    return problem.isEmpty() ? 0 : ENOTSUP;
}

int CheckNativeSyslinuxSelection(const RUFUS_IMG_REPORT& original, int filesystem,
    quint32 sectorBytes, quint64 offsetBytes, QString& problem)
{
    const RUFUS_IMG_REPORT report = SyslinuxSourceReport(original);
    problem.clear();
    if (!HAS_SYSLINUX(report))
    {
        problem = QStringLiteral("The image has no selected Syslinux configuration.");
        return ENOTSUP;
    }
    const int error = CheckNativeSyslinuxGeometry(filesystem, sectorBytes, offsetBytes, problem);
    if (error != 0)
        return error;
    const int major = SL_MAJOR(report.sl_version);
    if (major < 3 || major > 6) {
        problem = QStringLiteral("The image needs a recognized Syslinux major version 3, 4, 5 or 6."); return ENOTSUP;
    }
    if (report.sl_version_inferred && report.sl_version != nativeSyslinuxVersion && report.sl_version != 0x407) return ENOTSUP;
    return 0;
}

int CheckNativeSyslinuxPair(const RUFUS_IMG_REPORT& report, int filesystem,
    quint32 sectorBytes, quint64 offsetBytes, QByteArrayView module, QString& problem)
{
    const int error = CheckNativeSyslinuxSelection(report, filesystem, sectorBytes, offsetBytes, problem);
    if (error != 0)
        return error;
    return CheckNativeSyslinuxModule(module, problem);
}

int CheckNativeSyslinuxModule(QByteArrayView module, QString& problem)
{
    problem.clear();
    // Identity is mandatory for unknown-image inference and any root module
    // replaced or verified after installation, even for a known source version.
    constexpr char qualifiedModuleHash[] = "d3472c02263acf9cd1da5db51e263c5484bad13ea68618c403d9cb01ca070aee";
    if (module.size() != nativeSyslinuxModuleBytes)
    {
        problem = QStringLiteral("A matching 6.04-pre1 BIOS ldlinux.c32 companion is required.");
        return ENOTSUP;
    }
    const QByteArray bytes = QByteArray::fromRawData(module.data(), module.size());
    const QByteArray digest = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
    if (digest != qualifiedModuleHash)
    {
        problem = QStringLiteral("The BIOS ldlinux.c32 does not match the qualified installer payload.");
        return ENOTSUP;
    }
    return 0;
}
}
