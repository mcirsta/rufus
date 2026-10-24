/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "virtual_disk.h"
#include "rufus_reporting.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { VIRTUAL_DISK_DEPTH = 32, VIRTUAL_DISK_TRANSFER = 65536 };

int ReadVirtualDiskRawFile(const VIRTUAL_DISK_NODE* node, uint64_t offset, void* data, size_t bytes)
{
	const uint64_t size = (uint64_t)node->source.info.st_size;
	if (offset > size || bytes > size - offset) return EBADMSG;
	uint8_t* output = data;
	while (bytes != 0) {
		if (IsIsoScanCancelled()) return ECANCELED;
		const ssize_t count = pread(node->source.fd, output, bytes, (off_t)offset);
		if (count < 0 && errno == EINTR) continue;
		if (count <= 0) return count < 0 ? errno : EIO;
		output += count; bytes -= (size_t)count; offset += (uint64_t)count;
	}
	return 0;
}

int ReadVirtualDiskFile(VIRTUAL_DISK_NODE* node, uint64_t offset, void* data, size_t bytes)
{
	if (offset > node->file_bytes || bytes > node->file_bytes - offset) return EBADMSG;
	return node->replay ? ReadVhdxOverlay(node, offset, data, bytes) : ReadVirtualDiskRawFile(node, offset, data, bytes);
}

int VirtualDiskUtf16(const uint8_t* data, size_t bytes, bool little, bool padding, char result[4096])
{
	result[0] = 0;
	if (bytes == 0 || bytes % 2 != 0) return EBADMSG;
	size_t used = 0;
	for (size_t i = 0; i < bytes; i += 2) {
		uint32_t value = little ? data[i] | (uint32_t)data[i + 1] << 8 : (uint32_t)data[i] << 8 | data[i + 1];
		if (value == 0) {
			if (!padding) return EBADMSG;
			for (size_t j = i; j < bytes; j++) if (data[j] != 0) return EBADMSG;
			break;
		}
		if (value >= 0xd800 && value <= 0xdbff) {
			if (i + 3 >= bytes) return EILSEQ;
			i += 2;
			const uint32_t low = little ? data[i] | (uint32_t)data[i + 1] << 8 : (uint32_t)data[i] << 8 | data[i + 1];
			if (low < 0xdc00 || low > 0xdfff) return EILSEQ;
			value = 0x10000 + ((value - 0xd800) << 10) + low - 0xdc00;
		} else if (value >= 0xdc00 && value <= 0xdfff) return EILSEQ;
		const size_t count = value < 0x80 ? 1 : value < 0x800 ? 2 : value < 0x10000 ? 3 : 4;
		if (used + count >= 4096) return ENAMETOOLONG;
		if (count == 1) result[used++] = (char)value;
		else {
			result[used++] = (char)((count == 2 ? 0xc0 : count == 3 ? 0xe0 : 0xf0) | (value >> (6 * (count - 1))));
			for (size_t j = count - 1; j > 0; j--) result[used++] = (char)(0x80 | ((value >> (6 * (j - 1))) & 63));
		}
	}
	result[used] = 0;
	return used != 0 ? 0 : EBADMSG;
}

int ParseVirtualDiskGuid(const char* text, uint8_t result[16])
{
	if (strlen(text) != 38 || text[0] != '{' || text[37] != '}') return EBADMSG;
	uint8_t value[16]; unsigned count = 0;
	for (unsigned i = 1; i < 37;) {
		if (i == 9 || i == 14 || i == 19 || i == 24) { if (text[i++] != '-') return EBADMSG; continue; }
		unsigned byte = 0;
		for (unsigned j = 0; j < 2; j++) {
			const char c = text[i++];
			const unsigned digit = c >= '0' && c <= '9' ? (unsigned)(c - '0') : c >= 'a' && c <= 'f' ? (unsigned)(c - 'a' + 10)
				: c >= 'A' && c <= 'F' ? (unsigned)(c - 'A' + 10) : 16;
			if (digit == 16) return EBADMSG;
			byte = byte * 16 + digit;
		}
		if (count >= 16) return EBADMSG;
		value[count++] = (uint8_t)byte;
	}
	if (count != 16) return EBADMSG;
	const unsigned order[16] = {3,2,1,0,5,4,7,6,8,9,10,11,12,13,14,15};
	for (unsigned i = 0; i < 16; i++) result[i] = value[order[i]];
	return 0;
}

static int SourcePath(int fd, char** result)
{
	char descriptor[64], path[4096];
	snprintf(descriptor, sizeof(descriptor), "/proc/self/fd/%d", fd);
	const ssize_t count = readlink(descriptor, path, sizeof(path) - 1);
	if (count < 0) return errno;
	if (count == 0 || count == (ssize_t)sizeof(path) - 1 || path[0] != '/') return ENAMETOOLONG;
	path[count] = 0;
	*result = strdup(path);
	return *result ? 0 : ENOMEM;
}

static int InitializeNode(VIRTUAL_DISK_NODE* node, uint64_t maximum)
{
	return node->type == IMG_COMPRESSION_VHD ? InitializeVhdNode(node, maximum) : InitializeVhdxNode(node, maximum);
}

/* Explicit Linux relocation input: child.vhd[x].parents, UTF-8 records
 * original-locator<TAB>/absolute/Linux/parent. Never reinterpret a drive/UNC root. */
static int RelocatedParent(VIRTUAL_DISK_NODE* node, const char* locator, char result[4096])
{
	result[0] = 0;
	if (!node->relocation_checked) {
		node->relocation_checked = true;
		const size_t length = strlen(node->path);
		node->relocation_path = malloc(length + sizeof(".parents"));
		if (!node->relocation_path) return ENOMEM;
		strcpy(node->relocation_path, node->path); strcat(node->relocation_path, ".parents");
		const int fd = open(node->relocation_path, O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
		if (fd < 0) return errno == ENOENT ? 0 : errno;
		node->relocation.fd = fd;
		if (fstat(fd, &node->relocation.info) != 0) return errno;
		const struct stat* file = &node->relocation.info;
		if (!S_ISREG(file->st_mode) || file->st_uid != node->root->source.info.st_uid || !(file->st_mode & S_IRUSR)) return EACCES;
		if (file->st_size <= 0 || file->st_size > 65536) return EFBIG;
		node->relocation_data = calloc(1, (size_t)file->st_size + 1);
		if (!node->relocation_data) return ENOMEM;
		if (pread(fd, node->relocation_data, (size_t)file->st_size, 0) != file->st_size) return EIO;
		if (memchr(node->relocation_data, 0, (size_t)file->st_size)) return EBADMSG;
	}
	if (!node->relocation_data) return 0;
	unsigned entries = 0;
	const char* cursor = node->relocation_data;
	while (*cursor) {
		const char* end = strchr(cursor, '\n');
		if (!end) end = cursor + strlen(cursor);
		const char* separator = memchr(cursor, '\t', (size_t)(end - cursor));
		if (++entries > 32 || !separator || separator == cursor || separator + 1 == end || separator[1] != '/') return EBADMSG;
		const size_t key_bytes = (size_t)(separator - cursor), path_bytes = (size_t)(end - separator - 1);
		if (key_bytes >= 4096 || path_bytes >= 4096 || memchr(separator + 1, '\t', path_bytes) || memchr(cursor, '\r', (size_t)(end - cursor))) return EBADMSG;
		if (strlen(locator) == key_bytes && memcmp(locator, cursor, key_bytes) == 0) {
			if (*result) return EBADMSG;
			memcpy(result, separator + 1, path_bytes); result[path_bytes] = 0;
		}
		cursor = *end ? end + 1 : end;
	}
	return CheckImageFileSource(&node->relocation, node->relocation_path);
}

static int OpenParentNode(VIRTUAL_DISK_NODE* node, const char* locator, const uint8_t id[16], uint32_t stamp, bool timestamp)
{
	if (node->depth + 1 >= VIRTUAL_DISK_DEPTH) return ELOOP;
	if (node->parent || !locator || !*locator) return EBADMSG;
	char full[4096];
	int relocated = RelocatedParent(node, locator, full);
	if (relocated != 0) return relocated;
	if (!*full) {
		char path[4096];
		if (strlen(locator) >= sizeof(path)) return ENAMETOOLONG;
		strcpy(path, locator);
		for (char* p = path; *p; p++) { if (*p == ':') return ENOTSUP; if (*p == '\\') *p = '/'; }
		if (path[0] == '/') return ENOTSUP;
		const char* slash = strrchr(node->path, '/');
		if (!slash) return EINVAL;
		const size_t directory = (size_t)(slash - node->path) + 1;
		if (directory + strlen(path) >= sizeof(full)) return ENAMETOOLONG;
		memcpy(full, node->path, directory); strcpy(full + directory, path);
	}
	VIRTUAL_DISK_NODE* parent = calloc(1, sizeof(*parent));
	if (!parent) return ENOMEM;
	parent->source.fd = parent->relocation.fd = -1; parent->root = node->root; parent->depth = node->depth + 1; parent->type = node->type;
	node->parent = parent; /* Root cleanup owns partial initialization too. */
	parent->source.fd = open(full, O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
	if (parent->source.fd < 0) return errno;
	if (fstat(parent->source.fd, &parent->source.info) != 0) return errno;
	const struct stat* info = &parent->source.info;
	if (!S_ISREG(info->st_mode) || info->st_size < 0) return EINVAL;
	/* Match the helper's existing caller-owned readable top-level source policy,
	 * before parsing a single byte of a path supplied by untrusted metadata. */
	if (info->st_uid != node->root->source.info.st_uid || !(info->st_mode & S_IRUSR)) return EACCES;
	for (VIRTUAL_DISK_NODE* prior = node->root; prior != parent; prior = prior->parent)
		if (prior->source.info.st_dev == info->st_dev && prior->source.info.st_ino == info->st_ino) return ELOOP;
	int error = SourcePath(parent->source.fd, &parent->path);
	parent->file_bytes = (uint64_t)info->st_size;
	if (error == 0) error = InitializeNode(parent, node->disk_bytes);
	if (error == 0 && (parent->disk_bytes != node->disk_bytes || parent->logical_sector != node->logical_sector)) error = EBADMSG;
	if (error == 0 && memcmp(parent->identifier, id, 16) != 0) error = ESTALE;
	if (error == 0 && timestamp && (info->st_mtim.tv_sec < 946684800 || (uint64_t)(info->st_mtim.tv_sec - 946684800) != stamp)) error = ESTALE;
	return error;
}

int OpenVirtualDiskParent(VIRTUAL_DISK_NODE* node, const char* path, const uint8_t id[16], uint32_t stamp, bool timestamp)
{
	const uint64_t memory = node->root->log_memory;
	const int error = OpenParentNode(node, path, id, stamp, timestamp);
	if (error != 0 && node->parent) {
		CloseVirtualDiskSource(node->parent); node->parent = NULL;
		node->root->log_memory = memory;
	}
	return error;
}

int CheckVirtualDiskSource(const VIRTUAL_DISK_NODE* node)
{
	for (; node; node = node->parent) {
		const int mapped = node->relocation.fd >= 0 ? CheckImageFileSource(&node->relocation, node->relocation_path) : 0;
		if (mapped != 0) return mapped;
		const int error = CheckImageFileSource(&node->source, node->path);
		if (error != 0) return error;
	}
	return 0;
}

int CloseVirtualDiskSource(VIRTUAL_DISK_NODE* node)
{
	int error = 0;
	while (node) {
		VIRTUAL_DISK_NODE* parent = node->parent;
		if (node->release) node->release(node);
		CloseVhdxOverlay(node);
		if (node != node->root && node->source.fd >= 0 && close(node->source.fd) != 0 && error == 0) error = errno;
		if (node->relocation.fd >= 0 && close(node->relocation.fd) != 0 && error == 0) error = errno;
		free(node->relocation_path); free(node->relocation_data);
		free(node->path); free(node); node = parent;
	}
	return error;
}

int OpenVirtualDiskSource(const IMAGE_FILE_SOURCE* source, int type, uint64_t maximum, VIRTUAL_DISK_NODE** result)
{
	*result = NULL;
	if (type != IMG_COMPRESSION_VHD && type != IMG_COMPRESSION_VHDX) return EINVAL;
	VIRTUAL_DISK_NODE* node = calloc(1, sizeof(*node));
	if (!node) return ENOMEM;
	node->relocation.fd = -1;
	node->root = node; node->source = *source; node->source.virtual_disk = NULL;
	node->type = type; node->file_bytes = (uint64_t)source->info.st_size;
	int error = SourcePath(source->fd, &node->path);
	if (error == 0) error = InitializeNode(node, maximum);
	if (error == 0) error = CheckVirtualDiskSource(node);
	if (error != 0) { CloseVirtualDiskSource(node); return error; }
	*result = node;
	return 0;
}

int StreamVirtualDiskSource(const IMAGE_FILE_SOURCE* source, int type, uint64_t maximum,
	compressed_data_t consumer, void* context, COMPRESSED_IMAGE_RESULT* completed)
{
	VIRTUAL_DISK_NODE* node = source->virtual_disk;
	const bool temporary = node == NULL;
	int error = temporary ? OpenVirtualDiskSource(source, type, maximum, &node) : CheckVirtualDiskSource(node);
	if (error == 0 && (node->type != type || node->disk_bytes > maximum)) error = EFBIG;
	uint8_t bytes[VIRTUAL_DISK_TRANSFER];
	for (uint64_t offset = 0; error == 0 && offset < node->disk_bytes;) {
		if (IsIsoScanCancelled()) { error = ECANCELED; break; }
		const size_t count = node->disk_bytes - offset < sizeof(bytes) ? (size_t)(node->disk_bytes - offset) : sizeof(bytes);
		error = node->read(node, offset, bytes, count);
		if (error == 0 && consumer) { error = consumer(context, bytes, count, offset); if (error < 0) error = EIO; }
		if (error == 0) {
			const size_t room = sizeof(completed->prefix) - completed->prefix_size;
			const size_t copy = count < room ? count : room;
			memcpy(completed->prefix + completed->prefix_size, bytes, copy); completed->prefix_size += copy;
			completed->decoded_bytes += count; offset += count;
		}
	}
	if (error == 0) error = CheckVirtualDiskSource(node);
	if (temporary) { const int closed = CloseVirtualDiskSource(node); if (closed != 0 && error == 0) error = closed; }
	if (error == 0) completed->members = 1;
	return error;
}
