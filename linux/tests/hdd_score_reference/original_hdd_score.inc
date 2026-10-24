/*
 * Rufus: The Reliable USB Formatting Utility
 * SMART HDD vs Flash detection (using ATA over USB, S.M.A.R.T., etc.)
 * Copyright © 2013-2023 Pete Batard <pete@akeo.ie>
 *
 * Based in part on scsiata.cpp from Smartmontools: http://smartmontools.sourceforge.net
 * Copyright © 2006-2012 Douglas Gilbert <dgilbert@interlog.com>
 * Copyright © 2009-2013 Christian Franke <smartmontools-support@lists.sourceforge.net>
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
/* Frozen CP17d4 reference from src/smart.c before extraction. Test only. */
int IsHDD(DWORD DriveIndex, uint16_t vid, uint16_t pid, const char* strid)
{
	int score = 0;
	size_t i, mlen, ilen, score_list_size = 0;
	BOOL wc;
	uint64_t drive_size;
	int8_t score_list[16];
	char str[64] = { 0 };

	// Boost the score if fixed, as these are *generally* HDDs
	if (GetDriveTypeFromIndex(DriveIndex) == DRIVE_FIXED) {
		score_list[score_list_size] = 3;
		score += score_list[score_list_size++];
	}

	// Adjust the score depending on the size
	drive_size = GetDriveSize(DriveIndex);
	if (drive_size > 800 * GB) {
		score_list[score_list_size] = 15;
		score += score_list[score_list_size++];
		if (drive_size > 1800 * GB) {
			score_list[score_list_size] = 15;
			score += score_list[score_list_size++];
		}
	} else if (drive_size < 128 * GB) {
		score_list[score_list_size] = -15;
		score += score_list[score_list_size++];
	}

	// Check the string against well known HDD identifiers
	if (strid != NULL) {
		ilen = strlen(strid);
		for (i = 0; i < ARRAYSIZE(str_score); i++) {
			mlen = strlen(str_score[i].name);
			if (mlen > ilen)
				break;
			wc = (str_score[i].name[mlen-1] == '#');
			if ( (_strnicmp(strid, str_score[i].name, mlen-((wc)?1:0)) == 0)
			  && ((!wc) || ((strid[mlen] >= '0') && (strid[mlen] <= '9'))) ) {
				score_list[score_list_size] = str_score[i].score;
				score += score_list[score_list_size++];
				break;
			}
		}
	}

	// Adjust for oddball devices
	if (strid != NULL) {
		for (i = 0; i < ARRAYSIZE(str_adjust); i++)
			if (StrStrIA(strid, str_adjust[i].name) != NULL) {
				score_list[score_list_size] = str_adjust[i].score;
				score += score_list[score_list_size++];
			}
	}

	// Check against known VIDs
	for (i = 0; i < ARRAYSIZE(vid_score); i++) {
		if (vid == vid_score[i].vid) {
			score_list[score_list_size] = vid_score[i].score;
			score += score_list[score_list_size++];
			break;
		}
	}

	// Check against known VID:PIDs
	for (i = 0; i < ARRAYSIZE(vidpid_score); i++) {
		if ((vid == vidpid_score[i].vid) && (pid == vidpid_score[i].pid)) {
			score_list[score_list_size] = vidpid_score[i].score;
			score += score_list[score_list_size++];
			break;
		}
	}

	// Print a breakdown of the device score if requested
	if (usb_debug) {
		static_strcat(str, "Device score: ");
		for (i = 0; i < score_list_size; i++)
			safe_sprintf(&str[strlen(str)], sizeof(str) - strlen(str), "%+d", score_list[i]);
		uprintf("%s=%+d → Detected as %s", str, score, (score > 0) ? "HDD" : "UFD");
	}

	return score;
}
