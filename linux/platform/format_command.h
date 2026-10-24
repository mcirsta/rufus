/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stdint.h>

enum { FORMAT_DIAGNOSTIC_CAPACITY = 4096, FORMAT_TARGET_FD = 3, FORMAT_ARGUMENT_BYTES = 32 };
typedef struct {
	bool started;
	int exit_code;
	int signal;
	int sync_error;
	uint64_t filesystem_bytes;
	char diagnostics[FORMAT_DIAGNOSTIC_CAPACITY];
} FORMAT_RESULT;

#ifdef __cplusplus
extern "C" {
#endif

/* Filesystem adapters validate the borrowed descriptor and arguments first.
 * The child receives fd at FORMAT_TARGET_FD and LC_ALL=C.UTF-8, without a shell.
 * Preserve the existing FAT formatter's bounded diagnostics, 120 s timeout,
 * TERM/KILL cancellation and child reaping. Flush fd after every started child,
 * even on failure. Filesystem verification belongs to the calling adapter. */
int RunFormatCommand(int fd, char* const* arguments, FORMAT_RESULT* result);
/* Full format can legitimately take hours. Preserve cancellation, reaping and
 * sync, while allowing the trusted formatter to run without an overall deadline. */
int RunFullFormatCommand(int fd, char* const* arguments, FORMAT_RESULT* result);

/* For a dedicated, single-job worker already configured as a child subreaper.
 * The caller supplies an absolute trusted executable, a complete environment
 * (including the trusted tools PATH) and a private working directory. Only
 * stdin/stdout/stderr and the target descriptor survive exec. Own and reap the
 * whole process group before syncing, including on cancellation or failure.
 * Tools must not daemonize or leave this group; no other code may reap it.
 * SIGCHLD must not be ignored or configured with SA_NOCLDWAIT.
 * An installer that returns with unreaped descendants is an error. This is an
 * execution boundary only: adapters still validate the target and payloads. */
int RunInstallerCommand(int fd, char* const* arguments, char* const* environment,
	const char* directory, FORMAT_RESULT* result);

#ifdef __cplusplus
}
#endif
