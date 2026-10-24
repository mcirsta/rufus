/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright © 2018-2024 Pete Batard <pete@akeo.ie> */
/* Extracted from Rufus ui.c; preserve allocation and float operation order. */
#include "progress_policy.h"
#include <math.h>
#include <string.h>

/* Number of steps for each FS for FCC_STRUCTURE_PROGRESS. */
const int nb_steps[FS_MAX] = { 5, 5, 12, 1, 10, 1, 1, 1, 1 };

void InitProgressState(RUFUS_PROGRESS* state, const RUFUS_PROGRESS_OPTIONS* options)
{
	int i;
	float last_end = 0.0f, slots_discrete = 0.0f, slots_analog = 0.0f;
	int* nb_slots = state->nb_slots;
	float* slot_end = state->end;
	memset(state, 0, sizeof(*state));
	if (options->only_format) {
		nb_slots[OP_FORMAT] = -1;
	} else {
		nb_slots[OP_ANALYZE_MBR] = 1;
		if (options->bad_blocks) {
			nb_slots[OP_BADBLOCKS] = -1;
		}
		if (options->bootable) {
			// 1 extra slot for PBR writing
			switch (options->selection) {
			case BT_MSDOS:
				nb_slots[OP_FILE_COPY] = 3 + 1;
				break;
			case BT_FREEDOS:
				nb_slots[OP_FILE_COPY] = 5 + 1;
				break;
			case BT_IMAGE:
				nb_slots[OP_FILE_COPY] = options->image ? -1 : 0;
				if (options->windows_customization)
					nb_slots[OP_PATCH] = -1;
				break;
			default:
				nb_slots[OP_FILE_COPY] = 2 + 1;
				break;
			}
		}
		if (options->selection == BT_IMAGE && !options->image) {
			nb_slots[OP_FORMAT] = -1;
		} else {
			nb_slots[OP_ZERO_MBR] = 1;
			nb_slots[OP_PARTITION] = 1;
			nb_slots[OP_FIX_MBR] = 1;
			nb_slots[OP_CREATE_FS] = options->filesystem_steps;
			if (options->format_progress) {
				nb_slots[OP_FORMAT] = -1;
				nb_slots[OP_CREATE_FS] = 0;
			}
			nb_slots[OP_FINALIZE] = options->ntfs_image ? 3 : 2;
		}
	}
	if (options->extra_archive) {
		nb_slots[OP_EXTRACT_ZIP] = -1;
	}

	for (i = 0; i < OP_MAX; i++) {
		if (nb_slots[i] > 0) {
			slots_discrete += nb_slots[i] * 1.0f;
		}
		if (nb_slots[i] < 0) {
			slots_analog += nb_slots[i] * 1.0f;
		}
	}

	for (i = 0; i < OP_MAX; i++) {
		if (nb_slots[i] == 0) {
			slot_end[i + 1] = last_end;
		} else if (nb_slots[i] > 0) {
			slot_end[i + 1] = last_end + (1.0f * nb_slots[i]);
		} else if (nb_slots[i] < 0) {
			slot_end[i + 1] = last_end + (((100.0f - slots_discrete) * nb_slots[i]) / slots_analog);
		}
		last_end = slot_end[i + 1];
	}

	// If there's no analog, adjust our discrete ends to fill the whole bar
	if (slots_analog == 0.0f && slots_discrete != 0.0f) {
		for (i = 0; i < OP_MAX; i++) {
			slot_end[i + 1] *= 100.0f / slots_discrete;
		}
	}
}

int UpdateProgressState(RUFUS_PROGRESS* state, int op, float percent)
{
	int pos;
	const bool valid_operation = op >= 0 && op < OP_MAX;
	if (!valid_operation || !isfinite(percent) || percent > 100.1f)
		return -1;
	const int slots = state->nb_slots[op];
	if (slots == 0 || (percent < 0.0f && slots < 0))
		return -1;
	if (state->previous_end < state->end[op])
		state->previous_end = state->end[op];
	if (percent < 0.0f) {
		state->previous_end += (state->end[op + 1] - state->end[op]) / (1.0f * slots);
		pos = (int)(state->previous_end / 100.0f * MAX_PROGRESS);
	} else {
		pos = (int)((state->previous_end + ((state->end[op + 1] - state->previous_end) * (percent / 100.0f))) / 100.0f * MAX_PROGRESS);
	}
	return pos > MAX_PROGRESS ? MAX_PROGRESS : pos;
}

float ProgressPercent(uint64_t processed, uint64_t total)
{
	if (processed > total)
		total = processed;
	return total > 0 ? (100.0f * processed) / (1.0f * total) : 0.0f;
}
