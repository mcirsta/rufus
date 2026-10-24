/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "dos_locale_policy.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static bool WriteFile(const char* directory, const DOS_LOCALE* locale, bool config)
{
	char path[1024];
	int length = snprintf(path, sizeof(path), "%s/%s", directory, config ? "CONFIG.SYS" : "AUTOEXEC.BAT");
	const bool path_fits = length >= 0 && (size_t)length < sizeof(path);
	if (!path_fits)
		return false;
	FILE* file = fopen(path, "wb");
	if (file == NULL)
		return false;
	bool written = config ? WriteDOSConfig(file, locale) : WriteDOSAutoexec(file, locale);
	if (fclose(file) != 0)
		written = false;
	return written;
}

static bool CheckErrors(void)
{
	char keyboard[] = "ro";
	DOS_LOCALE locale = SelectDOSLocale(keyboard, 852, true);
	memset(keyboard, 0, sizeof(keyboard));
	if (strcmp(locale.keyboard, "ro") != 0)
		return false;
	const DOS_LOCALE unknown = SelectDOSLocale(NULL, UINT32_MAX, true);
	const bool fallback = unknown.keyboard_fallback && unknown.codepage_fallback && unknown.codepage == 437;
	if (!fallback || strcmp(GetDOSCodepageName(UINT32_MAX), "Unknown") != 0)
		return false;
	if (WriteDOSConfig(NULL, &locale) || WriteDOSAutoexec(NULL, &locale))
		return false;
	FILE* file = tmpfile();
	if (file == NULL)
		return false;
	bool valid = !WriteDOSConfig(file, NULL) && !WriteDOSAutoexec(file, NULL);
	if (fclose(file) != 0 || !valid)
		return false;
	for (int config = 0; config <= 1; config++) {
		file = fopen("/dev/full", "wb");
		if (file == NULL)
			return false;
		setvbuf(file, NULL, _IONBF, 0);
		valid = config ? !WriteDOSConfig(file, &locale) : !WriteDOSAutoexec(file, &locale);
		valid = valid && ferror(file) && errno == ENOSPC;
		fclose(file);
		if (!valid)
			return false;
	}
	return true;
}

int main(int argc, char** argv)
{
	if (argc == 2 && strcmp(argv[1], "--errors") == 0)
		return CheckErrors() ? 0 : 1;
	if (argc != 5)
		return 64;
	const DOS_LOCALE locale = SelectDOSLocale(argv[2], (uint32_t)strtoul(argv[3], NULL, 10), atoi(argv[4]) != 0);
	printf("KB=%s CP=%u DRIVER=%s EGA=%s MENU=%d KB_FALLBACK=%d CP_FALLBACK=%d NAME=%s\n",
		locale.keyboard, (unsigned)locale.codepage, locale.keyboard_driver, locale.ega_driver,
		locale.use_menu, locale.keyboard_fallback, locale.codepage_fallback, locale.keyboard_name);
	if (locale.use_menu && !WriteFile(argv[1], &locale, true))
		return 1;
	return WriteFile(argv[1], &locale, false) ? 0 : 1;
}
