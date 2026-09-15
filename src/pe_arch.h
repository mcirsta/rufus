/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stddef.h>
#include "image_report.h"

#ifdef __cplusplus
extern "C" {
#endif

enum ArchType MachineToArch(uint16_t machine);
/* Recognize the DOS/PE signatures and COFF machine field within actual bytes.
 * Unknown/truncated headers return ARCH_UNKNOWN. This does not validate the
 * executable, EFI subsystem, signature, or bootability. */
enum ArchType FindPeArch(const void* buffer, size_t size);

#ifdef __cplusplus
}
#endif
