// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QList>
#include <QtGlobal>
#include <sys/types.h>

namespace rufus
{
// Isolate the calling worker and its children after exclusive target acquisition.
// Other Qt threads keep their original namespace. Clean temporary mounts before
// the worker exits; do not use process-wide /proc/self mount views in this worker.
int CreatePrivateMountNamespace();
// Resolve a non-root authenticated UID using the system account database, never
// HOME. On success the caller owns an O_PATH directory descriptor until cleanup.
int OpenNativeCallerHome(uid_t caller, int& descriptor);

struct NativeTargetMount
{
    QByteArray path;
    quint64 device = 0;
    int id = 0;
    quint64 backing = 0; // Btrfs filesystem dev_t is anonymous; match UDisks by its verified member.
};

struct NativeTargetMounts
{
    quint64 device = 0;
    quint64 diskSequence = 0;
    QList<NativeTargetMount> volumes;
};

// Current namespace only. Borrow an already validated whole-disk descriptor and
// the retained source descriptor (-1 for source-free formatting). No writes.
// Fail closed for layered/ambiguous layouts; output is empty on any error.
// callerHome is a retained directory opened for the authenticated caller; -1
// omits that check only for standalone native qualification without a caller.
int CheckNativeTargetMounts(int disk, int source, NativeTargetMounts& result, int callerHome = -1);

// Root-only, synchronous native unmount, without force/lazy flags or retries.
// Revalidate the complete remaining plan before each call. Cancellation is
// checked between calls; an in-progress kernel unmount must finish before return.
// Report partial completion on error. Never remount on error or cancellation.
// This is not device authorization or an exclusive claim. The caller must match
// these mounts to fresh UDisks candidates, validate source identity and protected
// caller paths, handle other namespaces, then acquire O_EXCL before any writes.
int UnmountNativeTarget(int disk, int source, const NativeTargetMounts& expected, int& unmounted,
    int callerHome = -1);
}
