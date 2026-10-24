/*
 * Rufus: The Reliable USB Formatting Utility
 * Shared Windows installation answer-file generation, extracted from wue.c
 * Copyright © 2022-2026 Pete Batard <pete@akeo.ie>
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

#include "unattend.h"
#include "strarray.h"
#include <stdlib.h>
#include <string.h>

extern void uprintf(const char* format, ...);

#define ARRAYSIZE(array) (sizeof(array) / sizeof((array)[0]))
enum { UNATTEND_COMMAND_CAPACITY = 260 };

const char* const bypass_name[UNATTEND_BYPASS_COUNT] = { "BypassTPMCheck", "BypassSecureBootCheck", "BypassRAMCheck" };

bool IsUnattendUserNameReserved(const char* username)
{
	static const char* unallowed_account_names[] = {
		// From https://learn.microsoft.com/en-us/archive/technet-wiki/13813.localized-names-for-administrator-account-in-windows
		"Administrator", "Järjestelmänvalvoja", "Administrateur", "Rendszergazda", "Administrador", "Администратор", "Administratör",
		"Guest", "DefaultAccount", "WDAGUtilityAccount", "HelpAssistant", "KRBTGT", "Local", "NONE", "SYSTEM"
	};
	if (username == NULL)
		return false;
	for (size_t i = 0; i < ARRAYSIZE(unallowed_account_names); i++) {
		if (_stricmp(username, unallowed_account_names[i]) == 0)
			return true;
	}
	return false;
}

int GetWindowsUnattendFlags(const RUFUS_IMG_REPORT* report, bool expert_mode)
{
	if (report == NULL)
		return 0;
	if (!IS_WINDOWS_1X((*report)) || report->has_panther_unattend)
		return 0;
	int flags = UNATTEND_SET_USER | UNATTEND_DUPLICATE_LOCALE | UNATTEND_NO_DATA_COLLECTION;
	if (report->win_version.build >= 22500)
		flags |= UNATTEND_NO_ONLINE_ACCOUNT;
	if (IS_WINDOWS_11((*report))) {
		flags |= UNATTEND_SECUREBOOT_TPM_MINRAM | UNATTEND_SILENT_INSTALL |
			UNATTEND_DISABLE_BITLOCKER | UNATTEND_QOL_ENHANCEMENTS;
		if (report->win_version.build >= 26200)
			flags |= UNATTEND_USE_MS2023_BOOTLOADERS | UNATTEND_APPLY_SKUSIPOLICY;
	}
	if (expert_mode)
		flags |= UNATTEND_FORCE_S_MODE;
	return flags;
}

int GetWindowsToGoUnattendFlags(const RUFUS_IMG_REPORT* report, bool expert_mode)
{
	if (report == NULL || !IS_WINDOWS_1X((*report))) return 0;
	int flags = UNATTEND_OFFLINE_INTERNAL_DRIVES | UNATTEND_SET_USER |
		UNATTEND_DUPLICATE_LOCALE | UNATTEND_NO_DATA_COLLECTION;
	if (report->win_version.build >= 22500) flags |= UNATTEND_NO_ONLINE_ACCOUNT;
	if (IS_WINDOWS_11((*report))) {
		flags |= UNATTEND_QOL_ENHANCEMENTS;
		if (report->win_version.build >= 26200) flags |= UNATTEND_USE_MS2023_BOOTLOADERS;
	}
	if (expert_mode) flags |= UNATTEND_FORCE_S_MODE;
	return flags;
}

static bool UnattendInputsValid(const RUFUS_UNATTEND_OPTIONS* options)
{
	if (options == NULL)
		return false;
	const bool valid_arch = options->arch >= ARCH_X86_32 && options->arch <= ARCH_ARM_64;
	if (!valid_arch || options->flags == 0)
		return false;
	if (options->flags & UNATTEND_SET_USER) {
		if (options->username == NULL)
			return false;
		if (strlen(options->username) >= MAX_USERNAME_LENGTH)
			return false;
	}
	if (options->flags & UNATTEND_DUPLICATE_LOCALE) {
		const bool have_locales = options->system_locale != NULL && options->user_locale != NULL;
		const bool have_ui = options->ui_language != NULL && options->ui_language_fallback != NULL;
		if (!have_locales || !have_ui || options->input_locale == NULL)
			return false;
	}
	if (options->flags & UNATTEND_SILENT_INSTALL) {
		if (options->winpe_ui_language == NULL)
			return false;
	}
	return true;
}

static bool UnattendOffset(FILE* fd, uint32_t* offset)
{
	const long position = ftell(fd);
	if (position < 0)
		return false;
	if ((uint64_t)position > UINT32_MAX)
		return false;
	*offset = (uint32_t)position;
	return true;
}

bool WriteUnattendXml(FILE* fd, const RUFUS_UNATTEND_OPTIONS* options,
	uint32_t removable_section[2])
{
	static const char* xml_arch_names[5] = { "x86", "amd64", "arm", "arm64" };
	char tmp[UNATTEND_COMMAND_CAPACITY];
	int i, order;
	bool success = false;
	StrArray commands = { 0 };
	if (fd == NULL || removable_section == NULL)
		return false;
	removable_section[0] = removable_section[1] = 0;
	if (!UnattendInputsValid(options))
		return false;
	const int flags = options->flags;
	const int arch = options->arch - ARCH_X86_32;
	uprintf("Selected Windows User Experience options:");
	fprintf(fd, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n");
	fprintf(fd, "<unattend xmlns=\"urn:schemas-microsoft-com:unattend\">\n");

	StrArrayCreate(&commands, 16);
	if (commands.String == NULL)
		goto out;
	if (flags & UNATTEND_WINPE_SETUP_MASK) {
		fprintf(fd, "  <settings pass=\"windowsPE\">\n");
		fprintf(fd, "    <component name=\"Microsoft-Windows-Setup\" processorArchitecture=\"%s\" language=\"neutral\" "
			"xmlns:wcm=\"http://schemas.microsoft.com/WMIConfig/2002/State\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
			"publicKeyToken=\"31bf3856ad364e35\" versionScope=\"nonSxS\">\n", xml_arch_names[arch]);
		// WinPE will complain if we don't provide a product key. *Any* product key. This is soooo idiotic...
		fprintf(fd, "      <UserData>\n");
		fprintf(fd, "        <AcceptEula>true</AcceptEula>\n");
		fprintf(fd, "        <ProductKey>\n");
		fprintf(fd, "          <Key />\n");
		fprintf(fd, "        </ProductKey>\n");
		fprintf(fd, "      </UserData>\n");
		if (flags & UNATTEND_SILENT_INSTALL) {
			uprintf("• ⚠Silent Install⚠");
			// Automatically partition the disk and set the installation target
			fprintf(fd, "      <DiskConfiguration>\n");
			fprintf(fd, "        <WillShowUI>OnError</WillShowUI>\n");
			if (flags & UNATTEND_DISABLE_BITLOCKER)
				fprintf(fd, "        <DisableEncryptedDiskProvisioning>true</DisableEncryptedDiskProvisioning>\n");
			// The following ensures that we display the disk selection screen if only the boot media is
			// available (i.e. if the system does not see any target for installation) or (in most cases)
			// if the user happens to have more than one disk connected besides the instal USB. In that
			// case the label assignment below fails and the UI is displayed allowing the user to provide
			// drivers or pick their target disk explicitly.
			// This also prevents the install media from being scratched.
			// With a special mention to Claude AI, that explicitly said this could not be accomplished...
			// Note that this may result in the disk partition screen being produced on systems that have
			// card readers, but we'd rather inconvenience a few people, to prevent potential data loss,
			// than the opposite. Also note that you do *NOT* want to try to use drive letter mapping for
			// the detection here, as if your drive isn't of type FIXED, the file copy steps bails out at
			// 75%. See https://github.com/pbatard/rufus/issues/2960 for more details.
			fprintf(fd, "        <Disk wcm:action=\"modify\">\n");
			fprintf(fd, "          <DiskID>1</DiskID> \n");
			fprintf(fd, "          <ModifyPartitions>\n");
			fprintf(fd, "            <ModifyPartition wcm:action=\"modify\">\n");
			fprintf(fd, "              <Order>1</Order>\n");
			fprintf(fd, "              <PartitionID>2</PartitionID>\n");
			fprintf(fd, "              <Label>RUFUS_BOOT</Label>\n");
			fprintf(fd, "            </ModifyPartition>\n");
			fprintf(fd, "          </ModifyPartitions>\n");
			fprintf(fd, "        </Disk>\n");
			fprintf(fd, "        <Disk wcm:action=\"add\">\n");
			fprintf(fd, "          <DiskID>0</DiskID> \n");
			fprintf(fd, "          <WillWipeDisk>true</WillWipeDisk> \n");
			fprintf(fd, "          <CreatePartitions>\n");
			// NB: Don't bother creating a recovery partition. Windows setup will do it for us.
			fprintf(fd, "            <CreatePartition wcm:action=\"add\">\n");
			fprintf(fd, "              <Order>1</Order>\n");
			fprintf(fd, "              <Type>EFI</Type>\n");
			fprintf(fd, "              <Size>260</Size>\n");
			fprintf(fd, "            </CreatePartition>\n");
			fprintf(fd, "            <CreatePartition wcm:action=\"add\">\n");
			fprintf(fd, "              <Order>2</Order>\n");
			fprintf(fd, "              <Type>MSR</Type>\n");
			fprintf(fd, "              <Size>16</Size>\n");
			fprintf(fd, "            </CreatePartition>\n");
			fprintf(fd, "            <CreatePartition wcm:action=\"add\">\n");
			fprintf(fd, "              <Order>3</Order>\n");
			fprintf(fd, "              <Type>Primary</Type>\n");
			fprintf(fd, "              <Extend>true</Extend>\n");
			fprintf(fd, "            </CreatePartition>\n");
			fprintf(fd, "          </CreatePartitions>\n");
			fprintf(fd, "          <ModifyPartitions>\n");
			fprintf(fd, "            <ModifyPartition wcm:action=\"add\">\n");
			fprintf(fd, "              <Order>1</Order>\n");
			fprintf(fd, "              <PartitionID>1</PartitionID>\n");
			fprintf(fd, "              <Label>EFI</Label>\n");
			fprintf(fd, "              <Format>FAT32</Format>\n");
			fprintf(fd, "            </ModifyPartition>\n");
			fprintf(fd, "            <ModifyPartition wcm:action=\"add\">\n");
			fprintf(fd, "              <Order>2</Order>\n");
			fprintf(fd, "              <PartitionID>3</PartitionID>\n");
			fprintf(fd, "              <Label>Windows</Label>\n");
			fprintf(fd, "              <Letter>C</Letter>\n");
			fprintf(fd, "              <Format>NTFS</Format>\n");
			fprintf(fd, "            </ModifyPartition>\n");
			fprintf(fd, "          </ModifyPartitions>\n");
			fprintf(fd, "        </Disk>\n");
			fprintf(fd, "      </DiskConfiguration>\n");
			fprintf(fd, "      <ImageInstall>\n");
			fprintf(fd, "        <OSImage>\n");
			fprintf(fd, "          <WillShowUI>OnError</WillShowUI>\n");
			fprintf(fd, "          <InstallFrom>\n");
			fprintf(fd, "            <MetaData wcm:action=\"add\">\n");
			fprintf(fd, "              <Key>/IMAGE/INDEX</Key>\n");
			fprintf(fd, "              <Value>%d</Value>\n", options->edition_index);
			fprintf(fd, "            </MetaData>\n");
			fprintf(fd, "          </InstallFrom>\n");
			fprintf(fd, "          <InstallTo>\n");
			fprintf(fd, "            <DiskID>0</DiskID>\n");
			fprintf(fd, "            <PartitionID>3</PartitionID>\n");
			fprintf(fd, "          </InstallTo>\n");
			fprintf(fd, "        </OSImage>\n");
			fprintf(fd, "      </ImageInstall>\n");
		}
		if (flags & UNATTEND_SECUREBOOT_TPM_MINRAM) {
			// This part produces the unbecoming display of a command prompt window during initial setup as well
			// as alters the layout and options of the initial Windows installer screens, which may scare users.
			// So, in format.c, we'll try to insert the registry keys directly and drop this section. However,
			// because Microsoft prevents Store apps from editing an offline registry, we do need this fallback.
			// NB: We could probably avoid this by running powershell with -WindowStyle "Hidden" but hey...
			uprintf("• Bypass SB/TPM/RAM");
			order = 1;
			if (!UnattendOffset(fd, &removable_section[0]))
				goto out;
			fprintf(fd, "      <RunSynchronous>\n");
			for (i = 0; i < (int)ARRAYSIZE(bypass_name); i++) {
				fprintf(fd, "        <RunSynchronousCommand wcm:action=\"add\">\n");
				fprintf(fd, "          <Order>%d</Order>\n", order++);
				fprintf(fd, "          <Path>reg add HKLM\\SYSTEM\\Setup\\LabConfig /v %s /t REG_DWORD /d 1 /f</Path>\n", bypass_name[i]);
				fprintf(fd, "        </RunSynchronousCommand>\n");
			}
			fprintf(fd, "      </RunSynchronous>\n");
			if (!UnattendOffset(fd, &removable_section[1]))
				goto out;
		}
		fprintf(fd, "    </component>\n");
		if (flags & UNATTEND_SILENT_INSTALL) {
			fprintf(fd, "    <component name=\"Microsoft-Windows-International-Core-WinPE\" processorArchitecture=\"%s\" language=\"neutral\" "
				"xmlns:wcm=\"http://schemas.microsoft.com/WMIConfig/2002/State\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
				"publicKeyToken=\"31bf3856ad364e35\" versionScope=\"nonSxS\">\n", xml_arch_names[arch]);
			// Set the language from the <LANGUAGE> tag of the boot.wim index
			fprintf(fd, "      <UILanguage>%s</UILanguage>\n", options->winpe_ui_language);
			fprintf(fd, "    </component>\n");
		}
		fprintf(fd, "  </settings>\n");
	}

	if (flags & UNATTEND_SPECIALIZE_DEPLOYMENT_MASK) {
		fprintf(fd, "  <settings pass=\"specialize\">\n");
		fprintf(fd, "    <component name=\"Microsoft-Windows-Deployment\" processorArchitecture=\"%s\" language=\"neutral\" "
			"xmlns:wcm=\"http://schemas.microsoft.com/WMIConfig/2002/State\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
			"publicKeyToken=\"31bf3856ad364e35\" versionScope=\"nonSxS\">\n", xml_arch_names[arch]);
		// This part was picked from https://github.com/AveYo/MediaCreationTool.bat/blob/main/bypass11/AutoUnattend.xml
		// NB: This is INCOMPATIBLE with S-Mode below
		if (flags & UNATTEND_NO_ONLINE_ACCOUNT) {
			if (StrArrayAdd(&commands, "reg add \"HKLM\\Software\\Microsoft\\Windows\\CurrentVersion\\OOBE\" /v BypassNRO /t REG_DWORD /d 1 /f", true) < 0)
				goto out;
			uprintf("• Bypass online account requirement");
		}
		if (flags & UNATTEND_QOL_ENHANCEMENTS) {
			uprintf("• QoL: Disable OneDrive and Outlook by default");
			// Remove OneDrive
			if (StrArrayAdd(&commands, "reg add \"HKLM\\Software\\Policies\\Microsoft\\Windows\\OneDrive\" /v DisableFileSyncNGSC /t REG_DWORD /d 1 /f", true) < 0)
				goto out;
			if (StrArrayAdd(&commands, "PowerShell -NonInteractive -WindowStyle Hidden -Command "
				"\"Remove-Item -Path $env:SystemRoot\\System32\\OneDriveSetup.exe -Force -Confirm:$false; "
				"Remove-Item -Path $env:SystemRoot\\SysWOW64\\OneDriveSetup.exe -Force -Confirm:$false;\"", true) < 0)
				goto out;
			// Remove Outlook. How the frig is forcing Outlook on users legal when MS got pinned for bundling IE with Windows?
			if (StrArrayAdd(&commands, "PowerShell -NonInteractive -WindowStyle Hidden -Command "
				"\"Get-AppxProvisionedPackage -Online | Where-Object {$_.PackageName -like '*Outlook*'} |"
				" Remove-AppxProvisionedPackage -Online\"", true) < 0)
				goto out;
			if (StrArrayAdd(&commands, "PowerShell -NonInteractive -WindowStyle Hidden -Command "
				"\"Get-AppxPackage -AllUsers *Outlook* | Remove-AppxPackage -AllUsers\"", true) < 0)
				goto out;
			// Same for Teams. Also: "HEY, MICROSOFT, SCREW YOU!!!!"
			if (StrArrayAdd(&commands, "PowerShell -NonInteractive -WindowStyle Hidden -Command "
				"\"Get-AppxProvisionedPackage -Online | Where-Object {$_.PackageName -like '*Teams*'} |"
				" Remove-AppxProvisionedPackage -Online\"", true) < 0)
				goto out;
			if (StrArrayAdd(&commands, "PowerShell -NonInteractive -WindowStyle Hidden -Command "
				"\"Get-AppxPackage -AllUsers *Teams* | Remove-AppxPackage -AllUsers\"", true) < 0)
				goto out;
		}
		// Now that we have all the commands to run, create the RunSynchronous section.
		for (order = 1; order <= (int)commands.Index; order++) {
			if (order == 1)
				fprintf(fd, "      <RunSynchronous>\n");
			fprintf(fd, "        <RunSynchronousCommand wcm:action=\"add\">\n");
			fprintf(fd, "          <Order>%d</Order>\n", order);
			fprintf(fd, "          <Path>%s</Path>\n", commands.String[order - 1]);
			fprintf(fd, "        </RunSynchronousCommand>\n");
			if (order == (int)commands.Index)
				fprintf(fd, "      </RunSynchronous>\n");
		}
		fprintf(fd, "    </component>\n");
		fprintf(fd, "  </settings>\n");
		StrArrayClear(&commands);
	}

	if (flags & UNATTEND_OOBE_MASK) {
		fprintf(fd, "  <settings pass=\"oobeSystem\">\n");
		if (flags & UNATTEND_OOBE_SHELL_SETUP_MASK) {
			fprintf(fd, "    <component name=\"Microsoft-Windows-Shell-Setup\" processorArchitecture=\"%s\" language=\"neutral\" "
				"xmlns:wcm=\"http://schemas.microsoft.com/WMIConfig/2002/State\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
				"publicKeyToken=\"31bf3856ad364e35\" versionScope=\"nonSxS\">\n", xml_arch_names[arch]);
			// We should always have UNATTEND_NO_DATA_COLLECTION if UNATTEND_SILENT_INSTALL is set but whatever...
			if (flags & (UNATTEND_NO_DATA_COLLECTION | UNATTEND_SILENT_INSTALL)) {
				uprintf("• Disable data collection");
				fprintf(fd, "      <OOBE>\n");
				fprintf(fd, "        <HideEULAPage>true</HideEULAPage>\n");
				// https://docs.microsoft.com/en-us/windows-hardware/customize/desktop/unattend/microsoft-windows-shell-setup-oobe-protectyourpc
				// It is really super disingenuous of Microsoft to call this option "ProtectYourPC", when it's really only about
				// data collection. But of course, if it was called "AllowDataCollection", everyone would turn it off...
				fprintf(fd, "        <ProtectYourPC>3</ProtectYourPC>\n");
				// Without these, the "Let's connect you to a network" can appear in silent installs when Ethernet is disconnected.
				if (flags & UNATTEND_SILENT_INSTALL) {
					fprintf(fd, "        <HideOnlineAccountScreens>true</HideOnlineAccountScreens>\n");
					fprintf(fd, "        <HideWirelessSetupInOOBE>true</HideWirelessSetupInOOBE>\n");
				}
				fprintf(fd, "      </OOBE>\n");
			}
			if (flags & UNATTEND_DUPLICATE_LOCALE) {
				if (options->timezone != NULL)
					fprintf(fd, "      <TimeZone>%s</TimeZone>\n", options->timezone);
			}
			if (flags & UNATTEND_SET_USER) {
				if (IsUnattendUserNameReserved(options->username)) {
					uprintf("WARNING: '%s' is not allowed as local account name - Option ignored", options->username);
				} else if (options->username[0] != 0) {
					char* org_username = _strdup(options->username);
					if (org_username == NULL)
						goto out;
					for (char* p = strpbrk(options->username, USERNAME_INVALID_CHARS); p != NULL;
						p = strpbrk(p + 1, USERNAME_INVALID_CHARS))
						*p = '_';
					uprintf("• Use '%s' for local account name", options->username);
					if (strcmp(org_username, options->username) != 0)
						uprintf("WARNING: Local account name contained unallowed characters and has been sanitized");
					free(org_username);
					// If we create a local account in unattend.xml, then we can get Windows 11
					// 22H2 to skip MSA even if the network is connected during installation.
					fprintf(fd, "      <UserAccounts>\n");
					fprintf(fd, "        <LocalAccounts>\n");
					fprintf(fd, "          <LocalAccount wcm:action=\"add\">\n");
					fprintf(fd, "            <Name>%s</Name>\n", options->username);
					fprintf(fd, "            <DisplayName>%s</DisplayName>\n", options->username);
					fprintf(fd, "            <Group>Administrators;Power Users</Group>\n");
					// Sets an empty password for the account (which, in Microsoft's convoluted ways,
					// needs to be initialized to the Base64 encoded UTF-16 string "Password").
					// The use of an empty password has both the advantage of not having to ask users
					// to type in a password in Rufus (which they might be weary of) as well as allowing
					// automated logon during setup.
					fprintf(fd, "            <Password>\n");
					fprintf(fd, "              <Value>UABhAHMAcwB3AG8AcgBkAA==</Value>\n");
					fprintf(fd, "              <PlainText>false</PlainText>\n");
					fprintf(fd, "            </Password>\n");
					fprintf(fd, "          </LocalAccount>\n");
					fprintf(fd, "        </LocalAccounts>\n");
					fprintf(fd, "      </UserAccounts>\n");
					// Since we set a blank password, we'll ask the user to change it at next logon.
					// NB: In case you wanna try, please be aware that Microsoft doesn't let you have multiple
					// <FirstLogonCommands> sections in unattend.xml. Don't ask me how I know... :(
					snprintf(tmp, sizeof(tmp), "net user \"%s\" /logonpasswordchg:yes", options->username);
					if (StrArrayAdd(&commands, tmp, true) < 0)
						goto out;
					// The `net user` command above might reset the password expiration to 90 days...
					// To alleviate that, blanket set passwords on the target machine to never expire.
					if (StrArrayAdd(&commands, "net accounts /maxpwage:unlimited", true) < 0)
						goto out;
				}
			}
			// Apply SkuSiPolicy.p7b, if the user requested it and we're not creating a Windows To Go drive.
			// See https://support.microsoft.com/kb/5042562. We do it post install, on first logon, because
			// we'd have to patch tons of files otherwise. And we *ALWAYS* use the installed system's
			// SkuSiPolicy.p7b instead of the host system's, even if the latter might be more recent, on
			// account that the bootloaders from the installed system might be trailing behind, and wills
			// produce the 0xc0000428 signature validation error on (re)boot if the system hasn't gone
			// through a full Windows Update cycle.
			if (flags & UNATTEND_APPLY_SKUSIPOLICY) {
				uprintf("• Apply SkuSiPolicy.p7b");
				if (StrArrayAdd(&commands, "cmd /c mountvol S: /S &amp;&amp; "
					"copy %WINDIR%\\system32\\SecureBootUpdates\\SkuSiPolicy.p7b S:\\EFI\\Microsoft\\Boot &amp;&amp; "
					"mountvol S: /D", true) < 0)
					goto out;
			}
			if (flags & UNATTEND_QOL_ENHANCEMENTS) {
				uprintf("• QoL: Disable Fast Startup, Copilot, Recommendations, News and Teams by default");
				// Disable Fast Startup
				if (StrArrayAdd(&commands, "reg add \"HKLM\\System\\CurrentControlSet\\Control\\Session Manager\\Power\" "
					"/v HiberbootEnabled /t REG_DWORD /d 0 /f", true) < 0)
					goto out;
				// Disable Copilot and set search as an icon rather than the default real-estate gobbler
				if (StrArrayAdd(&commands, "reg add \"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced\" "
					"/v ShowCopilotButton /t REG_DWORD /d 0 /f", true) < 0)
					goto out;
				if (StrArrayAdd(&commands, "reg add \"HKLM\\Software\\Policies\\Microsoft\\Windows\\WindowsCopilot\" "
					"/v TurnOffWindowsCopilot /t REG_DWORD /d 1 /f", true) < 0)
					goto out;
				if (StrArrayAdd(&commands, "reg add \"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Search\" "
					"/v SearchboxTaskbarMode /t REG_DWORD /d 1 /f", true) < 0)
					goto out;
				if (StrArrayAdd(&commands, "reg add \"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Search\" "
					"/v SearchboxTaskbarModeCache /t REG_DWORD /d 1 /f", true) < 0)
					goto out;
				// Disable Windows Phone and similarly pushed unwanted crap
				if (StrArrayAdd(&commands, "reg add \"HKLM\\Software\\Policies\\Microsoft\\Windows\\CloudContent\" "
					"/v DisableWindowsConsumerFeatures /t REG_DWORD /d 1 /f", true) < 0)
					goto out;
				// Disable ads in menu
				if (StrArrayAdd(&commands, "reg add \"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\ContentDeliveryManager\" "
					"/v SystemPaneSuggestionsEnabled /t REG_DWORD /d 0 /f", true) < 0)
					goto out;
				if (StrArrayAdd(&commands, "reg add \"HKCU\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Search\" "
					"/v BingSearchEnabled /t REG_DWORD /d 0 /f", true) < 0)
					goto out;
				// Disable auto installation of manufacturer's crappy apps when plugging new hardware
				if (StrArrayAdd(&commands, "reg add \"HKLM\\Software\\Policies\\Microsoft\\Windows\\Device Metadata\" "
					"/v PreventDeviceMetadataFromNetwork /t REG_DWORD /d 1 /f", true) < 0)
					goto out;
				// Disable News
				if (StrArrayAdd(&commands, "reg add \"HKLM\\Software\\Policies\\Microsoft\\Dsh\" "
					"/v AllowNewsAndInterests /t REG_DWORD /d 0 /f", true) < 0)
					goto out;
				if (StrArrayAdd(&commands, "reg add \"HKLM\\Software\\Policies\\Microsoft\\Windows\\Windows Feeds\" "
					"/v EnableFeeds /t REG_DWORD /d 0 /f", true) < 0)
					goto out;
				// Disable Teams
				if (StrArrayAdd(&commands, "reg add \"HKLM\\Software\\Microsoft\\Windows\\CurrentVersion\\Communications\" "
					"/v ConfigureChatAutoInstall /t REG_DWORD /d 0 /f", true) < 0)
					goto out;
				// Prevent frigging Outlook from being forced into the user's taskbar
				if (StrArrayAdd(&commands, "reg add \"HKLM\\Software\\Policies\\Microsoft\\Windows\\CloudContent\" "
					"/v DisableCloudOptimizedContent /t REG_DWORD /d 1 /f", true) < 0)
					goto out;
				// Skip Edge's first run dialog
				if (StrArrayAdd(&commands, "reg add \"HKLM\\Software\\Policies\\Microsoft\\Edge\" "
					"/v HideFirstRunExperience /t REG_DWORD /d 1 /f", true) < 0)
					goto out;
				uprintf("• QoL: More pins for the Start Menu and enable useful shortcuts");
				// More pins by default on the Start Menu
				// https://learn.microsoft.com/en-us/windows/apps/develop/settings/settings-windows-11
				if (StrArrayAdd(&commands, "reg add \"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced\" "
					"/v Start_Layout /t REG_DWORD /d 1 /f", true) < 0)
					goto out;
				// Add Documents, Downloads, Network, Personal Folder, File Explorer and Settings as start menu shortcuts
				// I wish there was a more transparent way of doing it, but Microsoft made it obscure. Oh, and for some
				// reason, feeding the full hex string through 'reg add' doesn't create the key when ran through unattend
				// (but doesn't produce an error and works post logon). PowerShell it is then...
				if (StrArrayAdd(&commands, "PowerShell -NonInteractive -WindowStyle Hidden -Command "
					"\"Set-ItemProperty -Path 'Registry::HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Start' "
					"-Name 'VisiblePlaces' -Value $([convert]::FromBase64String('ztU0LVr6Q0WC8iLm6vd3PC+zZ+PeiVVDv85h83sYqTe8JIo"
					"UDNaJQqCAbtm7okiCRIF1/g0IrkKL2jTtl7ZjlEqwvXRK+WhPi9ZDmAcdqLyGCHNSqlFDQp97J3ZYRlnU')) -Type 'Binary'\"", true) < 0)
					goto out;
				// Restore the classic right click context menu
				uprintf("• QoL: Restore classic context menu");
				if (StrArrayAdd(&commands, "reg add \"HKCU\\Software\\Classes\\CLSID\\{86ca1aa0-34aa-4e8b-a509-50c905bae2a2}\\InprocServer32\" "
					"/ve /t REG_SZ /d \"\" /f", true) < 0)
					goto out;
			}
			// Now that we have all the commands to run, create the FirstLogonCommands section.
			for (order = 1; order <= (int)commands.Index; order++) {
				if (order == 1)
					fprintf(fd, "      <FirstLogonCommands>\n");
				fprintf(fd, "        <SynchronousCommand wcm:action=\"add\">\n");
				fprintf(fd, "          <Order>%d</Order>\n", order);
				fprintf(fd, "          <CommandLine>%s</CommandLine>\n", commands.String[order - 1]);
				fprintf(fd, "        </SynchronousCommand>\n");
				if (order == (int)commands.Index)
					fprintf(fd, "      </FirstLogonCommands>\n");
			}
			fprintf(fd, "    </component>\n");
			StrArrayClear(&commands);
		}
		if (flags & UNATTEND_OOBE_INTERNATIONAL_MASK) {
			uprintf("• Use the same regional options as this user's");
			fprintf(fd, "    <component name=\"Microsoft-Windows-International-Core\" processorArchitecture=\"%s\" language=\"neutral\" "
				"xmlns:wcm=\"http://schemas.microsoft.com/WMIConfig/2002/State\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
				"publicKeyToken=\"31bf3856ad364e35\" versionScope=\"nonSxS\">\n", xml_arch_names[arch]);
			// What a frigging mess retreiving and trying to match the various locales
			// Microsoft has made. And, *NO*, the new User Language Settings have not
			// improved things in the slightest. They made it much worse for developers!
			fprintf(fd, "      <InputLocale>%s</InputLocale>\n",
				options->input_locale);
			fprintf(fd, "      <SystemLocale>%s</SystemLocale>\n", options->system_locale);
			fprintf(fd, "      <UserLocale>%s</UserLocale>\n", options->user_locale);
			fprintf(fd, "      <UILanguage>%s</UILanguage>\n", options->ui_language);
			fprintf(fd, "      <UILanguageFallback>%s</UILanguageFallback>\n",
				// NB: Officially, this is a REG_MULTI_SZ string
				options->ui_language_fallback);
			fprintf(fd, "    </component>\n");
		}
		if (flags & UNATTEND_DISABLE_BITLOCKER) {
			uprintf("• Disable BitLocker");
			fprintf(fd, "    <component name=\"Microsoft-Windows-SecureStartup-FilterDriver\" processorArchitecture=\"%s\" language=\"neutral\" "
				"xmlns:wcm=\"http://schemas.microsoft.com/WMIConfig/2002/State\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
				"publicKeyToken=\"31bf3856ad364e35\" versionScope=\"nonSxS\">\n", xml_arch_names[arch]);
			fprintf(fd, "      <PreventDeviceEncryption>true</PreventDeviceEncryption>\n");
			fprintf(fd, "    </component>\n");
			fprintf(fd, "    <component name=\"Microsoft-Windows-EnhancedStorage-Adm\" processorArchitecture=\"%s\" language=\"neutral\" "
				"xmlns:wcm=\"http://schemas.microsoft.com/WMIConfig/2002/State\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
				"publicKeyToken=\"31bf3856ad364e35\" versionScope=\"nonSxS\">\n", xml_arch_names[arch]);
			fprintf(fd, "      <TCGSecurityActivationDisabled>1</TCGSecurityActivationDisabled>\n");
			fprintf(fd, "    </component>\n");
		}
		fprintf(fd, "  </settings>\n");
	}

	if (flags & UNATTEND_OFFLINE_SERVICING_MASK) {
		fprintf(fd, "  <settings pass=\"offlineServicing\">\n");
		if (flags & UNATTEND_OFFLINE_INTERNAL_DRIVES) {
			uprintf("• Set internal drives offline");
			fprintf(fd, "    <component name=\"Microsoft-Windows-PartitionManager\" processorArchitecture=\"%s\" language=\"neutral\" "
				"xmlns:wcm=\"http://schemas.microsoft.com/WMIConfig/2002/State\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
				"publicKeyToken=\"31bf3856ad364e35\" versionScope=\"nonSxS\">\n", xml_arch_names[arch]);
			fprintf(fd, "      <SanPolicy>4</SanPolicy>\n");
			fprintf(fd, "    </component>\n");
		}
		if (flags & UNATTEND_FORCE_S_MODE) {
			uprintf("• Enforce S Mode");
			fprintf(fd, "    <component name=\"Microsoft-Windows-CodeIntegrity\" processorArchitecture=\"%s\" language=\"neutral\" "
				"xmlns:wcm=\"http://schemas.microsoft.com/WMIConfig/2002/State\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
				"publicKeyToken=\"31bf3856ad364e35\" versionScope=\"nonSxS\">\n", xml_arch_names[arch]);
			fprintf(fd, "      <SkuPolicyRequired>1</SkuPolicyRequired>\n");
			fprintf(fd, "    </component>\n");
		}
		fprintf(fd, "  </settings>\n");
	}

	if (flags & UNATTEND_USE_MS2023_BOOTLOADERS)
		uprintf("• Use 'Windows CA 2023' signed bootloaders");

	fprintf(fd, "</unattend>\n");
	success = !ferror(fd);
out:
	StrArrayDestroy(&commands);
	return success;
}
