/*
 * Rufus: The Reliable USB Formatting Utility
 * Copyright © 2011-2026 Pete Batard <pete@akeo.ie>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <stdbool.h>
#include "image_report.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Apply post-scan ISO rules to a non-NULL, complete report with a terminated label.
 * Returns true when the caller
 * must use DD mode. The caller owns dialogs and the transition of is_iso to 0.
 * Does not access GUI, devices, host settings or the global img_report.
 * Diagnostics use Rufus's existing uprintf reporting entry point. */
bool ApplyIsoPolicy(RUFUS_IMG_REPORT* report);

enum IMAGE_WRITE_MODE {
	IMAGE_WRITE_NONE = 0,
	IMAGE_WRITE_ISO,
	IMAGE_WRITE_ESP,
	IMAGE_WRITE_DD
};

typedef struct {
	enum IMAGE_WRITE_MODE mode[3];
	int count;
} IMAGE_WRITE_CHOICES;

/* BootCheckThread's ISOHybrid/DD choices, in dialog order; the first is the default.
 * Call after applying ISO policy. A single choice does not need a dialog.
 * These are Rufus's choices, not proof that a host's installer is available. */
IMAGE_WRITE_CHOICES GetImageWriteChoices(const RUFUS_IMG_REPORT* report,
	uint64_t persistence_size, bool gpt, bool fat);

/* SelectionDialog returns one bit per row, or -1 for Cancel. Reject anything
 * except one offered row; never turn cancellation into a default mode. */
enum IMAGE_WRITE_MODE GetImageWriteMode(const IMAGE_WRITE_CHOICES* choices, int selected);

#ifdef __cplusplus
}
#endif
