// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "platform/native_disk_facts.h"

// Link-only replacement for tests with invented UDisks dev_t metadata. The
// production reader is separately exercised against real temporary sysfs trees.
rufus::NativeDiskFacts& TestNativeDisk(quint64 device);
void ResetTestNativeDisks();
extern int testNativeDiskError;
