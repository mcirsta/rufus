// SPDX-License-Identifier: GPL-3.0-or-later
// Compare ordinary ISO reading and Rufus's synthetic El Torito paths.
#include <cdio/iso9660.h>
#include <cdio/udf.h>

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

int main(int argc, char** argv)
{
    struct stat status;
    iso9660_t* iso;
    iso9660_stat_t* entry;
    udf_t* udf;
    char* label = NULL;
    unsigned char block[ISO_BLOCKSIZE];
    int result = 0;

    if (argc != 3 || stat(argv[1], &status) != 0 || !S_ISREG(status.st_mode))
    {
        fprintf(stderr, "Usage: %s regular-image-file internal-path\n", argv[0]);
        return 2;
    }
    iso = iso9660_open_ext(argv[1], ISO_EXTENSION_ALL);
    if (iso == NULL)
    {
        puts("ISO_OPEN_FAILED");
        return 1;
    }
    udf = udf_open(argv[1]);
    printf("UDF=%d\n", udf != NULL);
    udf_close(udf);
    if (iso9660_ifs_get_volume_id(iso, &label))
    {
        printf("LABEL=%s\n", label);
        free(label);
    }
    entry = iso9660_ifs_stat_translate(iso, argv[2]);
    if (entry == NULL)
    {
        puts("ENTRY_MISSING");
        result = 3;
    }
    else
    {
        // bundled_cdio exports this ABI selection and its aggregate 64-bit
        // extent size. The upstream system ABI exposes the ordinary size.
#ifdef DO_NOT_WANT_COMPATIBILITY
        const uint64_t entry_size = entry->total_size;
#else
        const uint64_t entry_size = entry->size;
#endif
        printf("SIZE=%" PRIu64 "\n", entry_size);
        if (iso9660_iso_seek_read(iso, block, entry->lsn, 1) != sizeof(block))
        {
            puts("READ_FAILED");
            result = 4;
        }
        else
        {
            printf("PREFIX=");
            for (size_t i = 0; i < 16; ++i)
                printf("%02x", block[i]);
            putchar('\n');
        }
        iso9660_stat_free(entry);
    }
    iso9660_close(iso);
    return result;
}
