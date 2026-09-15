/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "efi_boot_info.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int cases, failures, log_count;
static char last_log[512];
static const char* shim_marker = "UEFI SHIM\n$Version: ";
static const char* systemd_marker = "#### LoaderInfo: systemd-boot ";

#define CHECK(expression) do { \
	if (!(expression)) { \
		fprintf(stderr, "case %d, line %d: %s\n", cases, __LINE__, #expression); \
		failures++; \
	} \
} while (0)

void uprintf(const char* format, ...)
{
	va_list args;
	log_count++;
	va_start(args, format);
	vsnprintf(last_log, sizeof(last_log), format, args);
	va_end(args);
}

static void CheckBuffer(const char* buf, size_t size, const char* label, const char* version)
{
	const char* source = "/EFI/BOOT/boot's-%x.efi";
	char expected[512], *original = NULL;

	if (buf != NULL && size != 0) {
		original = malloc(size);
		if (original == NULL)
			exit(2);
		memcpy(original, buf, size);
	}
	cases++;
	last_log[0] = 0;
	log_count = 0;
	GetEfiBootInfo(buf, size, source);
	CHECK(log_count == (label != NULL ? 1 : 0));
	if (label != NULL) {
		snprintf(expected, sizeof(expected), "  Detected %s version: %s (from '%s')", label, version, source);
		CHECK(strcmp(last_log, expected) == 0);
	} else {
		CHECK(last_log[0] == 0);
	}
	if (original != NULL)
		CHECK(memcmp(original, buf, size) == 0);
	free(original);
}

static void PutBanner(char* buf, size_t offset, const char* marker, const char* text)
{
	size_t length = strlen(marker);
	memcpy(buf + offset, marker, length);
	memcpy(buf + offset + length, text, strlen(text) + 1);
}

static void CheckTokens(void)
{
	char buf[256], token[64];
	const char whitespace[] = " \t\n\r\f\v";
	const char* markers[] = { "UEFI SHIM\n$Version: ", "#### LoaderInfo: systemd-boot " };
	const char* labels[] = { "Shim", "systemd-boot" };

	for (size_t i = 0; i < 2; i++) {
		for (size_t j = 0; j < sizeof(whitespace) - 1; j++) {
			memset(buf, 0, sizeof(buf));
			snprintf(token, sizeof(token), "15.8-1%cignored", whitespace[j]);
			PutBanner(buf, 16, markers[i], token);
			CheckBuffer(buf, sizeof(buf), labels[i], "15.8-1");
		}
		PutBanner(buf, 16, markers[i], "");
		CheckBuffer(buf, sizeof(buf), labels[i], "");
		PutBanner(buf, 16, markers[i], " 15.8");
		CheckBuffer(buf, sizeof(buf), labels[i], "");
		PutBanner(buf, 16, markers[i], "v%02x-%s");
		CheckBuffer(buf, sizeof(buf), labels[i], "v%02x-%s");
		memset(token, 'A', sizeof(token) - 1);
		token[sizeof(token) - 1] = 0;
		PutBanner(buf, 16, markers[i], token);
		token[32] = 0;
		CheckBuffer(buf, sizeof(buf), labels[i], token);
	}
	// Embedded NUL terminates displayed text even when later bytes are non-space.
	memset(buf, 'x', sizeof(buf));
	PutBanner(buf, 0, shim_marker, "15.8");
	CheckBuffer(buf, sizeof(buf), "Shim", "15.8");
	// ctype receives unsigned byte values. The bytes are logged without rewriting.
	PutBanner(buf, 0, shim_marker, "v\200\377 ");
	CheckBuffer(buf, sizeof(buf), "Shim", "v\200\377");
}

static void CheckBoundsAndOrder(void)
{
	static const char readonly[] = "UEFI SHIM\n$Version: 15.8 "
		"                                                                ";
	char buf[256] = { 0 };
	PutBanner(buf, 0, shim_marker, "15.8");
	CheckBuffer(buf, 0, NULL, "");
	CheckBuffer(buf, 64, NULL, "");
	CheckBuffer(buf, 65, "Shim", "15.8");
	CheckBuffer(NULL, 256, NULL, "");
	for (size_t offset = 191; offset <= 192; offset++) {
		memset(buf, 0, sizeof(buf));
		PutBanner(buf, offset, systemd_marker, "261.3");
		CheckBuffer(buf, sizeof(buf), offset == 191 ? "systemd-boot" : NULL, "261.3");
	}
	// The actual span has no NUL, including at its last byte.
	memset(buf, 'A', sizeof(buf));
	memcpy(buf, systemd_marker, strlen(systemd_marker));
	CheckBuffer(buf, sizeof(buf), "systemd-boot", "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
	memset(buf, 0, sizeof(buf));
	PutBanner(buf, 0, systemd_marker, "261.3");
	PutBanner(buf, 128, shim_marker, "15.8");
	CheckBuffer(buf, sizeof(buf), "systemd-boot", "261.3");
	memset(buf, 0, sizeof(buf));
	PutBanner(buf, 0, shim_marker, "15.8");
	PutBanner(buf, 128, systemd_marker, "261.3");
	CheckBuffer(buf, sizeof(buf), "Shim", "15.8");
	memset(buf, 0, sizeof(buf));
	PutBanner(buf, 0, "uefi SHIM\n$Version: ", "15.8");
	PutBanner(buf, 128, "#### LoaderInfo: Systemd-boot ", "261.3");
	CheckBuffer(buf, sizeof(buf), NULL, "");
	memset(buf, 0, sizeof(buf));
	PutBanner(buf, 0, "ID=systemd-boot\nVERSION=", "261.3");
	CheckBuffer(buf, sizeof(buf), NULL, "");
	// Genuine read-only storage is accepted; callers no longer need a mutable copy.
	CheckBuffer(readonly, sizeof(readonly) - 1, "Shim", "15.8");
}

int main(void)
{
	CheckTokens();
	CheckBoundsAndOrder();
	printf("EFI boot info: %d cases, %d failures\n", cases, failures);
	return failures != 0;
}
