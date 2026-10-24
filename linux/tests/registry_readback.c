/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Keep wimlib's private C macros out of Qt headers. This independent reader is
 * test-only; the application edits registry hives through Hivex's public API. */
#include "wimlib/registry.h"

int ReadFixtureHiveNumber(const void* data, size_t size, const char* path,
	const char* name, int64_t* value)
{
	enum hive_status status = hive_validate(data, size);
	if (status == HIVE_OK)
		status = hive_get_number(data, path, name, value);
	return status;
}
