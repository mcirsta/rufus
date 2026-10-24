/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* The original parser.c label token rules on a bounded UTF-16 line. No section
 * restriction; spaces/tabs, optional XML delimiter and optional quotes match
 * get_token_data_line. Offsets refer to the borrowed input, without mutation. */
bool FindDriveLabel16(const uint16_t* line, size_t units, size_t* start, size_t* length);
#ifdef __cplusplus
}
#endif
