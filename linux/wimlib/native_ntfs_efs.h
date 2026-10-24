/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <wimlib.h>

struct _ntfs_inode;
struct WimNtfsEfs;

/* Native MS-EFSR raw backup stream restoration. A NULL target performs the
 * same structural validation without writing. Payloads stream directly to NTFS;
 * only bounded headers, stream names and the opaque $EFS metadata are retained.
 * No decryption, key discovery, device discovery, mounting or authorization.
 * The caller owns the target inode/volume and source for the complete call.
 * Errors leave partial output; the caller must close/sync/clean up the job.
 */
int BeginWimNtfsEfs(struct _ntfs_inode* target, uint64_t bytes, struct WimNtfsEfs** state);
int ConsumeWimNtfsEfs(struct WimNtfsEfs* state, uint64_t offset, const void* data, size_t bytes);
int FinishWimNtfsEfs(struct WimNtfsEfs* state);
int CloseWimNtfsEfs(struct WimNtfsEfs* state);
/* Return zero for no sparse-cluster constraint, otherwise the largest target
 * cluster that can preserve all serialized plaintext-zero holes. */
int ValidateWimNtfsEncryptedImage(WIMStruct* wim, int image, uint32_t* cluster_limit);
