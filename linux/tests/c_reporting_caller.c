/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "platform/rufus_reporting.h"

#include <string.h>

/* Compile this as C: it exercises the ABI and borrowed buffer ownership,
 * independently of the Qt headers and C++ signal implementation. */
void report_from_c(void)
{
    char filename[] = "Mediu \xc8\x99i fi\xc8\x99iere.iso";
    uprintf("Image: %s (%d)", filename, 7);
    memset(filename, 'x', sizeof(filename) - 1);

    char message[] = "Literal 100% text";
    uprintfs(message);
    memset(message, 'y', sizeof(message) - 1);

    char long_message[8193];
    memset(long_message, 'a', sizeof(long_message) - 1);
    long_message[sizeof(long_message) - 1] = '\0';
    uprintf("%s", long_message);
    uprintf((const char*)0);
    uprintfs((const char*)0);
}

void report_progress_from_c(void)
{
    /* OP_INIT uses a handle payload, not a count. Preserve it verbatim. */
    _UpdateProgressWithInfo(-1, 1, UINT64_C(0x1234), 0, 0);
    UpdateProgress(3, 0.0f);
    UpdateProgress(3, 25.0f);
    UpdateProgress(3, -1.0f);
    UpdateProgress(3, -1.0f);
    UpdateProgress(4, 0.0f);
    UpdateProgress(4, 100.0f);

    /* Values beyond double's exact integer range must remain integers. */
    _UpdateProgressWithInfo(4, 231, UINT64_C(9007199254740993),
                            UINT64_C(18014398509481991), 1);
    _UpdateProgressWithInfo(-2, 231, 1, 0, 0);
}

void report_burst_from_c(void)
{
    for (int i = 1; i <= 10000; ++i) {
        UpdateProgress(4, (float)i / 200.0f);
        _UpdateProgressWithInfo(4, 231, (uint64_t)i, 20000, 0);
    }
}
