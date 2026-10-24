/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright © 2018-2024 Pete Batard <pete@akeo.ie> */
/* Rufus progress calculations shared by Win32 and native frontends. */
#pragma once
#include "format_policy.h"

#define MAX_PROGRESS 0xFFFF

/* Action type, for progress bar breakdown */
enum action_type {
	OP_NOOP_WITH_TASKBAR = -3,
	OP_NOOP = -2,
	OP_INIT = -1,
	OP_ANALYZE_MBR = 0,
	OP_BADBLOCKS,
	OP_ZERO_MBR,
	OP_PARTITION,
	OP_FORMAT,
	OP_CREATE_FS,
	OP_FIX_MBR,
	OP_FILE_COPY,
	OP_PATCH,
	OP_FINALIZE,
	OP_EXTRACT_ZIP,
	OP_MAX
};

typedef struct {
	bool only_format;
	bool bad_blocks;
	bool bootable;
	int selection;
	bool image; /* ISO or standalone Windows image, rather than raw/DD. */
	bool windows_customization;
	int filesystem_steps;
	bool format_progress; /* Slow format, persistence, ext or large FAT32. */
	bool ntfs_image;
	bool extra_archive;
} RUFUS_PROGRESS_OPTIONS;

typedef struct {
	int nb_slots[OP_MAX];
	float end[OP_MAX + 1];
	float previous_end;
} RUFUS_PROGRESS;

#ifdef __cplusplus
extern "C" {
#endif
extern const int nb_steps[FS_MAX];
void InitProgressState(RUFUS_PROGRESS* state, const RUFUS_PROGRESS_OPTIONS* options);
/* Return the Win32 0..MAX_PROGRESS bar position, or -1 for an ignored update. */
int UpdateProgressState(RUFUS_PROGRESS* state, int operation, float percent);
/* Preserve the original float arithmetic before formatting to one decimal. */
float ProgressPercent(uint64_t processed, uint64_t total);
#ifdef __cplusplus
}
#endif
