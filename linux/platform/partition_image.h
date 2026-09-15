/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
	uint64_t disk_size;
	uint32_t sector_size;
	uint32_t sectors_per_track;
	uint32_t cluster_size;
	uint64_t device;  // Expected st_rdev for a block device; zero requires a regular file.
} GPT_IMAGE_OPTIONS;

typedef struct {
	bool started;
	int sync_error;
	uint64_t offset;
	uint64_t size;
} GPT_IMAGE_RESULT;

#ifdef __cplusplus
extern "C" {
#endif

/* Create an ordinary Rufus GPT with one "Main Data Partition" of Microsoft
 * Basic Data type. Uses common Rufus placement arithmetic and libfdisk for the
 * table/CRCs/IDs. Accepts a caller-owned, exclusively borrowed regular file or
 * block descriptor, O_RDWR, exact expected size, 512-byte logical sectors.
 * A block request must supply the expected device number from the accepted plan.
 * The caller must retain exclusive device ownership (UDisks OpenDevice with
 * O_EXCL); this function checks identity/capacity/sectors/read-only state, but
 * does not acquire authorization or detect mounted children itself.
 * Replaces partition metadata; does not wipe old filesystem signatures or data.
 *
 * Returns 0 or errno. Recheck size before writing; flush after any attempted
 * write, including failure/cancellation, then reopen the table through libfdisk
 * to verify the stored layout. Cancellation is checked between library calls;
 * an in-progress metadata write/flush finishes before returning. started=true
 * means failure can leave a partial table; there is no rollback. Offset/size
 * describe the planned main extent, valid for use only after success.
 * The caller's descriptor, flags and seek position remain owned by the caller.
 * Does not reread the kernel partition table or mount anything. ESP mode, extra
 * partitions, MBR and the service/mount handoff remain separate. */
int CreateGptImagePartition(int fd, const GPT_IMAGE_OPTIONS* options, GPT_IMAGE_RESULT* result);

#ifdef __cplusplus
}
#endif
