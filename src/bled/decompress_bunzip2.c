/* vi: set sw=4 ts=4: */
/*
 * Small bzip2 deflate implementation, by Rob Landley (rob@landley.net).
 *
 * Based on bzip2 decompression code by Julian R Seward (jseward@acm.org),
 * which also acknowledges contributions by Mike Burrows, David Wheeler,
 * Peter Fenwick, Alistair Moffat, Radford Neal, Ian H. Witten,
 * Robert Sedgewick, and Jon L. Bentley.
 *
 * Licensed under GPLv2 or later, see file LICENSE in this source tree.
 */
/*
	Size and speed optimizations by Manuel Novoa III  (mjn3@codepoet.org).

	More efficient reading of Huffman codes, a streamlined read_bunzip()
	function, and various other tweaks.  In (limited) tests, approximately
	20% faster than bzcat on x86 and about 10% faster on arm.

	Note that about 2/3 of the time is spent in read_bunzip() reversing
	the Burrows-Wheeler transformation.  Much of that time is delay
	resulting from cache misses.

	(2010 update by vda: profiled "bzcat <84mbyte.bz2 >/dev/null"
	on x86-64 CPU with L2 > 1M: get_next_block is hotter than read_bunzip:
	%time seconds   calls function
	71.01   12.69     444 get_next_block
	28.65    5.12   93065 read_bunzip
	00.22    0.04 7736490 get_bits
	00.11    0.02      47 dealloc_bunzip
	00.00    0.00   93018 full_write
	...)


	I would ask that anyone benefiting from this work, especially those
	using it in commercial products, consider making a donation to my local
	non-profit hospice organization (www.hospiceacadiana.com) in the name of
	the woman I loved, Toni W. Hagan, who passed away Feb. 12, 2003.

	Manuel
 */
#include "libbb.h"
#include "bb_archive.h"

#include "bunzip_core.inc"

/* Allocate the structure, read file header.  If in_fd==-1, inbuf must contain
   a complete bunzip file (len bytes long).  If in_fd!=-1, inbuf and len are
   ignored, and data is read from file handle into temporary buffer. */

/* Because bunzip2 is used for help text unpacking, and because bb_show_usage()
   should work for NOFORK applets too, we must be extremely careful to not leak
   any allocations! */
static int FAST_FUNC start_bunzip(
		void *jmpbuf,
		bunzip_data **bdp,
		int in_fd,
		const void *inbuf, int len)
{
	bunzip_data *bd;
	unsigned i;
	enum {
		BZh0 = ('B' << 24) + ('Z' << 16) + ('h' << 8) + '0',
		h0 = ('h' << 8) + '0',
	};

	/* Figure out how much data to allocate */
	i = sizeof(bunzip_data);
	if (in_fd != -1)
		i += IOBUF_SIZE;

	/* Allocate bunzip_data.  Most fields initialize to zero. */
	bd = *bdp = xzalloc(i);
	if (bd == NULL)
		return -1;

	bd->jmpbuf = jmpbuf;

	/* Setup input buffer */
	bd->in_fd = in_fd;
	if (-1 == in_fd) {
		/* in this case, bd->inbuf is read-only */
		bd->inbuf = (void*)inbuf; /* cast away const-ness */
	} else {
		bd->inbuf = (uint8_t*)(bd + 1);
		memcpy(bd->inbuf, inbuf, len);
	}
	bd->inbufCount = len;

	/* Init the CRC32 table (big endian) */
	crc32_filltable(bd->crc32Table, 1);

	/* Ensure that file starts with "BZh['1'-'9']." */
	/* Update: now caller verifies 1st two bytes, makes .gz/.bz2
	 * integration easier */
	/* was: */
	/* i = get_bits(bd, 32); */
	/* if ((unsigned)(i - BZh0 - 1) >= 9) return RETVAL_NOT_BZIP_DATA; */
	i = get_bits(bd, 16);
	if ((unsigned)(i - h0 - 1) >= 9) return RETVAL_NOT_BZIP_DATA;

	/* Fourth byte (ascii '1'-'9') indicates block size in units of 100k of
	   uncompressed data.  Allocate intermediate buffer for block. */
	/* bd->dbufSize = 100000 * (i - BZh0); */
	bd->dbufSize = 100000 * (i - h0);

	/* Cannot use xmalloc - may leak bd in NOFORK case! */
	bd->dbuf = malloc_or_warn(bd->dbufSize * sizeof(bd->dbuf[0]));
	if (!bd->dbuf) {
		free(bd);
		xfunc_die();
	}
	return RETVAL_OK;
}

static void FAST_FUNC dealloc_bunzip(bunzip_data *bd)
{
	free(bd->dbuf);
	free(bd);
}


/* Decompress src_fd to dst_fd.  Stops at end of bzip data, not end of file. */
IF_DESKTOP(long long) int FAST_FUNC
unpack_bz2_stream(transformer_state_t *xstate)
{
	IF_DESKTOP(long long total_written = 0;)
	bunzip_data *bd;
	char *outbuf;
	int i, nwrote;
	unsigned len;

	if (check_signature16(xstate, BZIP2_MAGIC))
		return -1;

	outbuf = aligned_xmalloc(IOBUF_SIZE);
	if (outbuf == NULL)
		return -1;
	len = 0;
	while (1) { /* "Process one BZ... stream" loop */
		jmp_buf jmpbuf;

		/* Setup for I/O error handling via longjmp */
		i = setjmp(jmpbuf);
		if (i == 0)
			i = start_bunzip(&jmpbuf, &bd, xstate->src_fd, outbuf + 2, len);

		if (i == 0) {
			while (1) { /* "Produce some output bytes" loop */
				i = read_bunzip(bd, outbuf, IOBUF_SIZE);
				if (i < 0) /* error? */
					break;
				i = IOBUF_SIZE - i; /* number of bytes produced */
				if (i == 0) /* EOF? */
					break;
				nwrote = (int)transformer_write(xstate, outbuf, i);
				if (nwrote != i) {
					i = (nwrote == -ENOSPC)?(int)xstate->mem_output_size_max:RETVAL_SHORT_WRITE;
					goto release_mem;
				}
				IF_DESKTOP(total_written += i;)
			}
		}

		if (i != RETVAL_LAST_BLOCK
		/* Observed case when i == RETVAL_OK:
		 * "bzcat z.bz2", where "z.bz2" is a bzipped zero-length file
		 * (to be exact, z.bz2 is exactly these 14 bytes:
		 * 42 5a 68 39 17 72 45 38 50 90 00 00 00 00).
		 */
		 && i != RETVAL_OK
		) {
			bb_error_msg("bunzip error %d", i);
			break;
		}
		if (bd->headerCRC != bd->totalCRC) {
			bb_simple_error_msg("CRC error");
			break;
		}

		/* Successfully unpacked one BZ stream */
		i = RETVAL_OK;

		/* Do we have "BZ..." after last processed byte?
		 * pbzip2 (parallelized bzip2) produces such files.
		 */
		len = bd->inbufCount - bd->inbufPos;
		memcpy(outbuf, &bd->inbuf[bd->inbufPos], len);
		if (len < 2) {
			if (safe_read(xstate->src_fd, outbuf + len, 2 - len) != 2 - len)
				break;
			len = 2;
		}
		if (*(uint16_t*)outbuf != BZIP2_MAGIC) /* "BZ"? */
			break;
		dealloc_bunzip(bd);
		len -= 2;
	}

 release_mem:
	dealloc_bunzip(bd);
	aligned_free(outbuf);

	return i ? i : IF_DESKTOP(total_written) + 0;
}

char* FAST_FUNC
unpack_bz2_data(const char *packed, int packed_len, int unpacked_len)
{
	char *outbuf = NULL;
	bunzip_data *bd = NULL;
	int i;
	jmp_buf jmpbuf;

	/* Setup for I/O error handling via longjmp */
	i = setjmp(jmpbuf);
	if (i == 0) {
		i = start_bunzip(&jmpbuf,
			&bd,
			/* src_fd: */ -1,
			/* inbuf:  */ packed,
			/* len:    */ packed_len
		);
	}
	/* read_bunzip can longjmp and end up here with i != 0
	 * on read data errors! Not trivial */
	if (i == 0) {
		/* Cannot use xmalloc: will leak bd in NOFORK case! */
		outbuf = malloc_or_warn(unpacked_len);
		if (outbuf)
			read_bunzip(bd, outbuf, unpacked_len);
	}
	dealloc_bunzip(bd);
	return outbuf;
}

#ifdef TESTING

static char *const bunzip_errors[] = {
	NULL, "Bad file checksum", "Not bzip data",
	"Unexpected input EOF", "Unexpected output EOF", "Data error",
	"Out of memory", "Obsolete (pre 0.9.5) bzip format not supported"
};

/* Dumb little test thing, decompress stdin to stdout */
int main(int argc, char **argv)
{
	char c;

	int i = unpack_bz2_stream(0, 1);
	if (i < 0)
		fprintf(stderr, "%s\n", bunzip_errors[-i]);
	else if (read(STDIN_FILENO, &c, 1))
		fprintf(stderr, "Trailing garbage ignored\n");
	return -i;
}
#endif
