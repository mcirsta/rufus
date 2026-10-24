/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Original Rufus Secure Boot fallback policy; shared with the native adapter.
 * Copyright © 2016-2026 Pete Batard <pete@akeo.ie> */
#pragma once

/*
 * Contains the SHA-1 thumbprints of the issuer certificate of the official
 * Secure Boot signing authority (i.e. Microsoft).
 */
static const char db_sb_active_txt[] =
	// 'Microsoft Windows Production PCA 2011'
	"580a6f4cc4e4b669b9ebdc1b2b3e087b80d0678d\n"
	// 'Microsoft Corporation UEFI CA 2011'
	"46def63b5ce61cf8ba0de2e6639c1019d0ed14f3\n"
	// 'Windows UEFI CA 2023'
	"45a0fa32604773c82433c3b7d59e7466b3ac0c67\n"
	// 'Microsoft UEFI CA 2023'
	"b5eeb4a6706048073f0ed296e7f580a790b59eaa";

/*
 * Contains the SHA-1 thumbprints of certificates that are being revoked by DBX.
 * This only includes the 'Microsoft Windows Production PCA 2011' for now.
 */
static const char db_sb_revoked_txt[] =
	"580a6f4cc4e4b669b9ebdc1b2b3e087b80d0678d";

/*
 * Extended SBATLevel.txt that merges Linux SBAT with Microsoft's SVN
 * See https://github.com/pbatard/rufus/issues/2244#issuecomment-2243661539
 * Use as fallback when https://rufus.ie/sbat_level.txt cannot be accessed.
 */
static const char db_sbat_level_txt[] =
	"sbat,1,2025051000\n"
	"shim,4\n"
	"grub,5\n"
	"grub.proxmox,2\n"
	"BOOTMGRSECURITYVERSIONNUMBER,0x70000";
