// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "raw_write.h"
#include "qt/prepare_image_protocol.h"

namespace rufus
{
// Native operation for an already authorized root job. Uses only the request's
// source/device identity fields; no formatting or ISO options are applied.
// Retains caller-owned source, validates/unmounts eligible target filesystems,
// owns the exclusive claim through copy/flush/readback and closes before return.
// The caller supplies the authenticated UID, confirmation and reporting context.
// Source-free full/fast zero jobs use the same claim and remove stale kernel
// partitions after verified clearing. Raw, compressed and zero jobs use the
// kernel-checked 512..65536 sector range.
// Compressed requests revalidate exact decoded size/hash before target acquisition.
// Checked source/target closes precede the outcome, including failure/cancellation.
// This entry point does not launch a helper or request authorization.
int WriteRawDevice(const PrepareImageRequest& request, uid_t caller, RAW_WRITE_RESULT& result);
}
