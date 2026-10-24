/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "fat_format.h"
#include "format_policy.h"

typedef enum {
	FAT_BOOT_STANDARD,
	FAT_BOOT_FREEDOS,
	FAT_BOOT_WINDOWS, // WritePBR's standard FAT16 or BOOTMGR FAT32 PE record.
	FAT_BOOT_REACTOS,
	FAT_BOOT_WINPE, // WritePBR's standard FAT16 or legacy WinPE FAT32 NT record.
	FAT_BOOT_KOLIBRI // Original KolibriOS FAT32 record; FAT16 is unsupported.
} FAT_BOOT_CODE;

typedef struct {
	uint64_t bytes_written;
	int sync_error;
} FAT_BOOT_RESULT;

#ifdef __cplusplus
extern "C" {
#endif

/* Install Rufus's original standard, FreeDOS, ReactOS, KolibriOS or Windows partition boot records on a newly formatted,
 * exclusively borrowed regular file. file_system is FS_FAT16 or FS_FAT32;
 * options are the successful native formatter request. No block devices, MBR,
 * payload copy, authorization or bootability claim in this entry point.
 *
 * Stage both FAT32 copies before writing (sectors 0..2 and 6..8); FAT16 uses
 * sector 0 only; KolibriOS refuses FAT16. Windows/legacy WinPE FAT32 also stage/write tail sectors 12 and 18, which
 * must fit in the reserved area. Preserve the BPB except Rufus's drive-ID and
 * boot-label changes. The directory volume label and all bytes outside those
 * prefixes/tails stay intact, including the gaps between them.
 * Return errno; success includes fsync and exact record readback. Partial writes
 * are flushed even after failure/cancellation; no rollback. sync_error remains
 * available when a prior error takes precedence. Readback can use the OS cache.
 * Preserve descriptor position/ownership. Serialize with other Rufus FAT jobs
 * because the existing formatter validator uses libfat's sector globals. */
/* Executable legacy BIOS payloads require 512-byte logical sectors.
 * Native 4-KiB data/UEFI FAT formatting is independent of this installer. */
int InstallFatBootRecords(int fd, int file_system, FAT_BOOT_CODE boot_code, const FAT_FORMAT_OPTIONS* options,
	FAT_BOOT_RESULT* result);

/* FAT32 adaptation for the supported BOOTMGR-only legacy WinPE source contract.
 * Reuse original PE code; preserve each copy's FSInfo allocation hints and use
 * 0x81 when the shared WINPE_SETUP_PLAN.masquerade_disk is true, otherwise 0x80.
 * Same regular-file ownership, geometry, staging, sync and readback guarantees.
 * Caller supplies a valid, privately owned FAT32 volume after format/file copy;
 * hints are preserved, not recomputed or checked against FAT allocation. This
 * does not validate source files, enable FAT16, or qualify Windows Setup. */
int InstallWinPeFatBootRecords(int fd, bool masquerade_disk, const FAT_FORMAT_OPTIONS* options,
	FAT_BOOT_RESULT* result);

/* Same writer on a retained whole-disk descriptor. Verify block identity,
 * capacity and FAT geometry before/after writing through the native formatter
 * validator. Caller owns authorization, exclusivity, disk sequence and MBR
 * checks; never mount the partition while installing these records. */
int InstallFatDeviceBootRecords(int fd, uint64_t device, uint64_t capacity, int file_system,
	FAT_BOOT_CODE boot_code, const FAT_FORMAT_OPTIONS* options, FAT_BOOT_RESULT* result);

/* Same WinPE FAT32 adaptation on the retained whole disk. Caller must finish
 * private-mount sync/unmount and verify the original MBR before installing.
 * Device/geometry checks, staged records, fsync and exact readback are shared
 * with the regular-file writer. Install the final masquerading MBR afterwards. */
int InstallWinPeFatDeviceBootRecords(int fd, uint64_t device, uint64_t capacity, bool masquerade_disk,
	const FAT_FORMAT_OPTIONS* options, FAT_BOOT_RESULT* result);

/* Same legacy FAT32 fixups for the shared dispatch's BOOTMGR PE or NT-loader
 * PBR. Only FAT_BOOT_WINDOWS and FAT_BOOT_WINPE are accepted. */
int InstallLegacyFatDeviceBootRecords(int fd, uint64_t device, uint64_t capacity,
	FAT_BOOT_CODE boot_code, bool masquerade_disk, const FAT_FORMAT_OPTIONS* options, FAT_BOOT_RESULT* result);

#ifdef __cplusplus
}
#endif
