/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef RUFUS_LINUX_REPORTING_H
#define RUFUS_LINUX_REPORTING_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Match the existing entry points in src/rufus.h. No Qt or Windows headers.
 * Windows BOOL is a signed 32-bit int in the original progress prototype.
 * This header declares the Linux implementation; it does not shadow rufus.h.
 */
void uprintf(const char *format, ...);
void uprintfs(const char *message);
void UpdateProgress(int operation, float percent);
void _UpdateProgressWithInfo(int operation, int message_id,
                            uint64_t processed, uint64_t total, int force);
/* Synchronous worker interruption query, also used by native write operations. */
int IsRufusJobCancelled(void);
/* Retained entry point for the shared read-only scan. */
int IsIsoScanCancelled(void);

#ifdef __cplusplus
}
#endif

#endif
