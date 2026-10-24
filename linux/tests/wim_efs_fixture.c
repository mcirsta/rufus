/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "wimlib/dentry.h"
#include "wimlib/inode.h"
#include "wimlib/wim.h"

/* The directory capture backend treats our supplied bytes as ordinary data.
 * Mark those same independently assembled raw bytes as an EFS stream before
 * serializing the fixture WIM. No Windows API or captured private key is used. */
int MarkWimEfsFixture(WIMStruct* wim, int image)
{
    int error = select_wim_image(wim, image);
    if (error != 0) return error;
    const char* paths[] = {"/encrypted.bin", "/encrypted-directory", "/empty-encrypted.bin"};
    for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        struct wim_dentry* entry = get_dentry(wim, paths[i], WIMLIB_CASE_SENSITIVE);
        if (entry == NULL) return WIMLIB_ERR_PATH_DOES_NOT_EXIST;
        struct wim_inode* inode = entry->d_inode;
        inode->i_attributes |= FILE_ATTRIBUTE_ENCRYPTED;
        if (i == 1) inode->i_attributes |= FILE_ATTRIBUTE_DIRECTORY;
        else if (i == 0) inode->i_attributes |= FILE_ATTRIBUTE_SPARSE_FILE;
        if (inode->i_num_streams != 1) return WIMLIB_ERR_INVALID_METADATA_RESOURCE;
        inode->i_streams[0].stream_type = STREAM_TYPE_EFSRPC_RAW_DATA;
    }
    return 0;
}
