// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <endian.h>
#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/statfs.h>
#include <sys/xattr.h>

#define RUFUS_NTFS_COMPRESSED UINT32_C(0x00000800)
#define RUFUS_NTFS_ATTRIBUTES "system.ntfs_attrib_be"

static inline int ReadNtfsDirectoryAttributes(int fd, uint32_t* attributes, bool* ntfs)
{
    const long ntfs_magic = 0x5346544e, fuse_magic = 0x65735546;
    struct statfs filesystem;
    uint32_t encoded = 0;
    *attributes = 0; *ntfs = false;
    if (fstatfs(fd, &filesystem) != 0)
        return errno;
    if (filesystem.f_type != ntfs_magic && filesystem.f_type != fuse_magic)
        return 0;
    *ntfs = true;
    const ssize_t size = fgetxattr(fd, RUFUS_NTFS_ATTRIBUTES, &encoded, sizeof(encoded));
    if (size < 0)
        return errno;
    if (size != sizeof(encoded))
        return EBADMSG;
    *attributes = be32toh(encoded);
    return 0;
}

static inline int SetNtfsDirectoryCompression(int fd, bool enable)
{
    uint32_t attributes = 0;
    bool ntfs;
    int error = ReadNtfsDirectoryAttributes(fd, &attributes, &ntfs);
    if (error != 0 || !ntfs)
        return error;
    const uint32_t expected = enable ? attributes | RUFUS_NTFS_COMPRESSED : attributes & ~RUFUS_NTFS_COMPRESSED;
    if (attributes == expected)
        return 0;
    const uint32_t encoded = htobe32(expected);
    if (fsetxattr(fd, RUFUS_NTFS_ATTRIBUTES, &encoded, sizeof(encoded), 0) != 0)
        return errno;
    error = ReadNtfsDirectoryAttributes(fd, &attributes, &ntfs);
    if (error != 0)
        return error;
    return (attributes & RUFUS_NTFS_COMPRESSED) == (expected & RUFUS_NTFS_COMPRESSED) ? 0 : EIO;
}

// NTFS-3G permits changing directory defaults, not individual file compression.
// The helper is the only writer in its private, exclusively claimed fresh volume.
// Restore the parent's default immediately after creating the uncompressed file,
// including when creation fails, before copying any payload.
static inline int PrepareNtfsUncompressedCreation(int parent, bool* restore)
{
    uint32_t attributes = 0;
    bool ntfs;
    *restore = false;
    const int error = ReadNtfsDirectoryAttributes(parent, &attributes, &ntfs);
    if (error != 0 || !ntfs)
        return error;
    *restore = (attributes & RUFUS_NTFS_COMPRESSED) != 0;
    return *restore ? SetNtfsDirectoryCompression(parent, false) : 0;
}
