/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "native_ntfs_efs.h"
#include <attrib.h>
#include <efs.h>
#include <volume.h>
#include "wimlib/avl_tree.h"
#include "wimlib/blob_table.h"
#include "wimlib/inode.h"
#include "wimlib/metadata.h"
#include "wimlib/resource.h"
#include "wimlib/wim.h"
#include <limits.h>

extern int IsIsoScanCancelled(void);

enum {
    EFS_METADATA_MAX_BYTES = 0x40000,
    EFS_HEADER_MAX_BYTES = 28 + 65535 * 4 + 16,
    EFS_NAME_MAX_BYTES = 2 * (1 + 255 + 6 + 1),
    EFS_FILE_HEADER_BYTES = 20,
    EFS_STREAM_HEADER_BYTES = 28,
    EFS_SEGMENT_HEADER_BYTES = 16,
    EFS_ENCRYPTION_HEADER_BYTES = 28,
    EFS_SECTOR_BYTES = 512,
    EFS_MAX_STREAMS = 65535
};
enum EfsStage { EfsFileHeader, EfsRecord, EfsStreamHeader, EfsEncryptionHeader, EfsPayload };

struct EfsName {
    struct EfsName* next;
    struct avl_tree_node index;
    size_t bytes;
    u8 name[];
};

struct WimNtfsEfs {
    ntfs_inode* target;
    ntfs_attr* attribute;
    struct EfsName* names;
    struct avl_tree_node* name_index;
    u8* header;
    u8* metadata;
    u64 total, consumed, logical_size, valid_size, segment_span;
    u64 remaining, block_left, block_offset, unit;
    size_t have, need, metadata_size;
    unsigned blocks, block_index, stream_count;
    u32 cluster_limit;
    enum EfsStage stage;
    bool stream_open, metadata_stream, encrypted, metadata_done, unnamed_seen;
};

static u16 Read16(const u8* data)
{
    le16 value;
    memcpy(&value, data, sizeof(value));
    return le16_to_cpu(value);
}
static u32 Read32(const u8* data)
{
    le32 value;
    memcpy(&value, data, sizeof(value));
    return le32_to_cpu(value);
}
static u64 Read64(const u8* data)
{
    le64 value;
    memcpy(&value, data, sizeof(value));
    return le64_to_cpu(value);
}
static int InvalidEfs(void) { return WIMLIB_ERR_INVALID_METADATA_RESOURCE; }

static void LimitEfsHole(struct WimNtfsEfs* state, u64 start, u64 end)
{
    if (start >= end) return;
    const u64 boundaries = start | end;
    const u64 alignment = boundaries & (~boundaries + 1);
    if (alignment < state->cluster_limit) state->cluster_limit = alignment;
}

static int CompareEfsNames(const struct avl_tree_node* left_node, const struct avl_tree_node* right_node)
{
    const struct EfsName* left = avl_tree_entry(left_node, struct EfsName, index);
    const struct EfsName* right = avl_tree_entry(right_node, struct EfsName, index);
    const int compared = memcmp(left->name, right->name, min(left->bytes, right->bytes));
    if (compared != 0) return compared;
    return (left->bytes > right->bytes) - (left->bytes < right->bytes);
}

static int WriteEfsBytes(ntfs_attr* attribute, u64 offset, const void* bytes, size_t size)
{
    if (attribute == NULL) return 0;
    const u8* input = bytes;
    while (size != 0) {
        if (IsIsoScanCancelled()) return WIMLIB_ERR_ABORTED_BY_PROGRESS;
        const s64 written = ntfs_attr_pwrite(attribute, offset, size, input);
        if (written <= 0 || (u64)written > size) return WIMLIB_ERR_NTFS_3G;
        input += written;
        offset += written;
        size -= written;
    }
    return 0;
}

static int CloseEfsStream(struct WimNtfsEfs* state)
{
    if (!state->stream_open) return 0;
    int error = 0;
    if (state->metadata_stream) {
        const bool valid_size = state->metadata_size > 0x54
            && state->metadata_size <= EFS_METADATA_MAX_BYTES;
        if (!valid_size || Read32(state->metadata) != state->metadata_size) return InvalidEfs();
        state->metadata_done = true;
    }
    if (state->attribute != NULL) {
        ntfs_attr* attribute = state->attribute;
        if (state->encrypted) {
            const u64 padded = (state->logical_size + EFS_SECTOR_BYTES - 1) & ~(u64)(EFS_SECTOR_BYTES - 1);
            if (state->logical_size != 0) {
                le16 padding = cpu_to_le16(padded - state->logical_size);
                if (ntfs_attr_truncate(attribute, padded + sizeof(padding))) error = WIMLIB_ERR_NTFS_3G;
                if (error == 0) error = WriteEfsBytes(attribute, padded, &padding, sizeof(padding));
            } else if (ntfs_attr_truncate(attribute, 0)) error = WIMLIB_ERR_NTFS_3G;
            if (error == 0 && ntfs_efs_fixup_attribute(NULL, attribute)) error = WIMLIB_ERR_NTFS_3G;
            if (error == 0) {
                ntfs_attr_search_ctx* context = ntfs_attr_get_search_ctx(state->target, NULL);
                if (context == NULL) error = WIMLIB_ERR_NTFS_3G;
                else {
                    if (ntfs_attr_lookup(AT_DATA, attribute->name, attribute->name_len,
                        CASE_SENSITIVE, 0, NULL, 0, context)) error = WIMLIB_ERR_NTFS_3G;
                    else {
                        /* The raw API retains the original VDL independently
                         * of ciphertext padding and the logical stream size. */
                        context->attr->initialized_size = cpu_to_le64(state->valid_size);
                        NInoSetDirty(state->target);
                    }
                    ntfs_attr_put_search_ctx(context);
                }
            }
        } else if (ntfs_attr_truncate(attribute, state->logical_size)) error = WIMLIB_ERR_NTFS_3G;
        if (ntfs_attr_pclose(attribute) && error == 0) error = WIMLIB_ERR_NTFS_3G;
        ntfs_attr_close(attribute);
        state->attribute = NULL;
    }
    state->stream_open = false;
    return error;
}

static int OpenEfsStream(struct WimNtfsEfs* state)
{
    const u8* header = state->header;
    const u32 name_bytes = Read32(header + 24);
    const u32 flags = Read32(header + 12);
    if (name_bytes != state->need - EFS_STREAM_HEADER_BYTES || flags > 1) return InvalidEfs();
    const u8* name = header + EFS_STREAM_HEADER_BYTES;
    const bool metadata = name_bytes == 2 && Read16(name) == 0x1910;
    if (metadata && (state->metadata_done || state->stream_count != 0)) return InvalidEfs();
    if (!metadata && !state->metadata_done) return InvalidEfs();
    state->metadata_stream = metadata;
    state->encrypted = !metadata && flags == 0;
    state->logical_size = state->valid_size = state->segment_span = 0;
    state->stream_open = true;
    if (++state->stream_count > EFS_MAX_STREAMS) return InvalidEfs();
    if (metadata) return 0;
    if (name_bytes < 16 || name_bytes > EFS_NAME_MAX_BYTES || (name_bytes & 1)) return InvalidEfs();
    const u16 suffix[] = {':', '$', 'D', 'A', 'T', 'A', 0};
    if (Read16(name) != ':') return WIMLIB_ERR_UNSUPPORTED;
    for (size_t i = 0; i < sizeof(suffix) / sizeof(suffix[0]); ++i)
        if (Read16(name + name_bytes - sizeof(suffix) + 2 * i) != suffix[i]) return WIMLIB_ERR_UNSUPPORTED;
    const size_t stream_bytes = name_bytes - 2 - sizeof(suffix);
    for (size_t i = 0; i < stream_bytes; i += 2) {
        const u16 character = Read16(name + 2 + i);
        if (character == 0 || character == ':') return InvalidEfs();
    }
    /* Keep names for preflight duplicate detection too; no target is needed.
     * NTFS's 16-bit attribute instance space bounds the complete name list. */
    struct EfsName* saved = CALLOC(1, sizeof(*saved) + stream_bytes);
    if (saved == NULL) return WIMLIB_ERR_NOMEM;
    saved->bytes = stream_bytes;
    memcpy(saved->name, name + 2, stream_bytes);
    if (avl_tree_insert(&state->name_index, &saved->index, CompareEfsNames) != NULL) {
        FREE(saved);
        return InvalidEfs();
    }
    saved->next = state->names;
    state->names = saved;
    if (stream_bytes == 0) state->unnamed_seen = true;
    if (state->target != NULL) {
        ntfschar* native_name = stream_bytes == 0 ? AT_UNNAMED : (ntfschar*)(name + 2);
        const size_t characters = stream_bytes / 2;
        if (characters != 0 && ntfs_attr_add(state->target, AT_DATA, native_name, characters, NULL, 0))
            return WIMLIB_ERR_NTFS_3G;
        state->attribute = ntfs_attr_open(state->target, AT_DATA, native_name, characters);
        if (state->attribute == NULL) return WIMLIB_ERR_NTFS_3G;
    }
    return 0;
}

static int ReadEfsEncryptionHeader(struct WimNtfsEfs* state)
{
    const u8* header = state->header;
    const u64 start = Read64(header);
    const u32 within_size = Read32(header + 12), within_vdl = Read32(header + 16);
    const unsigned shift = header[22], blocks = Read16(header + 26);
    const size_t table_end = EFS_ENCRYPTION_HEADER_BYTES + blocks * sizeof(le32);
    if (shift > 31 || header[23] != shift || header[24] > shift || header[25] != 1
        || Read16(header + 20) != 0) return WIMLIB_ERR_UNSUPPORTED;
    if (table_end > state->need || start > INT64_MAX - (u64)UINT32_MAX
        || start < state->segment_span || within_vdl > within_size) return InvalidEfs();
    const u64 unit = (u64)1 << shift;
    const u64 span = unit * blocks;
    if (within_size > span || start > INT64_MAX - span) return InvalidEfs();
    if (start + within_size > INT64_MAX - EFS_SECTOR_BYTES - sizeof(le16)) return InvalidEfs();
    u64 payload = 0;
    LimitEfsHole(state, state->segment_span, start);
    for (unsigned i = 0; i < blocks; ++i) {
        const u32 bytes = Read32(header + EFS_ENCRYPTION_HEADER_BYTES + 4 * i);
        /* ENCRYPTED_DATA_INFO indexes logical units independently of the
         * serialized byte counts. MS-EFSR permits an oversized block across
         * VDL or a sparse hole; it must not move the next unit's offset. */
        if (bytes > INT64_MAX - (start + (u64)i * unit)) return InvalidEfs();
        const u64 block = (u64)i * unit;
        if (bytes == 0 || (bytes < unit && block + bytes < within_vdl))
            LimitEfsHole(state, start + block + bytes, start + block + unit);
        payload += bytes;
    }
    if (state->target != NULL && state->target->vol->cluster_size > state->cluster_limit)
        return WIMLIB_ERR_UNSUPPORTED;
    if (payload != state->remaining || start + within_size < state->logical_size) return InvalidEfs();
    state->logical_size = start + within_size;
    if (within_vdl != 0) state->valid_size = start + within_vdl;
    state->segment_span = start + span;
    state->unit = unit;
    state->blocks = blocks;
    state->block_index = 0;
    state->block_left = 0;
    state->block_offset = start;
    return 0;
}

static void NextEfsRecord(struct WimNtfsEfs* state)
{
    state->stage = EfsRecord;
    state->have = 0;
    state->need = 12;
}

static int ParseEfsHeader(struct WimNtfsEfs* state)
{
    const u8* header = state->header;
    switch (state->stage) {
    case EfsFileHeader:
        if (Read32(header) != 0x100 || memcmp(header + 4, "R\0O\0B\0S\0", 8)) return InvalidEfs();
        NextEfsRecord(state);
        return 0;
    case EfsRecord: {
        const u32 length = Read32(header);
        if (memcmp(header + 4, "N\0T\0F\0S\0", 8) == 0) {
            const int closed = CloseEfsStream(state);
            if (closed != 0) return closed;
            if (length < EFS_STREAM_HEADER_BYTES + 2 || length > EFS_STREAM_HEADER_BYTES + EFS_NAME_MAX_BYTES
                || length - state->have > state->total - state->consumed) return InvalidEfs();
            state->stage = EfsStreamHeader;
            state->need = length;
        } else if (memcmp(header + 4, "G\0U\0R\0E\0", 8) == 0) {
            if (!state->stream_open || length < EFS_SEGMENT_HEADER_BYTES
                || length - state->have > state->total - state->consumed) return InvalidEfs();
            state->remaining = length - EFS_SEGMENT_HEADER_BYTES;
            state->need = EFS_SEGMENT_HEADER_BYTES;
            /* The reserved segment DWORD is consumed before its payload. */
            state->stage = EfsPayload;
        } else return InvalidEfs();
        return 0;
    }
    case EfsStreamHeader: {
        const int opened = OpenEfsStream(state);
        if (opened == 0) NextEfsRecord(state);
        return opened;
    }
    case EfsPayload:
        if (state->encrypted) {
            if (state->remaining < EFS_ENCRYPTION_HEADER_BYTES) return InvalidEfs();
            state->stage = EfsEncryptionHeader;
            state->have = 0;
            state->need = 12;
        } else {
            state->have = state->need = 0;
            if (state->remaining == 0) NextEfsRecord(state);
        }
        return 0;
    case EfsEncryptionHeader:
        if (state->need == 12) {
            const u32 length = Read32(header + 8);
            if (length < EFS_ENCRYPTION_HEADER_BYTES || length > EFS_HEADER_MAX_BYTES
                || length > state->remaining) return InvalidEfs();
            state->need = length;
            return 0;
        }
        state->remaining -= state->need;
        const int error = ReadEfsEncryptionHeader(state);
        if (error != 0) return error;
        state->stage = EfsPayload;
        state->have = state->need = 0;
        if (state->remaining == 0) NextEfsRecord(state);
        return 0;
    }
    return InvalidEfs();
}

int BeginWimNtfsEfs(ntfs_inode* target, uint64_t bytes, struct WimNtfsEfs** result)
{
    if (result == NULL) return WIMLIB_ERR_INVALID_PARAM;
    *result = NULL;
    if (bytes < EFS_FILE_HEADER_BYTES || bytes > INT64_MAX) return InvalidEfs();
    struct WimNtfsEfs* state = CALLOC(1, sizeof(*state));
    if (state == NULL) return WIMLIB_ERR_NOMEM;
    state->header = MALLOC(EFS_HEADER_MAX_BYTES);
    state->metadata = MALLOC(EFS_METADATA_MAX_BYTES);
    if (state->header == NULL || state->metadata == NULL) {
        CloseWimNtfsEfs(state);
        return WIMLIB_ERR_NOMEM;
    }
    state->target = target;
    state->total = bytes;
    state->cluster_limit = UINT32_MAX;
    state->need = EFS_FILE_HEADER_BYTES;
    state->stage = EfsFileHeader;
    *result = state;
    return 0;
}

int ConsumeWimNtfsEfs(struct WimNtfsEfs* state, uint64_t offset, const void* data, size_t bytes)
{
    if (state == NULL || (bytes != 0 && data == NULL)) return WIMLIB_ERR_INVALID_PARAM;
    if (offset != state->consumed || bytes > state->total - state->consumed) return InvalidEfs();
    const u8* input = data;
    while (bytes != 0) {
        if (IsIsoScanCancelled()) return WIMLIB_ERR_ABORTED_BY_PROGRESS;
        size_t count;
        int error = 0;
        if (state->need != 0) {
            count = min(bytes, state->need - state->have);
            memcpy(state->header + state->have, input, count);
            state->have += count;
            state->consumed += count;
            if (state->have == state->need) error = ParseEfsHeader(state);
        } else {
            count = min((u64)bytes, state->remaining);
            if (state->metadata_stream) {
                if (count > EFS_METADATA_MAX_BYTES - state->metadata_size) return InvalidEfs();
                memcpy(state->metadata + state->metadata_size, input, count);
                state->metadata_size += count;
            } else if (state->encrypted) {
                while (state->block_left == 0 && state->block_index < state->blocks) {
                    state->block_left = Read32(state->header + EFS_ENCRYPTION_HEADER_BYTES + 4 * state->block_index++);
                    if (state->block_left == 0) state->block_offset += state->unit;
                }
                if (state->block_left == 0) return InvalidEfs();
                count = min((u64)count, state->block_left);
                const u32 block_size = Read32(state->header + EFS_ENCRYPTION_HEADER_BYTES + 4 * (state->block_index - 1));
                const u64 within_unit = block_size - state->block_left;
                const size_t stored = within_unit < state->unit ? min((u64)count, state->unit - within_unit) : 0;
                /* Serialized encryption padding may span the next logical
                 * unit. Consume it without allocating that unit's zero hole. */
                error = WriteEfsBytes(state->attribute, state->block_offset + within_unit, input, stored);
                state->block_left -= count;
                if (state->block_left == 0) state->block_offset += state->unit;
            } else {
                if (count > INT64_MAX - state->logical_size) return InvalidEfs();
                error = WriteEfsBytes(state->attribute, state->logical_size, input, count);
                state->logical_size += count;
                state->valid_size = state->logical_size;
            }
            state->consumed += count;
            state->remaining -= count;
            if (state->remaining == 0) NextEfsRecord(state);
        }
        if (error != 0) return error;
        input += count;
        bytes -= count;
    }
    return 0;
}

int FinishWimNtfsEfs(struct WimNtfsEfs* state)
{
    if (state == NULL) return WIMLIB_ERR_INVALID_PARAM;
    if (state->consumed != state->total || state->stage != EfsRecord || state->have != 0) return InvalidEfs();
    int error = CloseEfsStream(state);
    if (error != 0) return error;
    if (!state->metadata_done) return InvalidEfs();
    if (state->target == NULL) return 0;
    const bool directory = state->target->mrec->flags & MFT_RECORD_IS_DIRECTORY;
    if (!directory && !state->unnamed_seen) {
        /* A metadata-only empty file still has a nonresident encrypted
         * default stream. Directories do not have a default $DATA stream. */
        state->attribute = ntfs_attr_open(state->target, AT_DATA, AT_UNNAMED, 0);
        if (state->attribute == NULL) return WIMLIB_ERR_NTFS_3G;
        state->stream_open = state->encrypted = true;
        state->metadata_stream = false;
        state->logical_size = state->valid_size = 0;
        error = CloseEfsStream(state);
        if (error != 0) return error;
    }
    ntfschar name[] = {cpu_to_le16('$'), cpu_to_le16('E'), cpu_to_le16('F'), cpu_to_le16('S')};
    if (ntfs_attr_add(state->target, AT_LOGGED_UTILITY_STREAM, name, 4, NULL, 0)) return WIMLIB_ERR_NTFS_3G;
    ntfs_attr* attribute = ntfs_attr_open(state->target, AT_LOGGED_UTILITY_STREAM, name, 4);
    if (attribute == NULL) return WIMLIB_ERR_NTFS_3G;
    error = WriteEfsBytes(attribute, 0, state->metadata, state->metadata_size);
    if (ntfs_attr_pclose(attribute) && error == 0) error = WIMLIB_ERR_NTFS_3G;
    ntfs_attr_close(attribute);
    if (error == 0) {
        state->target->flags &= ~FILE_ATTR_NORMAL;
        state->target->flags |= FILE_ATTR_ENCRYPTED;
        NInoSetDirty(state->target);
        NInoFileNameSetDirty(state->target);
    }
    return error;
}

int CloseWimNtfsEfs(struct WimNtfsEfs* state)
{
    if (state == NULL) return 0;
    int error = 0;
    if (state->attribute != NULL) {
        if (ntfs_attr_pclose(state->attribute)) error = WIMLIB_ERR_NTFS_3G;
        ntfs_attr_close(state->attribute);
    }
    while (state->names != NULL) {
        struct EfsName* name = state->names;
        state->names = name->next;
        FREE(name);
    }
    FREE(state->header);
    FREE(state->metadata);
    FREE(state);
    return error;
}

static int CheckEfsChunk(const struct blob_descriptor* blob, u64 offset, const void* chunk, size_t size, void* context)
{
    (void)blob;
    return ConsumeWimNtfsEfs(context, offset, chunk, size);
}

int ValidateWimNtfsEncryptedImage(WIMStruct* wim, int image, uint32_t* cluster_limit)
{
    if (cluster_limit == NULL) return WIMLIB_ERR_INVALID_PARAM;
    *cluster_limit = 0;
    int error = select_wim_image(wim, image);
    if (error != 0) return error;
    struct wim_inode* inode;
    hlist_for_each_entry(inode, &wim_get_current_image_metadata(wim)->inode_list, i_hlist_node) {
        if (IsIsoScanCancelled()) return WIMLIB_ERR_ABORTED_BY_PROGRESS;
        if (!(inode->i_attributes & FILE_ATTRIBUTE_ENCRYPTED)) continue;
        const u32 incompatible = FILE_ATTRIBUTE_COMPRESSED | FILE_ATTRIBUTE_REPARSE_POINT;
        if (inode->i_attributes & incompatible) return WIMLIB_ERR_UNSUPPORTED;
        bool found = false;
        for (unsigned i = 0; i < inode->i_num_streams; ++i) {
            const struct wim_inode_stream* stream = &inode->i_streams[i];
            if (stream->stream_type != STREAM_TYPE_EFSRPC_RAW_DATA) continue;
            struct blob_descriptor* blob = stream_blob(stream, wim->blob_table);
            if (found || blob == NULL) return InvalidEfs();
            found = true;
            struct WimNtfsEfs* state = NULL;
            error = BeginWimNtfsEfs(NULL, blob->size, &state);
            const struct read_blob_callbacks callbacks = {.continue_blob = CheckEfsChunk, .ctx = state};
            if (error == 0) error = read_blob_with_sha1(blob, &callbacks, false);
            if (error == 0) error = FinishWimNtfsEfs(state);
            if (error == 0 && state->cluster_limit != UINT32_MAX
                && (*cluster_limit == 0 || state->cluster_limit < *cluster_limit)) *cluster_limit = state->cluster_limit;
            const int closed = CloseWimNtfsEfs(state);
            if (error == 0) error = closed;
            if (error != 0) return error;
        }
        if (!found) return InvalidEfs();
    }
    return 0;
}
