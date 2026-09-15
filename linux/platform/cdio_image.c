// SPDX-License-Identifier: GPL-3.0-or-later
// Rufus src/iso.c also disables these optical-device entry points. The Linux reader
// accepts image files; libudf uses its existing stdio fallback.
#include <cdio/cdio.h>

CdIo_t* cdio_open(const char* source, driver_id_t driver)
{
    (void)source;
    (void)driver;
    return NULL;
}

void cdio_destroy(CdIo_t* cdio)
{
    (void)cdio;
}
