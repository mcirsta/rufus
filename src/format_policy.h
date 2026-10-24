/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "image_report.h"

/* File system indexes in our FS combobox. Keep these values stable. */
enum fs_type {
	FS_UNKNOWN = -1,
	FS_FAT16 = 0,
	FS_FAT32,
	FS_NTFS,
	FS_UDF,
	FS_EXFAT,
	FS_REFS,
	FS_EXT2,
	FS_EXT3,
	FS_EXT4,
	FS_MAX
};

enum boot_type {
	BT_NON_BOOTABLE = 0,
	BT_MSDOS,
	BT_FREEDOS,
	BT_IMAGE,
	BT_SYSLINUX_V4,		// Start of indexes that only display in advanced mode
	BT_SYSLINUX_V6,
	BT_REACTOS,
	BT_GRUB4DOS,
	BT_GRUB2,
	BT_UEFI_NTFS,
	BT_MAX
};

enum target_type {
	TT_BIOS = 0,
	TT_UEFI,
	TT_MAX
};

#define FS_MASK(fs) (UINT32_C(1) << (fs))

typedef struct {
	int boot_type;
	int target_type;
	bool image_selected;
	bool windows_to_go;
	bool allow_dual_uefi_bios;
} RUFUS_FS_OPTIONS;

/* Original extra-partition flags used by the Windows layout builder. */
#define XP_MSR                              0x01
#define XP_ESP                              0x02
#define XP_UEFI_NTFS                        0x04
#define XP_COMPAT                           0x08
#define XP_PERSISTENCE                      0x10

typedef struct {
	int boot_type;
	int fs_type;
	int target_type;
	bool gpt;
	bool windows_to_go;
	bool write_as_image;
	bool allow_dual_uefi_bios;
	bool old_bios_fixes;
	uint64_t persistence_size;
} RUFUS_PARTITION_OPTIONS;

/* WriteMBR's code choice, independent of the native disk writer. */
enum rufus_mbr_type {
	RUFUS_MBR_ZERO,
	RUFUS_MBR_PROTECTIVE,
	RUFUS_MBR_SYSLINUX,
	RUFUS_MBR_GRUB2,
	RUFUS_MBR_GRUB4DOS,
	RUFUS_MBR_REACTOS,
	RUFUS_MBR_KOLIBRIOS,
	RUFUS_MBR_RUFUS,
	RUFUS_MBR_WIN7
};

enum rufus_boot_install_type {
	RUFUS_BOOT_INSTALL_NONE,
	RUFUS_BOOT_INSTALL_EFI_CHECK,
	RUFUS_BOOT_INSTALL_SYSLINUX,
	RUFUS_BOOT_INSTALL_PBR
};

/* WritePBR's record choice after the caller selects the PBR operation. */
enum rufus_pbr_type {
	RUFUS_PBR_UNSUPPORTED,
	RUFUS_PBR_NONE,
	RUFUS_PBR_FAT16_STANDARD,
	RUFUS_PBR_FAT16_FREEDOS,
	RUFUS_PBR_FAT16_REACTOS,
	RUFUS_PBR_FAT32_STANDARD,
	RUFUS_PBR_FAT32_FREEDOS,
	RUFUS_PBR_FAT32_REACTOS,
	RUFUS_PBR_FAT32_KOLIBRIOS,
	RUFUS_PBR_FAT32_PE,
	RUFUS_PBR_FAT32_NT,
	RUFUS_PBR_NTFS
};

typedef struct {
	int boot_type;
	int fs_type;
	int target_type;
	bool gpt;
	bool allow_dual_uefi_bios;
	bool use_rufus_mbr;
	bool needs_masquerading;
} RUFUS_BOOT_OPTIONS;

enum { VOLUME_LABEL_CAPACITY = 33, FAT_VOLUME_LABEL_LENGTH = 11 };
typedef struct {
	uint32_t Allowed;
	uint32_t Default;
} RUFUS_CLUSTER_SIZES;

#ifdef __cplusplus
extern "C" {
#endif

/* Original EnableQuickFormat: large FAT32 and ReFS require quick mode. */
bool SupportsFullFormat(int filesystem, uint64_t disk_bytes);

/* Original WriteMBR priority, including the Windows/Alt+E BIOS exception.
 * GetBootInstallType chooses the later Syslinux/PBR operation, not PBR bytes.
 * report/options are required. Callers still own validation, native writes,
 * the separately selected SBR, and post-extraction BIOS SetupWinPE. */
enum rufus_mbr_type GetMasterBootRecord(const RUFUS_IMG_REPORT* report,
	const RUFUS_BOOT_OPTIONS* options);
enum rufus_boot_install_type GetBootInstallType(const RUFUS_IMG_REPORT* report,
	const RUFUS_BOOT_OPTIONS* options);

/* Original WritePBR priority for the actual filesystem. No target/Alt+E
 * operation gate is implied: call GetBootInstallType separately. report is
 * required for BT_IMAGE. Unsupported includes FAT16 KolibriOS; NONE is ext.
 * Geometry checks, writer calls, drive ID and post-copy fixups stay native. */
enum rufus_pbr_type GetPartitionBootRecord(int boot_type, int actual_fs_type,
	const RUFUS_IMG_REPORT* report);

/* WriteSBR's precedence: image Syslinux prevents GRUB SBR selection, GRUB2
 * overrides Grub4DOS, and GPT bootable jobs use BT_MAX's protective message.
 * report is required for BT_IMAGE; this chooses a record, not payload validity. */
int GetSecondaryBootType(int boot_type, const RUFUS_IMG_REPORT* report, bool gpt);

/* Rufus's image compatibility rule, including the dual-mode install.wim
 * exception. Compatibility does not mean that a host implements WIM splitting. */
bool IsFat32Compatible(const RUFUS_IMG_REPORT* report, bool allow_dual_uefi_bios);

/* SetAllowedFileSystems without controls, globals or logging. Report/options
 * and forced_fat must be non-NULL. windows_to_go means the applicable image
 * option is selected. The caller handles no-device selection and intersects
 * the result with its drive/formatter capabilities. forced_fat requests the
 * original warning when GRUB rules exclude NTFS but the image needs it. */
uint32_t GetAllowedFileSystems(const RUFUS_IMG_REPORT* report,
	const RUFUS_FS_OPTIONS* options, bool* forced_fat);

/* SetFSFromISO's preference order. fs_mask comes from the caller; no bits are
 * added here. Return FS_UNKNOWN if no preference applies. windows_to_go must
 * include the caller's image-option availability check. The caller owns the
 * current/default selection and only selects a preference present in its UI. */
int GetPreferredFileSystem(const RUFUS_IMG_REPORT* report,
	const RUFUS_FS_OPTIONS* options, uint32_t fs_mask, int preselected_fs);

/* FormatThread's extra-partition decision, before native layout/locking work.
 * Report/options are non-NULL; windows_to_go includes the caller's image-option
 * availability check. The result requests partitions; it does not validate
 * geometry or a platform's support. DD callers retain their whole-image path:
 * write_as_image suppresses persistence here, as in the original decision. */
uint8_t GetExtraPartitions(const RUFUS_IMG_REPORT* report,
	const RUFUS_PARTITION_OPTIONS* options);

/* ToValidLabel's rules, using UTF-16 code units on every platform. Source is
 * terminated; output has VOLUME_LABEL_CAPACITY units and may not alias source.
 * Returns true when the FAT underscore rule selected the English size label.
 * Conversion, reporting and copying to application buffers belong to callers. */
bool NormalizeVolumeLabel(const uint16_t* source, uint16_t label[VOLUME_LABEL_CAPACITY],
	bool fat, uint64_t disk_size);

/* Original EnableUefiValidation eligibility. This checks a source/selection,
 * not validator availability, source content, or an explicit later DD choice. */
bool CanUseRuntimeUefiValidation(const RUFUS_IMG_REPORT* report, int boot_type,
	bool windows_to_go, bool bios_target, bool allow_dual_uefi_bios);

/* Original FAT16/FAT32/NTFS/exFAT allowed/default cluster calculations, including sector-size
 * filtering. A zero result means unavailable. Native formatters may support a
 * smaller subset; validate actual partition geometry before offering it. */
RUFUS_CLUSTER_SIZES GetFat16ClusterSizes(uint64_t disk_size, uint32_t sector_size);
RUFUS_CLUSTER_SIZES GetFat32ClusterSizes(uint64_t disk_size, uint32_t sector_size);
RUFUS_CLUSTER_SIZES GetNtfsClusterSizes(uint64_t disk_size, uint32_t sector_size);
RUFUS_CLUSTER_SIZES GetExfatClusterSizes(uint64_t disk_size, uint32_t sector_size);
/* FormatLargeFAT32's default when no allocation unit was requested. */
uint32_t GetLargeFat32DefaultClusterSize(uint64_t volume_size);

#ifdef __cplusplus
}
#endif
