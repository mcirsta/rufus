/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "native_input.h"
#include "iso_file.h"
#include "wim_probe_output.h"
#include <cdio/logging.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static bool active;
static long reads, max_blocks, iso_reads;
static long cancel_at = -1, fail_at = -1, short_at = -1;
enum { MAX_FAULT_READ = 1000 };

int IsIsoScanCancelled(void)
{
	return active && cancel_at >= 0 && reads >= cancel_at;
}

static void LogCdio(cdio_log_level_t level, const char* message)
{
	(void)level;
	fprintf(stderr, "%s\n", message);
}

long __real_iso9660_iso_seek_read(const iso9660_t*, void*, lsn_t, long);
ssize_t __real_udf_read_block(const udf_dirent_t*, void*, size_t);

static void CountRead(long blocks)
{
	reads++;
	if (blocks > max_blocks)
		max_blocks = blocks;
}

long __wrap_iso9660_iso_seek_read(const iso9660_t* iso, void* buffer, lsn_t lsn, long blocks)
{
	if (active)
	{
		iso_reads++;
		CountRead(blocks);
		if (reads == fail_at || reads == short_at)
		{
			errno = EIO;
			return reads == fail_at ? -1 : 0;
		}
	}
	return __real_iso9660_iso_seek_read(iso, buffer, lsn, blocks);
}

ssize_t __wrap_udf_read_block(const udf_dirent_t* file, void* buffer, size_t blocks)
{
	if (active)
	{
		CountRead((long)blocks);
		if (reads == fail_at || reads == short_at)
		{
			errno = EIO;
			return reads == fail_at ? -1 : 0;
		}
	}
	return __real_udf_read_block(file, buffer, blocks);
}

static int SetFault(const char* mode, const char* value)
{
	char* end;
	long number;

	errno = 0;
	number = strtol(value, &end, 10);
	bool parsed = errno == 0 && *end == 0;
	bool valid_read = number >= 0 && number <= MAX_FAULT_READ;
	if (!parsed || !valid_read)
		return 64;
	if (strcmp(mode, "cancel") == 0)
		cancel_at = number;
	else if (strcmp(mode, "fail") == 0)
		fail_at = number;
	else if (strcmp(mode, "short") == 0)
		short_at = number;
	else
		return 64;
	return 0;
}

int main(int argc, char** argv)
{
	ISO_FILE_READER reader = { 0 };
	iso9660_t* iso = NULL;
	udf_t* udf = NULL;
	WIMStruct* wim = NULL;
	RUFUS_WIM_METADATA metadata;
	struct stat info;
	uint8_t signature[8];
	int result = ISO_SCAN_ERROR, error = 0, flags = 0;

	if (argc < 4 || argc > 6)
		return 64;
	if (stat(argv[1], &info) != 0 || !S_ISREG(info.st_mode))
		return 64;
	if (argc == 5)
	{
		if (strcmp(argv[4], "integrity") != 0)
			return 64;
		flags = WIMLIB_OPEN_FLAG_CHECK_INTEGRITY;
	}
	if (argc == 6 && SetFault(argv[4], argv[5]) != 0)
		return 64;
	if (wimlib_global_init(WIMLIB_INIT_FLAG_ASSUME_UTF8) != 0)
		return 65;
	cdio_log_set_handler(LogCdio);
	if (strcmp(argv[2], "udf") == 0)
		udf = udf_open(argv[1]);
	else if (strcmp(argv[2], "rr") == 0)
		iso = iso9660_open_ext(argv[1], ISO_EXTENSION_ALL & ~ISO_EXTENSION_JOLIET);
	else if (strcmp(argv[2], "joliet") == 0)
		iso = iso9660_open_ext(argv[1], ISO_EXTENSION_JOLIET);
	else
	{
		result = 64;
		goto out;
	}
	if (iso == NULL && udf == NULL)
		goto out;
	result = OpenIsoFileReader(&reader, iso, udf, argv[3]);
	if (result != ISO_SCAN_OK)
		goto out;
	active = true;
	error = OpenWimFromIsoReader(&reader, flags, &wim);
	if (error == 0)
		error = ReadWimMetadata(wim, 1, &metadata);
	result = error == 0 ? ISO_SCAN_OK : ISO_SCAN_ERROR;
	if (IsIsoScanCancelled())
		result = ISO_SCAN_CANCELLED;
	if (result == ISO_SCAN_OK)
		PrintWimMetadata(&metadata);
	wimlib_free(wim);
	wim = NULL;
	active = false;
	// The borrow survives both successful and failed wimlib opens.
	bool can_check_borrow = reader.size >= sizeof(signature) && fail_at < 0 && short_at < 0;
	if (can_check_borrow && ReadIsoFileRange(&reader, 0, signature, sizeof(signature)) != ISO_SCAN_OK)
		result = 66;
out:
	active = false;
	fprintf(stderr, "ERROR=%d READS=%ld MAX_BLOCKS=%ld ISO_READS=%ld\n", error, reads, max_blocks, iso_reads);
	wimlib_free(wim);
	CloseIsoFileReader(&reader);
	iso9660_close(iso);
	if (udf != NULL)
		udf_close(udf);
	wimlib_global_cleanup();
	return result;
}
