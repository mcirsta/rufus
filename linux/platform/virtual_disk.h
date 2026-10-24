/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "compressed_source.h"

typedef struct virtual_disk_node VIRTUAL_DISK_NODE;
/* Internal native reader contract. The root borrows its source; parent sources
 * are opened read-only once and retained until the complete helper job ends. */
struct virtual_disk_node {
	IMAGE_FILE_SOURCE source;
	char* path;
	IMAGE_FILE_SOURCE relocation;
	char *relocation_path, *relocation_data;
	bool relocation_checked;
	VIRTUAL_DISK_NODE *root, *parent;
	unsigned depth;
	int type;
	uint64_t disk_bytes, file_bytes, log_memory;
	uint32_t logical_sector;
	uint8_t identifier[16];
	void *layout, *replay;
	int (*read)(VIRTUAL_DISK_NODE*, uint64_t, void*, size_t);
	void (*release)(VIRTUAL_DISK_NODE*);
};

#ifdef __cplusplus
extern "C" {
#endif
int OpenVirtualDiskSource(const IMAGE_FILE_SOURCE* source, int type, uint64_t maximum, VIRTUAL_DISK_NODE** result);
int CheckVirtualDiskSource(const VIRTUAL_DISK_NODE* node);
int CloseVirtualDiskSource(VIRTUAL_DISK_NODE* node);
int StreamVirtualDiskSource(const IMAGE_FILE_SOURCE* source, int type, uint64_t maximum,
	compressed_data_t consumer, void* context, COMPRESSED_IMAGE_RESULT* completed);
int ReadVirtualDiskFile(VIRTUAL_DISK_NODE* node, uint64_t offset, void* data, size_t bytes);
int ReadVirtualDiskRawFile(const VIRTUAL_DISK_NODE* node, uint64_t offset, void* data, size_t bytes);
int OpenVirtualDiskParent(VIRTUAL_DISK_NODE* node, const char* path, const uint8_t identifier[16],
	uint32_t timestamp, bool check_timestamp);
int VirtualDiskUtf16(const uint8_t* data, size_t bytes, bool little_endian, bool padding, char result[4096]);
int ParseVirtualDiskGuid(const char* text, uint8_t result[16]);
int InitializeVhdNode(VIRTUAL_DISK_NODE* node, uint64_t maximum);
int InitializeVhdxNode(VIRTUAL_DISK_NODE* node, uint64_t maximum);
int ReplayVhdxLog(VIRTUAL_DISK_NODE* node, const uint8_t header[4096]);
int ReadVhdxOverlay(VIRTUAL_DISK_NODE* node, uint64_t offset, void* data, size_t bytes);
void CloseVhdxOverlay(VIRTUAL_DISK_NODE* node);
#ifdef __cplusplus
}
#endif
