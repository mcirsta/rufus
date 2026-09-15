/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Log the first Shim or systemd-boot version banner using the host's uprintf.
 * buf_size is the actual byte count; no terminating byte is required. The
 * buffer is not modified. Retains Rufus's 64-byte search margin and 32-byte
 * version limit. source is a non-NULL, terminated name used only in the log.
 * A missing/short/NULL buffer produces no message. This is banner recognition,
 * not PE validation, a Secure Boot check, or evidence that a file can boot. */
void GetEfiBootInfo(const char* buf, size_t buf_size, const char* source);

#ifdef __cplusplus
}
#endif
