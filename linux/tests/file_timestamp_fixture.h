/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <errno.h>
#include <stdbool.h>
#include <time.h>

/* Older Linux kernels stamp file changes from the coarse realtime clock.
 * A same-size write followed by restored mtime must happen in a later tick
 * when the fixture specifically tests the remaining ctime identity check. */
static bool WaitForFileTimestampTick(const struct timespec* initial)
{
	enum { FileTimestampWaitAttempts = 500, FileTimestampPollNanoseconds = 2000000 };
	const struct timespec pause = {0, FileTimestampPollNanoseconds};
	for (unsigned attempt = 0; attempt < FileTimestampWaitAttempts; ++attempt) {
		struct timespec current;
		if (clock_gettime(CLOCK_REALTIME_COARSE, &current) != 0)
			return false;
		const bool later_second = current.tv_sec > initial->tv_sec;
		const bool later_nanosecond = current.tv_sec == initial->tv_sec && current.tv_nsec > initial->tv_nsec;
		if (later_second || later_nanosecond)
			return true;
		if (nanosleep(&pause, NULL) != 0 && errno != EINTR)
			return false;
	}
	return false;
}
