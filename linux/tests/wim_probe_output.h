/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "wim_metadata.h"
#include <stdio.h>

static void PrintWimMetadata(const RUFUS_WIM_METADATA* m)
{
	printf("{\"format\":%u,\"images\":%u,\"boot\":%u,\"part\":%u,\"parts\":%u,"
		"\"raw\":[%u,%u,%u,%u],\"windows\":[%u,%u,%u,%u],\"valid\":%u,\"invalid\":%u}\n",
		m->archive.version, m->archive.image_count, m->archive.boot_index,
		m->archive.part_number, m->archive.total_parts,
		m->raw_version.major, m->raw_version.minor, m->raw_version.build, m->raw_version.revision,
		m->win_version.major, m->win_version.minor, m->win_version.build, m->win_version.revision,
		m->valid_fields, m->invalid_fields);
}
