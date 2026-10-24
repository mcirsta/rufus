/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdint.h>
#include <sys/stat.h>

typedef struct {
	struct stat info;
	uint64_t size;
	unsigned sector_size; /* Zero for a regular file. */
	unsigned physical_sector_size;
} WRITABLE_TARGET;

#ifdef __cplusplus
extern "C" {
#endif
/* Inspect the retained descriptor; device zero requires a regular file, otherwise
 * the exact block dev_t. Callers check expected capacity, extent and sector rules.
 * This does not acquire a claim, authorize writes or replace source/use checks. */
int InspectWritableTarget(int fd, uint64_t device, WRITABLE_TARGET* target);
/* Reopen a borrowed directory for enumeration without changing its position.
 * Returns ENOTEMPTY for any entry other than dot/dot-dot; propagates I/O errors. */
int CheckEmptyTargetDirectory(int fd);
#ifdef __cplusplus
}
#endif
