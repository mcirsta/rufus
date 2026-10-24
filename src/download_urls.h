/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

/* Original Rufus endpoints, shared by the Win32 and native downloaders. */
#if defined(RUFUS_TEST)
#define RUFUS_URL "http://nas/~rufus"
#else
#define RUFUS_URL "https://rufus.ie"
#endif
#define DOWNLOAD_URL RUFUS_URL "/downloads"
#define FILES_URL RUFUS_URL "/files"
#define FIDO_VERSION "z1"
