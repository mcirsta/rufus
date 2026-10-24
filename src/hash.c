/*
 * Rufus: The Reliable USB Formatting Utility
 * Message-Digest algorithms (md5sum, sha1sum, sha256sum, sha512sum)
 * Copyright © 1998-2001 Free Software Foundation, Inc.
 * Copyright © 2004-2019 Tom St Denis
 * Copyright © 2004 g10 Code GmbH
 * Copyright © 2002-2015 Wei Dai & Igor Pavlov
 * Copyright © 2015-2025 Pete Batard <pete@akeo.ie>
 * Copyright © 2022 Jeffrey Walton <noloader@gmail.com>
 * Copyright © 2016 Alexander Graf
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

/*
 * SHA-1 code taken from GnuPG, as per copyrights above.
 *
 * SHA-256 taken from 7-zip's Sha256.c, itself based on Crypto++ - Public Domain
 *
 * SHA-512 modified from LibTomCrypt - Public Domain
 *
 * PE256 modified from u-boot's efi_image_loader.c - GPLv2.0+
 *
 * CPU accelerated SHA code taken from SHA-Intrinsics - Public Domain
 *
 * MD5 code from various public domain sources sharing the following
 * copyright declaration:
 *
 * This code implements the MD5 message-digest algorithm.
 * The algorithm is due to Ron Rivest.  This code was
 * written by Colin Plumb in 1993, no copyright is claimed.
 * This code is in the public domain; do with it what you wish.
 *
 * Equivalent code is available from RSA Data Security, Inc.
 * This code has been tested against that, and is equivalent,
 * except that you don't need to include two pages of legalese
 * with every copy.
 *
 * To compute the message digest of a chunk of bytes, declare an
 * MD5Context structure, pass it to MD5Init, call MD5Update as
 * needed on buffers full of bytes, and then call MD5Final, which
 * will fill a supplied 16-byte array with the digest.
 */

 /* Memory leaks detection - define _CRTDBG_MAP_ALLOC as preprocessor macro */
#ifdef _CRTDBG_MAP_ALLOC
#include <stdlib.h>
#include <crtdbg.h>
#endif

#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>
#include <intrin.h>
#include <windows.h>
#include <windowsx.h>

#include "db.h"
#include "efi.h"
#include "rufus.h"
#include "winio.h"
#include "missing.h"
#include "darkmode.h"
#include "resource.h"
#include "msapi_utf8.h"
#include "localization.h"

#define BUFFER_SIZE         (64*KB)
#define WAIT_TIME           5000

/* Number of buffers we work with */
#define NUM_BUFFERS         3   // 2 + 1 as a mere double buffered async I/O
                                // would modify the buffer being processed.

/* Globals */
char hash_str[HASH_MAX][150];
HANDLE data_ready[HASH_MAX] = { 0 }, thread_ready[HASH_MAX] = { 0 };
DWORD read_size[NUM_BUFFERS];
BOOL enable_extra_hashes = FALSE, validate_md5sum = FALSE;
BOOL cpu_has_sha1_accel = FALSE, cpu_has_sha256_accel = FALSE;
uint8_t ALIGNED(64) buffer[NUM_BUFFERS][BUFFER_SIZE];
uint8_t* pe256ssp = NULL;
uint32_t proc_bufnum, hash_count[HASH_MAX] = { MD5_HASHSIZE, SHA1_HASHSIZE, SHA256_HASHSIZE, SHA512_HASHSIZE };
uint32_t pe256ssp_size = 0;
uint64_t md5sum_totalbytes;
StrArray modified_files = { 0 };

extern int default_thread_priority;
extern const char* efi_archname[ARCH_MAX];
extern char *sbat_level_txt, *sb_active_txt, *sb_revoked_txt;
extern BOOL expert_mode, usb_debug;

#include "hash_core.inc"

/* Compute an individual hash without threading or buffering, for a single file */
BOOL HashFile(const unsigned type, const char* path, uint8_t* hash)
{
	BOOL r = FALSE;
	HASH_CONTEXT hash_ctx = { {0} };
	HANDLE h = INVALID_HANDLE_VALUE;
	DWORD rs = 0;
	uint8_t buf[4096];

	if ((type >= HASH_MAX) || (path == NULL) || (hash == NULL))
		goto out;

	h = CreateFileU(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
	if (h == INVALID_HANDLE_VALUE) {
		uprintf("Could not open file: %s", WindowsErrorString());
		ErrorStatus = RUFUS_ERROR(ERROR_OPEN_FAILED);
		goto out;
	}

	hash_init[type](&hash_ctx);
	while(1) {
		CHECK_FOR_USER_CANCEL;
		if (!ReadFile(h, buf, sizeof(buf), &rs, NULL)) {
			ErrorStatus = RUFUS_ERROR(ERROR_READ_FAULT);
			uprintf("  Read error: %s", WindowsErrorString());
			goto out;
		}
		if (rs == 0)
			break;
		hash_write[type](&hash_ctx, buf, (size_t)rs);
	}
	hash_final[type](&hash_ctx);

	memcpy(hash, hash_ctx.buf, hash_count[type]);
	r = TRUE;

out:
	safe_closehandle(h);
	return r;
}

/* A part of an image, used for hashing */
struct image_region {
	const uint8_t*      data;
	uint32_t            size;
};

/**
 * struct efi_image_regions - A list of memory regions
 *
 * @max:    Maximum number of regions
 * @num:    Number of regions
 * @reg:    Array of regions
 */
struct efi_image_regions {
	int                 max;
	int                 num;
	struct image_region reg[];
};

/**
 * efi_image_region_add() - add an entry of region
 * @regs:       Pointer to array of regions
 * @start:      Start address of region (included)
 * @end:        End address of region (excluded)
 * @nocheck:    Flag against overlapped regions
 *
 * Take one entry of region \[@start, @end\[ and insert it into the list.
 *
 * * If @nocheck is false, the list will be sorted ascending by address.
 *   Overlapping entries will not be allowed.
 *
 * * If @nocheck is true, the list will be sorted ascending by sequence
 *   of adding the entries. Overlapping is allowed.
 *
 * Return:  TRUE on success, FALSE on error
 */
BOOL efi_image_region_add(struct efi_image_regions* regs,
	const void* start, const void* end, int nocheck)
{
	struct image_region* reg;
	int i, j;

	if (regs->num >= regs->max) {
		uprintf("%s: no more room for regions", __func__);
		return FALSE;
	}

	if (end < start)
		return FALSE;

	for (i = 0; i < regs->num; i++) {
		reg = &regs->reg[i];
		if (nocheck)
			continue;

		/* new data after registered region */
		if ((uint8_t*)start >= reg->data + reg->size)
			continue;

		/* new data preceding registered region */
		if ((uint8_t*)end <= reg->data) {
			for (j = regs->num - 1; j >= i; j--)
				memcpy(&regs->reg[j + 1], &regs->reg[j],
					sizeof(*reg));
			break;
		}

		/* new data overlapping registered region */
		uprintf("%s: new region already part of another", __func__);
		return FALSE;
	}

	reg = &regs->reg[i];
	reg->data = start;
	reg->size = (uint32_t)((uintptr_t)end - (uintptr_t)start);
	regs->num++;

	return TRUE;
}

/**
 * cmp_pe_section() - compare virtual addresses of two PE image sections
 * @arg1:   Pointer to pointer to first section header
 * @arg2:   Pointer to pointer to second section header
 *
 * Compare the virtual addresses of two sections of an portable executable.
 * The arguments are defined as const void * to allow usage with qsort().
 *
 * Return:  -1 if the virtual address of arg1 is less than that of arg2,
 *           0 if the virtual addresses are equal, 1 if the virtual address
 *             of arg1 is greater than that of arg2.
 */
static int cmp_pe_section(const void* arg1, const void* arg2)
{
	const IMAGE_SECTION_HEADER* section1, * section2;

	section1 = *((const IMAGE_SECTION_HEADER**)arg1);
	section2 = *((const IMAGE_SECTION_HEADER**)arg2);

	if (section1->VirtualAddress < section2->VirtualAddress)
		return -1;
	else if (section1->VirtualAddress == section2->VirtualAddress)
		return 0;
	else
		return 1;
}

/**
 * efi_image_parse() - parse a PE image
 * @efi:    Pointer to image
 * @len:    Size of @efi
 * @regp:   Pointer to a list of regions
 *
 * Parse image binary in PE32(+) format.
 *
 * Return:  TRUE on success, FALSE on error
 */
BOOL efi_image_parse(uint8_t* efi, size_t len, struct efi_image_regions** regp)
{
	struct efi_image_regions* regs;
	IMAGE_DOS_HEADER* dos;
	IMAGE_NT_HEADERS32* nt;
	IMAGE_SECTION_HEADER *sections, **sorted;
	int num_regions, num_sections, i;
	DWORD ctidx = IMAGE_DIRECTORY_ENTRY_SECURITY;
	uint32_t align, size, authsz;
	size_t bytes_hashed;

	if (len < 0x80)
		return FALSE;
	dos = (void*)efi;
	if (dos->e_lfanew > (LONG)len - 0x40)
		return FALSE;
	nt = (void*)(efi + dos->e_lfanew);
	authsz = 0;

	/*
	 * Count maximum number of regions to be digested.
	 * We don't have to have an exact number here.
	 * See efi_image_region_add()'s in parsing below.
	 */
	num_regions = 3; /* for header */
	num_regions += nt->FileHeader.NumberOfSections;
	num_regions++; /* for extra */

	regs = calloc(sizeof(*regs) + sizeof(struct image_region) * num_regions, 1);
	if (!regs)
		goto err;
	regs->max = num_regions;

	/*
	 * Collect data regions for hash calculation
	 * 1. File headers
	 */
	if (nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
		IMAGE_NT_HEADERS64* nt64 = (void*)nt;
		IMAGE_OPTIONAL_HEADER64* opt = &nt64->OptionalHeader;

		/* Skip CheckSum */
		efi_image_region_add(regs, efi, &opt->CheckSum, 0);
		if (nt64->OptionalHeader.NumberOfRvaAndSizes <= ctidx) {
			efi_image_region_add(regs,
				&opt->Subsystem,
				efi + opt->SizeOfHeaders, 0);
		} else {
			/* Skip Certificates Table */
			efi_image_region_add(regs,
				&opt->Subsystem,
				&opt->DataDirectory[ctidx], 0);
			efi_image_region_add(regs,
				&opt->DataDirectory[ctidx] + 1,
				efi + opt->SizeOfHeaders, 0);

			authsz = opt->DataDirectory[ctidx].Size;
		}

		bytes_hashed = opt->SizeOfHeaders;
		align = opt->FileAlignment;
	} else if (nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
		IMAGE_OPTIONAL_HEADER32* opt = &nt->OptionalHeader;

		/* Skip CheckSum */
		efi_image_region_add(regs, efi, &opt->CheckSum, 0);
		if (nt->OptionalHeader.NumberOfRvaAndSizes <= ctidx) {
			efi_image_region_add(regs,
				&opt->Subsystem,
				efi + opt->SizeOfHeaders, 0);
		} else {
			/* Skip Certificates Table */
			efi_image_region_add(regs, &opt->Subsystem,
				&opt->DataDirectory[ctidx], 0);
			efi_image_region_add(regs,
				&opt->DataDirectory[ctidx] + 1,
				efi + opt->SizeOfHeaders, 0);

			authsz = opt->DataDirectory[ctidx].Size;
		}

		bytes_hashed = opt->SizeOfHeaders;
		align = opt->FileAlignment;
	} else {
		uprintf("%s: Invalid optional header magic %x", __func__,
			nt->OptionalHeader.Magic);
		goto err;
	}

	/* 2. Sections */
	num_sections = nt->FileHeader.NumberOfSections;
	sections = (void*)((uint8_t*)&nt->OptionalHeader +
		nt->FileHeader.SizeOfOptionalHeader);
	sorted = calloc(sizeof(IMAGE_SECTION_HEADER*), num_sections);
	if (!sorted) {
		uprintf("%s: Out of memory", __func__);
		goto err;
	}

	/*
	 * Make sure the section list is in ascending order.
	 */
	for (i = 0; i < num_sections; i++)
		sorted[i] = &sections[i];
	qsort(sorted, num_sections, sizeof(sorted[0]), cmp_pe_section);

	for (i = 0; i < num_sections; i++) {
		if (!sorted[i]->SizeOfRawData)
			continue;

		size = (sorted[i]->SizeOfRawData + align - 1) & ~(align - 1);
		efi_image_region_add(regs, efi + sorted[i]->PointerToRawData,
			efi + sorted[i]->PointerToRawData + size, 0);
		//uprintf("section[%d](%s): raw: 0x%x-0x%x, virt: %x-%x",
		//	i, sorted[i]->Name,
		//	sorted[i]->PointerToRawData,
		//	sorted[i]->PointerToRawData + size,
		//	sorted[i]->VirtualAddress,
		//	sorted[i]->VirtualAddress
		//	+ sorted[i]->Misc.VirtualSize);

		bytes_hashed += size;
	}
	free(sorted);

	/* 3. Extra data excluding Certificates Table */
	if (bytes_hashed + authsz < len) {
		//uprintf("extra data for hash: %zu",
		//	len - (bytes_hashed + authsz));
		efi_image_region_add(regs, efi + bytes_hashed,
			efi + len - authsz, 0);
	}

	*regp = regs;
	return TRUE;

err:
	free(regs);
	return FALSE;
}

/*
 * Compute the PE256 (a.k.a. Applocker SHA256) hash of a single EFI executable.
 * This is a SHA-256 hash applied to only specific parts of a PE binary.
 * See https://security.stackexchange.com/a/199627/270178.
 * Oh, and you'd think that Windows's ImageGetDigestStream() API could be used
 * for some part of this but you'd be very, very wrong since the PE sections it
 * feeds to the hash function does include the PE header checksum field...
 */
BOOL PE256Buffer(uint8_t* buf, uint32_t len, uint8_t* hash)
{
	BOOL r = FALSE;
	HASH_CONTEXT hash_ctx = { {0} };
	int i;
	struct efi_image_regions* regs = NULL;

	if ((buf == NULL) || (len == 0) || (len < 1 * KB) || (len > 64 * MB) || (hash == NULL))
		goto out;

	/* Isolate the PE sections to hash */
	if (!efi_image_parse(buf, len, &regs))
		goto out;

	/* Hash the relevant PE data */
	sha256_init(&hash_ctx);
	for (i = 0; i < regs->num; i++)
		sha256_write(&hash_ctx, regs->reg[i].data, regs->reg[i].size);
	sha256_final(&hash_ctx);

	memcpy(hash, hash_ctx.buf, SHA256_HASHSIZE);
	r = TRUE;

out:
	free(regs);
	return r;
}

/*
 * Compute the hash of a single buffer.
 */
BOOL HashBuffer(const unsigned type, const uint8_t* buf, const size_t len, uint8_t* hash)
{
	BOOL r = FALSE;
	HASH_CONTEXT hash_ctx = { {0} };

	if ((type >= HASH_MAX) || (hash == NULL))
		goto out;

	hash_init[type](&hash_ctx);
	hash_write[type](&hash_ctx, buf, len);
	hash_final[type](&hash_ctx);

	memcpy(hash, hash_ctx.buf, hash_count[type]);
	r = TRUE;

out:
	return r;
}

/*
 * Hash dialog callback
 */
INT_PTR CALLBACK HashCallback(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
	static HFONT hFont = NULL;
	int i, dw, dh;
	RECT rc;
	HDC hDC;

	switch (message) {
	case WM_INITDIALOG:
		SetDarkModeForDlg(hDlg);
		apply_localization(IDD_HASH, hDlg);
		if (hFont == NULL) {
			hDC = GetDC(hDlg);
			hFont = CreateFontA(-MulDiv(9, GetDeviceCaps(hDC, LOGPIXELSY), 72),
				0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
				0, 0, PROOF_QUALITY, 0, "Courier New");
			safe_release_dc(hDlg, hDC);
		}
		SendDlgItemMessageA(hDlg, IDC_MD5, WM_SETFONT, (WPARAM)hFont, TRUE);
		SendDlgItemMessageA(hDlg, IDC_SHA1, WM_SETFONT, (WPARAM)hFont, TRUE);
		SendDlgItemMessageA(hDlg, IDC_SHA256, WM_SETFONT, (WPARAM)hFont, TRUE);
		SendDlgItemMessageA(hDlg, IDC_SHA512, WM_SETFONT, (WPARAM)hFont, TRUE);
		SetWindowTextA(GetDlgItem(hDlg, IDC_MD5), hash_str[0]);
		SetWindowTextA(GetDlgItem(hDlg, IDC_SHA1), hash_str[1]);
		SetWindowTextA(GetDlgItem(hDlg, IDC_SHA256), hash_str[2]);
		if (enable_extra_hashes)
			SetWindowTextA(GetDlgItem(hDlg, IDC_SHA512), hash_str[3]);
		else
			SetWindowTextU(GetDlgItem(hDlg, IDC_SHA512), lmprintf(MSG_311, "<Alt>-<H>"));

		// Move/Resize the controls as needed to fit our text
		hDC = GetDC(GetDlgItem(hDlg, IDC_MD5));
		SelectFont(hDC, hFont);	// Yes, you *MUST* reapply the font to the DC, even after SetWindowText!

		GetWindowRect(GetDlgItem(hDlg, IDC_MD5), &rc);
		dw = rc.right - rc.left;
		dh = rc.bottom - rc.top;
		DrawTextU(hDC, hash_str[0], -1, &rc, DT_CALCRECT);
		dw = rc.right - rc.left - dw + 12;	// Ideally we'd compute the field borders from the system, but hey...
		dh = rc.bottom - rc.top - dh + 6;
		ResizeMoveCtrl(hDlg, GetDlgItem(hDlg, IDC_SHA256), 0, 0, dw, dh, 1.0f);
		ResizeMoveCtrl(hDlg, GetDlgItem(hDlg, IDC_SHA512), 0, 0, dw, dh, 1.0f);

		GetWindowRect(GetDlgItem(hDlg, IDC_SHA1), &rc);
		dw = rc.right - rc.left;
		DrawTextU(hDC, hash_str[1], -1, &rc, DT_CALCRECT);
		dw = rc.right - rc.left - dw + 12;
		ResizeMoveCtrl(hDlg, GetDlgItem(hDlg, IDC_MD5), 0, 0, dw, 0, 1.0f);
		ResizeMoveCtrl(hDlg, GetDlgItem(hDlg, IDC_SHA1), 0, 0, dw, 0, 1.0f);
		ResizeButtonHeight(hDlg, IDOK);

		safe_release_dc(GetDlgItem(hDlg, IDC_MD5), hDC);

		if (image_path != NULL) {
			for (i = (int)strlen(image_path); (i > 0) && (image_path[i] != '\\'); i--);
			SetWindowTextU(hDlg, &image_path[i + 1]);
		}
		SetDarkModeForChild(hDlg);
		// Set focus on the OK button
		SendMessage(hDlg, WM_NEXTDLGCTL, (WPARAM)GetDlgItem(hDlg, IDOK), TRUE);
		CenterDialog(hDlg, NULL);
		break;
	case WM_NCDESTROY:
		safe_delete_object(hFont);
		break;
	case WM_COMMAND:
		switch (LOWORD(wParam)) {
		case IDOK:
		case IDCANCEL:
			reset_localization(IDD_HASH);
			EndDialog(hDlg, LOWORD(wParam));
			return (INT_PTR)TRUE;
		}
	}
	return (INT_PTR)FALSE;
}

/* Individual thread that computes one of MD5, SHA1, SHA256 or SHA512 in parallel */
DWORD WINAPI IndividualHashThread(void* param)
{
	HASH_CONTEXT hash_ctx = { {0} }; // There's a memset in hash_init, but static analyzers still bug us
	uint32_t i = (uint32_t)(uintptr_t)param, j;

	hash_init[i](&hash_ctx);
	// Signal that we're ready to service requests
	if (!SetEvent(thread_ready[i]))
		goto error;

	// Wait for requests
	while (1) {
		if (WaitForSingleObject(data_ready[i], WAIT_TIME) != WAIT_OBJECT_0) {
			uprintf("Failed to wait for event for hash thread #%d: %s", i, WindowsErrorString());
			return 1;
		}
		if (read_size[proc_bufnum] != 0) {
			hash_write[i](&hash_ctx, buffer[proc_bufnum], (size_t)read_size[proc_bufnum]);
			if (!SetEvent(thread_ready[i]))
				goto error;
		} else {
			hash_final[i](&hash_ctx);
			memset(&hash_str[i], 0, ARRAYSIZE(hash_str[i]));
			for (j = 0; j < hash_count[i]; j++) {
				hash_str[i][2 * j] = ((hash_ctx.buf[j] >> 4) < 10) ?
					((hash_ctx.buf[j] >> 4) + '0') : ((hash_ctx.buf[j] >> 4) - 0xa + 'a');
				hash_str[i][2 * j + 1] = ((hash_ctx.buf[j] & 15) < 10) ?
					((hash_ctx.buf[j] & 15) + '0') : ((hash_ctx.buf[j] & 15) - 0xa + 'a');
			}
			hash_str[i][2 * j] = 0;
			return 0;
		}
	}
error:
	uprintf("Failed to set event for hash thread #%d: %s", i, WindowsErrorString());
	return 1;
}

DWORD WINAPI HashThread(void* param)
{
	DWORD_PTR* thread_affinity = (DWORD_PTR*)param;
	HANDLE hash_thread[HASH_MAX] = { NULL, NULL, NULL, NULL };
	DWORD wr;
	VOID* fd = NULL;
	uint64_t processed_bytes;
	int i, read_bufnum, r = -1;
	int num_hashes = HASH_MAX - (enable_extra_hashes ? 0 : 1);

	if ((image_path == NULL) || (thread_affinity == NULL))
		ExitThread(r);

	uprintf("\r\nComputing hash for '%s'...", image_path);

	if (thread_affinity[0] != 0)
		// Use the first affinity mask, as our read thread is the least
		// CPU intensive (mostly waits on disk I/O or on the other threads)
		// whereas the OS is likely to requisition the first Core, which
		// is usually in this first mask, for other tasks.
		SetThreadAffinityMask(GetCurrentThread(), thread_affinity[0]);

	for (i = 0; i < num_hashes; i++) {
		// NB: Can't use a single manual-reset event for data_ready as we
		// wouldn't be able to ensure the event is reset before the thread
		// gets into its next wait loop
		data_ready[i] = CreateEvent(NULL, FALSE, FALSE, NULL);
		thread_ready[i] = CreateEvent(NULL, FALSE, FALSE, NULL);
		if ((data_ready[i] == NULL) || (thread_ready[i] == NULL)) {
			uprintf("Unable to create hash thread event: %s", WindowsErrorString());
			goto out;
		}
		hash_thread[i] = CreateThread(NULL, 0, IndividualHashThread, (LPVOID)(uintptr_t)i, 0, NULL);
		if (hash_thread[i] == NULL) {
			uprintf("Unable to start hash thread #%d", i);
			goto out;
		}
		SetThreadPriority(hash_thread[i], default_thread_priority);
		if (thread_affinity[i+1] != 0)
			SetThreadAffinityMask(hash_thread[i], thread_affinity[i+1]);
	}

	fd = CreateFileAsync(image_path, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN);
	if (fd == NULL) {
		uprintf("Could not open file: %s", WindowsErrorString());
		ErrorStatus = RUFUS_ERROR(ERROR_OPEN_FAILED);
		goto out;
	}

	read_bufnum = 0;
	proc_bufnum = 1;
	read_size[proc_bufnum] = 1;	// To avoid early loop exit
	UpdateProgressWithInfoInit(hMainDialog, FALSE);

	// Start the initial read
	ReadFileAsync(fd, buffer[read_bufnum], BUFFER_SIZE);

	for (processed_bytes = 0; read_size[proc_bufnum] != 0; processed_bytes += read_size[proc_bufnum]) {
		// 0. Update the progress and check for cancel
		UpdateProgressWithInfo(OP_NOOP_WITH_TASKBAR, MSG_271, processed_bytes, img_report.image_size);
		CHECK_FOR_USER_CANCEL;

		// 1. Wait for the current read operation to complete (and update the read size)
		if ((!WaitFileAsync(fd, DRIVE_ACCESS_TIMEOUT)) ||
			(!GetSizeAsync(fd, &read_size[read_bufnum]))) {
			uprintf("Read error: %s", WindowsErrorString());
			ErrorStatus = RUFUS_ERROR(ERROR_READ_FAULT);
			goto out;
		}

		// 2. Switch to the next reading buffer
		read_bufnum = (read_bufnum + 1) % NUM_BUFFERS;

		// 3. Launch the next asynchronous read operation
		ReadFileAsync(fd, buffer[read_bufnum], BUFFER_SIZE);

		// 4. Wait for all the hash threads to indicate that they are ready to process data
		wr = WaitForMultipleObjects(num_hashes, thread_ready, TRUE, WAIT_TIME);
		if (wr != WAIT_OBJECT_0) {
			if (wr == STATUS_TIMEOUT)
				SetLastError(ERROR_TIMEOUT);
			uprintf("Hash threads failed to signal: %s", WindowsErrorString());
			goto out;
		}

		// 5. Set the target buffer we want to process to the buffer we just read data into
		// Note that this variable should only be updated AFTER all the threads have signalled.
		proc_bufnum = (read_bufnum + NUM_BUFFERS - 1) % NUM_BUFFERS;

		// 6. Signal the waiting threads that there is data available
		for (i = 0; i < num_hashes; i++) {
			if (!SetEvent(data_ready[i])) {
				uprintf("Could not signal hash thread %d: %s", i, WindowsErrorString());
				goto out;
			}
		}
	}

	// Our last event with read_size=0 signaled the threads to exit - wait for that to happen
	if (WaitForMultipleObjects(num_hashes, hash_thread, TRUE, WAIT_TIME) != WAIT_OBJECT_0) {
		uprintf("Hash threads did not finalize: %s", WindowsErrorString());
		goto out;
	}

	uprintf("  MD5:    %s", hash_str[0]);
	uprintf("  SHA1:   %s", hash_str[1]);
	uprintf("  SHA256: %s", hash_str[2]);
	if (enable_extra_hashes) {
		char c = hash_str[3][SHA512_HASHSIZE];
		hash_str[3][SHA512_HASHSIZE] = 0;
		uprintf("  SHA512: %s", hash_str[3]);
		hash_str[3][SHA512_HASHSIZE] = c;
		uprintf("          %s", &hash_str[3][SHA512_HASHSIZE]);
	}
	r = 0;

out:
	for (i = 0; i < num_hashes; i++) {
		if (hash_thread[i] != NULL)
			TerminateThread(hash_thread[i], 1);
		safe_closehandle(data_ready[i]);
		safe_closehandle(thread_ready[i]);
	}
	CloseFileAsync(fd);
	PostMessage(hMainDialog, UM_FORMAT_COMPLETED, (WPARAM)FALSE, 0);
	if (r == 0)
		MyDialogBox(hMainInstance, IDD_HASH, hMainDialog, HashCallback);
	ExitThread(r);
}

/*
 * The following 2 calls are used to check whether a buffer/file is in our hash DB
 */
BOOL IsBufferInDB(const unsigned char* buf, const size_t len)
{
	int i;
	uint8_t hash[SHA256_HASHSIZE];
	if (!HashBuffer(HASH_SHA256, buf, len, hash))
		return FALSE;
	for (i = 0; i < ARRAYSIZE(sha256db); i += SHA256_HASHSIZE)
		if (memcmp(hash, &sha256db[i], SHA256_HASHSIZE) == 0)
			return TRUE;
	return FALSE;
}

BOOL IsFileInDB(const char* path)
{
	int i;
	uint8_t hash[SHA256_HASHSIZE];
	if (!HashFile(HASH_SHA256, path, hash))
		return FALSE;
	for (i = 0; i < ARRAYSIZE(sha256db); i += SHA256_HASHSIZE)
		if (memcmp(hash, &sha256db[i], SHA256_HASHSIZE) == 0)
			return TRUE;
	return FALSE;
}

BOOL FileMatchesHash(const char* path, const char* str)
{
	uint8_t hash[SHA256_HASHSIZE];
	if (!HashFile(HASH_SHA256, path, hash))
		return FALSE;
	return (memcmp(hash, StringToHash(str), SHA256_HASHSIZE) == 0);
}

BOOL BufferMatchesHash(const uint8_t* buf, const size_t len, const char* str)
{
	uint8_t hash[SHA256_HASHSIZE];
	if (!HashBuffer(HASH_SHA256, buf, len, hash))
		return FALSE;
	return (memcmp(hash, StringToHash(str), SHA256_HASHSIZE) == 0);
}

static BOOL IsRevokedBySbat(uint8_t* buf, uint32_t len)
{
	char* sbat = NULL, *version_str;
	uint32_t i, j, sbat_len;
	sbat_entry_t entry;

	// Fall back to embedded sbat_level.txt if we couldn't access remote
	if (sbat_entries == NULL) {
		sbat_level_txt = safe_strdup(db_sbat_level_txt);
		sbat_entries = GetSbatEntries(sbat_level_txt);
	}
	assert(sbat_entries != NULL);
	if (sbat_entries == NULL)
		return FALSE;

	// Look for a .sbat section
	sbat = GetPeSection(buf, ".sbat", &sbat_len);
	if (sbat == NULL || sbat < (char*)buf || sbat >= (char*)buf + len)
		return FALSE;

	for (i = 0; sbat[i] != '\0'; ) {
		while (sbat[i] == '\n')
			i++;
		if (sbat[i] == '\0')
			break;
		entry.product = &sbat[i];
		for (; sbat[i] != ',' && sbat[i] != '\0' && sbat[i] != '\n'; i++);
		if (sbat[i] == '\0' || sbat[i] == '\n')
			break;
		sbat[i++] = '\0';
		version_str = &sbat[i];
		for (; sbat[i] != ',' && sbat[i] != '\0' && sbat[i] != '\n'; i++);
		sbat[i++] = '\0';
		entry.version = atoi(version_str);
		uuprintf("  SBAT: %s,%d", entry.product, entry.version);
		for (; sbat[i] != '\0' && sbat[i] != '\n'; i++);
		if (entry.version == 0)
			continue;
		for (j = 0; sbat_entries[j].product != NULL; j++) {
			if (strcmp(entry.product, sbat_entries[j].product) == 0 && entry.version < sbat_entries[j].version) {
				uprintf("  SBAT version for '%s' (%d) is lower than the current minimum SBAT version (%d)!",
					entry.product, entry.version, sbat_entries[j].version);
				return TRUE;
			}
		}
	}

	return FALSE;
}

// NB: Can be tested using en_windows_8_1_x64_dvd_2707217.iso
extern BOOL UseLocalDbx(int arch);
static BOOL IsRevokedByDbx(uint8_t* hash, uint8_t* buf, uint32_t len)
{
	EFI_VARIABLE_AUTHENTICATION_2* efi_var_auth;
	EFI_SIGNATURE_LIST* efi_sig_list;
	BYTE* dbx_data = NULL;
	BOOL ret = FALSE, needs_free = FALSE;
	DWORD dbx_size = 0;
	char dbx_name[32], path[MAX_PATH];
	uint32_t i, fluff_size, nb_entries;

	i = MachineToArch(GetPeArch(buf));
	if (i == ARCH_UNKNOWN)
		goto out;

	// Check if a more recent local DBX should be preferred over embedded
	static_sprintf(dbx_name, "dbx_%s.bin", efi_archname[i]);
	if (UseLocalDbx(i)) {
		static_sprintf(path, "%s\\%s\\%s", app_data_dir, FILES_DIR, dbx_name);
		dbx_size = read_file(path, &dbx_data);
		needs_free = (dbx_data != NULL);
		if (needs_free)
			duprintf("  Using local %s for revocation check", path);
	}
	if (dbx_size == 0) {
		dbx_data = (BYTE*)GetResource(hMainInstance, MAKEINTRESOURCEA(IDR_DBX + i),
			_RT_RCDATA, dbx_name, &dbx_size, FALSE);
	}
	if (dbx_data == NULL || dbx_size <= sizeof(EFI_VARIABLE_AUTHENTICATION_2))
		goto out;

	efi_var_auth = (EFI_VARIABLE_AUTHENTICATION_2*)dbx_data;
	fluff_size = efi_var_auth->AuthInfo.Hdr.dwLength + sizeof(EFI_TIME);
	if (dbx_size <= fluff_size)
		goto out;
	efi_sig_list = (EFI_SIGNATURE_LIST*)&dbx_data[fluff_size];
	fluff_size += sizeof(EFI_SIGNATURE_LIST);
	if (dbx_size <= fluff_size)
		goto out;
	// Expect SHA-256 hashes
	if (!CompareGUID(&efi_sig_list->SignatureType, &EFI_CERT_SHA256_GUID)) {
		uprintf("  Warning: %s is not using SHA-256 hashes - Cannot check for UEFI revocation!", dbx_name);
		goto out;
	}
	fluff_size += efi_sig_list->SignatureHeaderSize;
	assert(efi_sig_list->SignatureSize != 0);
	nb_entries = (efi_sig_list->SignatureListSize - efi_sig_list->SignatureHeaderSize - sizeof(EFI_SIGNATURE_LIST)) / efi_sig_list->SignatureSize;
	assert(dbx_size >= fluff_size + nb_entries * efi_sig_list->SignatureSize);

	fluff_size += sizeof(GUID);
	for (i = 0; i < nb_entries && !ret; i++) {
		if (memcmp(hash, &dbx_data[fluff_size + i * efi_sig_list->SignatureSize], SHA256_HASHSIZE) == 0)
			ret = TRUE;
	}

out:
	if (needs_free)
		free(dbx_data);
	return ret;
}

static BOOL IsRevokedBySvn(uint8_t* buf, uint32_t len)
{
	wchar_t* rsrc_name = NULL;
	uint8_t *root;
	uint32_t i, j, rsrc_rva, rsrc_len, *svn_ver;
	IMAGE_DOS_HEADER* dos_header = (IMAGE_DOS_HEADER*)buf;
	IMAGE_NT_HEADERS32* pe_header;
	IMAGE_NT_HEADERS64* pe64_header;
	IMAGE_DATA_DIRECTORY img_data_dir;

	if (sbat_entries == NULL)
		return FALSE;

	for (i = 0; sbat_entries[i].product != NULL; i++) {
		// SVN entries are expected to be uppercase
		for (j = 0; j < strlen(sbat_entries[i].product) && isupper(sbat_entries[i].product[j]); j++);
		if (j < strlen(sbat_entries[i].product))
			continue;
		rsrc_name = utf8_to_wchar(sbat_entries[i].product);
		if (rsrc_name == NULL)
			continue;

		pe_header = (IMAGE_NT_HEADERS32*)&buf[dos_header->e_lfanew];
		if (pe_header->FileHeader.Machine == IMAGE_FILE_MACHINE_I386 || pe_header->FileHeader.Machine == IMAGE_FILE_MACHINE_ARM) {
			img_data_dir = pe_header->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE];
		} else {
			pe64_header = (IMAGE_NT_HEADERS64*)pe_header;
			img_data_dir = pe64_header->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE];
		}

		root = RvaToPhysical(buf, img_data_dir.VirtualAddress);
		rsrc_rva = FindResourceRva(rsrc_name, root, root, &rsrc_len);
		safe_free(rsrc_name);
		if (rsrc_rva != 0) {
			if (rsrc_len == sizeof(uint32_t)) {
				svn_ver = (uint32_t*)RvaToPhysical(buf, rsrc_rva);
				if (svn_ver != NULL) {
					uuprintf("  SVN version: %d.%d", *svn_ver >> 16, *svn_ver & 0xffff);
					if (*svn_ver < sbat_entries[i].version) {
						uprintf("  SVN version %d.%d is lower than required minimum SVN version %d.%d!",
							*svn_ver >> 16, *svn_ver & 0xffff, sbat_entries[i].version >> 16, sbat_entries[i].version & 0xffff);
						return TRUE;
					}
				}
			} else {
				uprintf("  Warning: Unexpected Secure Version Number size");
			}
		}
	}
	return FALSE;
}

static BOOL IsRevokedByCert(cert_info_t* info)
{
	uint32_t i;

	// TODO: Enable this for non expert mode after enforcement of PCA2011 cert revocation
	if (!expert_mode)
		return FALSE;

	// Fall back to embedded Secure Boot thumbprints if we couldn't access remote
	if (sb_revoked_certs == NULL) {
		sb_revoked_txt = safe_strdup(db_sb_revoked_txt);
		sb_revoked_certs = GetThumbprintEntries(sb_revoked_txt);
	}
	assert(sb_revoked_certs != NULL && sb_revoked_certs->count != 0);
	if (sb_revoked_certs == NULL)
		return FALSE;

	for (i = 0; i < sb_revoked_certs->count; i++) {
		if (memcmp(info->thumbprint, sb_revoked_certs->list[i], SHA1_HASHSIZE) == 0) {
			uuprintf("  Found '%s' revoked certificate", info->name);
			return TRUE;
		}
	}
	return FALSE;
}

BOOL IsSignedBySecureBootAuthority(uint8_t* buf, uint32_t len)
{
	uint32_t i;
	uint8_t* cert;
	cert_info_t info;

	if (buf == NULL || len < 0x100)
		return FALSE;

	// Get the signer/issuer info
	cert = GetPeSignatureData(buf);
	// Secure Boot Authority is always an issuer
	if (GetIssuerCertificateInfo(cert, &info) != 2)
		return FALSE;

	// Fall back to embedded Secure Boot thumbprints if we couldn't access remote
	if (sb_active_certs == NULL) {
		sb_active_txt = safe_strdup(db_sb_active_txt);
		sb_active_certs = GetThumbprintEntries(sb_active_txt);
	}
	// If we still manage to get an empty list at this stage, I sure wanna know about it!
	assert(sb_active_certs != NULL && sb_active_certs->count != 0);
	if (sb_active_certs == NULL || sb_active_certs->count == 0)
		return FALSE;

	for (i = 0; i < sb_active_certs->count; i++) {
		if (memcmp(info.thumbprint, sb_active_certs->list[i], SHA1_HASHSIZE) == 0)
			return TRUE;
	}
	return FALSE;
}

int IsBootloaderRevoked(uint8_t* buf, uint32_t len)
{
	uint32_t i;
	uint8_t hash[SHA256_HASHSIZE];
	IMAGE_DOS_HEADER* dos_header = (IMAGE_DOS_HEADER*)buf;
	IMAGE_NT_HEADERS32* pe_header;
	uint8_t* cert;
	cert_info_t info;
	int r, revoked = 0;

	if (buf == NULL || len < 0x100 || dos_header->e_magic != IMAGE_DOS_SIGNATURE)
		return -2;
	pe_header = (IMAGE_NT_HEADERS32*)&buf[dos_header->e_lfanew];
	if (pe_header->Signature != IMAGE_NT_SIGNATURE)
		return -2;

	// Get the signer/issuer info
	cert = GetPeSignatureData(buf);
	r = GetIssuerCertificateInfo(cert, &info);
	if (r == 0) {
		uprintf("  (Unsigned Bootloader)");
	} else if (r > 0) {
		uprintf("  Signed by '%s'", info.name);
		// Only perform revocation checks on signed bootloaders
		if (!PE256Buffer(buf, len, hash))
			return -1;
		// Check for UEFI DBX revocation
		if (IsRevokedByDbx(hash, buf, len))
			revoked = 1;
		// Check for Microsoft SSP revocation
		for (i = 0; revoked == 0 && i < pe256ssp_size * SHA256_HASHSIZE; i += SHA256_HASHSIZE)
			if (memcmp(hash, &pe256ssp[i], SHA256_HASHSIZE) == 0)
				revoked = 2;
		// Check for Linux SBAT revocation
		if (revoked == 0 && IsRevokedBySbat(buf, len))
			revoked = 3;
		// Check for Microsoft SVN revocation
		if (revoked == 0 && IsRevokedBySvn(buf, len))
			revoked = 4;
		// Check for UEFI DBX certificate revocation
		if (revoked == 0 && IsRevokedByCert(&info))
			revoked = 5;

		// If signed and not revoked, print the various Secure Boot "gotchas"
		if (revoked == 0) {
			if (strcmp(info.name, "Microsoft Windows Production PCA 2011") == 0) {
				uprintf("  Note: This bootloader may fail Secure Boot validation on systems that");
				uprintf("  have been updated to use the 'Windows UEFI CA 2023' certificate.");
			} else if (strcmp(info.name, "Windows UEFI CA 2023") == 0) {
				uprintf("  Note: This bootloader will fail Secure Boot validation on systems that");
				uprintf("  have not been updated to use the latest Secure Boot certificates");
			} else if (strcmp(info.name, "Microsoft Corporation UEFI CA 2011") == 0 ||
				strcmp(info.name, "Microsoft UEFI CA 2023") == 0) {
				uprintf("  Note: This bootloader may fail Secure Boot validation on *some* systems,");
				uprintf("  unless you enable \"Microsoft 3rd-party UEFI CA\" in your 'BIOS'.");
			}
		}
	}

	return revoked;
}

/*
 * Updates the MD5SUMS/md5sum.txt file that some distros (Ubuntu, Mint...)
 * use to validate the media. Because we may alter some of the validated files
 * to add persistence and whatnot, we need to alter the MD5 list as a result.
 * The format of the file is expected to always be "<MD5SUM> <FILE_PATH>" on
 * individual lines.
 * This function is also used to finalize the md5sum.txt we create for use with
 * our uefi-md5sum bootloaders.
 */
void UpdateMD5Sum(const char* dest_dir, const char* md5sum_name)
{
	BOOL display_header = TRUE;
	BYTE* res_data;
	DWORD res_size;
	HANDLE hFile;
	intptr_t pos;
	uint32_t i, j, size, md5_size, new_size;
	uint8_t sum[MD5_HASHSIZE];
	char md5_path[64], path1[64], path2[64], bootloader_name[32];
	char *md5_data = NULL, *new_data = NULL, *str_pos, *d, *s, *p;

	if (!img_report.has_md5sum && !validate_md5sum)
		return;

	static_sprintf(md5_path, "%s\\%s", dest_dir, md5sum_name);
	md5_size = read_file(md5_path, (uint8_t**)&md5_data);
	if (md5_size == 0)
		return;

	for (i = 0; i < modified_files.Index; i++) {
		for (j = 0; j < (uint32_t)strlen(modified_files.String[i]); j++)
			if (modified_files.String[i][j] == '\\')
				modified_files.String[i][j] = '/';
		str_pos = strstr(md5_data, &modified_files.String[i][2]);
		if (str_pos == NULL)
			// File is not listed in md5 sums
			continue;
		if (display_header) {
			uprintf("Updating %s:", md5_path);
			display_header = FALSE;
		}
		uprintf("● %s", &modified_files.String[i][2]);
		pos = str_pos - md5_data;
		HashFile(HASH_MD5, modified_files.String[i], sum);
		while ((pos > 0) && (md5_data[pos - 1] != '\n'))
			pos--;
		assert(IS_HEXASCII(md5_data[pos]));
		for (j = 0; j < 16; j++) {
			md5_data[pos + 2 * j] = ((sum[j] >> 4) < 10) ? ('0' + (sum[j] >> 4)) : ('a' - 0xa + (sum[j] >> 4));
			md5_data[pos + 2 * j + 1] = ((sum[j] & 15) < 10) ? ('0' + (sum[j] & 15)) : ('a' - 0xa + (sum[j] & 15));
		}
	}

	// If we validate md5sum we need to update the original bootloader names and add md5sum_totalbytes
	if (validate_md5sum) {
		new_size = md5_size;
		new_data = malloc(md5_size + 1024);
		assert(new_data != NULL);
		if (new_data == NULL)
			return;
		// Will be nonzero if we created the file, otherwise zero
		if (md5sum_totalbytes != 0) {
			snprintf(new_data, md5_size + 1024, "# md5sum_totalbytes = 0x%llx\n", md5sum_totalbytes);
			new_size += (uint32_t)strlen(new_data);
			d = &new_data[strlen(new_data)];
		} else {
			d = new_data;
		}
		s = md5_data;
		// Extract the MD5Sum bootloader(s)
		for (i = 1; i < ARRAYSIZE(efi_archname); i++) {
			static_sprintf(bootloader_name, "boot%s.efi", efi_archname[i]);
			static_sprintf(path1, "%s\\efi\\boot\\boot%s.efi", dest_dir, efi_archname[i]);
			if (!PathFileExistsA(path1))
				continue;
			res_data = (BYTE*)GetResource(hMainInstance, MAKEINTRESOURCEA(IDR_MD5_BOOT + i),
				_RT_RCDATA, bootloader_name, &res_size, FALSE);
			static_strcpy(path2, path1);
			path2[strlen(path2) - 4] = 0;
			static_strcat(path2, "_original.efi");
			if (res_data == NULL || !MoveFileU(path1, path2)) {
				uprintf("Could not rename: %s → %s", path1, path2);
				continue;
			}
			uprintf("Renamed: %s → %s", path1, path2);
			hFile = CreateFileA(path1, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL,
				CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
			if ((hFile == NULL) || (hFile == INVALID_HANDLE_VALUE)) {
				uprintf("Could not create '%s': %s.", path1, WindowsErrorString());
				MoveFileU(path2, path1);
				continue;
			}
			if (!WriteFileWithRetry(hFile, res_data, res_size, NULL, WRITE_RETRIES)) {
				uprintf("Could not write '%s': %s.", path1, WindowsErrorString());
				safe_closehandle(hFile);
				MoveFileU(path2, path1);
				continue;
			}
			safe_closehandle(hFile);
			uprintf("Created: %s (%s)", path1, SizeToHumanReadable(res_size, FALSE, FALSE));
		}
		// Rename the original bootloaders if present in md5sum.txt
		for (p = md5_data; (p = StrStrIA(p, " ./efi/boot/boot")) != NULL; ) {
			for (i = 1; i < ARRAYSIZE(efi_archname); i++) {
				static_sprintf(bootloader_name, "boot%s.efi", efi_archname[i]);
				if (p[12 + strlen(bootloader_name)] != 0x0a)
					continue;
				p[12 + strlen(bootloader_name)] = 0;
				if (lstrcmpiA(&p[12], bootloader_name) == 0) {
					size = (uint32_t)(p - s) + 12 + (uint32_t)strlen(bootloader_name) - 4;
					memcpy(d, s, size);
					d = &d[size];
					strcpy(d, "_original.efi\n");
					new_size += 9;
					d = &d[14];
					s = &p[12 + strlen(bootloader_name) + 1];
				}
				p[12 + strlen(bootloader_name)] = 0x0a;
			}
			p = &p[12];
		}
		p = &md5_data[md5_size];
		memcpy(d, s, p - s);
		free(md5_data);
		md5_data = new_data;
		md5_size = new_size;
	}

	write_file(md5_path, md5_data, md5_size);
	free(md5_data);
}

/* Convert an (unprefixed) hex string to hash binary. Non concurrent. */
uint8_t* StringToHash(const char* str)
{
	static uint8_t ret[MAX_HASHSIZE];
	size_t i, len = safe_strlen(str);
	uint8_t val = 0;
	char c;

	if_assert_fails(len / 2 == MD5_HASHSIZE || len / 2 == SHA1_HASHSIZE ||
		len / 2 == SHA256_HASHSIZE || len / 2 == SHA512_HASHSIZE)
		return NULL;
	memset(ret, 0, sizeof(ret));

	for (i = 0; i < len; i++) {
		val <<= 4;
		c = tolower(str[i]);
		if_assert_fails((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))
			return NULL;
		val |= ((c - '0') < 0xa) ? (c - '0') : (c - 'a' + 0xa);
		if (i % 2)
			ret[i / 2] = val;
	}

	return ret;
}

#if defined(_DEBUG) || defined(TEST) || defined(ALPHA)
const char test_msg[] = "Did you ever hear the tragedy of Darth Plagueis The Wise? "
	"I thought not. It's not a story the Jedi would tell you. It's a Sith legend. "
	"Darth Plagueis was a Dark Lord of the Sith, so powerful and so wise he could "
	"use the Force to influence the midichlorians to create life... He had such a "
	"knowledge of the dark side that he could even keep the ones he cared about "
	"from dying. The dark side of the Force is a pathway to many abilities some "
	"consider to be unnatural. He became so powerful... the only thing he was afraid "
	"of was losing his power, which eventually, of course, he did. Unfortunately, "
	"he taught his apprentice everything he knew, then his apprentice killed him "
	"in his sleep. Ironic. He could save others from death, but not himself.";

/*
 * Yeah, I'm not gonna bother with binary arrays of hash values since
 * they have different sizes and MSVC is an ass with initializing unions.
 * Much rather copy paste from md5sum/sha#sum output from Linux and just
 * convert the string.
 */
const char* test_hash[HASH_MAX][4] = {
	{
		"d41d8cd98f00b204e9800998ecf8427e",
		"74cac558072300385f7ab4dff7465e3c",
		"f99d37d3bee20f9c0ca3204991be2698",
		"e0ea372ac14a3574167543b851d4babb"
	}, {
		"da39a3ee5e6b4b0d3255bfef95601890afd80709",
		"a5bac908bf3e51ff0036a94d43b4f3bd2d01a75d",
		"8aa6c0064b013b8a6f4e88a0421d39bbf07e2e1b",
		"09463ec0b5917706c9cb1d6b164b2582c04018e0"
	}, {
		"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
		"62c1a97974dfe6848942794be4f2f027b5f4815e1eb76db63a30f0e290b5c1c4",
		"dbca61af192edba49ea215c49a23feee302c98cc4d2c018347fe78db572f07a5",
		"c9b43c1058bc7f7661619e9d983fc9d31356e97f9195a2405ab972d0737b11bf"
	}, {
		"cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e",
		"4913ace12f1169e5a5f524ef87ab8fc39dff0418851fbbbb1f609d3261b2b4072bd1746e6accb91bf38f3b1b3d59b0a60af5de67aab87b76c2456fde523efc1c",
		"33df8a16dd624cbc4613b5ae902b722411c7e90f37dd3947c9a86e01c51ada68fcf5a0cd4ca928d7cc1ed469bb34c2ed008af069d8b28cc4512e6c8b2e7a5592",
		"999b4eae14de584cce5fa5962b768beda076b06df00d384bb502c6389df8159c006a5b94d1324f47e8d7bd2efe9d8d3dc1fa1429798e49826987ab5ae7ed5c21"
	},
};

/* Tests the message digest algorithms */
int TestHashes(void)
{
	const uint32_t blocksize[HASH_MAX] = { MD5_BLOCKSIZE, SHA1_BLOCKSIZE, SHA256_BLOCKSIZE, SHA512_BLOCKSIZE };
	const char* hash_name[4] = { "MD5   ", "SHA1  ", "SHA256", "SHA512" };
	int i, j, errors = 0;
	uint8_t hash[MAX_HASHSIZE];
	size_t full_msg_len = strlen(test_msg);
	char* msg = malloc(full_msg_len + 1);
	if (msg == NULL)
		return -1;

	/* Display accelerations available */
	uprintf("SHA1   acceleration: %s", (cpu_has_sha1_accel ? "TRUE" : "FALSE"));
	uprintf("SHA256 acceleration: %s", (cpu_has_sha256_accel ? "TRUE" : "FALSE"));

	for (j = 0; j < HASH_MAX; j++) {
		size_t copy_msg_len[4];
		copy_msg_len[0] = 0;
		copy_msg_len[1] = 3;
		// Designed to test the case where we pad into the total message length area
		// For SHA-512 this is 128 - 16 = 112 bytes, for others 64 - 8 = 56 bytes
		copy_msg_len[2] = blocksize[j] - (blocksize[j] >> 3);
		copy_msg_len[3] = full_msg_len;
		for (i = 0; i < 4; i++) {
			memset(msg, 0, full_msg_len + 1);
			if (i != 0)
				memcpy(msg, test_msg, copy_msg_len[i]);
			HashBuffer(j, msg, copy_msg_len[i], hash);
			if (memcmp(hash, StringToHash(test_hash[j][i]), hash_count[j]) != 0) {
				uprintf("Test %s %d: FAIL", hash_name[j], i);
				errors++;
			} else {
				uprintf("Test %s %d: PASS", hash_name[j], i);
			}
		}
	}

	free(msg);
	return errors;
}
#endif
