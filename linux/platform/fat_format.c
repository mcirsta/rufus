/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "fat_format.h"
#include "target_io.h"
#include "zero_write.h"
#include "format_policy.h"
#include "progress_policy.h"
#include "rufus_reporting.h"
#include "syslinux/libfat/libfat.h"
#include "syslinux/libfat/fat.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/fs.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <unistd.h>

enum {
	FAT_SECTOR_BYTES = 512,
	FAT32_MAX_CLUSTER_BYTES = 64 * 1024,
	FAT16_MAX_CLUSTER_BYTES = 64 * 1024,
	FAT16_MIN_CLUSTERS = 4085,
	FAT16_MAX_CLUSTERS = 65524,
	FAT16_RESERVED_SECTORS = 1,
	FAT16_ROOT_ENTRIES = 512,
	FAT16_ENTRY_BYTES = sizeof(uint16_t),
	FAT_COPIES = 2,
	FAT32_RESERVED_SECTORS = 32,
	FAT32_BACKUP_SECTOR = 6,
	FAT_RESERVED_CLUSTERS = 2,
	FAT32_ENTRY_BYTES = sizeof(uint32_t),
	FAT32_MIN_CLUSTERS = 65525,
	FAT32_MAX_CLUSTERS = 0x0ffffff4,
	FORMAT_HEADS = 255,
	FORMAT_SECTORS_PER_TRACK = 63,
	// mkfs.fat can round down by at most one track minus one sector.
	FORMAT_MAX_UNUSED_SECTORS = FORMAT_SECTORS_PER_TRACK - 1,
	MKFS_BLOCK_BYTES = 1024
};

static uint32_t SectorBytes(const FAT_FORMAT_OPTIONS* options)
{
	return options->sector_size != 0 ? options->sector_size : FAT_SECTOR_BYTES;
}

static uint32_t PhysicalSectorBytes(const FAT_FORMAT_OPTIONS* options)
{
	return options->physical_sector_size != 0 ? options->physical_sector_size : FAT_SECTOR_BYTES;
}

static int CheckFat16ClusterCount(uint64_t sectors, uint32_t cluster_sectors, uint32_t sector_bytes)
{
	const uint64_t clusters = sectors / cluster_sectors;
	// Upper FAT/alignment bounds establish the minimum usable cluster count.
	uint64_t fat_sectors = ((clusters + FAT_RESERVED_CLUSTERS) * FAT16_ENTRY_BYTES + sector_bytes - 1) / sector_bytes;
	fat_sectors = (fat_sectors + cluster_sectors - 1) / cluster_sectors * cluster_sectors;
	const uint64_t root_sectors = FAT16_ROOT_ENTRIES * sizeof(struct fat_dirent) / sector_bytes;
	const uint64_t aligned_root = root_sectors > cluster_sectors ? root_sectors : cluster_sectors;
	const uint64_t overhead_sectors = cluster_sectors + aligned_root + FAT_COPIES * fat_sectors + FORMAT_MAX_UNUSED_SECTORS;
	if (sectors <= overhead_sectors)
		return ENOTSUP;
	const uint64_t minimum_clusters = (sectors - overhead_sectors) / cluster_sectors;
	if (minimum_clusters < FAT16_MIN_CLUSTERS)
		return ENOTSUP;
	// A lower FAT bound rules out FAT32-sized results, including when -a
	// disables alignment for an odd sector count. Readback checks the result.
	const uint64_t minimum_fat = ((minimum_clusters + FAT_RESERVED_CLUSTERS) * FAT16_ENTRY_BYTES
		+ sector_bytes - 1) / sector_bytes;
	const uint64_t minimum_overhead = FAT16_RESERVED_SECTORS + root_sectors + FAT_COPIES * minimum_fat;
	if (sectors <= minimum_overhead)
		return ENOTSUP;
	const uint64_t maximum_clusters = (sectors - minimum_overhead) / cluster_sectors;
	return maximum_clusters <= FAT16_MAX_CLUSTERS ? 0 : ENOTSUP;
}

static int CheckFormatOptions(const FAT_FORMAT_OPTIONS* options, bool fat16)
{
	if (options == NULL)
		return EINVAL;
	const uint32_t sector_bytes = SectorBytes(options);
	const uint32_t physical_bytes = PhysicalSectorBytes(options);
	const bool valid_sector = sector_bytes >= 512 && sector_bytes <= 4096 && (sector_bytes & (sector_bytes - 1)) == 0;
	const bool valid_physical = physical_bytes >= sector_bytes && physical_bytes <= 65536 &&
		(physical_bytes & (physical_bytes - 1)) == 0;
	if (!valid_sector || !valid_physical)
		return EINVAL;
	uint64_t sectors, clusters, fat_sectors, reserved_sectors, overhead_sectors;
	uint32_t cluster_bytes = options->cluster_size;
	uint32_t cluster_sectors;
	size_t length;

	const bool sector_aligned = options->offset % sector_bytes == 0 &&
		options->size % sector_bytes == 0;
	const bool fits_fat_sector_fields = options->offset / sector_bytes <= UINT32_MAX &&
		options->size / sector_bytes <= UINT32_MAX;
	const bool cluster_in_range = cluster_bytes >= sector_bytes &&
		cluster_bytes <= (fat16 ? FAT16_MAX_CLUSTER_BYTES : FAT32_MAX_CLUSTER_BYTES);
	const bool cluster_is_power_of_two = cluster_bytes != 0 &&
		(cluster_bytes & (cluster_bytes - 1)) == 0;

	if (!sector_aligned || !fits_fat_sector_fields)
		return EINVAL;
	if (!cluster_in_range || !cluster_is_power_of_two)
		return EINVAL;
	length = strnlen(options->label, sizeof(options->label));
	const bool label_too_long = length > FAT32_LABEL_LENGTH;
	const bool label_has_edge_spaces = length != 0 &&
		(options->label[0] == ' ' || options->label[length - 1] == ' ');
	if (label_too_long || label_has_edge_spaces)
		return EINVAL;
	for (size_t i = 0; i < length; i++) {
		unsigned char ch = (unsigned char)options->label[i];
		const bool printable_ascii = ch >= ' ' && ch <= '~';
		const bool lowercase = ch >= 'a' && ch <= 'z';
		const bool forbidden_punctuation = strchr("*?.,;:/\\|+=<>[]\"", ch) != NULL;
		if (!printable_ascii || lowercase || forbidden_punctuation)
			return EINVAL;
	}
	// Conservative space check using an upper bound for the two aligned FATs.
	// Cluster selection remains with the caller, not mkfs.fat's defaults.
	sectors = options->size / sector_bytes;
	cluster_sectors = cluster_bytes / sector_bytes;
	clusters = sectors / cluster_sectors;
	if (fat16)
		return CheckFat16ClusterCount(sectors, cluster_sectors, sector_bytes);
	fat_sectors = ((clusters + FAT_RESERVED_CLUSTERS) * FAT32_ENTRY_BYTES + sector_bytes - 1) / sector_bytes;
	fat_sectors = (fat_sectors + cluster_sectors - 1) / cluster_sectors * cluster_sectors;
	reserved_sectors = cluster_sectors > FAT32_RESERVED_SECTORS ? cluster_sectors : FAT32_RESERVED_SECTORS;
	overhead_sectors = reserved_sectors + FAT_COPIES * fat_sectors + FORMAT_MAX_UNUSED_SECTORS;
	const bool has_data_space = sectors > overhead_sectors;
	if (!has_data_space)
		return ENOTSUP;
	const bool enough_clusters = (sectors - overhead_sectors) / cluster_sectors >= FAT32_MIN_CLUSTERS;
	const bool too_many_clusters = clusters > FAT32_MAX_CLUSTERS;
	if (!enough_clusters || too_many_clusters)
		return ENOTSUP;
	return 0;
}

int CheckFat32FormatOptions(const FAT_FORMAT_OPTIONS* options)
{
	return CheckFormatOptions(options, false);
}

int CheckFat16FormatOptions(const FAT_FORMAT_OPTIONS* options)
{
	return CheckFormatOptions(options, true);
}

static int CheckFormatRequest(int fd, const FAT_FORMAT_OPTIONS* options,
	uint64_t device, uint64_t capacity, bool fat16, struct stat* info)
{
	int error = CheckFormatOptions(options, fat16);
	if (error != 0)
		return error;
	if (SectorBytes(options) == FAT_SECTOR_BYTES && LIBFAT_SECTOR_SIZE != FAT_SECTOR_BYTES)
		return ENOTSUP;
	WRITABLE_TARGET target;
	error = InspectWritableTarget(fd, device, &target);
	if (error != 0)
		return error;
	*info = target.info;
	const bool block = device != 0;
	const bool partition = block && capacity == 0;
	if (block) {
		if (target.sector_size != SectorBytes(options) || target.physical_sector_size != PhysicalSectorBytes(options))
			return ENOTSUP;
		const uint64_t expected_size = partition ? options->size : capacity;
		if (target.size != expected_size)
			return ESTALE;
		if (partition)
			return IsRufusJobCancelled() ? ECANCELED : 0;
	} else {
		capacity = target.size;
	}
	if (options->offset > capacity)
		return ENOSPC;
	const uint64_t available_bytes = capacity - options->offset;
	if (options->size > available_bytes)
		return ENOSPC;
	return IsRufusJobCancelled() ? ECANCELED : 0;
}

int CheckFat32ImageRequest(int fd, const char* mkfs, const FAT_FORMAT_OPTIONS* options)
{
	struct stat info;
	if (mkfs == NULL || mkfs[0] != '/')
		return EINVAL;
	return CheckFormatRequest(fd, options, 0, 0, false, &info);
}

typedef struct {
	int fd;
	const FAT_FORMAT_OPTIONS* options;
	uint64_t data_offset;
} FAT_FORMAT_READER;

static int ReadFatSector(intptr_t data, void* buffer, size_t size, libfat_sector_t sector)
{
	FAT_FORMAT_READER* reader = (FAT_FORMAT_READER*)data;
	ssize_t count;
	const bool sector_in_range = sector < reader->options->size / FAT_SECTOR_BYTES;
	if (size != FAT_SECTOR_BYTES || !sector_in_range)
		return 0;
	do {
		count = pread(reader->fd, buffer, size, (off_t)(reader->data_offset + sector * FAT_SECTOR_BYTES));
	} while (count < 0 && errno == EINTR);
	return count < 0 ? 0 : (int)count;
}

static bool HasFat16Geometry(struct fat_bootsect* boot, uint64_t sectors,
	const FAT_FORMAT_OPTIONS* options)
{
	const uint32_t sector_bytes = SectorBytes(options);
	uint32_t root_entries = FAT16_ROOT_ENTRIES;
	const bool align_root = options->size % MKFS_BLOCK_BYTES == 0;
	if (align_root && options->cluster_size > root_entries * sizeof(struct fat_dirent))
		root_entries = options->cluster_size / sizeof(struct fat_dirent);
	const uint32_t fat_sectors = read16(&boot->bsFATsecs);
	const uint32_t reserved_sectors = read16(&boot->bsResSectors);
	const bool valid_fat_start = fat_sectors != 0 && reserved_sectors >= FAT16_RESERVED_SECTORS;
	if (!valid_fat_start || read16(&boot->bsRootDirEnts) != root_entries)
		return false;
	const uint64_t overhead = reserved_sectors + FAT_COPIES * fat_sectors
		+ root_entries * sizeof(struct fat_dirent) / sector_bytes;
	if (sectors <= overhead || boot->bsSecPerClust == 0)
		return false;
	const uint64_t clusters = (sectors - overhead) / boot->bsSecPerClust;
	return clusters >= FAT16_MIN_CLUSTERS && clusters <= FAT16_MAX_CLUSTERS;
}

static int CheckFormattedImage(int fd, const FAT_FORMAT_OPTIONS* options, uint64_t data_offset, bool fat16,
	FORMAT_RESULT* result)
{
	FAT_FORMAT_READER reader = { fd, options, data_offset };
	struct fat_bootsect boot;
	struct libfat_filesystem* fs;
	char label[FAT32_LABEL_LENGTH];
	const char* expected_label = options->label[0] ? options->label : "NO NAME";
	const char* mismatch = NULL;
	const uint32_t sector_bytes = SectorBytes(options);
	const uint64_t requested_sectors = options->size / sector_bytes;
	int error = EBADMSG;

	ssize_t count;
	do { count = pread(fd, &boot, sizeof(boot), (off_t)data_offset); } while (count < 0 && errno == EINTR);
	if (count < 0) return errno;
	if (count != sizeof(boot)) return EIO;
	memset(label, ' ', sizeof(label));
	memcpy(label, expected_label, strlen(expected_label));

	const uint64_t sectors = fat16 && read16(&boot.bsSectors) != 0
		? read16(&boot.bsSectors) : read32(&boot.bsHugeSectors);
	const bool valid_signature = read16(&boot.bsSignature) == BS_SIGNATURE;
	const bool sector_size_matches = read16(&boot.bsBytesPerSec) == sector_bytes;
	const bool has_expected_header = fat16 ? HasFat16Geometry(&boot, sectors, options)
		: read16(&boot.bsFATsecs) == 0 && read16(&boot.bsRootDirEnts) == 0;
	const bool fat_count_matches = boot.bsFATs == FAT_COPIES;
	const bool cluster_size_matches = boot.bsSecPerClust * sector_bytes == options->cluster_size;
	const bool fits_partition = sectors <= requested_sectors;
	const bool acceptable_unused_tail = fits_partition &&
		requested_sectors - sectors <= FORMAT_MAX_UNUSED_SECTORS;
	const bool partition_offset_matches = read32(&boot.bsHiddenSecs) == options->offset / sector_bytes;
	const uint32_t volume_id = fat16 ? read32(&boot.u.fat16.bsVolumeID) : read32(&boot.u.fat32.bsVolumeID);
	const char* volume_label = fat16 ? boot.u.fat16.bsVolumeLabel : boot.u.fat32.bsVolumeLabel;
	const bool volume_id_matches = volume_id == options->volume_id;
	const bool label_matches = memcmp(volume_label, label, sizeof(label)) == 0;

	if (!valid_signature)
		mismatch = "invalid boot signature";
	else if (!sector_size_matches)
		mismatch = "unexpected sector size";
	else if (!has_expected_header)
		mismatch = fat16 ? "invalid FAT16 geometry" : "FAT12/16 fields are nonzero";
	else if (!fat_count_matches)
		mismatch = "unexpected number of FAT copies";
	else if (!cluster_size_matches)
		mismatch = "cluster size differs from the request";
	else if (!fits_partition)
		mismatch = "filesystem extends past the requested partition";
	else if (!acceptable_unused_tail)
		mismatch = "unused partition tail exceeds formatter rounding";
	else if (!partition_offset_matches)
		mismatch = "hidden-sector offset differs from the request";
	else if (!volume_id_matches)
		mismatch = "volume ID differs from the request";
	else if (!label_matches)
		mismatch = "volume label differs from the request";
	if (mismatch != NULL) {
		uprintf("FAT%u verification failed: %s", fat16 ? 16 : 32, mismatch);
		return EBADMSG;
	}

	if (sector_bytes != FAT_SECTOR_BYTES) {
		// The shared libfat reader has process-global 512-byte geometry. Validate
		// the same FAT extent/cluster bounds with bounded native-sector reads,
		// leaving its globals and concurrent image scans unchanged.
		const uint64_t reserved = read16(&boot.bsResSectors);
		const uint64_t fat_sectors = fat16 ? read16(&boot.bsFATsecs) : read32(&boot.u.fat32.bpb_fatsz32);
		const uint64_t root_sectors = ((uint64_t)read16(&boot.bsRootDirEnts) * sizeof(struct fat_dirent)
			+ sector_bytes - 1) / sector_bytes;
		const uint64_t data_start = reserved + FAT_COPIES * fat_sectors + root_sectors;
		if (fat_sectors == 0 || data_start >= sectors || boot.bsSecPerClust == 0)
			return EBADMSG;
		const uint64_t clusters = (sectors - data_start) / boot.bsSecPerClust;
		const bool valid_clusters = fat16 ? clusters >= FAT16_MIN_CLUSTERS && clusters <= FAT16_MAX_CLUSTERS
			: clusters >= FAT32_MIN_CLUSTERS && clusters <= FAT32_MAX_CLUSTERS;
		const uint64_t entry_bytes = fat16 ? FAT16_ENTRY_BYTES : FAT32_ENTRY_BYTES;
		if (!valid_clusters || (clusters + FAT_RESERVED_CLUSTERS) * entry_bytes > fat_sectors * sector_bytes)
			return EBADMSG;
		const uint64_t root_cluster = fat16 ? 0 : read32(&boot.u.fat32.bpb_rootclus);
		if (!fat16 && (root_cluster < FAT_RESERVED_CLUSTERS || root_cluster >= clusters + FAT_RESERVED_CLUSTERS))
			return EBADMSG;
		const uint64_t root_sector = fat16 ? reserved + FAT_COPIES * fat_sectors
			: data_start + (root_cluster - FAT_RESERVED_CLUSTERS) * boot.bsSecPerClust;
		unsigned char first[4096], second[4096];
		if (!fat16) {
			const uint32_t backup_sector = read16(&boot.u.fat32.bpb_bkbootsec);
			const uint32_t fsinfo_sector = read16(&boot.u.fat32.bpb_fsinfo);
			if (backup_sector == 0 || backup_sector >= reserved || fsinfo_sector == 0 || fsinfo_sector >= reserved)
				return EBADMSG;
			do { count = pread(fd, first, sector_bytes, (off_t)(data_offset + backup_sector * sector_bytes)); }
			while (count < 0 && errno == EINTR);
			if (count < 0) return errno;
			if (count != sector_bytes) return EIO;
			struct fat_bootsect* backup = (struct fat_bootsect*)first;
			const size_t bpb_start = offsetof(struct fat_bootsect, bsBytesPerSec);
			const size_t bpb_end = offsetof(struct fat_bootsect, u.fat32.bsCode);
			if (read16(&backup->bsSignature) != BS_SIGNATURE ||
				memcmp((const unsigned char*)&boot + bpb_start, first + bpb_start, bpb_end - bpb_start) != 0)
				return EBADMSG;
			do { count = pread(fd, first, sector_bytes, (off_t)(data_offset + fsinfo_sector * sector_bytes)); }
			while (count < 0 && errno == EINTR);
			if (count < 0) return errno;
			if (count != sector_bytes) return EIO;
			if (read32((le32_t*)first) != 0x41615252 ||
				read32((le32_t*)(first + 484)) != 0x61417272 ||
				read32((le32_t*)(first + 508)) != 0xaa550000)
				return EBADMSG;
		}
		do { count = pread(fd, first, sector_bytes, (off_t)(data_offset + reserved * sector_bytes)); }
		while (count < 0 && errno == EINTR);
		if (count < 0) return errno;
		if (count != sector_bytes) return EIO;
		do { count = pread(fd, second, sector_bytes, (off_t)(data_offset + (reserved + fat_sectors) * sector_bytes)); }
		while (count < 0 && errno == EINTR);
		if (count < 0) return errno;
		if (count != sector_bytes) return EIO;
		if (memcmp(first, second, sector_bytes) != 0) return EBADMSG;
		do { count = pread(fd, first, sector_bytes, (off_t)(data_offset + root_sector * sector_bytes)); }
		while (count < 0 && errno == EINTR);
		if (count < 0) return errno;
		if (count != sector_bytes) return EIO;
		result->filesystem_bytes = sectors * sector_bytes;
		return 0;
	}
	if (LIBFAT_SECTOR_SIZE != FAT_SECTOR_BYTES) return EIO;
	fs = libfat_open(ReadFatSector, (intptr_t)&reader);
	if (fs == NULL)
		return EBADMSG;
	if (libfat_get_sector(fs, libfat_clustertosector(fs, 0)) != NULL) {
		result->filesystem_bytes = sectors * FAT_SECTOR_BYTES;
		error = 0;
	}
	libfat_close(fs);
	return error;
}

int CheckFat32Image(int fd, const FAT_FORMAT_OPTIONS* options, FORMAT_RESULT* result)
{
	struct stat info;
	if (result == NULL)
		return EINVAL;
	memset(result, 0, sizeof(*result));
	result->exit_code = -1;
	int error = CheckFormatRequest(fd, options, 0, 0, false, &info);
	if (error != 0)
		return error;
	return CheckFormattedImage(fd, options, options->offset, false, result);
}

static int CheckFatDevice(int fd, uint64_t device, uint64_t capacity, bool fat16,
	const FAT_FORMAT_OPTIONS* options, FORMAT_RESULT* result)
{
	struct stat info;
	if (result == NULL)
		return EINVAL;
	memset(result, 0, sizeof(*result));
	result->exit_code = -1;
	if (device == 0 || capacity == 0)
		return EINVAL;
	int error = CheckFormatRequest(fd, options, device, capacity, fat16, &info);
	if (error != 0)
		return error;
	return CheckFormattedImage(fd, options, options->offset, fat16, result);
}

int CheckFat32Device(int fd, uint64_t device, uint64_t capacity,
	const FAT_FORMAT_OPTIONS* options, FORMAT_RESULT* result)
{
	return CheckFatDevice(fd, device, capacity, false, options, result);
}

int CheckFat16Device(int fd, uint64_t device, uint64_t capacity,
	const FAT_FORMAT_OPTIONS* options, FORMAT_RESULT* result)
{
	return CheckFatDevice(fd, device, capacity, true, options, result);
}

static int FormatFat(int fd, uint64_t device, const char* mkfs, const FAT_FORMAT_OPTIONS* options, bool fat16,
	FORMAT_RESULT* result)
{
	struct stat before, after;
	char offset[FORMAT_ARGUMENT_BYTES], hidden[FORMAT_ARGUMENT_BYTES], blocks[FORMAT_ARGUMENT_BYTES];
	char cluster[FORMAT_ARGUMENT_BYTES], serial[FORMAT_ARGUMENT_BYTES];
	char sector_bytes[FORMAT_ARGUMENT_BYTES], fat_copies[FORMAT_ARGUMENT_BYTES];
	char root_or_backup[FORMAT_ARGUMENT_BYTES];
	char reserved[FORMAT_ARGUMENT_BYTES], geometry[FORMAT_ARGUMENT_BYTES], target[FORMAT_ARGUMENT_BYTES];
	int error;

	if (result == NULL)
		return EINVAL;
	memset(result, 0, sizeof(*result));
	result->exit_code = -1;
	if (options == NULL)
		return EINVAL;
	if (mkfs == NULL || mkfs[0] != '/')
		return EINVAL;
	error = CheckFormatRequest(fd, options, device, 0, fat16, &before);
	if (error != 0)
		return error;
	if (options->full_format && !SupportsFullFormat(fat16 ? FS_FAT16 : FS_FAT32, options->size)) return ENOTSUP;
	const uint32_t logical_sector_bytes = SectorBytes(options);
	const uint64_t data_offset = device != 0 ? 0 : options->offset;
	snprintf(offset, sizeof(offset), "%" PRIu64, data_offset / logical_sector_bytes);
	snprintf(hidden, sizeof(hidden), "%" PRIu64, options->offset / logical_sector_bytes);
	snprintf(blocks, sizeof(blocks), "%" PRIu64, options->size / MKFS_BLOCK_BYTES);
	snprintf(cluster, sizeof(cluster), "%u", options->cluster_size / logical_sector_bytes);
	snprintf(serial, sizeof(serial), "%08" PRIx32, options->volume_id);
	snprintf(sector_bytes, sizeof(sector_bytes), "%u", logical_sector_bytes);
	snprintf(fat_copies, sizeof(fat_copies), "%d", FAT_COPIES);
	snprintf(reserved, sizeof(reserved), "%d", fat16 ? FAT16_RESERVED_SECTORS : FAT32_RESERVED_SECTORS);
	snprintf(root_or_backup, sizeof(root_or_backup), "%d", fat16 ? FAT16_ROOT_ENTRIES : FAT32_BACKUP_SECTOR);
	snprintf(geometry, sizeof(geometry), "%d/%d", FORMAT_HEADS, FORMAT_SECTORS_PER_TRACK);
	snprintf(target, sizeof(target), "/proc/self/fd/%d", FORMAT_TARGET_FD);
	char* arguments[] = { (char*)mkfs,
		"-F", fat16 ? "16" : "32",
		"-S", sector_bytes, "-s", cluster,
		"-f", fat_copies, "-R", reserved,
		fat16 ? "-r" : "-b", root_or_backup, // Root entries or FAT32 backup sector.
		"-g", geometry,
		"-D", "128", "-M", "248",           // BIOS hard disk (0x80), fixed-disk media (0xf8).
		"-h", hidden, "-i", serial, "-n", (char*)options->label,
		"--mbr=n", "--variant=standard", "--offset", offset, target, blocks, NULL, NULL };
	const bool partial_mkfs_block = options->size % MKFS_BLOCK_BYTES != 0;
	if (partial_mkfs_block) {
		// mkfs.fat takes whole KiB blocks. For an odd sector count, format
		// one sector less without another track-rounding loss. Cluster size is
		// unchanged; skipping internal alignment is only needed for this case.
		// Shift options and their terminator, leaving argv[0] and the spare slot out.
		const size_t shifted_arguments = sizeof(arguments) / sizeof(*arguments) - 2;
		memmove(arguments + 2, arguments + 1, shifted_arguments * sizeof(*arguments));
		arguments[1] = "-a";
	}
	bool cleared = false;
	if (options->full_format) {
		RAW_WRITE_RESULT zero = {0};
		int claim = -1;
		if (device != 0) {
			// beginFormat releases the whole-disk claim so mkfs can acquire
			// its own O_EXCL open. Own the partition during our earlier writes.
			char claim_path[FORMAT_ARGUMENT_BYTES];
			snprintf(claim_path, sizeof(claim_path), "/proc/self/fd/%d", fd);
			claim = open(claim_path, O_RDWR | O_EXCL | O_CLOEXEC);
			if (claim < 0) return errno;
		}
		error = ZeroFormatExtent(claim >= 0 ? claim : fd, device, data_offset, options->size, &zero);
		cleared = zero.bytes_written != 0;
		result->started = cleared;
		result->sync_error = zero.sync_error;
		if (claim >= 0 && close(claim) != 0 && error == 0) error = errno;
		if (error != 0) return error;
	}
	error = options->full_format ? RunFullFormatCommand(fd, arguments, result) : RunFormatCommand(fd, arguments, result);
	result->started = result->started || cleared;
	if (result->diagnostics[0])
		uprintf("mkfs.fat: %s", result->diagnostics);
	if (error == 0 && fstat(fd, &after) != 0)
		error = errno;
	if (error == 0) {
		const bool same_file = before.st_dev == after.st_dev && before.st_ino == after.st_ino;
		const bool same_size = before.st_size == after.st_size;
		if (!same_file || !same_size)
			error = ESTALE;
	}
	if (error == 0)
		error = CheckFormatRequest(fd, options, device, 0, fat16, &after);
	if (error == 0)
		error = CheckFormattedImage(fd, options, data_offset, fat16, result);
	if (error == 0 && options->full_format) _UpdateProgressWithInfo(OP_FORMAT, 0, 100, 100, 1);
	return error;
}

int FormatFat32Image(int fd, const char* mkfs, const FAT_FORMAT_OPTIONS* options, FORMAT_RESULT* result)
{
	return FormatFat(fd, 0, mkfs, options, false, result);
}

static int FormatPartition(int fd, uint64_t device, const char* mkfs, const FAT_FORMAT_OPTIONS* options,
	bool fat16, FORMAT_RESULT* result)
{
	if (device == 0) {
		if (result != NULL) {
			memset(result, 0, sizeof(*result));
			result->exit_code = -1;
		}
		return EINVAL;
	}
	return FormatFat(fd, device, mkfs, options, fat16, result);
}

int FormatFat32Partition(int fd, uint64_t device, const char* mkfs, const FAT_FORMAT_OPTIONS* options,
	FORMAT_RESULT* result)
{
	return FormatPartition(fd, device, mkfs, options, false, result);
}

int FormatFat16Partition(int fd, uint64_t device, const char* mkfs, const FAT_FORMAT_OPTIONS* options,
	FORMAT_RESULT* result)
{
	return FormatPartition(fd, device, mkfs, options, true, result);
}

int CheckFat16ImageRequest(int fd, const char* mkfs, const FAT16_FORMAT_OPTIONS* options)
{
	struct stat info;
	if (mkfs == NULL || mkfs[0] != '/')
		return EINVAL;
	return CheckFormatRequest(fd, options, 0, 0, true, &info);
}

int CheckFat16Image(int fd, const FAT16_FORMAT_OPTIONS* options, FAT16_FORMAT_RESULT* result)
{
	struct stat info;
	if (result == NULL)
		return EINVAL;
	memset(result, 0, sizeof(*result));
	result->exit_code = -1;
	int error = CheckFormatRequest(fd, options, 0, 0, true, &info);
	if (error != 0)
		return error;
	return CheckFormattedImage(fd, options, options->offset, true, result);
}

int FormatFat16Image(int fd, const char* mkfs, const FAT16_FORMAT_OPTIONS* options, FAT16_FORMAT_RESULT* result)
{
	return FormatFat(fd, 0, mkfs, options, true, result);
}
