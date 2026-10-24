/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "winpe_setup.h"
#include "image_report.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned int checks, failures, logs;
static char last_log[1024];
#define CHECK(condition) do { checks++; if (!(condition)) { \
    fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #condition); failures++; } } while (0)

void ReferencePatchWinPe(char* buffer, uint32_t size, unsigned int index, int uses_minint);

void uprintf(const char* format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(last_log, sizeof(last_log), format, arguments);
    va_end(arguments);
    logs++;
}

static void TestSelection(void)
{
    /* Independent constants from the original report, including partial bits. */
    for (uint16_t mask = 0; mask < 512; mask++) {
        const unsigned int expected_index = (mask & 0x23) == 0x23 ? 1 : (mask & 0x1c0) == 0x1c0 ? 2 : 0;
        const char* expected_setup = expected_index == 1 ? "amd64" : expected_index == 2 ? "minint" : "i386";
        for (int minint = 0; minint <= 1; minint++) {
            const WINPE_SETUP_PLAN plan = GetWinPeSetupPlan(mask, minint != 0);
            const WINPE_SETUP_ACTION action = (mask & 0x1c0) == 0 ? WINPE_PATCH_LOADER
                : minint ? WINPE_KEEP_LOADER : WINPE_UNSUPPORTED_LAYOUT;
            CHECK(plan.index == expected_index);
            CHECK(strcmp(plan.setup_directory, expected_setup) == 0);
            CHECK(strcmp(plan.loader_directory, expected_index == 2 ? "minint" : "i386") == 0);
            CHECK(plan.copy_sif == !minint);
            const bool has_winpe = (mask & 0x0007) == 0x0007 || (mask & 0x0023) == 0x0023 ||
                (mask & 0x01c0) == 0x01c0;
            CHECK(plan.masquerade_disk == (has_winpe && !minint));
            CHECK(plan.action == action);
        }
    }
    CHECK(strcmp(WINPE_SETUP_SECTION, "[SetupData]") == 0);
    CHECK(strcmp(WINPE_SETUP_SOURCE_DEVICE, "SetupSourceDevice = \"\\device\\harddisk1\\partition1\"") == 0);
}

static void TestExactBytes(void)
{
    char buffer[512], expected[512];
    static const struct {
        const char* source;
        const char* i386;
        const char* amd64;
        unsigned int offset;
        bool with_minint;
    } cases[] = {
        { "\\MiNiNt\\tXtSeTuP.siX", "\\i386\\txtsetup.sif", "\\amd64\\txtsetup.sif", 1, true },
        { "\\minint\\system32X", "\\i386\\system32\\", "\\amd64\\system32\\", 1, true },
        { "rdisk(0)", "rdisk(1)", "rdisk(1)", 0, false },
        { "RDISK(0X)", "RDISK(1X)", "RDISK(1X)", 0, false },
        { "rdisk(00)", "rdisk(10)", "rdisk(10)", 0, false },
        { "\\minint\\txtsetup.sif", "\\minint\\txtsetup.sif", "\\minint\\txtsetup.sif", 0, true },
        { "rdisk(0)", "rdisk(0)", "rdisk(0)", 480, false },
        { "\\minint\\system32\\", "\\minint\\system32\\", "\\minint\\system32\\", 480, true }
    };
    for (size_t n = 0; n < sizeof(cases) / sizeof(cases[0]); n++) {
        for (unsigned int index = 0; index < 2; index++) {
            memset(buffer, 0x5a, sizeof(buffer));
            memcpy(buffer + cases[n].offset, cases[n].source, strlen(cases[n].source) + 1);
            memcpy(expected, buffer, sizeof(expected));
            const char* replacement = index == 0 ? cases[n].i386 : cases[n].amd64;
            memcpy(expected + cases[n].offset, replacement, strlen(replacement) + 1);
            logs = 0;
            CHECK(PatchWinPeLoader(buffer, sizeof(buffer), index, cases[n].with_minint));
            CHECK(memcmp(buffer, expected, sizeof(buffer)) == 0);
        }
    }
    for (unsigned int index = 0; index < 2; index++) {
        memset(buffer, 0x5a, sizeof(buffer));
        memcpy(buffer + 64, "$WIN_NT$.~bX\\rest", sizeof("$WIN_NT$.~bX\\rest"));
        memcpy(expected, buffer, sizeof(expected));
        memcpy(expected + 64, index == 0 ? "i386\\" : "amd64\\", index == 0 ? 6 : 7);
        CHECK(PatchWinPeLoader(buffer, sizeof(buffer), index, false));
        CHECK(memcmp(buffer, expected, sizeof(buffer)) == 0);
        CHECK(strstr(last_log, index == 0 ? "i386\\rest" : "amd64\\rest") != NULL);
    }
    memset(buffer, 0x5a, sizeof(buffer));
    memcpy(buffer + 1, "rdisk(0) $win_nt$.~bt\\rest", sizeof("rdisk(0) $win_nt$.~bt\\rest"));
    memcpy(expected, buffer, sizeof(expected));
    CHECK(PatchWinPeLoader(buffer, sizeof(buffer), 0, true));
    CHECK(memcmp(buffer, expected, sizeof(buffer)) == 0);
}

static void TestCrcAndArguments(void)
{
    char buffer[0x2064], expected[sizeof(buffer)];
    for (uint32_t size = 0x205f; size <= sizeof(buffer); size++) {
        for (unsigned int mismatch = 0; mismatch < 3; mismatch++) {
            memset(buffer, 0x5a, sizeof(buffer));
            buffer[0x2060] = mismatch == 1 ? 0x75 : 0x74;
            buffer[0x2061] = mismatch == 2 ? 0x04 : 0x03;
            memcpy(expected, buffer, sizeof(expected));
            const bool patched = size >= 0x2062 && mismatch == 0;
            if (patched) {
                expected[0x2060] = (char)0xeb;
                expected[0x2061] = 0x1a;
            }
            logs = 0;
            CHECK(PatchWinPeLoader(buffer, size, 0, false));
            CHECK(memcmp(buffer, expected, sizeof(buffer)) == 0);
            CHECK(logs == (patched ? 1u : 0u));
        }
    }
    CHECK(PatchWinPeLoader(NULL, 0, 0, false));
    CHECK(!PatchWinPeLoader(NULL, 1, 0, false));
    memcpy(expected, buffer, sizeof(expected));
    CHECK(!PatchWinPeLoader(buffer, sizeof(buffer), 2, false));
    CHECK(memcmp(buffer, expected, sizeof(buffer)) == 0);
}

static void TestBounds(void)
{
    const char* patterns[] = { "\\minint\\txtsetup.sif", "\\minint\\system32\\", "rdisk(0)", "$win_nt$.~bt\\suffix" };
    for (uint32_t size = 1; size <= 96; size++) {
        char* buffer = malloc(size);
        char* original = malloc(size);
        if (buffer == NULL || original == NULL) abort();
        for (size_t n = 0; n < sizeof(patterns) / sizeof(patterns[0]); n++) {
            for (uint32_t offset = 0; offset < size; offset++) {
                memset(original, 0x5a, size); /* Deliberately no terminal NUL. */
                size_t length = strlen(patterns[n]);
                if (length > size - offset) length = size - offset;
                memcpy(original + offset, patterns[n], length);
                for (unsigned int index = 0; index < 2; index++) {
                    memcpy(buffer, original, size);
                    CHECK(PatchWinPeLoader(buffer, size, index, false));
                    if (size <= 32 || offset >= size - 32)
                        CHECK(memcmp(buffer, original, size) == 0);
                }
            }
        }
        free(original);
        free(buffer);
    }
}

static void TestOriginalAgreement(void)
{
    char reference[512], actual[512];
    const char* patterns[] = { "\\MININT\\txtsetup.sif", "\\minint\\system32\\", "rdisk(0)", "$win_nt$.~bt\\rest" };
    for (unsigned int index = 0; index < 2; index++) {
        for (int minint = 0; minint <= 1; minint++) {
            for (unsigned int offset = 0; offset < 96; offset++) {
                memset(reference, 0x5a, sizeof(reference));
                reference[sizeof(reference) - 1] = 0;
                for (size_t n = 0; n < sizeof(patterns) / sizeof(patterns[0]); n++) {
                    memcpy(reference + offset + n * 100, patterns[n], strlen(patterns[n]) + 1);
                }
                memcpy(actual, reference, sizeof(actual));
                ReferencePatchWinPe(reference, sizeof(reference), index, minint);
                CHECK(PatchWinPeLoader(actual, sizeof(actual), index, minint != 0));
                CHECK(memcmp(actual, reference, sizeof(actual)) == 0);
            }
        }
    }
    /* The original inner loop checks the second path after advancing i.
     * Dense and adjacent markers expose its skip/order behavior. */
    for (unsigned int index = 0; index < 2; index++) {
        for (int minint = 0; minint <= 1; minint++) {
            for (size_t first = 0; first < sizeof(patterns) / sizeof(patterns[0]); first++) {
                for (size_t second = 0; second < sizeof(patterns) / sizeof(patterns[0]); second++) {
                    for (unsigned int gap = 0; gap < 9; gap++) {
                        memset(reference, 0x5a, sizeof(reference));
                        reference[sizeof(reference) - 1] = 0;
                        memcpy(reference + 1, patterns[first], strlen(patterns[first]) + 1);
                        memcpy(reference + 1 + strlen(patterns[first]) + gap,
                            patterns[second], strlen(patterns[second]) + 1);
                        memcpy(actual, reference, sizeof(actual));
                        ReferencePatchWinPe(reference, sizeof(reference), index, minint);
                        CHECK(PatchWinPeLoader(actual, sizeof(actual), index, minint != 0));
                        CHECK(memcmp(actual, reference, sizeof(actual)) == 0);
                    }
                }
            }
        }
    }
}

static void TestRepeatedPatches(void)
{
    const char* path = "\\minint\\txtsetup.sif";
    const size_t path_length = strlen(path);
    for (unsigned int index = 0; index < 2; index++) {
        for (int adjacent = 0; adjacent <= 1; adjacent++) {
            char actual[128], reference[128], first[128];
            memset(actual, 0x5a, sizeof(actual));
            actual[sizeof(actual) - 1] = 0;
            memcpy(actual + 1, path, path_length + 1);
            if (adjacent)
                memcpy(actual + 1 + path_length, path, path_length + 1);
            memcpy(reference, actual, sizeof(reference));
            for (int pass = 0; pass < 2; pass++) {
                ReferencePatchWinPe(reference, sizeof(reference), index, false);
                CHECK(PatchWinPeLoader(actual, sizeof(actual), index, false));
                CHECK(memcmp(actual, reference, sizeof(actual)) == 0);
                if (pass == 0)
                    memcpy(first, actual, sizeof(first));
            }
            /* Original advancement skips the adjacent second path on pass one.
             * Admission must compare one pass per input, never assume idempotence. */
            CHECK((memcmp(first, actual, sizeof(first)) != 0) == adjacent);
        }
    }
}

int main(void)
{
    TestSelection();
    TestExactBytes();
    TestCrcAndArguments();
    TestBounds();
    TestOriginalAgreement();
    TestRepeatedPatches();
    printf("WinPE setup: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
