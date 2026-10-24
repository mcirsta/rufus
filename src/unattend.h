/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "image_report.h"

// Windows User Experience (unattend.xml) flags and masks
#define UNATTEND_SECUREBOOT_TPM_MINRAM      0x00001
#define UNATTEND_NO_ONLINE_ACCOUNT          0x00004
#define UNATTEND_NO_DATA_COLLECTION         0x00008
#define UNATTEND_OFFLINE_INTERNAL_DRIVES    0x00010
#define UNATTEND_DUPLICATE_LOCALE           0x00020
#define UNATTEND_SET_USER                   0x00040
#define UNATTEND_DISABLE_BITLOCKER          0x00080
#define UNATTEND_FORCE_S_MODE               0x00100
#define UNATTEND_USE_MS2023_BOOTLOADERS     0x00200
#define UNATTEND_APPLY_SKUSIPOLICY          0x00400
#define UNATTEND_SILENT_INSTALL             0x00800
#define UNATTEND_QOL_ENHANCEMENTS           0x01000
#define UNATTEND_FULL_MASK                  0x01FFF
#define UNATTEND_DEFAULT_MASK               0x016FF		// Mask of values that are persisted
#define UNATTEND_WINDOWS_TO_GO              0x10000		// Special flag for Windows To Go

#define UNATTEND_WINPE_SETUP_MASK           (UNATTEND_SECUREBOOT_TPM_MINRAM | UNATTEND_SILENT_INSTALL)
#define UNATTEND_SPECIALIZE_DEPLOYMENT_MASK (UNATTEND_NO_ONLINE_ACCOUNT | UNATTEND_QOL_ENHANCEMENTS)
#define UNATTEND_OOBE_SHELL_SETUP_MASK      (UNATTEND_NO_DATA_COLLECTION | UNATTEND_SET_USER | UNATTEND_DUPLICATE_LOCALE | UNATTEND_SILENT_INSTALL | \
                                             UNATTEND_APPLY_SKUSIPOLICY | UNATTEND_QOL_ENHANCEMENTS)
#define UNATTEND_OOBE_INTERNATIONAL_MASK    (UNATTEND_DUPLICATE_LOCALE)
#define UNATTEND_OOBE_MASK                  (UNATTEND_OOBE_SHELL_SETUP_MASK | UNATTEND_OOBE_INTERNATIONAL_MASK | UNATTEND_DISABLE_BITLOCKER | \
                                             UNATTEND_USE_MS2023_BOOTLOADERS | UNATTEND_APPLY_SKUSIPOLICY | UNATTEND_QOL_ENHANCEMENTS)
#define UNATTEND_OFFLINE_SERVICING_MASK     (UNATTEND_OFFLINE_INTERNAL_DRIVES | UNATTEND_FORCE_S_MODE)
#define UNATTEND_DEFAULT_SELECTION_MASK     (UNATTEND_SECUREBOOT_TPM_MINRAM | UNATTEND_NO_ONLINE_ACCOUNT | UNATTEND_OFFLINE_INTERNAL_DRIVES)

#define MAX_USERNAME_LENGTH         128			// Maximum size we'll accept for a WUE specified username
// Per https://learn.microsoft.com/en-us/windows-hardware/customize/desktop/unattend/microsoft-windows-shell-setup-useraccounts-localaccounts-localaccount-name
// and https://learn.microsoft.com/en-us/previous-versions/cc722458(v=technet.10)#user-name-policies
// Add '.' to the list because some folks also reported an issue with local accounts that have dots.
// Also add '&', even as it could be escaped, as it's just not worth the trouble...
#define USERNAME_INVALID_CHARS      "/\\[]:;|=.,+*?<>%@&\""

// The shared key names also serve Windows's offline registry customization.
enum { UNATTEND_BYPASS_COUNT = 3 };

/* UTF-8 values borrowed for one call. username is a writable buffer of at least
 * strlen(username) + 1 bytes; Rufus's original sanitization modifies it in place.
 * Locale fields are required only with DUPLICATE_LOCALE; timezone may be NULL
 * when retrieval failed. winpe_ui_language is required only for SILENT_INSTALL.
 * The Windows adapter supplies host locale/timezone values. Linux keeps the
 * "copy this user's regional settings" option visible but disabled. */
typedef struct {
	int arch;
	int flags;
	int edition_index;
	char* username;
	const char* timezone;
	const char* input_locale;
	const char* system_locale;
	const char* user_locale;
	const char* ui_language;
	const char* ui_language_fallback;
	const char* winpe_ui_language;
} RUFUS_UNATTEND_OPTIONS;

#ifdef __cplusplus
extern "C" {
#endif

extern const char* const bypass_name[UNATTEND_BYPASS_COUNT];

/* Original installation-media choice eligibility from rufus.c. Windows To Go
 * has a separate dialog. Platform support is checked separately by callers. */
int GetWindowsUnattendFlags(const RUFUS_IMG_REPORT* report, bool expert_mode);
/* Original To Go dialog eligibility from rufus.c. Native support is separate. */
int GetWindowsToGoUnattendFlags(const RUFUS_IMG_REPORT* report, bool expert_mode);

/* Original reserved-name check, shared with native input validation. */
bool IsUnattendUserNameReserved(const char* username);

/* Write the original Rufus answer file to a borrowed stream. A fresh stream is
 * expected. The caller chooses text/binary mode, creates the file and checks
 * flush/close. removable_section receives the stream offsets of the optional
 * WinPE bypass commands, for the existing Windows offline-registry path.
 * On failure the caller must discard the partial file. No host APIs are called. */
bool WriteUnattendXml(FILE* fd, const RUFUS_UNATTEND_OPTIONS* options,
	uint32_t removable_section[2]);

#ifdef __cplusplus
}
#endif
