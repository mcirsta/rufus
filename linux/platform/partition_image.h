/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "format_policy.h"

enum { RUFUS_EXT_DEFAULT_CLUSTER = 0x100 }; // Rufus's ext2/ext3 "Default" choice.

typedef struct {
	uint64_t disk_size;
	uint32_t sector_size;
	uint32_t sectors_per_track;
	uint32_t cluster_size;
	uint64_t device;  // Expected st_rdev for a block device; zero requires a regular file.
	uint64_t uefi_ntfs_size;  // Optional final bundled payload.
	uint64_t persistence_size; // Optional Linux partition before the payload.
	bool efi_system; // Main partition is an ESP for ISO -> ESP mode.
	bool windows_to_go; // GPT ESP + MSR before the main Windows NTFS partition.
	uint32_t physical_sector_size; // Zero defaults to sector_size for file/legacy callers.
} GPT_IMAGE_OPTIONS;

typedef struct {
	bool started;
	int sync_error;
	uint64_t offset;
	uint64_t size;
	uint64_t uefi_ntfs_offset;
	uint64_t uefi_ntfs_size;
	uint64_t persistence_offset;
	uint64_t persistence_size;
	uint64_t esp_offset;
	uint64_t esp_size;
	uint64_t msr_offset;
	uint64_t msr_size;
} GPT_IMAGE_RESULT;

typedef enum {
	MBR_BOOT_ZERO,
	MBR_BOOT_SYSLINUX,
	MBR_BOOT_RUFUS,
	MBR_BOOT_WIN7,
	MBR_BOOT_GRUB2,
	MBR_BOOT_RUFUS_MASQUERADE, // Original Rufus code with WinPE's 0x81 partition flag.
	MBR_BOOT_GRUB4DOS,
	MBR_BOOT_REACTOS,
	MBR_BOOT_KOLIBRIOS
} MBR_BOOT_CODE;

typedef struct {
	uint64_t disk_size;
	uint32_t sector_size;
	uint32_t sectors_per_track;
	uint32_t cluster_size;
	uint32_t disk_signature; // Retained by BIOS boot code; zero code clears it, as Win32 does.
	uint64_t device;  // Expected st_rdev for a block device; zero requires a regular file.
	int file_system;  // Explicit FS selection; ext2/ext3 layout is GRUB2-only.
	MBR_BOOT_CODE boot_code; // BIOS code requires an eligible active partition.
	uint64_t uefi_ntfs_size; // Optional trailing payload on BIOS NTFS or zero-code NTFS/exFAT.
	uint64_t persistence_size; // Optional Linux partition before the payload.
	bool old_bios_fixes; // Shared legacy alignment and optional final compatibility track.
	bool retain_disk_signature; // To Go UEFI/MBR needs a stable BCD identity even with zero bootstrap code.
	bool masquerade_disk; // Final WinPE partition flag; independent of the original selected MBR code.
	uint32_t physical_sector_size; // Zero defaults to sector_size for file/legacy callers.
} MBR_IMAGE_OPTIONS;

typedef struct {
	bool started;
	int sync_error;
	uint64_t offset;
	uint64_t size;
	uint64_t uefi_ntfs_offset;
	uint64_t uefi_ntfs_size;
	uint64_t persistence_offset;
	uint64_t persistence_size;
	uint64_t compatibility_offset;
	uint64_t compatibility_size;
} MBR_IMAGE_RESULT;

typedef struct {
	uint64_t bytes_written;
	int sync_error;
} MBR_CORE_RESULT;

typedef struct {
	uint64_t disk_size;
	uint32_t sector_size;
	uint64_t device; // Expected st_rdev; zero requires a regular file.
	uint32_t physical_sector_size; // Zero defaults to sector_size for file/legacy callers.
} SFD_IMAGE_OPTIONS;

typedef struct {
	bool started;
	int sync_error;
	uint64_t offset;
	uint64_t size;
} SFD_IMAGE_RESULT;

#ifdef __cplusplus
extern "C" {
#endif

/* SFD uses the whole device at byte zero without a partition table, matching
 * Win32 CreatePartition's SFD branch. Plan opens no target and does not round
 * the capacity to tracks/clusters. Callers must validate the selected formatter
 * separately; this layout alone does not establish filesystem support.
 * Accepts nonempty, sector-aligned media and 512..4096-byte logical sectors. */
int PlanSfdImageLayout(const SFD_IMAGE_OPTIONS* options, SFD_IMAGE_RESULT* result);

/* Remove old disk metadata using Rufus's ClearMBRGPT ranges (bounded to the
 * actual capacity), flush and verify zeroes. Create no table or filesystem.
 * Shares the MBR writer's retained-descriptor, exact-capacity, identity and
 * caller-owned exclusive-access contract. Flush even after partial failure or
 * cancellation; failed tail clearing is fatal rather than Windows's warning.
 * started=true means partial destruction is possible, with no rollback.
 * Preserve descriptor flags/position. No kernel table refresh, authorization,
 * formatter or mounting; block-device integration requires separate validation. */
int PrepareSfdTarget(int fd, const SFD_IMAGE_OPTIONS* options, SFD_IMAGE_RESULT* result);

/* Geometry-only planning before authorization, without opening a target.
 * The descriptor-based planner repeats it and validates actual device facts.
 * GPT accepts clusters through 32 MiB independently of filesystem/payload;
 * callers must also validate the extent with the selected formatter. */
int PlanGptImageLayout(const GPT_IMAGE_OPTIONS* options, GPT_IMAGE_RESULT* result);

/* Validate geometry/descriptor and calculate the main extent without writing
 * or changing the descriptor position. CreateGptImagePartition repeats this
 * check; a successful preflight is not a promise that later writes can proceed. */
int PlanGptImagePartition(int fd, const GPT_IMAGE_OPTIONS* options, GPT_IMAGE_RESULT* result);

/* Read-only verification of the stored table against the expected layout,
 * including both GPT copies, partition types/names/attributes and extents.
 * Applies the same target checks as planning and preserves the seek position.
 * Returns planned extents only on success; never creates or repairs a table. */
int CheckGptImagePartition(int fd, const GPT_IMAGE_OPTIONS* options, GPT_IMAGE_RESULT* result);

/* Create an ordinary Rufus GPT with a "Main Data Partition" of Microsoft
 * Basic Data type, an optional "Linux Persistence" Linux filesystem partition,
 * and optionally a trailing "UEFI:NTFS" Basic Data partition
 * with the no-drive-letter attribute. Native callers must validate both kernel
 * partition extents after rereading the table and retain ownership through handoff.
 * This writes only the table, not the UEFI:NTFS payload or either filesystem.
 * Uses common Rufus placement arithmetic and libfdisk for the
 * table/CRCs/IDs. Accepts a caller-owned, exclusively borrowed regular file or
 * block descriptor, O_RDWR, exact expected size, 512/1024/2048/4096-byte logical sectors.
 * A block request must supply the expected device number from the accepted plan.
 * The caller must retain exclusive device ownership (UDisks OpenDevice with
 * O_EXCL); this function checks identity/capacity/sectors/read-only state, but
 * does not acquire authorization or detect mounted children itself.
 * Replaces partition metadata; does not wipe old filesystem signatures or data.
 *
 * Returns 0 or errno. Recheck size before writing; flush after any attempted
 * write, including failure/cancellation, then reopen the table through libfdisk
 * to verify the stored layout. Cancellation is checked between library calls;
 * an in-progress metadata write/flush finishes before returning. started=true
 * means failure can leave a partial table; there is no rollback. Offset/size
 * describe the planned main extent, valid for use only after success.
 * The caller's descriptor, flags and seek position remain owned by the caller.
 * windows_to_go instead creates a 260-MiB ESP and 128-MiB MSR before the main
 * partition, using drive.c's track/cluster alignment. It is mutually exclusive
 * with persistence, UEFI:NTFS and a main ESP. Result supplies all three extents;
 * the Windows partition is then number 3. No boot files/filesystems are written.
 * Does not reread the kernel partition table or mount anything. ESP mode, other
 * extra partitions, MBR and the service/mount handoff remain separate. */
int CreateGptImagePartition(int fd, const GPT_IMAGE_OPTIONS* options, GPT_IMAGE_RESULT* result);

/* MBR data partition with shared ordinary or legacy-BIOS Rufus placement,
 * 512..4096-byte sectors and at most 2^32 logical sectors. Non-bootable FAT16,
 * FAT32, NTFS and exFAT use an inactive partition and zeroed first 446 bytes,
 * including the disk signature, matching Win32 write_zero_mbr(). Syslinux,
 * Rufus, Windows 7, GRUB2 and GRUB4DOS code accept active FAT16/FAT32/NTFS partitions.
 * Only GRUB2 accepts ext2/ext3. The ext route
 * accepts only Rufus's 0x100 "Default" layout sentinel on disks at least
 * 256 MiB; it does not format the filesystem. BIOS code uses the
 * caller's per-job disk signature.
 * Explicit retain_disk_signature also retains it with zero bootstrap code;
 * native To Go uses this to bind UEFI/MBR BCD to a stable disk identity.
 * Rufus masquerading uses the same original MBR code with partition flag 0x81,
 * as Win32 WriteMBR for HAS_WINPE without /minint. Select it only from validated
 * image policy; ordinary Rufus/Win7 modes retain 0x80. Readback checks the raw
 * flag because libfdisk reports only 0x80 as bootable. This does not enable a
 * WinPE helper/GUI route or establish a complete legacy boot chain.
 * The caller explicitly selects the bootstrap;
 * this does not install a partition boot record or qualify a complete BIOS boot path.
 * BIOS NTFS can reserve the trailing UEFI:NTFS partition when the caller's
 * image policy requires it. Optional persistence precedes that payload as an
 * inactive 0x83 partition. The planner does not format either extra partition.
 * Standalone UEFI:NTFS has active NTFS/exFAT data and an inactive trailing 0xEF
 * partition, with zero boot code/signature as in Win32's forced-UEFI path.
 * The planner opens no media; callers separately validate filesystem geometry.
 * Standalone UEFI:NTFS device jobs and GUI exposure require their own qualification. */
int PlanMbrImageLayout(const MBR_IMAGE_OPTIONS* options, MBR_IMAGE_RESULT* result);

/* Borrow an exclusively owned O_RDWR regular file or block device of exactly
 * disk_size; preserve fd flags and position. A block request supplies st_rdev
 * in device and requires the caller to retain the exclusive whole-disk claim,
 * check disk sequence/use and own authorization, as for the GPT writer. Create
 * the DOS table with libfdisk and use the selected original Win32 Rufus MBR bytes.
 * Recheck, flush after every attempted write (including failure/cancellation),
 * then verify boot code, signature, active/type fields and layout by readback.
 * started=true means failure may leave incomplete metadata; no rollback.
 * No filesystem creation, Syslinux VBR installation, mount or authorization.
 * Existing non-DOS labels are rejected. Old partition/filesystem metadata beyond
 * sector zero is not erased: full disk preparation remains a caller step before
 * use on previously partitioned media. No kernel table refresh. */
int CreateMbrImage(int fd, const MBR_IMAGE_OPTIONS* options, MBR_IMAGE_RESULT* result);
/* Same descriptor/layout checks and readback, without writing or repairing. */
int CheckMbrImage(int fd, const MBR_IMAGE_OPTIONS* options, MBR_IMAGE_RESULT* result);

/* Place already-selected immutable GRUB2 core bytes at byte 512, as WriteSBR
 * does on Windows, independently of MBR choice. Require the selected original
 * GRUB2, Rufus, Win7, Rufus-masqueraded or intermediate Syslinux MBR/layout,
 * 512-byte sectors and enough space before the first partition. A source-backed
 * intermediate Syslinux MBR requires the caller's independent ReactOS/GRUB
 * dispatch checks and final original MBR publication. Reject zero code. Accept a
 * regular file or a retained exclusive block descriptor with expected device
 * identity/capacity, as for MBR.
 * The caller owns payload version/module compatibility and keeps the bytes and
 * descriptor exclusively borrowed until return. The device caller owns disk-sequence,
 * mount/use and authorization checks. No resource loading or GUI policy; accepting
 * a Windows MBR here does not enable a new image route or make it execute GRUB2.
 * Preserve all other bytes, fd flags and seek position. Return errno; success
 * includes fsync, full payload readback and another MBR/target check. Partial
 * writes are flushed after failure/cancellation; no rollback. sync_error records
 * a secondary flush failure. Readback can use the OS cache. Does not write PBRs,
 * format, mount, authorize, or establish bootability of arbitrary payloads. */
int WriteGrub2CoreImage(int fd, const MBR_IMAGE_OPTIONS* options,
	const unsigned char* core, uint64_t core_size, MBR_CORE_RESULT* result);

/* Same ownership, bounded-write, flush and readback contract as the GRUB2
 * writer. Accept the complete selected grldr.mbr resource and skip its first
 * 512 bytes, exactly as Windows WriteSBR. MBR bytes are written separately from
 * the original ms-sys mbr_grub.h array, never from this resource's first sector.
 * Require GRUB4DOS, an original Windows MBR or the checked intermediate Syslinux
 * MBR described above. The caller owns compatibility
 * and installation of grldr; this operation alone does not enable a GUI route
 * or establish a complete boot chain. */
int WriteGrub4dosSbrImage(int fd, const MBR_IMAGE_OPTIONS* options,
	const unsigned char* grldr_mbr, uint64_t resource_size, MBR_CORE_RESULT* result);

/* Prepare previously used media before CreateMbrImage: apply Rufus's
 * ClearMBRGPT and new-partition-prefix clearing ranges, flush and verify zeroes,
 * then create/read back the MBR. This removes ordinary primary/backup GPT and
 * filesystem metadata at the new partition start, not all old data/signatures
 * elsewhere on the disk. All ranges stay within disk/partition bounds.
 * Any clearing/flush/readback failure stops creation, including tail failures
 * that Windows tolerates. Cancellation still flushes attempted writes. started
 * includes clearing even if MBR creation never starts; there is no rollback.
 * Same ownership/identity contract as CreateMbrImage; no kernel refresh,
 * formatter, mount or installer is invoked. Caller must preflight those steps. */
int PrepareMbrTarget(int fd, const MBR_IMAGE_OPTIONS* options, MBR_IMAGE_RESULT* result);

#ifdef __cplusplus
}
#endif
