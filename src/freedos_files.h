/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stddef.h>

typedef struct {
	const char* name;
	bool root; /* Root files need the FAT hidden and system attributes. */
	bool progress; /* Original ExtractFreeDOS discrete copy milestones. */
} FREEDOS_FILE;

#ifdef __cplusplus
extern "C" {
#endif
extern const FREEDOS_FILE freedos_files[];
extern const size_t freedos_file_count;
#ifdef __cplusplus
}
#endif
