/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright © 2013-2023 Pete Batard <pete@akeo.ie> */
/* Shared Rufus HDD/UFD heuristic. Platform code supplies verified facts. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HDD_SCORE_MAX_PARTS 16
/* Cards above 500 GiB need List USB Hard Drives (Alt-F). */
#define MAX_DEFAULT_LIST_CARD_SIZE (UINT64_C(500) * UINT64_C(1073741824))
typedef struct {
	int total;
	size_t count;
	int8_t parts[HDD_SCORE_MAX_PARTS];
} HDD_SCORE;

#ifdef __cplusplus
extern "C" {
#endif
/* Positive means likely HDD; zero/negative means likely flash. Neither result
 * establishes permission to erase a device or replaces native protection. */
HDD_SCORE GetHddScore(bool fixed, uint64_t size, uint16_t vid, uint16_t pid, const char* model);
#ifdef __cplusplus
}
#endif
