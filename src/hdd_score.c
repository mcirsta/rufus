/*
 * Rufus: The Reliable USB Formatting Utility
 * HDD vs Flash scoring, extracted from smart.c
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
#include "hdd_score.h"
#include "hdd_vs_ufd.h"
/* Angle brackets deliberately select the build's platform string header. */
#include <hdd_score_strings.h>
#include <string.h>

#ifndef ARRAYSIZE
#define ARRAYSIZE(array) (sizeof(array) / sizeof((array)[0]))
#endif

HDD_SCORE GetHddScore(bool fixed, uint64_t drive_size, uint16_t vid, uint16_t pid, const char* strid)
{
	HDD_SCORE result = { 0 };
	int score = 0;
	size_t i, mlen, ilen, score_list_size = 0;
	bool wc;
	int8_t* score_list = result.parts;
	const uint64_t gigabyte = UINT64_C(1073741824);

	// Boost the score if fixed, as these are *generally* HDDs
	if (fixed) {
		score_list[score_list_size] = 3;
		score += score_list[score_list_size++];
	}

	// Adjust the score depending on the size
	if (drive_size > 800 * gigabyte) {
		score_list[score_list_size] = 15;
		score += score_list[score_list_size++];
		if (drive_size > 1800 * gigabyte) {
			score_list[score_list_size] = 15;
			score += score_list[score_list_size++];
		}
	} else if (drive_size < 128 * gigabyte) {
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
			if ( (HddPrefixCompare(strid, str_score[i].name, mlen-((wc)?1:0)) == 0)
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
			if (HddFindString(strid, str_adjust[i].name) != NULL) {
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

	result.total = score;
	result.count = score_list_size;
	return result;
}
