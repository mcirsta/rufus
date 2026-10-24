// SPDX-License-Identifier: GPL-3.0-or-later
#include "job_priority.h"
#include <cerrno>
#include <mutex>
#include <linux/ioprio.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace rufus
{
namespace
{
std::mutex priorityMutex;
int selectedPriority = DefaultJobPriority;
int activeThread = 0;

int applyPriority(int thread)
{
    constexpr int ioLevels[] = {7, 6, 4, 2, 0};
    constexpr int cpuLevels[] = {10, 5, 0, -5, -10};
    const int index = selectedPriority - MinimumJobPriority;
    const int io = IOPRIO_PRIO_VALUE(IOPRIO_CLASS_BE, ioLevels[index]);
    int error = syscall(SYS_ioprio_set, IOPRIO_WHO_PROCESS, thread, io) == 0 ? 0 : errno;
    // Do not lower an unprivileged thread's CPU nice value: it could not
    // necessarily restore it on the next shortcut. I/O priority is reversible.
    if (geteuid() == 0 && setpriority(PRIO_PROCESS, id_t(thread), cpuLevels[index]) != 0 && error == 0)
        error = errno;
    return error;
}
}

int JobPriority()
{
    std::lock_guard<std::mutex> lock(priorityMutex);
    return selectedPriority;
}

int SetJobPriority(int level)
{
    if (level < MinimumJobPriority || level > MaximumJobPriority) return EINVAL;
    std::lock_guard<std::mutex> lock(priorityMutex);
    selectedPriority = level;
    return activeThread == 0 ? 0 : applyPriority(activeThread);
}

JobPriorityScope::JobPriorityScope()
{
    std::lock_guard<std::mutex> lock(priorityMutex);
    // RufusJob's operation mutex guarantees one active job per process.
    if (activeThread != 0) { error_ = EBUSY; return; }
    thread_ = int(syscall(SYS_gettid));
    originalIo_ = int(syscall(SYS_ioprio_get, IOPRIO_WHO_PROCESS, thread_));
    originalNice_ = getpriority(PRIO_PROCESS, id_t(thread_));
    activeThread = thread_;
    error_ = applyPriority(thread_);
}

JobPriorityScope::~JobPriorityScope()
{
    std::lock_guard<std::mutex> lock(priorityMutex);
    if (thread_ == 0) return;
    if (originalIo_ >= 0) (void)syscall(SYS_ioprio_set, IOPRIO_WHO_PROCESS, thread_, originalIo_);
    if (geteuid() == 0) (void)setpriority(PRIO_PROCESS, id_t(thread_), originalNice_);
    activeThread = 0;
}
}
