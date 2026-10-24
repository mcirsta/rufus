// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <linux/magic.h>

// Older UAPI headers omit these existing statfs filesystem identifiers.
#ifndef EXFAT_SUPER_MAGIC
#define EXFAT_SUPER_MAGIC 0x2011BAB0
#endif
#ifndef FUSE_SUPER_MAGIC
#define FUSE_SUPER_MAGIC 0x65735546
#endif
