/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stddef.h>
#include <stdint.h>

/* Isolate the Syslinux version numbers. */
#define SL_MAJOR(x) ((uint8_t)((x)>>8))
#define SL_MINOR(x) ((uint8_t)(x))

#ifdef __cplusplus
extern "C" {
#endif

/* Detect the first usable SYSLINUX/ISOLINUX banner in a writable byte span.
 * buf_size is the accessible size; the banner must terminate within it. Pass
 * byte_count + 1 for a buffer returned by ReadISOFileToBuffer[Ext]. The search
 * retains Rufus's 256-byte minimum and 64-byte margins around the LINUX marker.
 * A nonempty ext points into buf, which is modified to normalize the suffix.
 * Copy ext before freeing buf; do not free ext separately. An unknown version
 * returns zero and an empty ext. This identifies a banner, not bootability or
 * compatibility with an installed tool. */
uint16_t GetSyslinuxVersion(char* buf, size_t buf_size, char** ext);

#ifdef __cplusplus
}
#endif
