/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "format_command.h"
#include "rufus_reporting.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char** environ;
enum {
	FORMAT_FIRST_STAGING_FD = FORMAT_TARGET_FD + 1,
	MILLISECONDS_PER_SECOND = 1000,
	NANOSECONDS_PER_MILLISECOND = 1000000,
	FORMAT_TIMEOUT_MS = 120 * MILLISECONDS_PER_SECOND,
	FORMAT_TERMINATE_GRACE_MS = 500,
	FORMAT_POLL_MS = 25,
	DIAGNOSTIC_READS_PER_POLL = 16
};

/* Give native tools a UTF-8 locale without changing the GUI's process locale.
 * Copy pointers only; the child inherits its own environment at posix_spawn. */
static char** FormatterEnvironment(void)
{
	size_t count = 0, used = 0;
	while (environ[count] != NULL)
		count++;
	char** environment = calloc(count + 2, sizeof(*environment));
	if (environment == NULL)
		return NULL;
	for (size_t i = 0; i < count; i++)
		if (strncmp(environ[i], "LC_ALL=", 7) != 0)
			environment[used++] = environ[i];
	environment[used] = "LC_ALL=C.UTF-8";
	return environment;
}

static int StartFormatter(int fd, char* const* arguments, char* const* environment,
	const char* directory, pid_t* child, int* output)
{
	posix_spawn_file_actions_t actions;
	posix_spawnattr_t attributes;
	int pipefd[2], copy, error;
	char** inherited = NULL;
	bool attributes_ready = false;

	copy = fcntl(fd, F_DUPFD_CLOEXEC, FORMAT_FIRST_STAGING_FD);
	if (copy < 0)
		return errno;
	if (pipe2(pipefd, O_CLOEXEC) != 0) {
		error = errno;
		close(copy);
		return error;
	}
	// Keep staging descriptors out of the child's stdin/stdout/stderr/target slots.
	for (int i = 0; i < 2; i++) {
		if (pipefd[i] >= FORMAT_FIRST_STAGING_FD)
			continue;
		int moved = fcntl(pipefd[i], F_DUPFD_CLOEXEC, FORMAT_FIRST_STAGING_FD);
		if (moved < 0) {
			error = errno;
			goto out;
		}
		close(pipefd[i]);
		pipefd[i] = moved;
	}
	if (fcntl(pipefd[0], F_SETFL, O_NONBLOCK) != 0) {
		error = errno;
		goto out;
	}
	error = posix_spawn_file_actions_init(&actions);
	if (error != 0)
		goto out;
	error = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
	if (error == 0)
		error = posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
	if (error == 0)
		error = posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDERR_FILENO);
	if (error == 0)
		error = posix_spawn_file_actions_adddup2(&actions, copy, FORMAT_TARGET_FD);
	if (error == 0 && directory != NULL)
		error = posix_spawn_file_actions_addchdir_np(&actions, directory);
	if (error == 0 && directory != NULL)
		error = posix_spawn_file_actions_addclosefrom_np(&actions, FORMAT_FIRST_STAGING_FD);
	if (error == 0 && directory != NULL) {
		error = posix_spawnattr_init(&attributes);
		attributes_ready = error == 0;
		if (error == 0)
			error = posix_spawnattr_setpgroup(&attributes, 0);
		if (error == 0)
			error = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
	}
	if (error == 0) {
		if (environment == NULL)
			environment = inherited = FormatterEnvironment();
		error = environment == NULL ? ENOMEM : posix_spawn(child, arguments[0], &actions,
			attributes_ready ? &attributes : NULL, arguments, environment);
		free(inherited);
	}
	if (attributes_ready)
		posix_spawnattr_destroy(&attributes);
	posix_spawn_file_actions_destroy(&actions);
out:
	close(copy);
	close(pipefd[1]);
	if (error != 0)
		close(pipefd[0]);
	else
		*output = pipefd[0];
	return error;
}

static int64_t MonotonicMilliseconds(void)
{
	struct timespec now;
	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return -1;
	return (int64_t)now.tv_sec * MILLISECONDS_PER_SECOND + now.tv_nsec / NANOSECONDS_PER_MILLISECOND;
}

static void DrainDiagnostics(int fd, FORMAT_RESULT* result, size_t* used)
{
	char buffer[FORMAT_DIAGNOSTIC_CAPACITY];
	ssize_t count;
	size_t keep;

	// Bound work per poll so a noisy child cannot starve cancellation.
	for (int i = 0; i < DIAGNOSTIC_READS_PER_POLL; i++) {
		count = read(fd, buffer, sizeof(buffer));
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			break;
		keep = sizeof(result->diagnostics) - 1 - *used;
		if (keep > (size_t)count)
			keep = (size_t)count;
		memcpy(result->diagnostics + *used, buffer, keep);
		*used += keep;
	}
	result->diagnostics[*used] = 0;
}

/* Observe the leader without reaping it first: its PID must remain reserved
 * until we signal the group. Subreaper adoption lets us wait for grandchildren
 * too, without consuming children owned by other parts of the worker. */
static int ReapInstallerGroup(pid_t child, int* status)
{
	bool descendants = false;
	int child_status;
	kill(-child, SIGKILL);
	for (;;) {
		pid_t waited = waitpid(-child, &child_status, WNOHANG);
		if (waited > 0) {
			if (waited == child)
				*status = child_status;
			else
				descendants = true;
			continue;
		}
		if (waited < 0) {
			if (errno == EINTR)
				continue;
			if (errno == ECHILD)
				return descendants ? EIO : 0;
			return errno;
		}
		// A killed parent can still be handing children to us. Do not sync while
		// any group member survives, even if it closed its diagnostic pipe.
		kill(-child, SIGKILL);
		poll(NULL, 0, FORMAT_POLL_MS);
	}
}

static int WaitForFormatter(pid_t child, int output, bool group, bool full_format, FORMAT_RESULT* result)
{
	struct pollfd event = { output, POLLIN, 0 };
	int64_t start = MonotonicMilliseconds(), stopped = 0, now;
	size_t used = 0;
	int status = 0, error = 0;
	const pid_t signal_target = group ? -child : child;
	pid_t waited;

	for (;;) {
		DrainDiagnostics(output, result, &used);
		if (group) {
			siginfo_t info = { 0 };
			waited = waitid(P_PID, child, &info, WEXITED | WNOHANG | WNOWAIT);
			if (waited == 0)
				waited = info.si_pid;
		} else {
			waited = waitpid(child, &status, WNOHANG);
		}
		if (waited == child) {
			if (group) {
				int cleanup_error = ReapInstallerGroup(child, &status);
				if (error == 0)
					error = cleanup_error;
			}
			break;
		}
		if (waited < 0 && errno != EINTR) {
			error = errno;
			if (error != ECHILD) {
				kill(signal_target, SIGKILL);
				if (group)
					ReapInstallerGroup(child, &status);
				else
					while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
			}
			return error;
		}
		now = MonotonicMilliseconds();
		if (error == 0) {
			const bool clock_failed = start < 0 || now < 0;
			const bool timed_out = !full_format && !clock_failed && now - start >= FORMAT_TIMEOUT_MS;
			if (IsRufusJobCancelled())
				error = ECANCELED;
			else if (clock_failed)
				error = EIO;
			else if (timed_out)
				error = ETIMEDOUT;
			if (error != 0) {
				kill(signal_target, SIGTERM);
				stopped = now;
			}
		}
		if (error != 0) {
			const bool clock_failed = now < 0 || stopped < 0;
			const bool grace_expired = !clock_failed && now - stopped >= FORMAT_TERMINATE_GRACE_MS;
			if (clock_failed || grace_expired)
				kill(signal_target, SIGKILL);
		}
		// If the child closes output early, still wait without spinning on POLLHUP.
		if (event.revents & POLLHUP)
			event.fd = -1;
		poll(&event, 1, FORMAT_POLL_MS);
	}
	DrainDiagnostics(output, result, &used);
	if (WIFEXITED(status))
		result->exit_code = WEXITSTATUS(status);
	if (WIFSIGNALED(status))
		result->signal = WTERMSIG(status);
	if (error != 0)
		return error;
	return result->exit_code == 0 ? 0 : EIO;
}

static int RunCommand(int fd, char* const* arguments, char* const* environment,
	const char* directory, bool full_format, FORMAT_RESULT* result)
{
	pid_t child = -1;
	int output = -1, error;
	if (result == NULL)
		return EINVAL;
	memset(result, 0, sizeof(*result));
	result->exit_code = -1;
	if (directory != NULL) {
		int subreaper = 0;
		struct sigaction child_signal;
		const bool absolute_program = arguments != NULL && arguments[0] != NULL && arguments[0][0] == '/';
		if (!absolute_program || directory[0] != '/' || environment == NULL)
			return EINVAL;
		if (prctl(PR_GET_CHILD_SUBREAPER, &subreaper) != 0)
			return errno;
		if (!subreaper)
			return EINVAL;
		if (sigaction(SIGCHLD, NULL, &child_signal) != 0)
			return errno;
		const bool children_auto_reaped = child_signal.sa_handler == SIG_IGN ||
			(child_signal.sa_flags & SA_NOCLDWAIT) != 0;
		if (children_auto_reaped)
			return EINVAL;
		if (IsRufusJobCancelled())
			return ECANCELED;
	}
	error = StartFormatter(fd, arguments, environment, directory, &child, &output);
	if (error != 0)
		return error;
	result->started = true;
	error = WaitForFormatter(child, output, directory != NULL, full_format, result);
	close(output);
	while (fsync(fd) != 0) {
		if (errno == EINTR)
			continue;
		result->sync_error = errno;
		break;
	}
	if (error == 0 && IsRufusJobCancelled())
		error = ECANCELED;
	return error != 0 ? error : result->sync_error;
}

int RunFormatCommand(int fd, char* const* arguments, FORMAT_RESULT* result)
{
	return RunCommand(fd, arguments, NULL, NULL, false, result);
}

int RunFullFormatCommand(int fd, char* const* arguments, FORMAT_RESULT* result)
{
	return RunCommand(fd, arguments, NULL, NULL, true, result);
}

int RunInstallerCommand(int fd, char* const* arguments, char* const* environment,
	const char* directory, FORMAT_RESULT* result)
{
	// A missing working directory must not silently select single-process mode.
	return RunCommand(fd, arguments, environment, directory == NULL ? "" : directory, false, result);
}
