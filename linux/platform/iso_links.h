// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "iso_scan.h"
#include "iso_file.h"
#ifdef __cplusplus
extern "C" {
#endif
enum { ISO_LINK_LITERAL, ISO_LINK_SKIP, ISO_LINK_DUPLICATE, ISO_LINK_REPARSE, ISO_LINK_SYSLINUX };
typedef struct { int action; bool directory; uint64_t size; char source[ISO_SCAN_PATH_MAX], relative[ISO_SCAN_PATH_MAX]; } ISO_LINK_PLAN;
int PlanIsoLink(iso9660_t* iso, const ISO_TREE_ENTRY* entry, bool windows, bool syslinux, ISO_LINK_PLAN* plan);
int RelocateIsoLink(char* path, ISO_LINK_PLAN* plan, const char* original, const char* destination);
int OpenResolvedIsoFile(ISO_FILE_READER* reader, iso9660_t* iso, udf_t* udf, const char* path);
int CreateNtfsIsoLink(int parent, const char* name, const ISO_LINK_PLAN* plan);
#ifdef __cplusplus
}
#endif
