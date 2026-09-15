/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "strarray.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail_realloc;
static int failures;

void* __real_realloc(void* pointer, size_t size);

/* Link-time allocation failure injection, only in this Linux test executable. */
void* __wrap_realloc(void* pointer, size_t size)
{
	if (fail_realloc)
		return NULL;
	return __real_realloc(pointer, size);
}

void uprintf(const char* format, ...)
{
	va_list args;
	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
	fputc('\n', stderr);
}

#define CHECK(expression) do { \
	if (!(expression)) { \
		fprintf(stderr, "line %d: %s\n", __LINE__, #expression); \
		failures++; \
	} \
} while (0)

int main(void)
{
	StrArray paths = { 0 };
	char mutable_path[] = "/boot/config";
	char** original_table;
	char* owned_path = strdup("/owned");

	StrArrayCreate(&paths, 2);
	CHECK(StrArrayAdd(&paths, mutable_path, 1) == 0);
	mutable_path[1] = 'X';
	CHECK(StrArrayFind(&paths, "/boot/config") == 0);
	CHECK(StrArrayAddUnique(&paths, "/boot/config", 1) == 0);
	CHECK(paths.Index == 1);
	CHECK(StrArrayAdd(&paths, owned_path, 0) == 1);
	CHECK(paths.String[1] == owned_path);
	original_table = paths.String;
	fail_realloc = 1;
	CHECK(StrArrayAdd(&paths, "/third", 1) == -1);
	CHECK(paths.String == original_table && paths.Index == 2 && paths.Max == 2);
	CHECK(StrArrayFind(&paths, "/boot/config") == 0);
	CHECK(StrArrayFind(&paths, "/owned") == 1);
	fail_realloc = 0;
	CHECK(StrArrayAdd(&paths, "/third", 1) == 2);
	CHECK(StrArrayFind(&paths, "/third") == 2);
	StrArrayClear(&paths);
	CHECK(IsStrArrayEmpty(paths));
	CHECK(StrArrayFind(&paths, "/third") == -1);
	CHECK(StrArrayAdd(&paths, "/after-clear", 1) == 0);
	StrArrayDestroy(&paths);
	CHECK(paths.String == NULL && paths.Index == 0);
	StrArrayDestroy(&paths);
	CHECK(StrArrayAdd(&paths, "/after-destroy", 1) == -1);
	return failures != 0;
}
