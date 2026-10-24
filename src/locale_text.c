/* SPDX-License-Identifier: GPL-3.0-or-later
 * Quoted arguments and logical-line rules from parser.c, Copyright Pete Batard.
 */
#include "locale_text.h"
#include <string.h>

bool ReadLocaleQuoted(char* line, size_t capacity, size_t* position, size_t* start)
{
    if (line == NULL || position == NULL || start == NULL)
        return false;
    size_t index = *position;
    if (index >= capacity)
        return false;
    char* zero = memchr(line + index, 0, capacity - index);
    if (zero == NULL || line[index] != '"')
        return false;
    size_t end = (size_t)(zero - line);
    *start = ++index;
    while (index < end && (line[index] != '"' || line[index - 1] == '\\')) {
        if (line[index] == '"' && line[index - 1] == '\\') {
            memmove(line + index - 1, line + index, end - index + 1);
            --end;
        } else {
            ++index;
        }
    }
    if (index == end)
        return false;
    line[index++] = 0;
    *position = index;
    return true;
}

int ReadLocaleLine(const char* data, size_t size, size_t* position, char* line, size_t capacity)
{
    if (data == NULL || position == NULL || line == NULL || capacity < 3 || *position > size)
        return -1;
    size_t used = 0, last = 0;
    bool eol = false, escape = false;
    while (*position < size) {
        const char c = data[(*position)++];
        if (c == '\r' || c == '\n') {
            if (escape) {
                escape = false;
                continue;
            }
            if (!eol) {
                last = used;
                while (last > 0 && (line[last - 1] == ' ' || line[last - 1] == '\t'))
                    --last;
                eol = true;
            }
            continue;
        }
        if (c == ' ' || c == '\t') {
            if (escape) {
                escape = false;
                continue;
            }
            if (eol)
                continue;
        } else if (c == '\\' && !escape) {
            escape = true;
            continue;
        } else if (eol) {
            if (c == '"' && last > 0 && line[last - 1] == '"') {
                used = last - 1;
                eol = false;
                continue;
            }
            --*position;
            line[used] = 0;
            return 1;
        }
        if (used >= capacity - 2)
            return -1;
        if (escape) {
            if (c == 'n') {
                line[used++] = '\r';
                line[used++] = '\n';
            } else if (c == '"') {
                line[used++] = '\\';
                line[used++] = '"';
            } else if (c == '\\') {
                line[used++] = '\\';
            }
            escape = false;
        } else {
            line[used++] = c;
        }
    }
    line[used] = 0;
    return used != 0 ? 1 : 0;
}
