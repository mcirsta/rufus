/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "progress_policy.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int checks, failures;
#define CHECK(expression) do { checks++; if (!(expression)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression); failures++; } } while (0)

static void IsoRanges(void)
{
    /* Independent golden boundaries from Win32 InitProgress for quick format. */
    const struct { int filesystem; bool patch, large; float start, end; } cases[] = {
        { FS_NTFS, false, false, 16.0f, 97.0f },
        { FS_NTFS, true, false, 16.0f, 56.5f },
        { FS_FAT32, false, false, 9.0f, 98.0f },
        { FS_FAT32, true, false, 9.0f, 53.5f },
        { FS_FAT32, false, true, 51.0f, 98.0f }
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        RUFUS_PROGRESS_OPTIONS options = { 0 };
        RUFUS_PROGRESS state;
        options.bootable = options.image = true;
        options.selection = BT_IMAGE;
        options.filesystem_steps = nb_steps[cases[i].filesystem];
        options.windows_customization = cases[i].patch;
        options.format_progress = cases[i].large;
        options.ntfs_image = cases[i].filesystem == FS_NTFS;
        InitProgressState(&state, &options);
        CHECK(state.end[OP_FILE_COPY] == cases[i].start);
        CHECK(state.end[OP_FILE_COPY + 1] == cases[i].end);
        CHECK(state.end[OP_MAX] == 100.0f);
        CHECK(UpdateProgressState(&state, OP_FILE_COPY, 0) == (int)(cases[i].start / 100.0f * 65535));
        CHECK(UpdateProgressState(&state, OP_FILE_COPY, 100) == (int)(cases[i].end / 100.0f * 65535));
        CHECK(UpdateProgressState(&state, OP_FINALIZE, -1) < 65535);
    }
    RUFUS_PROGRESS_OPTIONS options = { 0 };
    RUFUS_PROGRESS state;
    options.bootable = options.image = options.ntfs_image = true;
    options.selection = BT_IMAGE;
    options.filesystem_steps = 12;
    InitProgressState(&state, &options);
    CHECK(UpdateProgressState(&state, OP_ANALYZE_MBR, -1) == 655);
    CHECK(UpdateProgressState(&state, OP_ZERO_MBR, -1) == 1310);
    CHECK(UpdateProgressState(&state, OP_PARTITION, -1) == 1966);
    CHECK(UpdateProgressState(&state, OP_CREATE_FS, 100) == 9830);
    CHECK(UpdateProgressState(&state, OP_FIX_MBR, -1) == 10485);
    CHECK(UpdateProgressState(&state, OP_FILE_COPY, 50) == 37027);
    CHECK(UpdateProgressState(&state, OP_FINALIZE, -1) == 64224);
    CHECK(UpdateProgressState(&state, OP_FINALIZE, -1) == 64879);
    CHECK(UpdateProgressState(&state, OP_FINALIZE, -1) == 65535);
}

static void OtherWin32Modes(void)
{
    RUFUS_PROGRESS_OPTIONS options = { 0 };
    RUFUS_PROGRESS state;
    options.only_format = true;
    InitProgressState(&state, &options);
    CHECK(UpdateProgressState(&state, OP_FORMAT, 50) == 32767);
    options.only_format = false;
    options.selection = BT_IMAGE; /* Raw image: analyze 1%, write 99%. */
    InitProgressState(&state, &options);
    CHECK(state.end[OP_FORMAT] == 1.0f);
    CHECK(state.end[OP_FORMAT + 1] == 100.0f);
    options.bootable = true;
    options.selection = BT_FREEDOS;
    options.filesystem_steps = 5;
    InitProgressState(&state, &options);
    CHECK(state.nb_slots[OP_FILE_COPY] == 6);
    CHECK(state.end[OP_MAX] == 100.0f);
    options.selection = BT_MSDOS;
    InitProgressState(&state, &options);
    CHECK(state.nb_slots[OP_FILE_COPY] == 4);
    options.selection = BT_SYSLINUX_V6;
    InitProgressState(&state, &options);
    CHECK(state.nb_slots[OP_FILE_COPY] == 3);
    options.selection = BT_IMAGE;
    options.image = options.windows_customization = options.bad_blocks = options.extra_archive = true;
    options.filesystem_steps = 2; /* VDS. */
    options.ntfs_image = true;
    InitProgressState(&state, &options);
    CHECK(state.nb_slots[OP_CREATE_FS] == 2);
    CHECK(state.nb_slots[OP_BADBLOCKS] == -1);
    CHECK(state.nb_slots[OP_PATCH] == -1);
    CHECK(state.nb_slots[OP_EXTRACT_ZIP] == -1);
    CHECK(state.end[OP_MAX] == 100.0f);
    CHECK(UpdateProgressState(&state, OP_FILE_COPY, -1) == -1);
    CHECK(UpdateProgressState(&state, OP_MAX, 50) == -1);
    CHECK(UpdateProgressState(&state, OP_FILE_COPY, NAN) == -1);
    CHECK(UpdateProgressState(&state, OP_FILE_COPY, INFINITY) == -1);
    CHECK(UpdateProgressState(&state, OP_FILE_COPY, 101) == -1);
    CHECK(UpdateProgressState(&state, OP_ZERO_MBR, 0) >= 0);
    options.only_format = true;
    options.extra_archive = false;
    InitProgressState(&state, &options);
    CHECK(UpdateProgressState(&state, OP_FILE_COPY, 50) == -1);
    CHECK(state.previous_end == 0.0f);
}

int main(void)
{
    char text[32];
    IsoRanges();
    OtherWin32Modes();
    CHECK(ProgressPercent(0, 0) == 0.0f);
    CHECK(ProgressPercent(2, 1) == 100.0f);
    CHECK(ProgressPercent(UINT64_MAX / 2, UINT64_MAX) == 50.0f);
    snprintf(text, sizeof(text), "%0.1f%%", (double)ProgressPercent(2, 3));
    CHECK(strcmp(text, "66.7%") == 0);
    printf("Rufus progress policy: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
