/*
  Copyright (C) 2005, 2006, 2008, 2010, 2012, 2024 Rocky Bernstein <rocky@gnu.org>

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
/* Access routines */

/* udf_private.h has to come first else _FILE_OFFSET_BITS are redefined in
   say opensolaris. */
#include "udf_private.h"
#include <cdio/bytesex.h>
#include "udf_fs.h"
#include <limits.h>

#ifdef HAVE_STRING_H
# include <string.h>
#endif

#ifdef HAVE_STDIO_H
#include <stdio.h>  /* Remove when adding cdio/logging.h */
#endif

const char *
udf_get_filename(const udf_dirent_t *p_udf_dirent)
{
  if (!p_udf_dirent) return NULL;
  if (!p_udf_dirent->psz_name) return "..";
  return p_udf_dirent->psz_name;
}

/* Copy an UDF File Entry into a Directory Entry structure. */
bool
udf_get_file_entry(const udf_dirent_t *p_udf_dirent,
		   /*out*/ udf_file_entry_t *p_udf_fe)
{
  if (!p_udf_dirent) return false;
  memcpy(p_udf_fe, &p_udf_dirent->fe, sizeof(udf_file_entry_t));
  return true;
}

/*!
  Return the file id descriptor of the given file.
*/
bool udf_get_fileid_descriptor(const udf_dirent_t *p_udf_dirent,
			       /*out*/ udf_fileid_desc_t *p_udf_fid)
{

  if (!p_udf_dirent) return false;
  if (!p_udf_dirent->fid) {
    /* FIXME do something about trying to get the descriptor. */
    return false;
  }
  memcpy(p_udf_fid, p_udf_dirent->fid, sizeof(udf_fileid_desc_t));
  return true;
}


/*!
  Return the number of hard links of the file. Return 0 if error.
*/
uint16_t udf_get_link_count(const udf_dirent_t *p_udf_dirent)
{
  if (p_udf_dirent) {
    return uint16_from_le(p_udf_dirent->fe.link_count);
  }
  return 0; /* Error. Non-error case handled above. */
}

/*!
  Return the file length the file. Return 2147483647L if error.
*/
uint64_t udf_get_file_length(const udf_dirent_t *p_udf_dirent)
{
  if (p_udf_dirent) {
    return uint64_from_le(p_udf_dirent->fe.info_len);
  }
  return 2147483647L; /* Error. Non-error case handled above. */
}

/*!
  Return true if the file is a directory.
*/
bool
udf_is_dir(const udf_dirent_t *p_udf_dirent)
{
  return p_udf_dirent->b_dir;
}

/*
 * Translate a file offset into a logical block and then into a physical
 * block.
 */
static lba_t
offset_to_lba(const udf_dirent_t *p_udf_dirent, off_t i_offset,
              /*out*/ uint32_t *pi_max_size)
{
  const udf_file_entry_t *fe = &p_udf_dirent->fe;
  uint32_t attrs = uint32_from_le(fe->u_extended_attr);
  uint32_t bytes = uint32_from_le(fe->u_alloc_descs);
  uint16_t type = uint16_from_le(fe->icb_tag.flags) & ICBTAG_FLAG_AD_MASK;
  uint32_t offset, stride, length, location;
  uint64_t sector;

  if (i_offset < 0 || i_offset % UDF_BLOCKSIZE ||
      uint16_from_le(fe->icb_tag.strat_type) != ICBTAG_STRATEGY_TYPE_4)
    return CDIO_INVALID_LBA;
  if (attrs > sizeof(fe->u) || bytes > sizeof(fe->u) - attrs)
    return CDIO_INVALID_LBA;
  if (type == ICBTAG_FLAG_AD_SHORT)
    stride = sizeof(udf_short_ad_t);
  else if (type == ICBTAG_FLAG_AD_LONG)
    stride = sizeof(udf_long_ad_t);
  else {
    cdio_warn("Unsupported allocation descriptor %u", type);
    return CDIO_INVALID_LBA;
  }

  for (offset = 0; offset < bytes; offset += stride) {
    if (bytes - offset < stride)
      return CDIO_INVALID_LBA;
    if (type == ICBTAG_FLAG_AD_SHORT) {
      const udf_short_ad_t *ad = (const udf_short_ad_t *)&fe->u.alloc_descs[attrs + offset];
      length = uint32_from_le(ad->len);
      location = uint32_from_le(ad->pos);
    } else {
      const udf_long_ad_t *ad = (const udf_long_ad_t *)&fe->u.alloc_descs[attrs + offset];
      /* The bundled reader supports the first partition map only. */
      if (uint16_from_le(ad->loc.partitionReferenceNum) != 0)
        return CDIO_INVALID_LBA;
      length = uint32_from_le(ad->len);
      location = uint32_from_le(ad->loc.lba);
    }
    /* Sparse data and indirect allocation descriptors need separate support. */
    if (length == 0 || (length & ~UDF_LENGTH_MASK) != EXT_RECORDED_ALLOCATED)
      return CDIO_INVALID_LBA;
    if ((uint64_t)i_offset >= length) {
      if (length % UDF_BLOCKSIZE)
        return CDIO_INVALID_LBA;
      i_offset -= length;
      continue;
    }
    sector = (uint64_t)p_udf_dirent->i_part_start + location + i_offset / UDF_BLOCKSIZE;
    if (sector > INT32_MAX)
      return CDIO_INVALID_LBA;
    /* A seek inside an extent leaves only its remaining bytes available. */
    *pi_max_size = length - (uint32_t)i_offset;
    return (lba_t)sector;
  }
  return CDIO_INVALID_LBA;
}

/**
  Read up to count blocks from the current file position, stopping at the end
  of the current recorded extent or file. Return bytes read, zero at EOF, or
  a negative driver error. The caller provides count * UDF_BLOCKSIZE bytes.
  Non-final extents and the starting position must be block-aligned.
*/
ssize_t
udf_read_block(const udf_dirent_t *p_udf_dirent, void *buf, size_t count)
{
  udf_t *p_udf;
  lba_t lba;
  uint32_t remaining;
  uint64_t file_left, requested;
  size_t blocks;
  off_t next_position;
  driver_return_code_t ret;

  if (count == 0)
    return 0;
  if (!p_udf_dirent || !buf || !p_udf_dirent->p_udf || count > LONG_MAX / UDF_BLOCKSIZE)
    return DRIVER_OP_BAD_PARAMETER;
  p_udf = p_udf_dirent->p_udf;
  if (p_udf->i_position < 0 ||
      (uint64_t)p_udf->i_position > udf_get_file_length(p_udf_dirent))
    return DRIVER_OP_BAD_PARAMETER;
  file_left = udf_get_file_length(p_udf_dirent) - (uint64_t)p_udf->i_position;
  if (file_left == 0)
    return 0;
  lba = offset_to_lba(p_udf_dirent, p_udf->i_position, &remaining);
  if (lba == CDIO_INVALID_LBA)
    return DRIVER_OP_ERROR;
  requested = (uint64_t)count * UDF_BLOCKSIZE;
  if (requested > remaining)
    requested = remaining;
  if (requested > file_left)
    requested = file_left;
  next_position = (off_t)((uint64_t)p_udf->i_position + requested);
  if (next_position < 0 || (uint64_t)next_position != (uint64_t)p_udf->i_position + requested)
    return DRIVER_OP_ERROR;
  blocks = (size_t)((requested + UDF_BLOCKSIZE - 1) / UDF_BLOCKSIZE);
  if (blocks > (uint64_t)INT32_MAX - lba + 1)
    return DRIVER_OP_ERROR;
  ret = udf_read_sectors(p_udf, buf, lba, (long)blocks);
  if (ret != DRIVER_OP_SUCCESS)
    return ret;
  p_udf->i_position = next_position;
  return (ssize_t)requested;
}
