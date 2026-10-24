// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace rufus
{
constexpr int MinimumJobPriority = -2;
constexpr int MaximumJobPriority = 2;
constexpr int DefaultJobPriority = 1;

// Five Windows thread levels map to reversible native best-effort I/O levels.
// The already-authorized helper additionally sets its worker's CPU nice value.
// Neither operation changes GUI scheduling or requests separate authorization.
int JobPriority();
int SetJobPriority(int level);

class JobPriorityScope
{
public:
    JobPriorityScope();
    ~JobPriorityScope();
    JobPriorityScope(const JobPriorityScope&) = delete;
    JobPriorityScope& operator=(const JobPriorityScope&) = delete;
    int error() const { return error_; }
private:
    int thread_ = 0, originalIo_ = -1, originalNice_ = 0, error_ = 0;
};
}
