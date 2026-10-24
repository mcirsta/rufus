/* SPDX-License-Identifier: GPL-3.0-or-later
 * Label-token rules extracted from Rufus's parser.c, Copyright Pete Batard.
 */
#include "drive_label.h"

static bool IsSpace(uint16_t value)
{
    return value == ' ' || value == '\t';
}

bool FindDriveLabel16(const uint16_t* line, size_t units, size_t* start, size_t* length)
{
    if (line == NULL || start == NULL || length == NULL)
        return false;
    *start = *length = 0;
    size_t position = 0;
    while (position < units && IsSpace(line[position])) position++;
    if (position < units && line[position] == '<') position++;
    while (position < units && IsSpace(line[position])) position++;
    static const char token[] = "label";
    const size_t token_units = sizeof(token) - 1;
    if (units - position < token_units) return false;
    for (size_t index = 0; index < token_units; index++) {
        uint16_t value = line[position + index];
        if (value >= 'A' && value <= 'Z') value += 'a' - 'A';
        if (value != (uint16_t)token[index]) return false;
    }
    position += token_units;
    while (position < units && IsSpace(line[position])) position++;
    if (position == units) return false;
    const bool xml = line[position] == '>';
    if (!xml && line[position] != '=') return false;
    position++;
    while (position < units && IsSpace(line[position])) position++;
    const bool quoted = position < units && line[position] == '"';
    if (quoted) position++;
    const size_t begin = position;
    while (position < units && line[position] != 0) {
        const bool closing_quote = quoted && line[position] == '"';
        const bool closing_xml = xml && line[position] == '<';
        if (closing_quote || closing_xml) break;
        position++;
    }
    while (position > begin && (line[position - 1] == '\r' || line[position - 1] == '\n')) position--;
    if (position == begin) return false;
    *start = begin; *length = position - begin;
    return true;
}
