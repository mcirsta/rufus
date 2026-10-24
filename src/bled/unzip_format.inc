/*
 * unzip implementation for Bled/busybox
 *
 * Copyright © 2015-2023 Pete Batard <pete@akeo.ie>
 * Based on mini unzip implementation for busybox © Ed Clark
 * Loosely based on original busybox unzip applet © Laurence Anderson.
 *
 * Licensed under GPLv2 or later, see file LICENSE in this source tree.
 */

enum {
#if BB_BIG_ENDIAN
	ZIP_FILEHEADER_MAGIC = 0x504b0304,
	ZIP_CDF_MAGIC        = 0x504b0102, /* CDF item */
	ZIP_CDE_MAGIC        = 0x504b0506, /* End of CDF */
	ZIP64_CDE_MAGIC      = 0x504b0606, /* End of Zip64 CDF */
	ZIP_DD_MAGIC         = 0x504b0708,
#else
	ZIP_FILEHEADER_MAGIC = 0x04034b50,
	ZIP_CDF_MAGIC        = 0x02014b50,
	ZIP_CDE_MAGIC        = 0x06054b50,
	ZIP64_CDE_MAGIC      = 0x06064b50,
	ZIP_DD_MAGIC         = 0x08074b50,
#endif
};

#define ZIP_HEADER_LEN 26

PRAGMA_BEGIN_PACKED
typedef union {
	uint8_t raw[ZIP_HEADER_LEN];
	struct {
		uint16_t version;               /* 0-1 */
		uint16_t zip_flags;             /* 2-3 */
		uint16_t method;                /* 4-5 */
		uint16_t modtime;               /* 6-7 */
		uint16_t moddate;               /* 8-9 */
		uint32_t crc32 PACKED;          /* 10-13 */
		uint32_t cmpsize PACKED;        /* 14-17 */
		uint32_t ucmpsize PACKED;       /* 18-21 */
		uint16_t filename_len;          /* 22-23 */
		uint16_t extra_len;             /* 24-25 */
		/* filename follows (not NUL terminated) */
		/* extra field follows */
		/* data follows */
	} fmt PACKED;
} zip_header_t; /* PACKED - gcc 4.2.1 doesn't like it (spews warning) */
PRAGMA_END_PACKED

#define FIX_ENDIANNESS_ZIP(zip) \
do { if (BB_BIG_ENDIAN) { \
	(zip).fmt.method        = SWAP_LE16((zip).fmt.method      ); \
	(zip).fmt.modtime       = SWAP_LE16((zip).fmt.modtime     ); \
	(zip).fmt.moddate       = SWAP_LE16((zip).fmt.moddate     ); \
	(zip).fmt.crc32         = SWAP_LE32((zip).fmt.crc32       ); \
	(zip).fmt.cmpsize       = SWAP_LE32((zip).fmt.cmpsize     ); \
	(zip).fmt.ucmpsize      = SWAP_LE32((zip).fmt.ucmpsize    ); \
	(zip).fmt.filename_len  = SWAP_LE16((zip).fmt.filename_len); \
	(zip).fmt.extra_len     = SWAP_LE16((zip).fmt.extra_len   ); \
}} while (0)

#define CDF_HEADER_LEN 42

PRAGMA_BEGIN_PACKED
typedef union {
	uint8_t raw[CDF_HEADER_LEN];
	struct {
		/* uint32_t signature; 50 4b 01 02 */
		uint16_t version_made_by;       /* 0-1 */
		uint16_t version_needed;        /* 2-3 */
		uint16_t cdf_flags;             /* 4-5 */
		uint16_t method;                /* 6-7 */
		uint16_t modtime;               /* 8-9 */
		uint16_t moddate;               /* 10-11 */
		uint32_t crc32;                 /* 12-15 */
		uint32_t cmpsize;               /* 16-19 */
		uint32_t ucmpsize;              /* 20-23 */
		uint16_t filename_len;          /* 24-25 */
		uint16_t extra_len;             /* 26-27 */
		uint16_t file_comment_length;   /* 28-29 */
		uint16_t disk_number_start;     /* 30-31 */
		uint16_t internal_attributes;   /* 32-33 */
		uint32_t external_attributes PACKED; /* 34-37 */
		uint32_t relative_offset_of_local_header PACKED; /* 38-41 */
		/* filename follows (not NUL terminated) */
		/* extra field follows */
		/* file comment follows */
	} fmt PACKED;
} cdf_header_t;
PRAGMA_END_PACKED

#define FIX_ENDIANNESS_CDF(cdf) \
do { if (BB_BIG_ENDIAN) { \
	(cdf).fmt.version_made_by = SWAP_LE16((cdf).fmt.version_made_by); \
	(cdf).fmt.version_needed  = SWAP_LE16((cdf).fmt.version_needed ); \
	(cdf).fmt.method          = SWAP_LE16((cdf).fmt.method         ); \
	(cdf).fmt.modtime         = SWAP_LE16((cdf).fmt.modtime        ); \
	(cdf).fmt.moddate         = SWAP_LE16((cdf).fmt.moddate        ); \
	(cdf).fmt.crc32           = SWAP_LE32((cdf).fmt.crc32          ); \
	(cdf).fmt.cmpsize         = SWAP_LE32((cdf).fmt.cmpsize        ); \
	(cdf).fmt.ucmpsize        = SWAP_LE32((cdf).fmt.ucmpsize       ); \
	(cdf).fmt.filename_len    = SWAP_LE16((cdf).fmt.filename_len   ); \
	(cdf).fmt.extra_len       = SWAP_LE16((cdf).fmt.extra_len      ); \
	(cdf).fmt.file_comment_length = SWAP_LE16((cdf).fmt.file_comment_length); \
	(cdf).fmt.external_attributes = SWAP_LE32((cdf).fmt.external_attributes); \
}} while (0)

#define CDE_LEN 16

PRAGMA_BEGIN_PACKED
typedef union {
	uint8_t raw[CDE_LEN];
	struct {
		/* uint32_t signature; 50 4b 05 06 */
		uint16_t this_disk_no;
		uint16_t disk_with_cdf_no;
		uint16_t cdf_entries_on_this_disk;
		uint16_t cdf_entries_total;
		uint32_t cdf_size;
		uint32_t cdf_offset;
		/* uint16_t archive_comment_length; */
		/* archive comment follows */
	} fmt PACKED;
} cde_t;
PRAGMA_END_PACKED

#define FIX_ENDIANNESS_CDE(cde) \
do { if (BB_BIG_ENDIAN) { \
	(cde).fmt.cdf_offset = SWAP_LE32((cde).fmt.cdf_offset); \
}} while (0)

/* Extra records */
#define EXTRA_HEADER_LEN 4

PRAGMA_BEGIN_PACKED
typedef union {
	uint8_t raw[EXTRA_HEADER_LEN];
	struct {
		uint16_t tag;
		uint16_t length;
		/* extra record follows */
	} fmt PACKED;
} extra_header_t;
PRAGMA_END_PACKED

#define FIX_ENDIANNESS_EXTRA(ext) \
do { if (BB_BIG_ENDIAN) { \
	(ext).fmt.tag = SWAP_LE16((ext).fmt.tag); \
	(ext).fmt.length = SWAP_LE32((ext).fmt.length); \
}} while (0)

/* ZIP64 records */
#define ZIP64_LEN 20

PRAGMA_BEGIN_PACKED
typedef union {
	uint8_t raw[ZIP64_LEN];
	struct {
		/* uint16_t tag; 00 01 */
		uint16_t tag;
		uint16_t length;
		uint64_t ucmpsize PACKED;
		uint64_t cmpsize PACKED;
	} fmt PACKED;
} zip64_t;
PRAGMA_END_PACKED

#define FIX_ENDIANNESS_ZIP64(z64) \
do { if (BB_BIG_ENDIAN) { \
	(z64).fmt.cmpsize = SWAP_LE64((z64).fmt.cmpsize); \
	(z64).fmt.ucmpsize = SWAP_LE64((z64).fmt.ucmpsize); \
}} while (0)

#define CDE64_LEN 52

PRAGMA_BEGIN_PACKED
typedef union {
	uint8_t raw[CDE64_LEN];
	struct {
		/* uint32_t signature; 50 4b 06 06 */
		uint64_t size PACKED;
		uint16_t version_made_by;
		uint16_t version_needed;
		uint32_t this_disk_no;
		uint32_t disk_with_cdf_no;
		uint64_t cdf_entries_on_this_disk PACKED;
		uint64_t cdf_entries_total PACKED;
		uint64_t cdf_size PACKED;
		uint64_t cdf_offset PACKED;
		/* archive comment follows */
	} fmt PACKED;
} cde64_t;
PRAGMA_END_PACKED

#define FIX_ENDIANNESS_CDE64(c64) \
do { if (BB_BIG_ENDIAN) { \
	(c64).fmt.size = SWAP_LE64((c64).fmt.size); \
	(c64).fmt.cdf_offset = SWAP_LE64((c64).fmt.cdf_offset); \
}} while (0)

inline void BUG(void) {
	/* Check the offset of the last element, not the length.  This leniency
	 * allows for poor packing, whereby the overall struct may be too long,
	 * even though the elements are all in the right place.
	 */
	BUILD_BUG_ON(
		offsetof(zip_header_t, fmt.extra_len) + 2
			!= ZIP_HEADER_LEN);
	BUILD_BUG_ON(
		offsetof(cdf_header_t, fmt.relative_offset_of_local_header) + 4
			!= CDF_HEADER_LEN);
	BUILD_BUG_ON(sizeof(cde_t) != CDE_LEN);
	BUILD_BUG_ON(sizeof(extra_header_t) != EXTRA_HEADER_LEN);
	BUILD_BUG_ON(sizeof(zip64_t) != ZIP64_LEN);
	BUILD_BUG_ON(sizeof(cde64_t) != CDE64_LEN);
}

