/* SPDX-License-Identifier: GPL-3.0-or-later */
/* File order, destinations and milestones from Rufus's ExtractFreeDOS. */
#include "freedos_files.h"

const FREEDOS_FILE freedos_files[] = {
#define FREEDOS_FILE_ENTRY(name, resource, root, progress) { name, root, progress },
#include "freedos_files.inc"
#undef FREEDOS_FILE_ENTRY
};
const size_t freedos_file_count = sizeof(freedos_files) / sizeof(freedos_files[0]);
