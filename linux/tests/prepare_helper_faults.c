/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <errno.h>
#include <spawn.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int __real_posix_spawn(pid_t*, const char*, const posix_spawn_file_actions_t*,
	const posix_spawnattr_t*, char* const[], char* const[]);
int __real_fsync(int);

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
		replacement = "/opt/rufus/slow-formatter";
	}
	return __real_posix_spawn(pid, replacement != NULL ? replacement : path, actions, attributes, argv, env);
}

int __wrap_fsync(int fd)
{
	if (getenv("RUFUS_TEST_SYNC_ERROR") != NULL) {
		errno = EIO;
		return -1;
	}
	return __real_fsync(fd);
}
