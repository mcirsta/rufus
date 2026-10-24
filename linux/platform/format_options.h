// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "format_policy.h"
#include "partition_image.h"
#include <QString>

namespace rufus
{
struct PrepareImageRequest;

// Recompute the same native layout/settings in the GUI and authorized helper.
bool ValidFormatGeometry(const PrepareImageRequest& request);
int PlanFormatPartition(const PrepareImageRequest& request, GPT_IMAGE_RESULT& partition);
int CheckFormatSettings(const PrepareImageRequest& request, const GPT_IMAGE_RESULT& partition);
quint32 FormatFatClusterSize(const PrepareImageRequest& request, const GPT_IMAGE_RESULT& partition);
quint32 FormatLayoutClusterSize(const PrepareImageRequest& request);
MBR_BOOT_CODE FormatMbrBootCode(const PrepareImageRequest& request);

enum class PartitionScheme { ImageDefault = -1, Mbr, Gpt, Sfd };

QString NormalizeFormatLabel(const QString& text, quint64 diskSize, bool ntfs);
// Explicit FS_* selection for native geometry/formatter preflight. This is not
// the creation-job support mask; image/bootloader compatibility is checked separately.
// BT_NON_BOOTABLE omits the payload; BT_UEFI_NTFS reserves it for NTFS/exFAT
// on GPT/MBR; BT_FREEDOS requires MBR and FAT16/FAT32.
// Other filesystem geometry remains available for Win32's default
// marker before GetAllowedFileSystems filters the boot selection.
// imageUefiNtfs reserves the extra partition selected by the shared image policy.
// windowsBoot selects the original Rufus/Windows 7 bootstrap for a Windows NTFS
// image, instead of using GRUB geometry for every non-Syslinux MBR image.
RUFUS_CLUSTER_SIZES NativeFileSystemClusterSizes(quint64 diskSize, int fileSystem, bool syslinux = false,
    int bootType = BT_IMAGE, PartitionScheme scheme = PartitionScheme::Gpt, bool imageUefiNtfs = false,
    MBR_BOOT_CODE windowsBoot = MBR_BOOT_ZERO, bool oldBiosFixes = false,
    bool windowsToGo = false, int windowsTarget = TT_UEFI, bool sizeChecks = true,
    quint32 logicalSectorSize = 512, quint32 physicalSectorSize = 512);
RUFUS_CLUSTER_SIZES NativeClusterSizes(quint64 diskSize, bool ntfs, bool syslinux = false);
}
