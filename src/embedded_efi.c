/*
 * Rufus: The Reliable USB Formatting Utility
 * Embedded EFI presence inspection extracted from iso.c
 * Copyright © 2011-2026 Pete Batard <pete@akeo.ie>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "embedded_efi.h"
#include "iso_file.h"
#include "image_file.h"
#include "syslinux/libfat/libfat.h"
#include "syslinux/libfat/ulint.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void uprintf(const char* format, ...);

typedef struct {
	ISO_FILE_READER file;
	uint64_t base, length;
} EFI_IMAGE_READER;

/* libfat's existing read-function contract is a storage adapter. The shared
 * file reader supplies the same 32 KiB read-ahead for ISO9660 and UDF. */
static int ReadEfiSector(intptr_t opaque, void* buffer, size_t size, libfat_sector_t sector)
{
	EFI_IMAGE_READER* reader = (EFI_IMAGE_READER*)opaque;
	uint64_t offset;

	if (size == 0 || size > ISO_FILE_CACHE_SIZE || size > reader->length ||
		sector > (reader->length - size) / size)
		return 0;
	offset = reader->base + sector * size;
	if (ReadIsoFileRange(&reader->file, offset, buffer, size) != ISO_SCAN_OK)
		return 0;
	return (int)size;
}

static bool PrepareEfiFat(EFI_IMAGE_READER* reader)
{
	uint8_t boot[512];
	uint32_t start, sectors, type;
	uint16_t size;

	if (ReadEfiSector((intptr_t)reader, boot, sizeof(boot), 0) != sizeof(boot))
		return false;
	type = boot[0x1c2];
	// Preserve Rufus's first active FAT partition rule; do not guess GPT or
	// another MBR partition. ISO alignment is retained from the original reader.
	if (boot[510] == 0x55 && boot[511] == 0xaa && boot[0x1be] == 0x80 &&
		(type == 1 || type == 4 || type == 6 || type == 0x0b || type == 0x0c || type == 0x0e)) {
		start = read32((le32_t*)&boot[0x1c6]);
		sectors = read32((le32_t*)&boot[0x1ca]);
		if (start == 0 || start % 4 != 0 || sectors == 0 ||
			(uint64_t)start + sectors > reader->file.size / 512)
			return false;
		reader->base = (uint64_t)start * 512;
		reader->length = (uint64_t)sectors * 512;
		if (ReadEfiSector((intptr_t)reader, boot, sizeof(boot), 0) != sizeof(boot))
			return false;
	}
	if (boot[510] != 0x55 || boot[511] != 0xaa)
		return false;
	size = read16((le16_t*)&boot[11]);
	if (size != 512 && size != 1024 && size != 2048 && size != 4096)
		return false;
	sectors = read16((le16_t*)&boot[19]);
	if (sectors == 0)
		sectors = read32((le32_t*)&boot[32]);
	if (sectors == 0 || sectors > reader->length / size)
		return false;
	reader->length = (uint64_t)sectors * size;
	LIBFAT_SECTOR_SIZE = size;
	LIBFAT_SECTOR_MASK = size - 1;
	LIBFAT_SECTOR_SHIFT = 0;
	while ((size >>= 1) != 0)
		LIBFAT_SECTOR_SHIFT++;
	return true;
}

static int32_t FindEfiDirectory(struct libfat_filesystem* fs, int32_t parent, const char* name)
{
	struct libfat_direntry entry;
	uint32_t cluster;
	int32_t result = libfat_searchdir(fs, parent, name, &entry);

	if (result < 0)
		return result;
	if (!(entry.entry[11] & 0x10))
		return -1;
	// Directory entries have size zero; use both halves of their FAT32 cluster.
	cluster = read16((le16_t*)&entry.entry[26]) |
		((uint32_t)read16((le16_t*)&entry.entry[20]) << 16);
	if (cluster < 2 || cluster > INT32_MAX ||
		libfat_clustertosector(fs, (int32_t)cluster) == (libfat_sector_t)-1)
		return -1;
	return (int32_t)cluster;
}

static int FindEfiBootloaders(struct libfat_filesystem* fs, const char* path, bool* found)
{
	struct libfat_direntry entry;
	char name[12];
	int32_t directory, cluster;
	size_t i, j, length;

	directory = FindEfiDirectory(fs, 0, "EFI        ");
	if (directory >= 0)
		directory = FindEfiDirectory(fs, directory, "BOOT       ");
	if (directory < 0)
		return directory == -2 ? ISO_SCAN_OK : ISO_SCAN_PARTIAL;
	for (i = 1; i < ARCH_MAX; i++) {
		if (IsIsoScanCancelled())
			return ISO_SCAN_CANCELLED;
		length = strlen(efi_archname[i]);
		// Retain Rufus's ~1 aliases for RISC-V/LoongArch long names.
		snprintf(name, sizeof(name), "BOOT%c%c%c%cEFI", efi_archname[i][0], efi_archname[i][1],
			length > 4 ? '~' : efi_archname[i][2],
			length > 4 ? '1' : (length > 3 ? efi_archname[i][3] : ' '));
		for (j = 0; j < 11; j++)
			name[j] = (char)toupper((unsigned char)name[j]);
		cluster = libfat_searchdir(fs, directory, name, &entry);
		if (cluster == -1)
			return ISO_SCAN_PARTIAL;
		if (cluster > 0 && !(entry.entry[11] & 0x10)) {
			if (cluster < 2 || libfat_clustertosector(fs, cluster) == (libfat_sector_t)-1)
				return ISO_SCAN_PARTIAL;
			if (!*found)
				uprintf("  Detected EFI bootloader(s) (from '%s'):", path);
			uprintf("  ● 'boot%s.efi'", efi_archname[i]);
			*found = true;
		}
	}
	return ISO_SCAN_OK;
}

int InspectEfiImage(iso9660_t* iso, udf_t* udf, const char* path, bool* found)
{
	EFI_IMAGE_READER* reader = NULL;
	struct libfat_filesystem* fs = NULL;
	uint32_t saved_size = LIBFAT_SECTOR_SIZE, saved_mask = LIBFAT_SECTOR_MASK;
	uint32_t saved_shift = LIBFAT_SECTOR_SHIFT;
	int result = ISO_SCAN_PARTIAL;

	if (found == NULL)
		return ISO_SCAN_ERROR;
	*found = false;
	if ((iso == NULL) == (udf == NULL) || path == NULL)
		return ISO_SCAN_ERROR;
	if (IsIsoScanCancelled())
		return ISO_SCAN_CANCELLED;
	reader = calloc(1, sizeof(*reader));
	if (reader == NULL) {
		result = ISO_SCAN_ERROR;
		goto out;
	}
	result = OpenIsoFileReader(&reader->file, iso, udf, path);
	if (result != ISO_SCAN_OK)
		goto out;
	result = ISO_SCAN_PARTIAL;
	if (reader->file.size < 512)
		goto out;
	reader->length = reader->file.size;
	if (!PrepareEfiFat(reader))
		goto out;
	fs = libfat_open(ReadEfiSector, (intptr_t)reader);
	if (fs == NULL)
		goto out;
	result = FindEfiBootloaders(fs, path, found);
out:
	if (fs != NULL)
		libfat_close(fs);
	LIBFAT_SECTOR_SIZE = saved_size;
	LIBFAT_SECTOR_MASK = saved_mask;
	LIBFAT_SECTOR_SHIFT = saved_shift;
	if (reader != NULL)
		CloseIsoFileReader(&reader->file);
	free(reader);
	if (IsIsoScanCancelled())
		result = ISO_SCAN_CANCELLED;
	if (result != ISO_SCAN_OK)
		*found = false;
	if (result == ISO_SCAN_PARTIAL)
		uprintf("  Could not complete embedded EFI inspection of '%s'", path);
	return result;
}
