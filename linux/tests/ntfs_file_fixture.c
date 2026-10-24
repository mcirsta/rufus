/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Test-only NTFS-3G file population/readback. Never mounts a host filesystem
 * or runs as root. Production copying still uses the existing native driver. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <ntfs-3g/volume.h>
#include <ntfs-3g/dir.h>
#include <ntfs-3g/attrib.h>
#include <ntfs-3g/endians.h>

enum { BUFFER_BYTES = 64 * 1024, MAX_FILE_BYTES = 1024 * 1024 };

static int CheckRegularFile(const char* path, off_t maximum)
{
    struct stat info;
    if (lstat(path, &info) != 0)
        return errno;
    const int regular = S_ISREG(info.st_mode) && info.st_nlink == 1;
    const int owned = info.st_uid == getuid();
    const int size_valid = info.st_size > 0 && (maximum == 0 || info.st_size <= maximum);
    return regular && owned && size_valid ? 0 : EINVAL;
}

static int CopyData(ntfs_attr* attribute, FILE* input, int putting)
{
    unsigned char data[BUFFER_BYTES];
    s64 position = 0;
    if (!putting && (attribute->data_size < 0 || attribute->data_size > MAX_FILE_BYTES))
        return EFBIG;
    for (;;) {
        const s64 count = putting ? (s64)fread(data, 1, sizeof(data), input) :
            ntfs_attr_pread(attribute, position, sizeof(data), data);
        if (count < 0)
            return errno;
        if (count == 0)
            return putting && ferror(input) ? EIO : 0;
        if (position > MAX_FILE_BYTES - count)
            return EFBIG;
        s64 done = 0;
        while (done < count) {
            const s64 written = putting ? ntfs_attr_pwrite(attribute, position + done,
                count - done, data + done) : (s64)fwrite(data + done, 1, count - done, stdout);
            if (written <= 0)
                return errno != 0 ? errno : EIO;
            done += written;
        }
        position += count;
    }
}

int main(int argc, char** argv)
{
    if (getuid() == 0 || argc < 4)
        return 64;
    const int putting = strcmp(argv[1], "put") == 0;
    const int getting = strcmp(argv[1], "get") == 0;
    const int removing = strcmp(argv[1], "remove") == 0;
    const int directory = strcmp(argv[1], "mkdir") == 0;
    if ((!putting && !getting && !removing && !directory) || argc != (putting ? 5 : 4))
        return 64;
    const char* name = argv[3];
    const int image_readback = getting && (strcmp(name, "menu.lst") == 0 ||
        strcmp(name, "image-marker") == 0 || strcmp(name, "payload.bin") == 0 ||
        strcmp(name, "EFI/BOOT/BOOTX64.EFI") == 0);
    const int recovery_file = strcmp(name, "Windows/System32/ntoskrnl.exe") == 0 ||
        strcmp(name, "Windows/System32/Recovery/ReAgent.xml") == 0 || strcmp(name, "Recovery/WindowsRE/ReAgent.xml") == 0;
    const int recovery_directory = strcmp(name, "Windows") == 0 || strcmp(name, "Windows/System32") == 0 ||
        strcmp(name, "Windows/System32/Recovery") == 0 || strcmp(name, "Recovery") == 0 || strcmp(name, "Recovery/WindowsRE") == 0;
    const int allowed_name = directory ? recovery_directory : recovery_file || strcmp(name, "payload.bin") == 0 ||
        strcmp(name, "BOOTMGR") == 0 || strcmp(name, "NTLDR") == 0 ||
        strcmp(name, "ntdetect.com") == 0 || strcmp(name, "TXTSETUP.SIF") == 0 ||
        strcmp(name, "grldr") == 0 || image_readback;
    const int removable_name = !removing || strcmp(name, "grldr") == 0;
    if (!allowed_name || !removable_name)
        return 64;
    int error = CheckRegularFile(argv[2], 0);
    if (error == 0 && putting)
        error = CheckRegularFile(argv[4], MAX_FILE_BYTES);
    if (error != 0)
        return 65;
    FILE* input = putting ? fopen(argv[4], "rb") : NULL;
    if (putting && input == NULL)
        return 65;
    const ntfs_mount_flags flags = putting || removing || directory ? NTFS_MNT_EXCLUSIVE : NTFS_MNT_RDONLY | NTFS_MNT_FORENSIC;
    ntfs_volume* volume = ntfs_mount(argv[2], flags);
    if (volume == NULL) {
        if (input != NULL)
            fclose(input);
        perror("NTFS fixture open");
        return 66;
    }
    ntfs_inode* root = ntfs_inode_open(volume, FILE_root);
    ntfs_inode* file = NULL;
    ntfs_attr* attribute = NULL;
    if (root == NULL)
        error = errno;
    else if (putting || directory) {
        char parent[128];
        const char* leaf = strrchr(name, '/');
        if (leaf != NULL) {
            const size_t parent_length = (size_t)(leaf - name);
            if (parent_length >= sizeof(parent)) abort();
            memcpy(parent, name, parent_length); parent[parent_length] = 0;
            if (ntfs_inode_close(root) != 0) error = errno;
            root = error == 0 ? ntfs_pathname_to_inode(volume, NULL, parent) : NULL;
            if (root == NULL && error == 0) error = errno != 0 ? errno : EIO;
            name = leaf + 1;
        }
        ntfschar unicode[32];
        const size_t length = strlen(name);
        if (length > sizeof(unicode) / sizeof(*unicode)) abort();
        for (size_t i = 0; i < length; ++i)
            unicode[i] = cpu_to_le16((unsigned char)name[i]);
        if (error == 0) file = ntfs_create(root, 0, unicode, length, directory ? S_IFDIR : S_IFREG);
    } else
        file = ntfs_pathname_to_inode(volume, root, name);
    if (file == NULL && error == 0)
        error = errno != 0 ? errno : EIO;
    if (error == 0 && removing) {
        ntfschar unicode[32];
        const size_t length = strlen(name);
        for (size_t i = 0; i < length; ++i)
            unicode[i] = cpu_to_le16((unsigned char)name[i]);
        if (ntfs_delete(volume, name, file, root, unicode, length) != 0)
            error = errno;
        /* ntfs_delete closes both the child and parent, including on failure. */
        file = NULL;
        root = NULL;
    }
    if (error == 0 && !removing && !directory) {
        attribute = ntfs_attr_open(file, AT_DATA, AT_UNNAMED, 0);
        if (attribute == NULL)
            error = errno;
    }
    if (error == 0 && !removing && !directory)
        error = CopyData(attribute, input, putting);
    if (attribute != NULL)
        ntfs_attr_close(attribute);
    /* Mutations use the retained parent for FILE_NAME updates. Opening it again
     * during close can replace its cache entry. Readback may address a nested
     * path and needs no parent-directory update. */
    if (file != NULL) {
        const int closed = getting ? ntfs_inode_close(file) : ntfs_inode_close_in_dir(file, root);
        if (closed != 0 && error == 0)
            error = errno;
    }
    if (root != NULL && ntfs_inode_close(root) != 0 && error == 0)
        error = errno;
    if (ntfs_umount(volume, FALSE) != 0 && error == 0)
        error = errno;
    if (input != NULL && fclose(input) != 0 && error == 0)
        error = errno;
    if (getting && fflush(stdout) != 0 && error == 0)
        error = errno;
    if (error != 0)
        fprintf(stderr, "NTFS fixture error: %s\n", strerror(error));
    return error == 0 ? 0 : 67;
}
