/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "config_text.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned int checks, failures;
#define CHECK(condition) do { checks++; if (!(condition)) { \
	fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #condition); failures++; } } while (0)

static void TestSectionMatching(void)
{
	static const struct {
		const wchar_t* line;
		bool matches;
	} cases[] = {
		{ L"[SetupData]\r\n", true }, { L"  \t[setupdata] trailing", true },
		{ L"[SetupData]suffix", true }, { L"[SetupData]", true },
		{ L"; [SetupData]", false }, { L"#[SetupData]", false },
		{ L"\v[SetupData]", false }, { L"[SetupDat]", false },
		{ L"[SetupDataExtra]", false }, { L"x[SetupData]", false },
		{ L"\ufeff[SetupData]", false }, { L"", false }, { L" \t", false }
	};
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
		CHECK(IsConfigSection(cases[i].line, L"[SetupData]") == cases[i].matches);
	const wchar_t* lines[] = { L"[SetupData]\n", L"key=value\n", L"[setupdata]\n" };
	size_t insertions = 0;
	for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++)
		insertions += IsConfigSection(lines[i], L"[SetupData]");
	CHECK(insertions == 2); // Original insertion applies to every matching chunk.
}

static void TestMatching(void)
{
	static const struct {
		const wchar_t* line;
		const wchar_t* token;
		const wchar_t* source;
		const wchar_t* replacement;
		const wchar_t* expected;
		size_t matches;
	} cases[] = {
		{ L"\t APPEND LABEL=old\n", L"append", L"old", L"new", L"\t APPEND LABEL=new\n", 1 },
		{ L"append\told old old old old\r\n", L"APPEND", L"old", L"X", L"append\tX X X X old\r\n", 4 },
		{ L"append oldold", L"append", L"old", L"old2", L"append old2old2", 2 },
		{ L"append aaaaa", L"append", L"aa", L"X", L"append XXa", 2 },
		{ L"append old old", L"append", L"old", L"", L"append  ", 2 },
		{ L"append Old old", L"append", L"old", L"new", L"append Old new", 1 },
		{ L"append old", L"append", L"old", L"old", L"append old", 1 },
		{ L"appendix old", L"append", L"old", L"new", L"appendix old", 0 },
		{ L"append=old", L"append", L"old", L"new", L"append=old", 0 },
		{ L"append\vold", L"append", L"old", L"new", L"append\vold", 0 },
		{ L"\vappend old", L"append", L"old", L"new", L"\vappend old", 0 },
		{ L"# append old", L"append", L"old", L"new", L"# append old", 0 },
		{ L"linuxefi old", L"linux", L"old", L"new", L"linuxefi old", 0 },
		{ L"$LiNuX old", L"$linux", L"old", L"new", L"$LiNuX new", 1 },
		{ L"append old\\x20disk", L"append", L"old\\x20disk", L"new\\x20disk", L"append new\\x20disk", 1 },
		{ L"append \u010d-\u0219", L"append", L"\u0219", L"\u00e9", L"append \u010d-\u00e9", 1 },
		{ L"old old", L"old", L"old", L"new", L"old new", 1 },
		{ L"append", L"append", L"old", L"new", L"append", 0 },
		{ L"", L"append", L"old", L"new", L"", 0 },
		{ L"append old", L"", L"old", L"new", L"append old", 0 },
		{ L"append old", L"append", L"", L"new", L"append old", 0 },
	};
	wchar_t line[CONFIG_TEXT_LINE_CAPACITY], output[CONFIG_TEXT_LINE_CAPACITY];
	size_t offsets[CONFIG_TEXT_MAX_REPLACEMENTS + 1];
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		wcscpy(line, cases[i].line);
		memset(offsets, 0xa5, sizeof(offsets));
		size_t matches = SplitConfigToken(line, cases[i].token, cases[i].source, offsets);
		CHECK(matches == cases[i].matches);
		CHECK(offsets[0] == 0);
		output[0] = 0;
		for (size_t j = 0; j <= matches; j++) {
			CHECK(offsets[j] <= wcslen(cases[i].line));
			wcscat(output, line + offsets[j]);
			if (j < matches)
				wcscat(output, cases[i].replacement);
		}
		CHECK(wcscmp(output, cases[i].expected) == 0);
		if (matches == 0)
			CHECK(wcscmp(line, cases[i].line) == 0);
	}
}

static void TestIsoPatches(void)
{
	static const char* tokens[] = { "options", "append", "linux", "linuxefi", "$linux", "search", "for" };
	static const struct { bool redhat; const char* path; size_t count; } cases[] = {
		{ false, "dvd.iso", 7 }, { true, "dvd.iso", 14 },
		{ true, "Fedora-netinst.iso", 7 }, { true, "/netinst/archive/dvd.iso", 7 },
		{ true, "NETINST.iso", 14 }, { true, "/proc/self/fd/7", 14 }
	};
	CONFIG_TEXT_PATCH patches[ISO_CONFIG_MAX_PATCHES];
	char* source = replace_char(" OLD LABEL ", ' ', "\\x20");
	char* replacement = replace_char("USB LABEL", ' ', "\\x20");
	CHECK(source != NULL && replacement != NULL);
	if (source == NULL || replacement == NULL) {
		free(source);
		free(replacement);
		return;
	}
	CHECK(strcmp(source, "\\x20OLD\\x20LABEL\\x20") == 0);
	CHECK(strcmp(replacement, "USB\\x20LABEL") == 0);
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		size_t count = GetIsoConfigPatches(source, replacement, cases[i].redhat, cases[i].path, patches);
		CHECK(count == cases[i].count);
		for (size_t j = 0; j < count; j++) {
			CHECK(strcmp(patches[j].token, tokens[j % 7]) == 0);
			CHECK(strcmp(patches[j].source, j < 7 ? source : "inst.stage2") == 0);
			CHECK(strcmp(patches[j].replacement, j < 7 ? replacement : "inst.repo") == 0);
		}
	}
	free(source);
	free(replacement);
	CHECK(replace_char(NULL, ' ', "x") == NULL);
	CHECK(replace_char("a", ' ', NULL) == NULL);
	source = replace_char("aaa", 'a', "");
	CHECK(source != NULL && strcmp(source, "") == 0);
	free(source);
}

static void TestFreeNasPatch(void)
{
	char source[CONFIG_VOLUME_PATH_CAPACITY], replacement[CONFIG_VOLUME_PATH_CAPACITY];
	CONFIG_TEXT_PATCH patch = GetFreeNasConfigPatch("OLD LABEL", "NEW USB", source, replacement);
	CHECK(strcmp(patch.token, "set") == 0);
	CHECK(strcmp(patch.source, "cd9660:/dev/iso9660/OLD LABEL") == 0);
	CHECK(strcmp(patch.replacement, "msdosfs:/dev/msdosfs/NEW USB") == 0);
	CHECK(patch.source == source && patch.replacement == replacement);
	patch = GetFreeNasConfigPatch("SAME", "SAME", source, replacement);
	CHECK(strcmp(patch.source, "cd9660:/dev/iso9660/SAME") == 0);
	CHECK(strcmp(patch.replacement, "msdosfs:/dev/msdosfs/SAME") == 0);
	char label[192];
	memset(label, 'a', sizeof(label) - 1);
	label[sizeof(label) - 1] = 0;
	patch = GetFreeNasConfigPatch(label, label, source, replacement);
	CHECK(strlen(patch.source) == strlen("cd9660:/dev/iso9660/") + 191);
	CHECK(strcmp(patch.source + strlen("cd9660:/dev/iso9660/"), label) == 0);
	CHECK(strlen(patch.replacement) == strlen("msdosfs:/dev/msdosfs/") + 191);
	CHECK(strcmp(patch.replacement + strlen("msdosfs:/dev/msdosfs/"), label) == 0);
}

int main(void)
{
	TestSectionMatching();
	TestMatching();
	TestIsoPatches();
	TestFreeNasPatch();
	printf("Config token matching: %u checks, %u failures\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
