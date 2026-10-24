/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Rufus bad-block patterns shared by the Win32 and Linux implementations. */
#pragma once
#define BADLOCKS_PATTERN_TYPES      5
#define BADBLOCK_PATTERN_COUNT      4
#define BADBLOCK_PATTERN_ONE_PASS   {0x55, 0x00, 0x00, 0x00}
#define BADBLOCK_PATTERN_TWO_PASSES {0x55, 0xaa, 0x00, 0x00}
#define BADBLOCK_PATTERN_SLC        {0x00, 0xff, 0x55, 0xaa}
#define BADCLOCK_PATTERN_MLC        {0x00, 0xff, 0x33, 0xcc}
#define BADBLOCK_PATTERN_TLC        {0x00, 0xff, 0x1c71c7, 0xe38e38}
#define BADBLOCK_BLOCK_SIZE         (512 * 1024)
#define BB_BAD_BLOCKS_THRESHOLD     256
#define BB_BLOCKS_AT_ONCE           64
