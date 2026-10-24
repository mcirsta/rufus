/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "iso_bootloaders.h"
#include "iso_file.h"
#include "image_file.h"
#include <string.h>
int ScanOriginalGrubAndEfiBootloaders(const char*, iso_extension_mask_t, RUFUS_IMG_REPORT*);

/* Windows repairs Mint's dangling small x64 fallback from El Torito later.
 * Its missing banner is not an incomplete source. Keep every other original
 * content check, and only omit this entry when the repair image is readable. */
int ScanGrubAndEfiBootloaders(const char* image, iso_extension_mask_t mask, RUFUS_IMG_REPORT* report)
{
    if (!(report->has_efi & 0x4000) || !HAS_EFI_IMG((*report)))
        return ScanOriginalGrubAndEfiBootloaders(image,mask,report);
    udf_t* udf = udf_open(image);
    iso9660_t* iso = udf ? NULL : iso9660_open_ext(image,mask);
    ISO_FILE_READER reader = {0};
    const int opened = (iso || udf) ? OpenIsoFileReader(&reader,iso,udf,report->efi_img_path) : ISO_SCAN_ERROR;
    CloseIsoFileReader(&reader);
    if (iso) iso9660_close(iso);
    if (udf) udf_close(udf);
    if (opened == ISO_SCAN_CANCELLED) return opened;
    if (opened != ISO_SCAN_OK) return ScanOriginalGrubAndEfiBootloaders(image,mask,report);
    RUFUS_IMG_REPORT repaired = *report;
    for (size_t i = 0; i < sizeof(repaired.efi_boot_entry)/sizeof(repaired.efi_boot_entry[0]); ++i)
        if (repaired.efi_boot_entry[i].type == EBT_MAIN && strcasecmp(repaired.efi_boot_entry[i].path,"/efi/boot/bootx64.efi") == 0)
            repaired.efi_boot_entry[i].path[0] = 0;
    const int status = ScanOriginalGrubAndEfiBootloaders(image,mask,&repaired);
    memcpy(repaired.efi_boot_entry,report->efi_boot_entry,sizeof(repaired.efi_boot_entry));
    *report = repaired;
    return status;
}
