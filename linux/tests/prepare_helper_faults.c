/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int __real_posix_spawn(pid_t*, const char*, const posix_spawn_file_actions_t*,
	const posix_spawnattr_t*, char* const[], char* const[]);
int __real_fsync(int);
uid_t __real_getuid(void);
uid_t __real_geteuid(void);
static int formatter_started;

/* Exercise device preflight without acquiring real privileges. GRUB tests
 * additionally intercept target.open below, before any device access. */
uid_t __wrap_getuid(void)
{
	const int preflight = getenv("RUFUS_TEST_FREEDOS_PREFLIGHT") != NULL ||
		getenv("RUFUS_TEST_GRUB2_PREFLIGHT") != NULL ||
		getenv("RUFUS_TEST_WINDOWS_BIOS_PREFLIGHT") != NULL;
	return preflight ? 0 : __real_getuid();
}

uid_t __wrap_geteuid(void)
{
	const int preflight = getenv("RUFUS_TEST_FREEDOS_PREFLIGHT") != NULL ||
		getenv("RUFUS_TEST_GRUB2_PREFLIGHT") != NULL ||
		getenv("RUFUS_TEST_WINDOWS_BIOS_PREFLIGHT") != NULL;
	return preflight ? 0 : __real_geteuid();
}

/* C linkage for the Linux C++ symbol: PrepareDeviceTarget::open(uid_t,
 * const PrepareIsoSource*). This test-only gate never acquires a target. */
int __real__ZN5rufus19PrepareDeviceTarget4openEjPKNS_16PrepareIsoSourceE(void*, uid_t, const void*);
int __wrap__ZN5rufus19PrepareDeviceTarget4openEjPKNS_16PrepareIsoSourceE(void* target, uid_t caller, const void* source)
{
	if (getenv("RUFUS_TEST_GRUB2_PREFLIGHT") != NULL ||
		getenv("RUFUS_TEST_WINDOWS_BIOS_PREFLIGHT") != NULL) {
		fputs("RUFUS_TEST_TARGET_GATE\n", stderr);
		return ENOTSUP;
	}
	return __real__ZN5rufus19PrepareDeviceTarget4openEjPKNS_16PrepareIsoSourceE(target, caller, source);
}

/* Linked only into prepare_helper_probe. Production has no tool override or
 * fault environment variables. Real child creation, cancellation and reaping
 * remain in the production formatter implementation. */
int __wrap_posix_spawn(pid_t* pid, const char* path, const posix_spawn_file_actions_t* actions,
	const posix_spawnattr_t* attributes, char* const argv[], char* const env[])
{
	const char* replacement = getenv("RUFUS_TEST_FORMATTER");
	if (geteuid() == 0) {
		char marker[32] = { 0 };
		FILE* file = fopen("/etc/rufus-gpt-fixture", "r");
		if (file == NULL)
			return EPERM;
		size_t size = fread(marker, 1, sizeof(marker) - 1, file);
		fclose(file);
		if (size != strlen("RUFUS-GPT-TEST\n") || strcmp(marker, "RUFUS-GPT-TEST\n") != 0)
			return EPERM;
		if (access("/etc/rufus-ntfs-fixture", F_OK) == 0) {
			int disk = open("/dev/sda", O_RDWR | O_EXCL | O_CLOEXEC);
			int error = errno;
			if (disk >= 0) {
				close(disk);
				return EBUSY;
			}
			if (error != EBUSY)
				return error;
			fputs("RUFUS_NTFS_CLAIM_RETAINED\n", stderr);
		}
		replacement = "/opt/rufus/slow-formatter";
	}
	int error = __real_posix_spawn(pid, replacement != NULL ? replacement : path, actions, attributes, argv, env);
	if (error == 0)
		formatter_started = 1;
	return error;
}

int __wrap_fsync(int fd)
{
	const int fail_after_format = formatter_started && getenv("RUFUS_TEST_SYNC_AFTER_FORMAT") != NULL;
	if (getenv("RUFUS_TEST_SYNC_ERROR") != NULL || fail_after_format) {
		errno = EIO;
		return -1;
	}
	return __real_fsync(fd);
}
