/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Pure geometry is a separate object so wire validation performs no source I/O. */
#include "compressed_write.h"
#include <errno.h>

enum { MIN_SECTOR_BYTES = 512, MAX_SECTOR_BYTES = 64 * 1024 };

int CompressedImageWriteSize(uint64_t decoded_bytes, unsigned sector_size, uint64_t* target_bytes)
{
	if (target_bytes == NULL)
		return EINVAL;
	*target_bytes = 0;
	const bool supported_sector = sector_size >= MIN_SECTOR_BYTES && sector_size <= MAX_SECTOR_BYTES;
	if (!supported_sector)
		return EINVAL;
	const bool power_of_two = (sector_size & (sector_size - 1)) == 0;
	if (!power_of_two || decoded_bytes == 0)
		return EINVAL;
	const uint64_t remainder = decoded_bytes % sector_size;
	const uint64_t padding = remainder == 0 ? 0 : sector_size - remainder;
	if (decoded_bytes > INT64_MAX || padding > INT64_MAX - decoded_bytes)
		return EOVERFLOW;
	*target_bytes = decoded_bytes + padding;
	return 0;
}

