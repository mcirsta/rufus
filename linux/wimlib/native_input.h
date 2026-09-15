/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <wimlib.h>

struct iso_file_reader;
struct filedes;

/* Private to this build variant, next to wimlib's private FROM_PIPE flag. */
#define RUFUS_WIM_OPEN_FROM_READER 0x40000000

/* Initialize wimlib before workers and clean it up after they finish. The reader
 * and filesystem remain caller-owned and serialized through wimlib_free().
 * Metadata and bounded ReadWimFile calls are qualified. Exports can retain
 * references longer and remain unqualified. */
int OpenWimFromIsoReader(struct iso_file_reader* reader, int flags, WIMStruct** wim);
uint64_t WimReaderSize(const struct iso_file_reader* reader);
int WimReaderRead(struct filedes* fd, void* buffer, size_t count, off_t offset);
off_t WimReaderSeek(struct filedes* fd, off_t offset);
