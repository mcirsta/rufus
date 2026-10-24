/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <wimlib.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

static void PrintHex(const unsigned char* bytes, size_t size)
{
	for (size_t i = 0; i < size; i++)
		printf("%02x", bytes[i]);
}

static int PrintResource(const struct wimlib_resource_entry* resource, void* data)
{
	unsigned* image = data;
	// Metadata order identifies each image; file resources are sorted by the
	// host comparator. Their content hashes/sizes survive compression/layout.
	printf("%c %u %" PRIu64 " ", resource->is_metadata ? 'M' : 'R',
		resource->is_metadata ? ++*image : 0, resource->uncompressed_size);
	PrintHex(resource->sha1_hash, sizeof(resource->sha1_hash));
	putchar('\n');
	return ferror(stdout) ? WIMLIB_ERR_WRITE : 0;
}

static int PrintWimInventory(WIMStruct* wim)
{
	struct wimlib_wim_info info;
	void* xml = NULL;
	size_t size = 0;
	unsigned image = 0;
	int error = wimlib_get_wim_info(wim, &info);
	if (error == 0)
		error = wimlib_get_xml_data(wim, &xml, &size);
	if (error != 0)
		return error;
	printf("IMAGES %u\nXML ", info.image_count);
	PrintHex(xml, size);
	putchar('\n');
	free(xml);
	error = wimlib_iterate_lookup_table(wim, 0, PrintResource, &image);
	if (error == 0 && fflush(stdout) != 0)
		error = WIMLIB_ERR_WRITE;
	return error;
}
