/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "unattend.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static int allocation_count, fail_allocation, offset_count, fail_offset;
extern void* __real_calloc(size_t count, size_t size);
extern void* __real_realloc(void* pointer, size_t size);
extern char* __real_strdup(const char* string);
extern long __real_ftell(FILE* file);
#define CHECK(condition) do { if (!(condition)) { \
	fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); exit(1); } } while (0)

void uprintf(const char* format, ...)
{
	(void)format;
}

void* __wrap_calloc(size_t count, size_t size)
{
	if (++allocation_count == fail_allocation)
		return NULL;
	return __real_calloc(count, size);
}

void* __wrap_realloc(void* pointer, size_t size)
{
	if (++allocation_count == fail_allocation)
		return NULL;
	return __real_realloc(pointer, size);
}

char* __wrap_strdup(const char* string)
{
	if (++allocation_count == fail_allocation)
		return NULL;
	return __real_strdup(string);
}

long __wrap_ftell(FILE* file)
{
	if (++offset_count == fail_offset) {
		errno = ESPIPE;
		return -1;
	}
	return __real_ftell(file);
}

static bool Generate(void)
{
	FILE* file = tmpfile();
	CHECK(file != NULL);
	uint32_t offsets[2];
	char username[] = "Test.User";
	RUFUS_UNATTEND_OPTIONS options = { 0 };
	options.arch = ARCH_X86_64;
	options.flags = UNATTEND_SET_USER | UNATTEND_QOL_ENHANCEMENTS | UNATTEND_SECUREBOOT_TPM_MINRAM;
	options.username = username;
	allocation_count = offset_count = 0;
	const bool result = WriteUnattendXml(file, &options, offsets);
	CHECK(fclose(file) == 0);
	return result;
}

int main(void)
{
	CHECK(Generate());
	const int allocations = allocation_count;
	const int offsets = offset_count;
	CHECK(allocations > 16); /* Includes command-array growth as well as copies. */
	CHECK(offsets == 2);
	for (fail_allocation = 1; fail_allocation <= allocations; fail_allocation++)
		CHECK(!Generate());
	fail_allocation = 0;
	for (fail_offset = 1; fail_offset <= offsets; fail_offset++)
		CHECK(!Generate());
	fail_offset = 0;
	CHECK(Generate());
	printf("%d allocation failures and %d offset failures propagated\n", allocations, offsets);
	return 0;
}
