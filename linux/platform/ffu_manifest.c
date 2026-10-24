/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ffu_manifest.h"
#include "rufus_reporting.h"
#include <errno.h>
#include <iconv.h>
#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <wctype.h>
#include <openssl/evp.h>

enum {
    STORE_ID = 1, STORE_START = 2, STORE_LENGTH = 4, STORE_MAXIMUM = 8,
    PART_ID = 1, PART_TYPE = 2, PART_OFFSET = 4, PART_LENGTH = 8,
    PART_ATTRIBUTES = 16, PART_NAME = 32, PART_NAME_U = 64,
    PART_USE_ALL = 128, PART_REWRITE = 256,
    REQUIRED_STORE = STORE_ID | STORE_START | STORE_LENGTH | STORE_MAXIMUM,
    REQUIRED_PART = PART_ID | PART_TYPE | PART_OFFSET | PART_LENGTH | PART_ATTRIBUTES,
    MAX_PARTITIONS = 4 * 1024 * 1024 / 128, INITIAL_PARTITIONS = 8
};

static char* Trim(char* text)
{
    while (*text == ' ' || *text == '\t' || *text == '\r') text++;
    size_t bytes = strlen(text);
    while (bytes && (text[bytes - 1] == ' ' || text[bytes - 1] == '\t' || text[bytes - 1] == '\r')) text[--bytes] = 0;
    return text;
}

static int Number(const char* text, uint64_t* value)
{
    if (*text < '0' || *text > '9') return EBADMSG;
    char* end;
    errno = 0;
    unsigned long long parsed = strtoull(text, &end, 10);
    if (errno != 0 || *end != 0) return EBADMSG;
    *value = parsed;
    return 0;
}

static int Boolean(const char* text, bool* value)
{
    if (strcasecmp(text, "True") == 0) *value = true;
    else if (strcasecmp(text, "False") == 0) *value = false;
    else return EBADMSG;
    return 0;
}

static int Hex(char digit)
{
    if (digit >= '0' && digit <= '9') return digit - '0';
    if (digit >= 'a' && digit <= 'f') return digit - 'a' + 10;
    if (digit >= 'A' && digit <= 'F') return digit - 'A' + 10;
    return -1;
}

static int Guid(const char* text, uint8_t result[16])
{
    size_t bytes = strlen(text);
    if (bytes == 38 && text[0] == '{' && text[37] == '}') { text++; bytes -= 2; }
    if (bytes != 36) return EBADMSG;
    static const unsigned positions[16] = {6,4,2,0,11,9,16,14,19,21,24,26,28,30,32,34};
    if (text[8] != '-' || text[13] != '-' || text[18] != '-' || text[23] != '-') return EBADMSG;
    for (unsigned index = 0; index < 16; index++) {
        int high = Hex(text[positions[index]]), low = Hex(text[positions[index] + 1]);
        if (high < 0 || low < 0) return EBADMSG;
        result[index] = (uint8_t)((high << 4) | low);
    }
    return 0;
}

static int UnicodeName(const char* text, uint8_t name[72])
{
    const size_t length = strlen(text);
    if (length == 0 || length % 4 != 0 || length > 100) return EBADMSG;
    size_t padding = text[length - 1] == '=' ? 1 : 0;
    if (padding && text[length - 2] == '=') padding++;
    for (size_t index = 0; index < length - padding; index++) {
        const char c = text[index];
        const bool base64 = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
            || (c >= '0' && c <= '9') || c == '+' || c == '/';
        if (!base64) return EBADMSG;
    }
    uint8_t decoded[75];
    const int count = EVP_DecodeBlock(decoded, (const unsigned char*)text, (int)length);
    if (count < 0 || (size_t)count < padding) return EBADMSG;
    const size_t bytes = (size_t)count - padding;
    if (bytes < 2 || bytes > 74 || bytes % 2 || decoded[bytes - 1] || decoded[bytes - 2]) return EBADMSG;
    memset(name, 0, 72);
    memcpy(name, decoded, bytes - 2);
    return 0;
}

static int PlainName(const char* text, uint8_t name[72])
{
    iconv_t conversion = iconv_open("UTF-16LE", "UTF-8");
    if (conversion == (iconv_t)-1) return errno;
    char* source = (char*)text;
    char* output = (char*)name;
    size_t input_bytes = strlen(text), output_bytes = 72;
    memset(name, 0, 72);
    const size_t result = iconv(conversion, &source, &input_bytes, &output, &output_bytes);
    const int error = result == (size_t)-1 || input_bytes != 0 ? EBADMSG : 0;
    iconv_close(conversion);
    return error;
}

static int SetStoreField(FFU_OPTIMIZATION* store, const char* key, const char* value)
{
    unsigned field = 0;
    int error = 0;
    uint64_t number = 0;
    if (strcasecmp(key, "StoreId") == 0) { field = STORE_ID; error = Guid(value, store->id); }
    else {
        if (strcasecmp(key, "StartingUsableOffset") == 0) field = STORE_START;
        else if (strcasecmp(key, "UsableLength") == 0) field = STORE_LENGTH;
        else if (strcasecmp(key, "MaxPartitionCount") == 0) field = STORE_MAXIMUM;
        if (field == 0) return 0;
        error = Number(value, &number);
        if (field == STORE_START) store->usable_offset = number;
        if (field == STORE_LENGTH) store->usable_bytes = number;
        if (field == STORE_MAXIMUM) {
            if (number == 0 || number > MAX_PARTITIONS) return ENOTSUP;
            store->maximum_partitions = (uint32_t)number;
        }
    }
    if (store->fields & field) return EBADMSG;
    store->fields |= field;
    return error;
}

static int SetPartitionField(FFU_MANIFEST_PARTITION* part, const char* key, const char* value, const char** plain)
{
    unsigned field = 0;
    int error = 0;
    if (strcasecmp(key, "Id") == 0) { field = PART_ID; error = Guid(value, part->id); }
    else if (strcasecmp(key, "Type") == 0) { field = PART_TYPE; error = Guid(value, part->type); }
    else if (strcasecmp(key, "Name") == 0) { field = PART_NAME; *plain = value; }
    else if (strcasecmp(key, "Name_U") == 0) { field = PART_NAME_U; error = UnicodeName(value, part->name); }
    else if (strcasecmp(key, "UseAllSpace") == 0) { field = PART_USE_ALL; error = Boolean(value, &part->use_all_space); }
    else if (strcasecmp(key, "RewritePartition") == 0) { field = PART_REWRITE; error = Boolean(value, &part->rewrite); }
    else {
        uint64_t number = 0;
        if (strcasecmp(key, "OffsetInSectors") == 0) field = PART_OFFSET;
        else if (strcasecmp(key, "TotalSectors") == 0) field = PART_LENGTH;
        else if (strcasecmp(key, "Attributes") == 0) field = PART_ATTRIBUTES;
        if (field == 0) return 0;
        error = Number(value, &number);
        if (field == PART_OFFSET) part->offset_sectors = number;
        if (field == PART_LENGTH) part->sectors = number;
        if (field == PART_ATTRIBUTES) part->attributes = number;
    }
    if (part->fields & field) return EBADMSG;
    part->fields |= field;
    return error;
}

static int FinishPartition(FFU_MANIFEST_PARTITION* part, const char* plain)
{
    if (part == NULL) return 0;
    const bool complete = (part->fields & REQUIRED_PART) == REQUIRED_PART
        && (part->fields & (PART_NAME | PART_NAME_U)) != 0;
    if (!complete || part->sectors == 0) return EBADMSG;
    if (part->fields & PART_NAME_U) return 0;
    return PlainName(plain, part->name);
}

int ParseFfuOptimizedManifest(const char* manifest, unsigned count, FFU_OPTIMIZATION* stores)
{
    if (manifest == NULL || stores == NULL || count == 0) return EINVAL;
    char* copy = strdup(manifest);
    if (copy == NULL) return ENOMEM;
    bool full_flash = false, enabled = false, declared = false;
    int error = 0;
    char* state;
    for (char* line = strtok_r(copy, "\n", &state); line != NULL && error == 0; line = strtok_r(NULL, "\n", &state)) {
        line = Trim(line);
        if (*line == '[') { full_flash = strcasecmp(line, "[FullFlash]") == 0; continue; }
        char* separator = strchr(line, '=');
        if (!full_flash || separator == NULL) continue;
        *separator = 0;
        if (strcasecmp(Trim(line), "Optimized") != 0) continue;
        if (declared) { error = EBADMSG; break; }
        declared = true;
        error = Boolean(Trim(separator + 1), &enabled);
    }
    free(copy);
    if (error != 0 || !enabled) return error;
    copy = strdup(manifest);
    if (copy == NULL) return ENOMEM;
    unsigned store_index = 0;
    FFU_OPTIMIZATION* store = NULL;
    FFU_MANIFEST_PARTITION* part = NULL;
    const char* plain = NULL;
    bool store_section = false;
    for (char* line = strtok_r(copy, "\n", &state); line != NULL && error == 0; line = strtok_r(NULL, "\n", &state)) {
        if (IsRufusJobCancelled()) { error = ECANCELED; break; }
        line = Trim(line);
        if (*line == '[') {
            error = FinishPartition(part, plain);
            if (error != 0) break;
            part = NULL; plain = NULL;
            store_section = strcasecmp(line, "[Store]") == 0;
            if (store_section) {
                if (store_index >= count) { error = EBADMSG; break; }
                store = &stores[store_index++]; store->enabled = true;
            } else if (strcasecmp(line, "[Partition]") == 0) {
                if (store == NULL) { error = EBADMSG; break; }
                if (store->count >= MAX_PARTITIONS) { error = ENOTSUP; break; }
                const bool grow = store->count == 0 || (store->count >= INITIAL_PARTITIONS
                    && (store->count & (store->count - 1)) == 0);
                if (grow) {
                    const size_t capacity = store->count == 0 ? INITIAL_PARTITIONS : store->count * 2;
                    FFU_MANIFEST_PARTITION* grown = realloc(store->partitions, capacity * sizeof(*grown));
                    if (grown == NULL) { error = ENOMEM; break; }
                    store->partitions = grown;
                }
                part = &store->partitions[store->count++]; memset(part, 0, sizeof(*part));
            }
            continue;
        }
        if (*line == ';' || *line == '#') continue;
        char* separator = strchr(line, '=');
        if (separator == NULL) continue;
        *separator = 0;
        const char* key = Trim(line), *value = Trim(separator + 1);
        if (part != NULL) error = SetPartitionField(part, key, value, &plain);
        else if (store_section && store != NULL) error = SetStoreField(store, key, value);
    }
    if (error == 0) error = FinishPartition(part, plain);
    if (error == 0 && store_index != count) error = EBADMSG;
    unsigned optimized_partitions = 0;
    for (unsigned index = 0; error == 0 && index < count; index++) {
        store = &stores[index];
        if (store->fields != REQUIRED_STORE || store->count == 0 || store->count > store->maximum_partitions) { error = EBADMSG; break; }
        for (size_t part_index = 0; part_index < store->count; part_index++) {
            if (!store->partitions[part_index].use_all_space) continue;
            if (store->selected_partition != 0 || part_index == 0) { error = EBADMSG; break; }
            store->selected_partition = (unsigned)part_index + 1;
            optimized_partitions++;
        }
        store->enabled = store->selected_partition != 0;
    }
    if (error == 0 && optimized_partitions != 1) error = EBADMSG;
    free(copy);
    return error;
}

void FreeFfuOptimization(FFU_OPTIMIZATION* store)
{
    if (store == NULL) return;
    free(store->partitions);
    memset(store, 0, sizeof(*store));
}

bool SameFfuPartitionName(const uint8_t first[72], const uint8_t second[72])
{
    locale_t invariant = (locale_t)0;
    bool same = true;
    for (unsigned index = 0; index < 72; index += 2) {
        wint_t a = first[index] | (unsigned)first[index + 1] << 8;
        wint_t b = second[index] | (unsigned)second[index + 1] << 8;
        if (a == b) { if (a == 0) break; continue; }
        if (invariant == (locale_t)0) invariant = newlocale(LC_CTYPE_MASK, "C.UTF-8", (locale_t)0);
        if (invariant == (locale_t)0 || towlower_l(a, invariant) != towlower_l(b, invariant)) { same = false; break; }
    }
    if (invariant != (locale_t)0) freelocale(invariant);
    return same;
}
