/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "platform/ffu_catalog.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int cancelled;
int IsRufusJobCancelled(void) { return cancelled; }

int main(int argc, char** argv)
{
    if (argc != 2 && argc != 3) return 2;
    FILE* input = fopen(argv[1], "rb");
    if (input == NULL || fseek(input, 0, SEEK_END) != 0) return 3;
    long size = ftell(input);
    if (size < 0 || size > FFU_MAX_CATALOG_BYTES + 1L || fseek(input, 0, SEEK_SET) != 0) return 3;
    void* bytes = malloc(size == 0 ? 1 : (size_t)size);
    if (bytes == NULL || fread(bytes, 1, size, input) != (size_t)size || fclose(input) != 0) return 3;
    cancelled = argc == 3 && strcmp(argv[2], "cancel") == 0;
    FFU_CATALOG_RESULT result;
    memset(&result, 0xa5, sizeof(result));
    int error = VerifyFfuCatalog(bytes, size, &result);
    printf("{\"error\":%d,\"signed\":%s,\"sha256\":\"", error, result.signed_catalog ? "true" : "false");
    for (size_t index = 0; index < sizeof(result.sha256); index++) printf("%02x", result.sha256[index]);
    printf("\"}\n");
    free(bytes);
    return 0;
}
