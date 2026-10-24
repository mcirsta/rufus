/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "hash_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Shared once-only CPU dispatch initialization for all native core consumers.
 * Call before using a context, including concurrent first use. Returns errno.
 * Does not change the original algorithms or Windows startup. */
int InitializeNativeHashes(void);

#ifdef __cplusplus
}
#endif
