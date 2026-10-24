/*
 * Rufus: The Reliable USB Formatting Utility
 * DOS keyboard locale setup
 * Copyright © 2011-2024 Pete Batard <pete@akeo.ie>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/* Memory leaks detection - define _CRTDBG_MAP_ALLOC as preprocessor macro */
#ifdef _CRTDBG_MAP_ALLOC
#include <stdlib.h>
#include <crtdbg.h>
#endif

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "rufus.h"
#include "dos_locale_policy.h"
#include <errno.h>

// http://blogs.msdn.com/b/michkap/archive/2004/12/05/275231.aspx
static const char* get_kb(void)
{
	unsigned int kbid;
	char kbid_str[KL_NAMELENGTH];
	int pass;

	// Count on Microsoft to add convolution to a simple operation.
	// We use GetKeyboardLayout() because it returns an HKL, which for instance
	// doesn't tell us if the *LAYOUT* is Dvorak or something else. For that we
	// need an KLID which GetKeyboardLayoutNameA() does return ...but only as a
	// string of an hex value...
	GetKeyboardLayoutNameA(kbid_str);
	if (sscanf(kbid_str, "%x", &kbid) <= 0) {
		uprintf("Could not scan keyboard layout name - defaulting to US");
		kbid = 0x00000409;
	}
	uprintf("Windows KBID 0x%08x\n", kbid);

	for (pass = 0; pass < 3; pass++) {
		// Some of these return values are defined in
		// HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Keyboard Layout\DosKeybCodes
		// Others are picked up in FreeDOS official keyboard layouts, v3.0
		// Note: keyboard values are meant to start at 0x400. The cases below 0x400 are added
		// to attempt to figure out a "best match" in case we couldn't find a supported keyboard
		// by using the one most relevant to the language being spoken. Also we intentionally
		// group keyboards that shouldn't be together

		// Note: these cases are mostly organized first by (kbid & 0x3ff) and then by
		// ascending order for same (kbid & 0x3ff)
		switch(kbid) {
		case 0x00000001:
		case 0x00010401: // Arabic (102)
		case 0x00020401: // Arabic (102) AZERTY
			return "ar";
		case 0x00000002:
		case 0x00000402: // Bulgarian (Typewriter)
		case 0x00010402: // Bulgarian (Latin)
		case 0x00020402: // Bulgarian (Phonetic)
		case 0x00030402: // Bulgarian
		case 0x00040402: // Bulgarian (Phonetic Traditional)
			return "bg";
		case 0x00000004:
		case 0x00000404: // Chinese (Traditional) - US Keyboard
		case 0x00000804: // Chinese (Simplified) - US Keyboard
		case 0x00000c04: // Chinese (Traditional, Hong Kong) - US Keyboard
		case 0x00001004: // Chinese (Simplified, Singapore) - US Keyboard
		case 0x00001404: // Chinese (Traditional, Macao) - US Keyboard
			return "ch";
		case 0x00000005:
		case 0x00000405: // Czech
		case 0x00010405: // Czech (QWERTY)
		case 0x00020405: // Czech Programmers
			return "cz";
		case 0x00000006:
		case 0x00000406: // Danish
			return "dk";
		case 0x00000007:
		case 0x00000407: // German
		case 0x00010407: // German (IBM)
			return "gr";
		case 0x00000807: // Swiss German
			return "sg";
		case 0x00000008:
		case 0x00000408: // Greek
		case 0x00010408: // Greek (220)
		case 0x00020408: // Greek (319)
		case 0x00030408: // Greek (220) Latin
		case 0x00040408: // Greek (319) Latin
		case 0x00050408: // Greek Latin
		case 0x00060408: // Greek Polytonic
			return "gk";
		case 0x00000009:
		case 0x00000409: // US
		case 0x00020409: // United States-International
		case 0x00050409: // US English Table for IBM Arabic 238_L
			return "us";
		case 0x00000809: // United Kingdom
		case 0x00000452: // United Kingdom Extended (Welsh)
		case 0x00001809: // Irish
		case 0x00011809: // Gaelic
			return "uk";
		case 0x00000c0c: // Canadian French (Legacy)
		case 0x00001009: // Canadian French
		case 0x00011009: // Canadian Multilingual Standard
			return "cf";
		case 0x00010409: // United States-Dvorak
			return "dv";
		case 0x00030409: // United States-Dvorak for left hand
			return "lh";
		case 0x00040409: // United States-Dvorak for right hand
			return "rh";
		case 0x0000000a:
		case 0x0000040a: // Spanish
		case 0x0001040a: // Spanish Variation
			return "sp";
		case 0x0000080a: // Latin American
			return "la";
		case 0x0000000b:
		case 0x0000040b: // Finnish
		case 0x0001083b: // Finnish with Sami
			return "su";
		case 0x0000000c:
		case 0x0000040c: // French
		case 0x0000046e: // Luxembourgish
			return "fr";
		case 0x0000080c: // Belgian French
		case 0x0001080c: // Belgian (Comma)
			return "be";
		case 0x0000100c: // Swiss French
			return "sf";
		case 0x0000000d:
		case 0x0000040d: // Hebrew
			return "il";
		case 0x0000000e:
		case 0x0000040e: // Hungarian
		case 0x0001040e: // Hungarian 101-key
			return "hu";
		case 0x0000000f:
		case 0x0000040f: // Icelandic
			return "is";
		case 0x00000010:
		case 0x00000410: // Italian
		case 0x00010410: // Italian (142)
			return "it";
		case 0x00000011:
		case 0x00000411: // Japanese
			return "jp";
	//	case 0x00000012:
	//	case 0x00000412: // Korean
	//		return "ko";	// NOT IMPLEMENTED IN FREEDOS?
		case 0x00000013:
		case 0x00000413: // Dutch
		case 0x00000813: // Belgian (Period)
			return "nl";
		case 0x00000014:
		case 0x00000414: // Norwegian
		case 0x0000043b: // Norwegian with Sami
		case 0x0001043b: // Sami Extended Norway
			return "no";
		case 0x00000015:
		case 0x00010415: // Polish (214)
		case 0x00000415: // Polish (Programmers)
			return "pl";
		case 0x00000016:
		case 0x00000416: // Portuguese (Brazilian ABNT)
		case 0x00010416: // Portuguese (Brazilian ABNT2)
			return "br";
		case 0x00000816: // Portuguese (Portugal)
			return "po";
		case 0x00000018:
		case 0x00000418: // Romanian (Legacy)
		case 0x00010418: // Romanian (Standard)
		case 0x00020418: // Romanian (Programmers)
			return "ro";
		case 0x00000019:
		case 0x00000419: // Russian
		case 0x00010419: // Russian (Typewriter)
			return "ru";
		case 0x0000001a:
		case 0x0000041a: // Croatian
		case 0x0000081a: // Serbian (Latin)
		case 0x00000024:
		case 0x00000424: // Slovenian
			return "yu";
		case 0x00000c1a: // Serbian (Cyrillic)
		case 0x0000201a: // Bosnian (Cyrillic)
			return "yc";
		case 0x0000001b:
		case 0x0000041b: // Slovak
		case 0x0001041b: // Slovak (QWERTY)
			return "sl";
		case 0x0000001c:
		case 0x0000041c: // Albanian
			return "sq";
		case 0x0000001d:
		case 0x0000041d: // Swedish
		case 0x0000083b: // Swedish with Sami
			return "sv";
		case 0x0000001f:
		case 0x0000041f: // Turkish Q
		case 0x0001041f: // Turkish F
			return "tr";
		case 0x00000022:
		case 0x00000422: // Ukrainian
		case 0x00020422: // Ukrainian (Enhanced)
			return "ur";
		case 0x00000023:
		case 0x00000423: // Belarusian
			return "bl";
		case 0x00000025:
		case 0x00000425: // Estonian
			return "et";
		case 0x00000026:
		case 0x00000426: // Latvian
		case 0x00010426: // Latvian (QWERTY)
			return "lv";
		case 0x00000027:
		case 0x00000427: // Lithuanian IBM
		case 0x00010427: // Lithuanian
		case 0x00020427: // Lithuanian Standard
			return "lt";
		case 0x00000028:
		case 0x00000428: // Tajik
			return "tj";
	//	case 0x00000029:
	//	case 0x00000429: // Persian
	//		return "fa";	// NOT IMPLEMENTED IN FREEDOS?
		case 0x0000002a:
		case 0x0000042a: // Vietnamese
			return "vi";
		case 0x0000002b:
		case 0x0000042b: // Armenian Eastern
		case 0x0001042b: // Armenian Western
			return "hy";
		case 0x0000002c:
		case 0x0000042c: // Azeri Latin
		case 0x0000082c: // Azeri Cyrillic
			return "az";
		case 0x0000002f:
		case 0x0000042f: // Macedonian (FYROM)
		case 0x0001042f: // Macedonian (FYROM) - Standard
			return "mk";
		case 0x00000037:
		case 0x00000437: // Georgian
		case 0x00010437: // Georgian (QWERTY)
		case 0x00020437: // Georgian (Ergonomic)
			return "ka";
		case 0x00000038:
		case 0x00000438: // Faeroese
			return "fo";
		case 0x0000003a:
		case 0x0000043a: // Maltese 47-Key
		case 0x0001043a: // Maltese 48-Key
			return "mt";
		case 0x0000003f:
		case 0x0000043f: // Kazakh
			return "kk";
		case 0x00000040:
		case 0x00000440: // Kyrgyz Cyrillic
			return "ky";
		case 0x00000043:
		case 0x00000843: // Uzbek Cyrillic
			return "uz";
		case 0x00000042:
		case 0x00000442: // Turkmen
			return "tm";
		case 0x00000044:
		case 0x00000444: // Tatar
			return "tt";

	// Below are more Windows 7 listed keyboards that were left out
	#if 0
		case 0x0000041e: // Thai Kedmanee
		case 0x0001041e: // Thai Pattachote
		case 0x0002041e: // Thai Kedmanee (non-ShiftLock)
		case 0x0003041e: // Thai Pattachote (non-ShiftLock)
		case 0x00000420: // Urdu
		case 0x0000042e: // Sorbian Standard (Legacy)
		case 0x0001042e: // Sorbian Extended
		case 0x0002042e: // Sorbian Standard
		case 0x00000432: // Setswana
		case 0x00000439: // Devanagari - INSCRIPT#
		case 0x00010439: // Hindi Traditional
		case 0x0002083b: // Sami Extended Finland-Sweden
		case 0x00000445: // Bengali
		case 0x00010445: // Bengali - INSCRIPT (Legacy)
		case 0x00020445: // Bengali - INSCRIPT
		case 0x00000446: // Punjabi
		case 0x00000447: // Gujarati
		case 0x00000448: // Oriya
		case 0x00000449: // Tamil
		case 0x0000044a: // Telugu
		case 0x0000044b: // Kannada
		case 0x0000044c: // Malayalam
		case 0x0000044d: // Assamese - INSCRIPT
		case 0x0000044e: // Marathi
		case 0x00000450: // Mongolian Cyrillic
		case 0x00000451: // Tibetan
		case 0x00000850: // Mongolian (Mongolian Script)
		case 0x0000085d: // Inuktitut - Latin
		case 0x0001045d: // Inuktitut - Naqittaut
		case 0x00000453: // Khmer
		case 0x00000454: // Lao
		case 0x0000045a: // Syriac
		case 0x0001045a: // Syriac Phonetic
		case 0x0000045b: // Sinhala
		case 0x0001045b: // Sinhala - Wij 9
		case 0x00000461: // Nepali
		case 0x00000463: // Pashto (Afghanistan)
		case 0x00000465: // Divehi Phonetic
		case 0x00010465: // Divehi Typewriter
		case 0x00000468: // Hausa
		case 0x0000046a: // Yoruba
		case 0x0000046c: // Sesotho sa Leboa
		case 0x0000046d: // Bashkir
		case 0x0000046f: // Greenlandic
		case 0x00000470: // Igbo
		case 0x00000480: // Uyghur (Legacy)
		case 0x00010480: // Uyghur
		case 0x00000481: // Maori
		case 0x00000485: // Yakut
		case 0x00000488: // Wolof
	#endif
		default:
			if (pass == 0) {
				// If we didn't get a match 1st time around, try to match
				// the primary language of the keyboard
				kbid = PRIMARYLANGID(kbid);
			} else if (pass == 1) {
				// If we still didn't get a match, use the system's primary language
				kbid = PRIMARYLANGID(GetSystemDefaultLangID());
				uprintf("Unable to match KBID, trying LangID 0x%04x", kbid);
			}
			break;
		}
	}
	uprintf("Unable to match KBID and LangID - defaulting to US");
	return "us";
}

static BOOL WriteDOSLocaleFile(const char* path, const DOS_LOCALE* locale, BOOL config)
{
	char filename[MAX_PATH];
	const char* name = config ? "CONFIG.SYS" : "AUTOEXEC.BAT";
	const int length = snprintf(filename, sizeof(filename), "%s\\%s", path, name);
	const BOOL path_fits = length >= 0 && (size_t)length < sizeof(filename);
	if (!path_fits) {
		uprintf("Invalid path supplied for DOS locale files");
		return FALSE;
	}
	FILE* file = fopen(filename, "wb");
	if (file == NULL) {
		uprintf("Unable to create '%s': %s", name, strerror(errno));
		return FALSE;
	}
	BOOL written = config ? WriteDOSConfig(file, locale) : WriteDOSAutoexec(file, locale);
	int error = errno;
	if (fclose(file) != 0) {
		error = errno;
		written = FALSE;
	}
	if (!written) {
		uprintf("Unable to write '%s': %s", name, strerror(error));
		return FALSE;
	}
	uprintf("Successfully wrote '%s'", name);
	return TRUE;
}

// Host facts stay here; both platforms use the same DOS selection and bytes.
BOOL SetDOSLocale(const char* path, BOOL bFreeDOS)
{
	if (path == NULL)
		return FALSE;
	const char* kb = get_kb();
	ULONG cp;
	UINT actual_cp;
	cp = GetOEMCP();
	if (cp == 65001) {
		// GetOEMCP() may return UTF-8 for the codepage (65001),
		// in which case we need to find the actual system OEM cp.
		if (GetLocaleInfoA(GetUserDefaultUILanguage(), LOCALE_IDEFAULTCODEPAGE | LOCALE_RETURN_NUMBER,
			(char*)&actual_cp, sizeof(actual_cp)))
			cp = actual_cp;
	}
	const DOS_LOCALE locale = SelectDOSLocale(kb, cp, bFreeDOS != FALSE);
	if (locale.keyboard_fallback)
		uprintf("Keyboard id '%s' is not supported - falling back to 'us'", kb);
	uprintf("Will use DOS keyboard '%s' [%s]", locale.keyboard, locale.keyboard_name);
	if (locale.codepage_fallback)
		uprintf("Unable to find an EGA file with codepage %u [%s]", (unsigned)cp, GetDOSCodepageName(cp));
	uprintf("Will use codepage %u [%s]", (unsigned)locale.codepage, GetDOSCodepageName(locale.codepage));
	if (locale.use_menu && !WriteDOSLocaleFile(path, &locale, TRUE))
		return FALSE;
	return WriteDOSLocaleFile(path, &locale, FALSE);
}
