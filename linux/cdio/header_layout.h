/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stddef.h>
#include <cdio/iso9660.h>
#include <cdio/udf.h>

#ifdef __cplusplus
#define CDIO_STAT_FILENAME_OFFSET (offsetof(iso9660_stat_t, b_xa) + sizeof(bool))
#else
#define CDIO_STAT_FILENAME_OFFSET offsetof(iso9660_stat_t, filename)
#endif

/* Compare every fixed record affected by native variable-payload declarations. */
#define CDIO_HEADER_LAYOUT_CASES(X) \
    X(iso_stat_size, sizeof(iso9660_stat_t)) \
    X(rock_extension_size, sizeof(iso_su_er_t)) \
    X(rock_link_part_size, sizeof(iso_rock_sl_part_t)) \
    X(rock_link_size, sizeof(iso_rock_sl_t)) \
    X(rock_name_size, sizeof(iso_rock_nm_t)) \
    X(rock_time_size, sizeof(iso_rock_tf_t)) \
    X(rock_record_size, sizeof(iso_extension_record_t)) \
    X(logical_volume_size, sizeof(struct logical_vol_desc_s)) \
    X(partition_map_size, sizeof(struct generic_partition_map)) \
    X(unallocated_space_size, sizeof(struct unalloc_space_desc_s)) \
    X(volume_integrity_size, sizeof(struct logvol_integrity_desc_s)) \
    X(file_identifier_size, sizeof(udf_fileid_desc_t)) \
    X(file_entry_size, sizeof(udf_file_entry_t)) \
    X(extended_file_entry_size, sizeof(struct extended_file_entry)) \
    X(generic_attribute_size, sizeof(struct generic_format_s)) \
    X(charset_attribute_size, sizeof(struct charSet_info_s)) \
    X(times_attribute_size, sizeof(struct infoTimesExtAttr)) \
    X(device_attribute_size, sizeof(struct deviceSpec)) \
    X(implementation_attribute_size, sizeof(struct impUseExtAttr)) \
    X(application_attribute_size, sizeof(struct appUseExtAttr)) \
    X(unallocated_entry_size, sizeof(struct unallocSpaceEntry)) \
    X(space_bitmap_size, sizeof(struct spaceBitmapDesc)) \
    X(path_component_size, sizeof(struct pathComponent)) \
    X(udf_directory_size, sizeof(udf_dirent_t)) \
    X(iso_stat_rr_offset, offsetof(iso9660_stat_t, rr)) \
    X(iso_stat_tm_offset, offsetof(iso9660_stat_t, tm)) \
    X(iso_stat_lsn_offset, offsetof(iso9660_stat_t, lsn)) \
    X(iso_stat_total_size_offset, offsetof(iso9660_stat_t, total_size)) \
    X(iso_stat_xa_offset, offsetof(iso9660_stat_t, xa)) \
    X(iso_stat_type_offset, offsetof(iso9660_stat_t, type)) \
    X(iso_stat_b_xa_offset, offsetof(iso9660_stat_t, b_xa)) \
    X(udf_directory_i_part_start_offset, offsetof(udf_dirent_t, i_part_start)) \
    X(udf_directory_fid_offset, offsetof(udf_dirent_t, fid)) \
    X(udf_directory_fe_offset, offsetof(udf_dirent_t, fe)) \
    X(file_entry_icb_tag_offset, offsetof(udf_file_entry_t, icb_tag)) \
    X(file_entry_u_offset, offsetof(udf_file_entry_t, u)) \
    X(file_identifier_icb_offset, offsetof(udf_fileid_desc_t, icb)) \
    X(file_identifier_u_offset, offsetof(udf_fileid_desc_t, u)) \
    X(extended_file_entry_u_offset, offsetof(struct extended_file_entry, u)) \
    X(iso_filename_offset, CDIO_STAT_FILENAME_OFFSET)

#define CDIO_LAYOUT_COUNT_CASE(name, value) + 1
enum { CDIO_HEADER_LAYOUT_COUNT = 0 CDIO_HEADER_LAYOUT_CASES(CDIO_LAYOUT_COUNT_CASE) };
#undef CDIO_LAYOUT_COUNT_CASE

#ifdef __cplusplus
extern "C" {
#endif
size_t CdioPrivateHeaderLayout(size_t* values, size_t capacity);
size_t CdioPublicHeaderLayout(size_t* values, size_t capacity);
#ifdef __cplusplus
}
#endif
