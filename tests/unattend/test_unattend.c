/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "unattend.h"
#include <stdlib.h>
#include <string.h>

enum { XML_CAPACITY = 64 * 1024 };
static unsigned checks;
#define CHECK(condition) do { checks++; if (!(condition)) { \
	fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); exit(1); } } while (0)

void uprintf(const char* format, ...)
{
	(void)format;
}

static void Generate(const RUFUS_UNATTEND_OPTIONS* options, char* xml, uint32_t offsets[2])
{
	FILE* file = tmpfile();
	CHECK(file != NULL);
	CHECK(WriteUnattendXml(file, options, offsets));
	CHECK(fflush(file) == 0);
	CHECK(fseek(file, 0, SEEK_SET) == 0);
	const size_t size = fread(xml, 1, XML_CAPACITY - 1, file);
	CHECK(!ferror(file));
	CHECK(feof(file));
	xml[size] = '\0';
	CHECK(fclose(file) == 0);
}

static void CheckArchitectureAndSections(void)
{
	const char* architectures[] = { "x86", "amd64", "arm", "arm64" };
	char xml[XML_CAPACITY], expected[80];
	uint32_t offsets[2];
	RUFUS_UNATTEND_OPTIONS options = { 0 };
	options.flags = UNATTEND_NO_DATA_COLLECTION;
	for (int arch = ARCH_X86_32; arch <= ARCH_ARM_64; arch++) {
		options.arch = arch;
		Generate(&options, xml, offsets);
		snprintf(expected, sizeof(expected), "processorArchitecture=\"%s\"", architectures[arch - ARCH_X86_32]);
		CHECK(strstr(xml, expected) != NULL);
		CHECK(strstr(xml, "<ProtectYourPC>3</ProtectYourPC>") != NULL);
		CHECK(strstr(xml, "<settings pass=\"windowsPE\">") == NULL);
		CHECK(offsets[0] == 0 && offsets[1] == 0);
	}
	options.flags = UNATTEND_SECUREBOOT_TPM_MINRAM | UNATTEND_NO_ONLINE_ACCOUNT;
	Generate(&options, xml, offsets);
	CHECK(strstr(xml, "<Key />") != NULL);
	CHECK(strstr(xml, "BypassTPMCheck") != NULL);
	CHECK(strstr(xml, "BypassSecureBootCheck") != NULL);
	CHECK(strstr(xml, "BypassRAMCheck") != NULL);
	CHECK(strstr(xml, "BypassNRO") != NULL);
	CHECK(offsets[0] < offsets[1]);
	CHECK(offsets[1] < strlen(xml));
	CHECK(strncmp(xml + offsets[0], "      <RunSynchronous>\n", 23) == 0);
	CHECK(strncmp(xml + offsets[1], "    </component>\n", 17) == 0);
	/* Windows removes exactly this range after applying the registry entries. */
	memmove(xml + offsets[0], xml + offsets[1], strlen(xml + offsets[1]) + 1);
	CHECK(strstr(xml, "BypassTPMCheck") == NULL);
	CHECK(strstr(xml, "BypassNRO") != NULL);

	options.flags = UNATTEND_DISABLE_BITLOCKER;
	Generate(&options, xml, offsets);
	CHECK(strstr(xml, "<PreventDeviceEncryption>true</PreventDeviceEncryption>") != NULL);
	CHECK(strstr(xml, "<TCGSecurityActivationDisabled>1</TCGSecurityActivationDisabled>") != NULL);
	CHECK(strstr(xml, "<settings pass=\"windowsPE\">") == NULL);
	CHECK(strstr(xml, "FirstLogonCommands") == NULL);

	options.flags = UNATTEND_OFFLINE_INTERNAL_DRIVES | UNATTEND_FORCE_S_MODE;
	Generate(&options, xml, offsets);
	CHECK(strstr(xml, "<settings pass=\"offlineServicing\">") != NULL);
	CHECK(strstr(xml, "<SanPolicy>4</SanPolicy>") != NULL);
	CHECK(strstr(xml, "<SkuPolicyRequired>1</SkuPolicyRequired>") != NULL);
}

static void CheckUserAndHostInputs(void)
{
	char xml[XML_CAPACITY], username[MAX_USERNAME_LENGTH] = "Rufus.Test&User";
	uint32_t offsets[2];
	RUFUS_UNATTEND_OPTIONS options = { 0 };
	options.arch = ARCH_X86_64;
	options.flags = UNATTEND_SET_USER | UNATTEND_DUPLICATE_LOCALE;
	options.username = username;
	options.timezone = "GTB Standard Time";
	options.input_locale = "00000409";
	options.system_locale = "ro-RO";
	options.user_locale = "fr-FR";
	options.ui_language = "de-DE";
	options.ui_language_fallback = "en-US";
	Generate(&options, xml, offsets);
	CHECK(strcmp(username, "Rufus_Test_User") == 0);
	CHECK(strstr(xml, "<Name>Rufus_Test_User</Name>") != NULL);
	CHECK(strstr(xml, "net user \"Rufus_Test_User\" /logonpasswordchg:yes") != NULL);
	CHECK(strstr(xml, "net accounts /maxpwage:unlimited") != NULL);
	CHECK(strstr(xml, "<TimeZone>GTB Standard Time</TimeZone>") != NULL);
	CHECK(strstr(xml, "<InputLocale>00000409</InputLocale>") != NULL);
	CHECK(strstr(xml, "<SystemLocale>ro-RO</SystemLocale>") != NULL);
	CHECK(strstr(xml, "<UserLocale>fr-FR</UserLocale>") != NULL);
	CHECK(strstr(xml, "<UILanguage>de-DE</UILanguage>") != NULL);
	CHECK(strstr(xml, "<UILanguageFallback>en-US</UILanguageFallback>") != NULL);
	options.timezone = NULL;
	Generate(&options, xml, offsets);
	CHECK(strstr(xml, "<TimeZone>") == NULL);
	options.flags = UNATTEND_SET_USER;
	strcpy(username, "aDmInIsTrAtOr");
	Generate(&options, xml, offsets);
	CHECK(strstr(xml, "<LocalAccount ") == NULL);
	CHECK(strcmp(username, "aDmInIsTrAtOr") == 0);
	username[0] = '\0';
	Generate(&options, xml, offsets);
	CHECK(strstr(xml, "<LocalAccount ") == NULL);
	memset(username, 'a', sizeof(username) - 1);
	username[sizeof(username) - 1] = '\0';
	Generate(&options, xml, offsets);
	CHECK(strstr(xml, username) != NULL);
}

static void CheckCombinedOptions(void)
{
	char xml[XML_CAPACITY], username[] = "TestUser";
	uint32_t offsets[2];
	RUFUS_UNATTEND_OPTIONS options = { 0 };
	options.arch = ARCH_X86_64;
	options.flags = UNATTEND_SILENT_INSTALL | UNATTEND_SET_USER | UNATTEND_DISABLE_BITLOCKER |
		UNATTEND_QOL_ENHANCEMENTS | UNATTEND_APPLY_SKUSIPOLICY;
	options.username = username;
	options.edition_index = 6;
	options.winpe_ui_language = "en-GB";
	Generate(&options, xml, offsets);
	CHECK(strstr(xml, "<Value>6</Value>") != NULL);
	CHECK(strstr(xml, "<UILanguage>en-GB</UILanguage>") != NULL);
	CHECK(strstr(xml, "<WillWipeDisk>true</WillWipeDisk>") != NULL);
	CHECK(strstr(xml, "<DisableEncryptedDiskProvisioning>true</DisableEncryptedDiskProvisioning>") != NULL);
	CHECK(strstr(xml, "<HideOnlineAccountScreens>true</HideOnlineAccountScreens>") != NULL);
	CHECK(strstr(xml, "<HideWirelessSetupInOOBE>true</HideWirelessSetupInOOBE>") != NULL);
	CHECK(strstr(xml, "SkuSiPolicy.p7b S:\\EFI\\Microsoft\\Boot &amp;&amp;") != NULL);
	const char* commands = strstr(xml, "<FirstLogonCommands>");
	CHECK(commands != NULL);
	CHECK(strstr(commands + 1, "<FirstLogonCommands>") == NULL);
	CHECK(strstr(commands, "<Order>20</Order>") != NULL);
}

static void CheckStandaloneFirstLogonOptions(void)
{
	char xml[XML_CAPACITY];
	uint32_t offsets[2];
	const int flags[] = { UNATTEND_APPLY_SKUSIPOLICY, UNATTEND_QOL_ENHANCEMENTS,
		UNATTEND_APPLY_SKUSIPOLICY | UNATTEND_QOL_ENHANCEMENTS };
	RUFUS_UNATTEND_OPTIONS options = { 0 };
	options.arch = ARCH_X86_64;
	for (size_t i = 0; i < sizeof(flags) / sizeof(flags[0]); i++) {
		options.flags = flags[i];
		Generate(&options, xml, offsets);
		const char* commands = strstr(xml, "<FirstLogonCommands>");
		CHECK(commands != NULL);
		CHECK(strstr(commands + 1, "<FirstLogonCommands>") == NULL);
		const bool policy = (flags[i] & UNATTEND_APPLY_SKUSIPOLICY) != 0;
		const bool qol = (flags[i] & UNATTEND_QOL_ENHANCEMENTS) != 0;
		CHECK((strstr(commands, "SkuSiPolicy.p7b S:\\EFI\\Microsoft\\Boot &amp;&amp;") != NULL) == policy);
		CHECK((strstr(commands, "HiberbootEnabled") != NULL) == qol);
		CHECK(strstr(xml, "ProtectYourPC") == NULL);
		CHECK(strstr(xml, "UserAccounts") == NULL);
		CHECK(strstr(xml, "International-Core") == NULL);
		CHECK(strstr(xml, "windowsPE") == NULL);
	}
}

static void CheckRefusals(void)
{
	FILE* file = tmpfile();
	uint32_t offsets[2] = { 99, 99 };
	RUFUS_UNATTEND_OPTIONS options = { 0 };
	CHECK(file != NULL);
	CHECK(!WriteUnattendXml(NULL, &options, offsets));
	CHECK(!WriteUnattendXml(file, NULL, offsets));
	CHECK(!WriteUnattendXml(file, &options, NULL));
	options.flags = UNATTEND_NO_DATA_COLLECTION;
	CHECK(!WriteUnattendXml(file, &options, offsets));
	options.arch = ARCH_IA_64;
	CHECK(!WriteUnattendXml(file, &options, offsets));
	options.arch = ARCH_X86_64;
	options.flags = 0;
	CHECK(!WriteUnattendXml(file, &options, offsets));
	options.flags = UNATTEND_SET_USER;
	CHECK(!WriteUnattendXml(file, &options, offsets));
	char oversized[MAX_USERNAME_LENGTH + 1];
	memset(oversized, 'a', MAX_USERNAME_LENGTH);
	oversized[MAX_USERNAME_LENGTH] = '\0';
	options.username = oversized;
	CHECK(!WriteUnattendXml(file, &options, offsets));
	options.flags = UNATTEND_DUPLICATE_LOCALE;
	CHECK(!WriteUnattendXml(file, &options, offsets));
	options.flags = UNATTEND_SILENT_INSTALL;
	CHECK(!WriteUnattendXml(file, &options, offsets));
	CHECK(ftell(file) == 0);
	CHECK(fclose(file) == 0);
	FILE* readonly = fopen(__FILE__, "r");
	CHECK(readonly != NULL);
	options.flags = UNATTEND_NO_DATA_COLLECTION;
	CHECK(!WriteUnattendXml(readonly, &options, offsets));
	CHECK(ferror(readonly));
	CHECK(fclose(readonly) == 0);
}

static void CheckChoiceEligibility(void)
{
	RUFUS_IMG_REPORT report = { 0 };
	CHECK(GetWindowsUnattendFlags(NULL, false) == 0);
	CHECK(GetWindowsUnattendFlags(&report, true) == 0);
	report.has_bootmgr_efi = true;
	report.win_version.major = 10;
	report.win_version.build = 19045;
	const int basic = UNATTEND_SET_USER | UNATTEND_DUPLICATE_LOCALE | UNATTEND_NO_DATA_COLLECTION;
	CHECK(GetWindowsUnattendFlags(&report, false) == basic);
	CHECK(GetWindowsUnattendFlags(&report, true) == (basic | UNATTEND_FORCE_S_MODE));
	report.win_version.major = 11;
	report.win_version.build = 22000;
	const int windows11 = basic | UNATTEND_SECUREBOOT_TPM_MINRAM | UNATTEND_SILENT_INSTALL |
		UNATTEND_DISABLE_BITLOCKER | UNATTEND_QOL_ENHANCEMENTS;
	CHECK(GetWindowsUnattendFlags(&report, false) == windows11);
	report.win_version.build = 22500;
	CHECK(GetWindowsUnattendFlags(&report, false) == (windows11 | UNATTEND_NO_ONLINE_ACCOUNT));
	report.win_version.build = 26200;
	CHECK(GetWindowsUnattendFlags(&report, false) == (windows11 | UNATTEND_NO_ONLINE_ACCOUNT |
		UNATTEND_USE_MS2023_BOOTLOADERS | UNATTEND_APPLY_SKUSIPOLICY));
	report.has_panther_unattend = true;
	CHECK(GetWindowsUnattendFlags(&report, true) == 0);
}

int main(void)
{
	CheckArchitectureAndSections();
	CheckUserAndHostInputs();
	CheckCombinedOptions();
	CheckStandaloneFirstLogonOptions();
	CheckRefusals();
	CheckChoiceEligibility();
	printf("%u checks passed\n", checks);
	return 0;
}
