/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "compressed_source.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { FFU_CAPTURE_DEFAULT_CHUNK_BYTES = 128 * 1024,
	FFU_MAX_MANIFEST_BYTES = 1024 * 1024, FFU_GPT_TABLE_BUDGET = 4 * 1024 * 1024 };

typedef struct {
	uint32_t block_bytes, blocks;
	uint64_t image_bytes, metadata_bytes, hash_bytes, security_bytes, file_bytes;
} FFU_CAPTURE_GEOMETRY;

/* Choose power-of-two compressed capture chunks within native reader budgets.
 * file_bytes is a worst-case bound including LZNT1 literal chunk overhead.
 * The final non-aligned bytes use one full end-relative block. */
int PlanFfuCapture(uint64_t disk_bytes, uint64_t catalog_bytes, uint64_t manifest_bytes,
	FFU_CAPTURE_GEOMETRY* geometry);

/* Metadata query on a retained, already authenticated source. */
int FfuRequiresBase(const IMAGE_FILE_SOURCE* source, bool* required);

/* Enumerate authenticated store metadata without reading the payload. The UTF-16LE
 * device path is borrowed only during the callback. No store is selected implicitly
 * when a container has multiple stores. Deployment authenticates every payload.
 * The callback returns zero or a positive errno and must not throw. */
typedef int (*ffu_store_t)(void* context, unsigned index, uint64_t disk_bytes,
	unsigned sector_bytes, const uint8_t* device_path, size_t path_bytes);
int ReadFfuStoreList(const IMAGE_FILE_SOURCE* source, ffu_store_t consumer, void* context);

int ReadFfuImageSource(const IMAGE_FILE_SOURCE* source, uint64_t maximum_output,
	compressed_data_t consumer, void* context, COMPRESSED_IMAGE_RESULT* completed);

typedef struct {
	bool optimized;
	uint64_t minimum_bytes, partition_offset, partition_bytes, filesystem_bytes;
	unsigned partition;
	uint8_t disk_id[16];
} FFU_DEPLOYMENT_INFO;

typedef struct {
	unsigned number;
	uint64_t offset, bytes;
	uint8_t type[16], id[16];
} FFU_DEPLOYMENT_PARTITION;

typedef int (*ffu_partition_t)(void* context, const FFU_DEPLOYMENT_PARTITION* partition);
/* Authenticate the selected source and frozen layout, then plan optimized GPT
 * geometry at this capacity. A callback receives each resulting partition.
 * No source or target is changed. The helper must finish filesystem/recovery
 * maintenance after the verified sector deployment, before publishing READY. */
int PlanFfuDeployment(const IMAGE_FILE_SOURCE* source, uint64_t capacity, unsigned sector,
	const uint8_t expected_layout[32], FFU_DEPLOYMENT_INFO* result, ffu_partition_t consumer, void* context);

/* Full or partial selected-store deployment at the selected capacity. Validate every chunk
 * and the frozen layout digest before any callback. Resolve end-relative runs,
 * preserve omitted bytes, and relocate full-image GPT metadata on larger disks.
 * Partial updates apply their declared metadata bytes to the validated base.
 * Optimized images adjust GPT and descriptor locations; the caller must finish
 * filesystem growth and recovery maintenance. Callback offsets can be non-sequential.
 * Caller retains source identity, target claim, sync and readback ownership. */
typedef enum { FFU_DEPLOY, FFU_READBACK } FFU_VISIT_OPERATION;

/* DEPLOY requires a comparator for every partial-update base range before
 * any data callback. READBACK verifies the new data and skips old-base checks. */
int VisitFfuImageBlocks(const IMAGE_FILE_SOURCE* source, uint64_t capacity, unsigned sector,
	const uint8_t expected_layout[32], FFU_VISIT_OPERATION operation, compressed_data_t validate_base,
	compressed_data_t consumer, void* context, uint64_t* transferred);

#ifdef __cplusplus
}
#endif
