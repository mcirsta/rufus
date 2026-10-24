/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <inode.h>
struct wim_inode;

/* Adapt WIMGAPI/DISM's packed TAG_XATTRS to the native library's EA list.
 * Caller owns the open inode and restores timestamps after this metadata. */
int SetWimNtfsExtendedAttributes(ntfs_inode* target, const struct wim_inode* source);
