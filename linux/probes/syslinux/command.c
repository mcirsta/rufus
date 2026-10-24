/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Disposable regular-file worker. Not installed or used by the GUI/helper. */
#include "platform/format_command.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum {
	INITIAL_POSITION = 17, FOREIGN_FD = 80, CANCEL_DELAY_MS = 150, TIMEOUT_STEP_SECONDS = 61,
	MILLISECONDS_PER_SECOND = 1000, NANOSECONDS_PER_MILLISECOND = 1000000
};
static const char* fault;
static char ready[4096];
static int syncs, ticks, sync_children;
static int64_t start;
static pid_t unrelated = -1;
int __real_fsync(int);
int __real_clock_gettime(clockid_t, struct timespec*);

static int64_t Milliseconds(void)
{
	struct timespec now;
	if (__real_clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		abort();
	return (int64_t)now.tv_sec * MILLISECONDS_PER_SECOND + now.tv_nsec / NANOSECONDS_PER_MILLISECOND;
}

int __wrap_clock_gettime(clockid_t clock, struct timespec* now)
{
	if (strcmp(fault, "clock-fail") == 0) {
		errno = EIO;
		return -1;
	}
	int result = __real_clock_gettime(clock, now);
	if (result == 0 && strcmp(fault, "timeout") == 0 && access(ready, F_OK) == 0)
		now->tv_sec += TIMEOUT_STEP_SECONDS * ticks++;
	return result;
}

int IsRufusJobCancelled(void)
{
	const bool cancel_running = strncmp(fault, "cancel-", 7) == 0 && access(ready, F_OK) == 0;
	return strcmp(fault, "cancel-before") == 0 || (cancel_running && Milliseconds() - start >= CANCEL_DELAY_MS);
}

int __wrap_fsync(int fd)
{
	siginfo_t info = { 0 };
	syncs++;
	if (unrelated < 0) {
		int waited = waitid(P_ALL, 0, &info, WEXITED | WNOHANG | WNOWAIT);
		if (waited != -1 || errno != ECHILD)
			sync_children++;
	}
	if (strcmp(fault, "sync-fail") == 0) {
		errno = EIO;
		return -1;
	}
	if (strcmp(fault, "sync-eintr") == 0 && syncs == 1) {
		errno = EINTR;
		return -1;
	}
	return __real_fsync(fd);
}

static int DescriptorCount(void)
{
	DIR* directory = opendir("/proc/self/fd");
	struct dirent* entry;
	int count = 0;
	if (directory == NULL)
		abort();
	while ((entry = readdir(directory)) != NULL)
		if (entry->d_name[0] != '.')
			count++;
	closedir(directory);
	return count;
}

int main(int argc, char** argv)
{
	if (argc != 7)
		return 64;
	fault = argv[5];
	if (strcmp(fault, "no-subreaper") != 0 && prctl(PR_SET_CHILD_SUBREAPER, 1) != 0)
		return 65;
	if (strcmp(fault, "ignore-children") == 0)
		signal(SIGCHLD, SIG_IGN);
	if (strcmp(fault, "auto-reap") == 0) {
		struct sigaction action = { 0 };
		action.sa_handler = SIG_DFL;
		action.sa_flags = SA_NOCLDWAIT;
		if (sigaction(SIGCHLD, &action, NULL) != 0)
			abort();
	}
	int fd = open(argv[2], O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
	struct stat st;
	if (fd < 0 || fstat(fd, &st) != 0)
		return 66;
	if (!S_ISREG(st.st_mode) || st.st_size < 512)
		return 67;
	if (lseek(fd, INITIAL_POSITION, SEEK_SET) != INITIAL_POSITION)
		return 68;
	int flags = fcntl(fd, F_GETFD);
	int foreign = fcntl(fd, F_DUPFD, FOREIGN_FD);
	if (foreign != FOREIGN_FD)
		return 69;
	if (strcmp(fault, "replace-path") == 0) {
		char retained[4096];
		snprintf(retained, sizeof(retained), "%s.held", argv[2]);
		if (rename(argv[2], retained) != 0)
			abort();
		int replacement = open(argv[2], O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
		if (replacement < 0 || write(replacement, "untouched", 9) != 9)
			abort();
		close(replacement);
	}
	if (strcmp(fault, "unrelated") == 0) {
		unrelated = fork();
		if (unrelated < 0)
			abort();
		if (unrelated == 0)
			_exit(37);
	}
	char path[4096], temporary[4096], home[4096], mode[128];
	snprintf(path, sizeof(path), "PATH=%s", argv[3]);
	snprintf(temporary, sizeof(temporary), "TMPDIR=%s", argv[4]);
	snprintf(home, sizeof(home), "HOME=%s", argv[4]);
	snprintf(mode, sizeof(mode), "TEST_MODE=%s", fault);
	snprintf(ready, sizeof(ready), "%s/ready", argv[4]);
	char* environment[] = { path, temporary, home, mode, "LC_ALL=C.UTF-8", NULL };
	char* arguments[] = { argv[1], "--install", "--offset", argv[6], "/proc/self/fd/3", NULL };
	FORMAT_RESULT result;
	int before = DescriptorCount();
	start = Milliseconds();
	char** selected_environment = strcmp(fault, "no-environment") == 0 ? NULL : environment;
	const char* directory = strcmp(fault, "no-directory") == 0 ? NULL : argv[4];
	if (strcmp(fault, "relative-program") == 0)
		arguments[0] = "syslinux";
	int error = RunInstallerCommand(fd, arguments, selected_environment, directory, &result);
	int leaks = DescriptorCount() - before;
	int status, unrelated_ok = 1;
	if (unrelated > 0)
		unrelated_ok = waitpid(unrelated, &status, 0) == unrelated && WIFEXITED(status) && WEXITSTATUS(status) == 37;
	int child = waitpid(-1, &status, WNOHANG);
	int child_error = errno;
	printf("ERROR=%d STARTED=%d EXIT=%d SIGNAL=%d SYNC=%d SYNCS=%d SYNC_CHILDREN=%d "
		"FDS=%d OFFSET=%jd FLAGS=%d CHILD=%d CHILD_ERRNO=%d UNRELATED=%d DIAG=%zu MS=%" PRId64 "\n",
		error, result.started, result.exit_code, result.signal, result.sync_error, syncs, sync_children,
		leaks, (intmax_t)lseek(fd, 0, SEEK_CUR), fcntl(fd, F_GETFD) == flags,
		child, child_error, unrelated_ok, strlen(result.diagnostics), Milliseconds() - start);
	fprintf(stderr, "%s", result.diagnostics);
	close(foreign);
	close(fd);
	return 0;
}
