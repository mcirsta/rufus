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

#include "dos_locale_policy.h"
#include <assert.h>
#include <string.h>

#define ARRAYSIZE(A) (sizeof(A) / sizeof((A)[0]))

/*
 * Note: if you want a book that can be used as a keyboards and codepages bible, I
 * would recommend the "OS/2 Warp Server for e-business - Keyboards and Codepages".
 * See http://www.borgendale.com/keyboard.pdf
 */

/* WinME DOS keyboard 2 letter codes & supported keyboard ID(s), as retrieved
 * from the Millennium disk image in diskcopy.dll on a Windows 7 system
 *
 *	KEYBOARD.SYS
 *		GR 129*
 *		SP 172
 *		PO 163*
 *		FR 120*, 189*
 *		DK 159*
 *		SG 000*
 *		IT 141*, 142*
 *		UK 166*, 168*
 *		SF 150*
 *		BE 120*
 *		NL 143*
 *		NO 155*
 *		CF 058*
 *		SV 153*
 *		SU 153
 *		LA 171*
 *		BR 274*
 *		PL 214*
 *		CZ 243*
 *		SL 245*
 *		YU 234*
 *		HU 208*
 *		US/XX 103*
 *		JP defines ID:194 but points to SP entry
 *
 *	KEYBRD2.SYS
 *		GR 129*
 *		RU 441
 *		IT 141*, 142*
 *		UK 166*, 168*
 *		NO 155*
 *		CF 058*
 *		SV 153*
 *		SU 153
 *		BR 274*, 275*
 *		BG 442*
 *		PL 214*
 *		CZ 243*
 *		SL 245*
 *		YU 234*
 *		YC 118
 *		HU 208*
 *		RO 333
 *		IS 161*
 *		TR 179*, 440*
 *		GK 319*
 *		US/XX 103*
 *
 *	KEYBRD3.SYS
 *		GR 129*
 *		SP 172*
 *		FR 189*
 *		DK 159*
 *		SG 000*
 *		IT 141*
 *		UK 166*
 *		SF 150*
 *		BE 120*
 *		NL 143*
 *		SV 153*
 *		SU 153
 *		PL 214*
 *		CZ 243*
 *		SL 245*
 *		YU 234*
 *		HU 208*
 *		RU 091*, 092*, 093*, 341*
 *		UR 094*, 095*, 096*
 *		BL 097*, 098*, 099*
 *		US/XX 103*
 *		JP defines ID:194 but points to SP entry
 *
 *	KEYBRD4.SYS
 *		GK 101*, 319*, 220*
 *		PL 214*
 *		ET 425*
 *		HE 400*
 *		AR 401*, 402*, 403*
 *		US/XX 103*
 */

/*
 * The following lists the keyboard code that are supported in each
 * of the WinMe DOS KEYBOARD.SYS, KEYBRD2.SYS, ...
 */
static const char* ms_kb1[] = {
	"be", "br", "cf", "cz", "dk", "fr", "gr", "hu", "it", "la",
	"nl", "no", "pl", "po", "sf", "sg", "sl", "sp", "su", "sv",
	"uk", "us", "yu" };
static const char* ms_kb2[] = {
	"bg", "br", "cf", "cz", "gk", "gr", "hu", "is", "it", "no",
	"pl", "ro", "ru", "sl", "su", "sv", "tr", "uk", "us", "yc",
	"yu" };
static const char* ms_kb3[] = {
	"be", "bl", "cz", "dk", "fr", "gr", "hu", "it", "nl", "pl",
	"ru", "sf", "sg", "sl", "sp", "su", "sv", "uk", "ur", "us",
	"yu" };
static const char* ms_kb4[] = {
	"ar", "et", "gk", "he", "pl", "us" };

/*
 * The following lists the keyboard code that are supported in each
 * of the FreeDOS DOS KEYBOARD.SYS, KEYBRD2.SYS, ...
 */
static const char* fd_kb1[] = {
	"be", "br", "cf", "co", "cz", "dk", "dv", "fr", "gr", "hu",
	"it", "jp", "la", "lh", "nl", "no", "pl", "po", "rh", "sf",
	"sg", "sk", "sp", "su", "sv", "uk", "us", "yu" };
static const char* fd_kb2[] = {
	"bg", "ce", "gk", "is", "ro", "ru", "rx", "tr", "tt", "yc" };
static const char* fd_kb3[] = {
	"az", "bl", "et", "fo", "hy", "il", "ka", "kk", "ky", "lt",
	"lv", "mk", "mn", "mt", "ph", "sq", "tj", "tm", "ur", "uz",
	"vi" };
static const char* fd_kb4[] = {
	"ar", "bn", "bx", "fx", "ix", "kx", "ne", "ng", "px", "sx",
	"ux" };

typedef struct {
	const char* name;
	uint32_t default_cp;
} kb_default;

static kb_default kbdrv_data[] = {
	{ "keyboard.sys", 437 },
	{ "keybrd2.sys", 850 },
	{ "keybrd3.sys", 850 },
	{ "keybrd4.sys", 853 }
};

typedef struct {
	size_t size;
	const char** list;
} kb_list;

static kb_list ms_kb_list[] = {
	{ ARRAYSIZE(ms_kb1), ms_kb1 },
	{ ARRAYSIZE(ms_kb2), ms_kb2 },
	{ ARRAYSIZE(ms_kb3), ms_kb3 },
	{ ARRAYSIZE(ms_kb4), ms_kb4 }
};

static kb_list fd_kb_list[] = {
	{ ARRAYSIZE(fd_kb1), fd_kb1 },
	{ ARRAYSIZE(fd_kb2), fd_kb2 },
	{ ARRAYSIZE(fd_kb3), fd_kb3 },
	{ ARRAYSIZE(fd_kb4), fd_kb4 }
};

static int GetKeyboardDriver(const char** keyboard, bool freedos)
{
	const kb_list* lists = freedos ? fd_kb_list : ms_kb_list;
	if (*keyboard == NULL)
		return -1;
	for (size_t i = 0; i < ARRAYSIZE(kbdrv_data); i++) {
		for (size_t j = 0; j < lists[i].size; j++) {
			if (strcmp(lists[i].list[j], *keyboard) == 0) {
				*keyboard = lists[i].list[j];
				return (int)i;
			}
		}
	}
	return -1;
}

/*
 * We display human readable descriptions of the locale in the menu
 * As real estate might be limited, keep it short
 */
static const char* kb_hr_list[][2] = {
	{"ar", "Arabic"},			// Left enabled, but doesn't seem to work in FreeDOS
	{"bg", "Bulgarian"},
	{"ch", "Chinese"},
	{"cz", "Czech"},
	{"dk", "Danish"},
	{"gr", "German"},
	{"sg", "Swiss-German"},
	{"gk", "Greek"},
	{"us", "US-English"},
	{"uk", "UK-English"},
	{"cf", "CA-French"},
	{"dv", "US-Dvorak"},
	{"lh", "US-Dvorak (LH)"},
	{"rh", "US-Dvorak (RH)"},
	{"sp", "Spanish"},
	{"la", "Latin-American"},
	{"su", "Finnish"},
	{"fr", "French"},
	{"be", "Belgian-French"},
	{"sf", "Swiss-French"},
	{"il", "Hebrew"},
	{"hu", "Hungarian"},
	{"is", "Icelandic"},
	{"it", "Italian"},
	{"jp", "Japanese"},
//	{"ko", "Korean"},			// Unsupported by FreeDOS
	{"nl", "Dutch"},
	{"no", "Norwegian"},
	{"pl", "Polish"},
	{"br", "Brazilian"},
	{"po", "Portuguese"},
	{"ro", "Romanian"},
	{"ru", "Russian"},
	{"yu", "YU-Latin"},
	{"yc", "YU-Cyrillic"},
	{"sl", "Slovak"},
	{"sq", "Albanian"},
	{"sv", "Swedish"},
	{"tr", "Turkish"},
	{"ur", "Ukrainian"},
	{"bl", "Belarusian"},
	{"et", "Estonian"},
	{"lv", "Latvian"},
	{"lt", "Lithuanian"},
	{"tj", "Tajik"},
//	{"fa", "Persian"};			// Unsupported by FreeDOS
	{"vi", "Vietnamese"},
	{"hy", "Armenian"},
	{"az", "Azeri"},
	{"mk", "Macedonian"},
	{"ka", "Georgian"},
	{"fo", "Faeroese"},
	{"mt", "Maltese"},
	{"kk", "Kazakh"},
	{"ky", "Kyrgyz"},
	{"uz", "Uzbek"},
	{"tm", "Turkmen"},
	{"tt", "Tatar"},
};

static const char* kb_to_hr(const char* kb)
{
	size_t i;
	for (i = 0; i < ARRAYSIZE(kb_hr_list); i++) {
		if (strcmp(kb, kb_hr_list[i][0]) == 0) {
			return kb_hr_list[i][1];
		}
	}
	// Some driver-supported IDs have no friendly label. Keep the canonical
	// DOS ID visible instead of asserting or passing NULL to a formatter.
	return kb;
}

typedef struct {
	uint32_t cp;
	const char* name;
} cp_list;

// From FreeDOS CPX pack as well as
// http://msdn.microsoft.com/en-us/library/dd317756.aspx
static cp_list cp_hr_list[] = {
	{ 113, "Lat-Yugoslavian"},
	{ 437, "US-English"},
	{ 667, "Polish"},
	{ 668, "Polish (Alt)"},
	{ 708, "Arabic (708)"},
	{ 709, "Arabic (709)"},
	{ 710, "Arabic (710)"},
	{ 720, "Arabic (DOS)"},
	{ 737, "Greek (DOS)"},
	{ 770, "Baltic"},
	{ 771, "Cyr-Russian (KBL)"},
	{ 772, "Cyr-Russian"},
	{ 773, "Baltic Rim (Old)"},
	{ 774, "Lithuanian"},
	{ 775, "Baltic Rim"},
	{ 777, "Acc-Lithuanian (Old)"},
	{ 778, "Acc-Lithuanian"},
	{ 790, "Mazovian-Polish"},
	{ 808, "Cyr-Russian (Euro)"},
	{ 848, "Cyr-Ukrainian (Euro)"},
	{ 849, "Cyr-Belarusian (Euro)"},
	{ 850, "Western-European"},
	{ 851, "Greek"},
	{ 852, "Central-European"},
	{ 853, "Southern-European"},
	{ 855, "Cyr-South-Slavic"},
	{ 856, "Hebrew II"},
	{ 857, "Turkish"},
	{ 858, "Western-European (Euro)"},
	{ 859, "Western-European (Alt)"},
	{ 860, "Portuguese"},
	{ 861, "Icelandic"},
	{ 862, "Hebrew"},
	{ 863, "Canadian-French"},
	{ 864, "Arabic"},
	{ 865, "Nordic"},
	{ 866, "Cyr-Russian"},
	{ 867, "Czech Kamenicky"},
	{ 869, "Modern Greek"},
	{ 872, "Cyr-South-Slavic (Euro)"},
	{ 874, "Thai"},
	{ 895, "Czech Kamenicky (Alt)"},
	{ 899, "Armenian"},
	{ 932, "Japanese"},
	{ 936, "Chinese (Simplified)"},
	{ 949, "Korean"},
	{ 950, "Chinese (Traditional)"},
	{ 991, "Mazovian-Polish (Zloty)"},
	{ 1116, "Estonian"},
	{ 1117, "Latvian"},
	{ 1118, "Lithuanian"},
	{ 1119, "Cyr-Russian (Alt)"},
	{ 1125, "Cyr-Ukrainian"},
	{ 1131, "Cyr-Belarusian"},
	{ 1250, "Central European"},
	{ 1251, "Cyrillic"},
	{ 1252, "Western European"},
	{ 1253, "Greek"},
	{ 1254, "Turkish"},
	{ 1255, "Hebrew"},
	{ 1256, "Arabic"},
	{ 1257, "Baltic"},
	{ 1258, "Vietnamese"},
	{ 1361, "Korean"},
	{ 3012, "Cyr-Latvian"},
	{ 3021, "Cyr-Bulgarian"},
	{ 3845, "Hungarian"},
	{ 3846, "Turkish"},
	{ 3848, "Brazilian (ABICOMP)"},
	{ 30000, "Saami"},
	{ 30001, "Celtic"},
	{ 30002, "Cyr-Tajik"},
	{ 30003, "Latin American"},
	{ 30004, "Greenlandic"},
	{ 30005, "Nigerian"},
	{ 30006, "Vietnamese"},
	{ 30007, "Latin"},
	{ 30008, "Cyr-Ossetian"},
	{ 30009, "Romani"},
	{ 30010, "Cyr-Moldovan"},
	{ 30011, "Cyr-Chechen"},
	{ 30012, "Cyr-Siberian"},
	{ 30013, "Cyr-Turkic"},
	{ 30014, "Cyr-Finno-Ugric"},
	{ 30015, "Cyr-Khanty"},
	{ 30016, "Cyr-Mansi"},
	{ 30017, "Cyr-Northwestern"},
	{ 30018, "Lat-Tatar"},
	{ 30019, "Lat-Chechen"},
	{ 30020, "Low-Saxon and Frisian"},
	{ 30021, "Oceanian"},
	{ 30022, "First Nations"},
	{ 30023, "Southern African"},
	{ 30024, "North & East African"},
	{ 30025, "Western African"},
	{ 30026, "Central African"},
	{ 30027, "Beninese"},
	{ 30028, "Nigerian (Alt)"},
	{ 30029, "Mexican"},
	{ 30030, "Mexican (Alt)"},
	{ 30031, "Northern-European"},
	{ 30032, "Nordic"},
	{ 30033, "Crimean-Tatar (Hryvnia)"},
	{ 30034, "Cherokee"},
	{ 30039, "Cyr-Ukrainian (Hryvnia)"},
	{ 30040, "Cyr-Russian (Hryvnia)"},
	{ 58152, "Cyr-Kazakh (Euro)"},
	{ 58210, "Cyr-Azeri"},
	{ 58335, "Kashubian"},
	{ 59234, "Cyr-Tatar"},
	{ 59829, "Georgian"},
	{ 60258, "Lat-Azeri"},
	{ 60853, "Georgian (Alt)"},
	{ 62306, "Cyr-Uzbek"},
	{ 65001, "Unicode (UTF-8)" }
};

const char* GetDOSCodepageName(uint32_t cp)
{
	size_t i;
	for (i = 0; i < ARRAYSIZE(cp_hr_list); i++) {
		if (cp_hr_list[i].cp == cp) {
			return cp_hr_list[i].name;
		}
	}
	// Unknown host code pages still have a defined selection fallback.
	return "Unknown";
}

/*
 * From WinME DOS
 *
 *	EGA.CPI:
 *		0x01B5	437 (United States)
 *		0x0352	850 (Latin 1)
 *		0x0354	852 (Latin 2)
 *		0x035C	860 (Portuguese)
 *		0x035F	863 (French Canadian)
 *		0x0361	865 (Nordic)
 *
 *	EGA2.CPI:
 *		0x0352	850 (Latin 1)
 *		0x0354	852 (Latin 2)
 *		0x0359	857 (Turkish)
 *		0x035D	861 (Icelandic)
 *		0x0365	869 (Greek)
 *		0x02E1	737 (Greek II)
 *
 *	EGA3.CPI:
 *		0x01B5	437 (United States)
 *		0x0307	775 (Baltic)
 *		0x0352	850 (Latin 1)
 *		0x0354	852 (Latin 2)
 *		0x0357	855 (Cyrillic I)
 *		0x0362	866 (Cyrillic II)
 */


// Pick the EGA to use according to the DOS target codepage (see above)
static const char* ms_get_ega(uint32_t cp)
{
	switch(cp) {
	case   437: // United States
	case   850: // Latin-1 (Western European)
	case   852: // Latin-2 (Central European)
	case   860: // Portuguese
	case   863: // French Canadian
	case   865: // Nordic
		return "ega.cpi";

//	case   850: // Latin-1 (Western European)
//	case   852: // Latin-2 (Central European)
	case   857: // Turkish
	case   861: // Icelandic
	case   869: // Greek
	case   737: // Greek II
		return "ega2.cpi";

//	case   437: // United States
	case   775: // Baltic
//	case   850: // Latin-1 (Western European)
//	case   852: // Latin-2 (Central European)
	case   855: // Cyrillic I
	case   866: // Cyrillic II
		return "ega3.cpi";

	default:
		return NULL;
	}
}

// Pick the EGA to use according to the DOS target codepage (from CPIDOS' Codepage.txt)
static const char* fd_get_ega(uint32_t cp)
{
	switch(cp) {
	case   437: // United States
	case   850: // Latin-1 (Western European)
	case   852: // Latin-2 (Central European)
	case   853: // Latin-3 (Southern European)
	case   857: // Latin-5
	case   858: // Latin-1 with Euro
		return "ega.cpx";
	case   775: // Latin-7 (Baltic Rim)
	case   859: // Latin-9
	case  1116: // Estonian
	case  1117: // Latvian
	case  1118: // Lithuanian
	case  1119: // Cyrillic Russian and Lithuanian (*)
		return "ega2.cpx";
	case   771: // Cyrillic Russian and Lithuanian (KBL)
	case   772: // Cyrillic Russian and Lithuanian
	case   808: // Cyrillic Russian with Euro
	case   855: // Cyrillic South Slavic
	case   866: // Cyrillic Russian
	case   872: // Cyrillic South Slavic with Euro
		return "ega3.cpx";
	case   848: // Cyrillic Ukrainian with Euro
	case   849: // Cyrillic Belarusian with Euro
	case  1125: // Cyrillic Ukrainian
	case  1131: // Cyrillic Belarusian
	case  3012: // Cyrillic Russian and Latvian ("RusLat")
	case 30010: // Cyrillic Gagauz and Moldovan
		return "ega4.cpx";
	case   113: // Yugoslavian Latin
	case   737: // Greek-2
	case   851: // Greek (old codepage)
//	case   852: // Latin-2
//	case   858: // Multilingual Latin-1 with Euro
	case   869: // Greek
		return "ega5.cpx";
	case   899: // Armenian
	case 30008: // Cyrillic Abkhaz and Ossetian
	case 58210: // Cyrillic Russian and Azeri
	case 59829: // Georgian
	case 60258: // Cyrillic Russian and Latin Azeri
	case 60853: // Georgian with capital letters
		return "ega6.cpx";
	case 30011: // Cyrillic Russian Southern District
	case 30013: // Cyrillic Volga District: // Turkic languages
	case 30014: // Cyrillic Volga District: // Finno-ugric languages
	case 30017: // Cyrillic Northwestern District
	case 30018: // Cyrillic Russian and Latin Tatar
	case 30019: // Cyrillic Russian and Latin Chechen
		return "ega7.cpx";
	case   770: // Baltic
	case   773: // Latin-7 (old standard)
	case   774: // Lithuanian
//	case   775: // Latin-7
	case   777: // Accented Lithuanian (old)
	case   778: // Accented Lithuanian
		return "ega8.cpx";
//	case   858: // Latin-1 with Euro
	case   860: // Portuguese
	case   861: // Icelandic
	case   863: // Canadian French
	case   865: // Nordic
	case   867: // Czech Kamenicky
		return "ega9.cpx";
	case   667: // Polish
	case   668: // Polish (polish letters on cp852 codepoints)
	case   790: // Polish Mazovia
//	case   852: // Latin-2
	case   991: // Polish Mazovia with Zloty sign
	case  3845: // Hungarian
		return "ega10.cpx";
//	case   858: // Latin-1 with Euro
	case 30000: // Saami
	case 30001: // Celtic
	case 30004: // Greenlandic
	case 30007: // Latin
	case 30009: // Romani
		return "ega11.cpx";
//	case   852: // Latin-2
//	case   858: // Latin-1 with Euro
	case 30003: // Latin American
	case 30029: // Mexican
	case 30030: // Mexican II
	case 58335: // Kashubian
		return "ega12.cpx";
//	case   852: // Latin-2
	case   895: // Czech Kamenicky
	case 30002: // Cyrillic Tajik
	case 58152: // Cyrillic Kazakh with Euro
	case 59234: // Cyrillic Tatar
	case 62306: // Cyrillic Uzbek
		return "ega13.cpx";
	case 30006: // Vietnamese
	case 30012: // Cyrillic Russian Siberian and Far Eastern Districts
	case 30015: // Cyrillic Khanty
	case 30016: // Cyrillic Mansi
	case 30020: // Low saxon and frisian
	case 30021: // Oceania
		return "ega14.cpx";
	case 30023: // Southern Africa
	case 30024: // Northern and Eastern Africa
	case 30025: // Western Africa
	case 30026: // Central Africa
	case 30027: // Beninese
	case 30028: // Nigerian II
		return "ega15.cpx";
//	case   858: // Latin-1 with Euro
	case  3021: // Cyrillic MIK Bulgarian
	case 30005: // Nigerian
	case 30022: // Canadian First Nations
	case 30031: // Latin-4 (Northern European)
	case 30032: // Latin-6
		return "ega16.cpx";
	case   862: // Hebrew
	case   864: // Arabic
	case 30034: // Cherokee
	case 30033: // Crimean Tatar with Hryvnia
	case 30039: // Cyrillic Ukrainian with Hryvnia
	case 30040: // Cyrillic Russian with Hryvnia
		return "ega17.cpx";
	case   856: // Hebrew II
	case  3846: // Turkish
	case  3848: // Brazilian ABICOMP
		return "ega18.cpx";
	default:
		return NULL;
	}
}

// Transliteration of the codepage (to add currency symbol, etc - FreeDOS only)
static uint32_t fd_upgrade_cp(uint32_t cp)
{
	switch(cp) {
	case   850: // Latin-1 (Western European)
		return 858; // Latin-1 with Euro
	default:
		return cp;
	}
}

const char* GetDOSKeyboard(size_t index, bool freedos)
{
    const kb_list* lists = freedos ? fd_kb_list : ms_kb_list;
    const size_t count = freedos ? ARRAYSIZE(fd_kb_list) : ARRAYSIZE(ms_kb_list);
    for (size_t driver = 0; driver < count; driver++) {
        if (index < lists[driver].size)
            return lists[driver].list[index];
        index -= lists[driver].size;
    }
    return NULL;
}

uint32_t GetDOSCodepage(size_t index, bool freedos)
{
    for (size_t i = 0; i < ARRAYSIZE(cp_hr_list); i++) {
        const uint32_t cp = cp_hr_list[i].cp;
        const bool usable = freedos ? fd_get_ega(cp) != NULL && fd_upgrade_cp(cp) == cp : ms_get_ega(cp) != NULL;
        if (!usable)
            continue;
        if (index == 0)
            return cp;
        index--;
    }
    return 0;
}

const char* GetFreeDOSKeyboard(size_t index)
{
    return GetDOSKeyboard(index, true);
}

uint32_t GetFreeDOSCodepage(size_t index)
{
    return GetDOSCodepage(index, true);
}

DOS_LOCALE SelectDOSLocale(const char* keyboard, uint32_t codepage, bool freedos)
{
	DOS_LOCALE locale = { 0 };
	int driver = GetKeyboardDriver(&keyboard, freedos);
	locale.keyboard_fallback = driver < 0;
	if (locale.keyboard_fallback) {
		keyboard = "us";
		driver = GetKeyboardDriver(&keyboard, freedos);
	}
	assert(driver >= 0);
	locale.ega_driver = freedos ? fd_get_ega(codepage) : ms_get_ega(codepage);
	locale.codepage_fallback = locale.ega_driver == NULL;
	if (locale.codepage_fallback) {
		codepage = kbdrv_data[driver].default_cp;
		locale.ega_driver = freedos ? "ega.cpx" : "ega.cpi";
	} else if (freedos) {
		codepage = fd_upgrade_cp(codepage);
	}
	locale.keyboard = keyboard;
	locale.keyboard_name = kb_to_hr(keyboard);
	locale.keyboard_driver = kbdrv_data[driver].name;
	locale.codepage = codepage;
	locale.freedos = freedos;
	locale.use_menu = codepage != 437 || strcmp(keyboard, "us") != 0;
	return locale;
}

bool WriteDOSConfig(FILE* file, const DOS_LOCALE* locale)
{
	if (file == NULL || locale == NULL)
		return false;
	if (!locale->use_menu)
		return true;
	const uint32_t cp = locale->codepage;
	if (locale->freedos) {
		fprintf(file, "!MENUCOLOR=7,0\r\nMENU\r\nMENU   FreeDOS Language Selection Menu\r\n");
		fprintf(file, "MENU   \xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD"
			"\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\r\nMENU\r\n");
	} else {
		fprintf(file, "[MENU]\r\n");
	}
	fprintf(file, "MENUDEFAULT=1,5\r\n");
	// Menu item max: 70 characters
	fprintf(file, "%s1%c Use %s keyboard with %s codepage [%d]\r\n",
		locale->freedos?"MENU ":"MENUITEM=", locale->freedos?')':',', locale->keyboard_name, GetDOSCodepageName(cp), (int)cp);
	fprintf(file, "%s2%c Use %s keyboard with %s codepage [%d]\r\n",
		locale->freedos?"MENU ":"MENUITEM=", locale->freedos?')':',', kb_to_hr("us"), GetDOSCodepageName(437), 437);
	fprintf(file, "%s", locale->freedos?"MENU\r\n12?\r\n":"[1]\r\ndevice=\\locale\\display.sys con=(ega,,1)\r\n[2]\r\n");
	return !ferror(file);
}

bool WriteDOSAutoexec(FILE* file, const DOS_LOCALE* locale)
{
	if (file == NULL || locale == NULL)
		return false;
	if (!locale->use_menu) {
		fprintf(file, "@echo off\r\n");
		fprintf(file, "set PATH=.;\\;\\LOCALE\r\n");
		fprintf(file, "echo Using %s keyboard with %s codepage [%d]\r\n", kb_to_hr("us"), GetDOSCodepageName(437), 437);
		return !ferror(file);
	}
	const uint32_t cp = locale->codepage;
	const char* kb = locale->keyboard;
	const char* egadrv = locale->ega_driver;
	fprintf(file, "@echo off\r\n");
	fprintf(file, "set PATH=.;\\;\\LOCALE\r\n");
	if (locale->freedos)
		fprintf(file, "display con=(ega,,1)\r\n");
	fprintf(file, "GOTO %%CONFIG%%\r\n");
	fprintf(file, ":1\r\n");
	fprintf(file, "mode con codepage prepare=((%d) \\locale\\%s) > NUL\r\n", (int)cp, egadrv);
	fprintf(file, "mode con codepage select=%d > NUL\r\n", (int)cp);
	fprintf(file, "keyb %s,,\\locale\\%s\r\n", kb, locale->keyboard_driver);
	fprintf(file, ":2\r\n");
	return !ferror(file);
}
