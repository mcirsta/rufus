/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "native_ntfs_ea.h"
#include <ea.h>
#include "wimlib/xattr.h"
#include "wimlib/inode.h"
#include "wimlib/error.h"

/* Native NTFS EA_INFORMATION stores the packed size in a 16-bit field. */
enum { NTFS_EA_PACKED_MAX_BYTES = 65535, NTFS_EA_HEADER_BYTES = 8, NTFS_EA_ALIGNMENT = 4 };

int SetWimNtfsExtendedAttributes(ntfs_inode* target, const struct wim_inode* source)
{
	u32 length = 0;
	const u8* entries = inode_get_xattrs(source, &length);
	size_t input = 0, output = 0;
	u8* encoded;
	int error = 0;

	if (entries == NULL || length == 0)
		return 0;
	if (length > NTFS_EA_PACKED_MAX_BYTES)
		return WIMLIB_ERR_INVALID_XATTR;
	while (input < length) {
		const struct wim_xattr_entry* entry = (const void*)(entries + input);
		if (!valid_xattr_entry(entry, length - input))
			return WIMLIB_ERR_INVALID_XATTR;
		if (entry->flags & ~NEED_EA)
			return WIMLIB_ERR_INVALID_XATTR;
		const size_t bytes = NTFS_EA_HEADER_BYTES + entry->name_len + 1 + le16_to_cpu(entry->value_len);
		output += ALIGN(bytes, NTFS_EA_ALIGNMENT);
		input += xattr_entry_size(entry);
	}
	/* The input bound also bounds this expansion: each entry adds four header
	 * bytes and at most three padding bytes. No unbounded per-file allocation. */
	encoded = CALLOC(1, output);
	if (encoded == NULL)
		return WIMLIB_ERR_NOMEM;
	input = output = 0;
	while (input < length) {
		const struct wim_xattr_entry* entry = (const void*)(entries + input);
		EA_ATTR* native = (void*)(encoded + output);
		const size_t payload = entry->name_len + 1 + le16_to_cpu(entry->value_len);
		const size_t bytes = ALIGN(NTFS_EA_HEADER_BYTES + payload, NTFS_EA_ALIGNMENT);
		/* libntfs requires a nonzero next offset even for the final entry,
		 * unlike NtSetEaFile. The offset describes the complete padded row. */
		native->next_entry_offset = cpu_to_le32(bytes);
		native->flags = entry->flags;
		native->name_length = entry->name_len;
		native->value_length = entry->value_len;
		memcpy(native->name, entry->name, payload);
		input += xattr_entry_size(entry);
		output += bytes;
	}
	if (ntfs_set_ntfs_ea(target, (const char*)encoded, output, 0) != 0) {
		ERROR_WITH_ERRNO("Failed to restore extended attributes in NTFS volume");
		error = WIMLIB_ERR_SET_XATTR;
	}
	FREE(encoded);
	return error;
}
