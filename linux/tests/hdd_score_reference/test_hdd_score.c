/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Reference behavior only: no Windows enumeration or native disk access. */
#define _GNU_SOURCE
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "hdd_vs_ufd.h"
#include "hdd_score.h"

typedef uint32_t DWORD;
typedef bool BOOL;
enum { DRIVE_UNKNOWN = 0, DRIVE_REMOVABLE = 2, DRIVE_FIXED = 3 };
#define GB INT64_C(1073741824)
#define ARRAYSIZE(array) (sizeof(array) / sizeof((array)[0]))
/* The reference cases use ASCII names in the C locale. These shims do not
 * qualify Windows's locale-sensitive string APIs for arbitrary device names. */
#define _strnicmp strncasecmp
#define StrStrIA strcasestr
#define static_strcat(destination, text) strcat(destination, text)
#define safe_sprintf snprintf

static unsigned drive_type;
static const DWORD reference_drive_index = 7;
static uint64_t capacity;
static bool usb_debug;
static unsigned type_queries, size_queries, messages;
static char last_message[256];

static unsigned GetDriveTypeFromIndex(DWORD index)
{
    if (index != reference_drive_index) abort();
    ++type_queries;
    return drive_type;
}

static uint64_t GetDriveSize(DWORD index)
{
    if (index != reference_drive_index) abort();
    ++size_queries;
    return capacity;
}

static void uprintf(const char* format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(last_message, sizeof(last_message), format, arguments);
    va_end(arguments);
    ++messages;
}

#define IsHDD OriginalIsHDD
#include "original_hdd_score.inc"
#undef IsHDD
#include "current_hdd_wrapper.inc"

struct score_case {
    const char* name;
    unsigned type;
    uint64_t bytes;
    uint16_t vid, pid;
    const char* model;
    int expected;
};

static const struct score_case cases[] = {
    {"unknown-type", DRIVE_UNKNOWN, 256 * GB, 0, 0, NULL, 0},
    {"removable-medium", DRIVE_REMOVABLE, 256 * GB, 0, 0, NULL, 0},
    {"fixed-medium", DRIVE_FIXED, 256 * GB, 0, 0, NULL, 3},
    {"unknown-size-original", DRIVE_FIXED, 0, 0, 0, NULL, -12},
    {"below-128", DRIVE_FIXED, 128 * GB - 1, 0, 0, NULL, -12},
    {"at-128", DRIVE_FIXED, 128 * GB, 0, 0, NULL, 3},
    {"above-128", DRIVE_FIXED, 128 * GB + 1, 0, 0, NULL, 3},
    {"below-800", DRIVE_REMOVABLE, 800 * GB - 1, 0, 0, NULL, 0},
    {"at-800", DRIVE_REMOVABLE, 800 * GB, 0, 0, NULL, 0},
    {"above-800", DRIVE_REMOVABLE, 800 * GB + 1, 0, 0, NULL, 15},
    {"below-1800", DRIVE_FIXED, 1800 * GB - 1, 0, 0, NULL, 18},
    {"at-1800", DRIVE_FIXED, 1800 * GB, 0, 0, NULL, 18},
    {"above-1800", DRIVE_FIXED, 1800 * GB + 1, 0, 0, NULL, 33},
    {"empty-name", DRIVE_REMOVABLE, 256 * GB, 0, 0, "", 0},
    {"hdd-prefix", DRIVE_REMOVABLE, 256 * GB, 0, 0, "WDC Portable", 10},
    {"case-insensitive-prefix", DRIVE_REMOVABLE, 256 * GB, 0, 0, "wdc Portable", 10},
    {"prefix-not-substring", DRIVE_REMOVABLE, 256 * GB, 0, 0, "USB WDC Portable", 0},
    {"short-name-early-stop", DRIVE_REMOVABLE, 256 * GB, 0, 0, "PNY USB", 0},
    {"longer-name-reaches-pny", DRIVE_REMOVABLE, 256 * GB, 0, 0, "PNY USB Device", -15},
    {"wildcard-at-end", DRIVE_REMOVABLE, 256 * GB, 0, 0, "ST1", 0},
    {"wildcard-following-letter", DRIVE_REMOVABLE, 256 * GB, 0, 0, "ST1A", 0},
    {"wildcard-following-digit", DRIVE_REMOVABLE, 256 * GB, 0, 0, "STX1", 10},
    {"wildcard-normal-model", DRIVE_REMOVABLE, 256 * GB, 0, 0, "ST1000", 10},
    {"cumulative-adjustments", DRIVE_FIXED, 256 * GB, 0, 0, "Flash SSD SATA", 33},
    {"case-insensitive-adjustments", DRIVE_FIXED, 256 * GB, 0, 0, "flash ssd sata", 33},
    {"known-flash-vid", DRIVE_FIXED, 256 * GB, 0x0781, 0, NULL, -2},
    {"vid-and-pid-add", DRIVE_FIXED, 256 * GB, 0x0781, 0x5580, NULL, -22},
    {"bridge-flash-exception", DRIVE_FIXED, 256 * GB, 0x152d, 0x0901, NULL, -7},
    {"known-hdd-vid-pid", DRIVE_FIXED, 256 * GB, 0x1058, 0x0702, NULL, 18},
    {"small-fixed-flash", DRIVE_FIXED, 32 * GB, 0, 0, NULL, -12},
    {"large-removable-hdd", DRIVE_REMOVABLE, 2000 * GB, 0, 0, NULL, 30},
    {"all-adjustments-and-debug-capacity", DRIVE_FIXED, 2000 * GB, 0x1058, 0x0702,
        "WDC SSK Port able Gadget Flash SD-CARD uSD Card HDD SATA SCSI SSD", 78},
};

static void check_case(const struct score_case* test, bool has_expected)
{
    drive_type = test->type;
    capacity = test->bytes;
    for (unsigned debug = 0; debug < 2; ++debug) {
        usb_debug = debug != 0;
        type_queries = size_queries = messages = 0;
        last_message[0] = '\0';
        const int original = OriginalIsHDD(reference_drive_index, test->vid, test->pid, test->model);
        if (type_queries != 1 || size_queries != 1 || messages != debug) abort();
        char original_message[sizeof(last_message)];
        memcpy(original_message, last_message, sizeof(original_message));

        type_queries = size_queries = messages = 0;
        last_message[0] = '\0';
        const int score = IsHDD(reference_drive_index, test->vid, test->pid, test->model);
        const HDD_SCORE result = GetHddScore(drive_type == DRIVE_FIXED, capacity, test->vid, test->pid, test->model);
        const bool queries_match = type_queries == 1 && size_queries == 1;
        const bool logs_match = messages == debug && strcmp(last_message, original_message) == 0;
        const bool scores_match = score == original && result.total == original;
        const bool expected_matches = !has_expected || original == test->expected;
        if (!scores_match || !expected_matches || !queries_match || !logs_match) {
            fprintf(stderr, "%s model=%s vid:pid=%04x:%04x size=%llu type=%u: score=%d original=%d expected=%d\n",
                test->name, test->model ? test->model : "(null)", test->vid, test->pid,
                (unsigned long long)test->bytes, test->type, score, original, test->expected);
            exit(1);
        }
        if (result.count > HDD_SCORE_MAX_PARTS) abort();
        int sum = 0;
        for (size_t i = 0; i < result.count; ++i) sum += result.parts[i];
        if (sum != score) abort();
    }
}

static size_t compare_tables(void)
{
    const unsigned types[] = {DRIVE_UNKNOWN, DRIVE_REMOVABLE, DRIVE_FIXED};
    const uint64_t sizes[] = {0, 32 * GB, 128 * GB - 1, 128 * GB, 800 * GB,
        800 * GB + 1, 1800 * GB, 1800 * GB + 1};
    size_t compared = 0;
    for (size_t size = 0; size < ARRAYSIZE(sizes); ++size) {
        for (size_t type = 0; type < ARRAYSIZE(types); ++type) {
            struct score_case test = {"table-differential", types[type], sizes[size], 0, 0, NULL, 0};
            for (size_t i = 0; i < ARRAYSIZE(vid_score); ++i) {
                test.vid = vid_score[i].vid;
                check_case(&test, false);
                ++compared;
            }
            for (size_t i = 0; i < ARRAYSIZE(vidpid_score); ++i) {
                test.vid = vidpid_score[i].vid;
                test.pid = vidpid_score[i].pid;
                check_case(&test, false);
                ++compared;
            }
            test.vid = test.pid = 0;
            for (size_t i = 0; i < ARRAYSIZE(str_score); ++i) {
                char model[128];
                snprintf(model, sizeof(model), "%s", str_score[i].name);
                test.model = model;
                check_case(&test, false);
                ++compared;
                const size_t length = strlen(model);
                if (model[length - 1] == '#') model[length - 1] = '1';
                strcat(model, "0 Device Flash SSD");
                check_case(&test, false);
                ++compared;
            }
            for (size_t i = 0; i < ARRAYSIZE(str_adjust); ++i) {
                test.model = str_adjust[i].name;
                check_case(&test, false);
                ++compared;
            }
        }
    }
    return compared;
}

int main(void)
{
    for (size_t i = 0; i < ARRAYSIZE(cases); ++i) {
        check_case(&cases[i], true);
        printf("%s: score=%d default=%s\n", cases[i].name, cases[i].expected,
            cases[i].expected > 0 ? "hidden" : "listed");
    }
    const size_t compared = compare_tables();
    printf("PASS: %zu expected cases and %zu table cases match the original score and full diagnostics, debug off/on.\n",
        ARRAYSIZE(cases), compared);
    return 0;
}
