// SPDX-License-Identifier: GPL-3.0-or-later
#include "windows_apply.h"
#include "target_io.h"
#include "rufus_reporting.h"
#include "progress_policy.h"
extern "C" {
#include "native_ntfs_efs.h"
}

#include <QSet>
#include <QtEndian>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <utility>

namespace rufus
{
namespace
{
struct PlanContext
{
    WindowsApplyPlan plan;
    QSet<quint64> inodes;
    int error = 0;
};

int countEntry(const wimlib_dir_entry* entry, void* opaque)
{
    auto& context = *static_cast<PlanContext*>(opaque);
    if (IsIsoScanCancelled() || IsRufusJobCancelled())
        return context.error = ECANCELED;
    const quint32 unsupported = WIMLIB_FILE_ATTRIBUTE_DEVICE;
    if (entry->attributes & unsupported)
        return context.error = ENOTSUP;
    if (entry->attributes & WIMLIB_FILE_ATTRIBUTE_DIRECTORY)
        ++context.plan.directories;
    else
        ++context.plan.files;
    // Single-link entries may all use zero. Only multiply linked inodes have
    // a unique group ID; count their payload once without losing aliases.
    if (entry->num_links > 1)
    {
        if (context.inodes.contains(entry->hard_link_group_id))
        {
            ++context.plan.hardLinks;
            return 0;
        }
        context.inodes.insert(entry->hard_link_group_id);
    }
    for (quint64 stream = 0; stream <= entry->num_named_streams; ++stream)
    {
        const quint64 bytes = entry->streams[stream].resource.uncompressed_size;
        if (bytes > quint64(INT64_MAX) - context.plan.bytes)
            return context.error = EFBIG;
        context.plan.bytes += bytes;
    }
    return 0;
}

struct ApplyContext
{
    const IMAGE_FILE_SOURCE& source;
    const char* path;
    WindowsApplyResult& result;
    int error = 0;
};

wimlib_progress_status applyProgress(wimlib_progress_msg message,
    wimlib_progress_info* info, void* opaque)
{
    auto& context = *static_cast<ApplyContext*>(opaque);
    context.error = CheckImageFileSource(&context.source, context.path);
    if (context.error == 0 && IsRufusJobCancelled())
        context.error = ECANCELED;
    if (context.error != 0)
        return WIMLIB_PROGRESS_STATUS_ABORT;
    if (message == WIMLIB_PROGRESS_MSG_EXTRACT_STREAMS)
    {
        context.result.completedBytes = info->extract.completed_bytes;
        context.result.totalBytes = info->extract.total_bytes;
        _UpdateProgressWithInfo(OP_FILE_COPY, 0, info->extract.completed_bytes, info->extract.total_bytes, 0);
    }
    return WIMLIB_PROGRESS_STATUS_CONTINUE;
}

int nativeError(int error)
{
    if (error == 0) return 0;
    if (error == WIMLIB_ERR_ABORTED_BY_PROGRESS) return ECANCELED;
    if (error == WIMLIB_ERR_NOMEM) return ENOMEM;
    if (error == WIMLIB_ERR_INVALID_IMAGE || error == WIMLIB_ERR_INVALID_PARAM) return EINVAL;
    if (error == WIMLIB_ERR_UNSUPPORTED) return ENOTSUP;
    return EIO;
}
}

int ReadWindowsApplyPlan(WIMStruct* wim, int image, WindowsApplyPlan& plan)
{
    plan = {};
    if (wim == nullptr || image < 1)
        return EINVAL;
    if (IsIsoScanCancelled() || IsRufusJobCancelled())
        return ECANCELED;
    PlanContext context;
    int error = ReadWimMetadata(wim, image, &context.plan.metadata);
    if (error == 0)
    {
        constexpr int flags = WIMLIB_ITERATE_DIR_TREE_FLAG_RECURSIVE
            | WIMLIB_ITERATE_DIR_TREE_FLAG_RESOURCES_NEEDED;
        error = wimlib_iterate_dir_tree(wim, image, "/", flags, countEntry, &context);
        if (error == 0) error = ValidateWimNtfsEncryptedImage(wim, image, &context.plan.efsClusterLimit);
    }
    if (IsIsoScanCancelled() || IsRufusJobCancelled())
        return ECANCELED;
    if (context.error != 0)
        return context.error;
    if (error != 0)
    {
        plan.wimError = error;
        return nativeError(error);
    }
    plan = std::move(context.plan);
    return 0;
}

int ApplyWindowsImage(WIMStruct* wim, int image, const IMAGE_FILE_SOURCE& source,
    const char* sourcePath, int target, quint64 device, quint64 size,
    WindowsApplyResult& result)
{
    result = {};
    if (sourcePath == nullptr || *sourcePath == 0 || size == 0)
        return EINVAL;
    int error = CheckImageFileSource(&source, sourcePath);
    if (error == 0)
        error = ReadWindowsApplyPlan(wim, image, result.plan);
    if (error != 0)
        return error;
    const int access = fcntl(target, F_GETFL);
    if (access < 0) return errno;
    if ((access & O_ACCMODE) != O_RDWR) return EBADF;
    WRITABLE_TARGET before = {};
    error = InspectWritableTarget(target, device, &before);
    if (error != 0) return error;
    if (before.size != size) return ESTALE;
    const bool sameSource = before.info.st_dev == source.info.st_dev && before.info.st_ino == source.info.st_ino;
    if (sameSource) return EBUSY;
    if (result.plan.efsClusterLimit != 0) {
        unsigned char boot[14];
        if (pread(target, boot, sizeof(boot), 0) != sizeof(boot)) return EIO;
        if (memcmp(boot + 3, "NTFS    ", 8) != 0) return EINVAL;
        const quint32 cluster = quint32(qFromLittleEndian<quint16>(boot + 11)) * boot[13];
        if (cluster == 0 || cluster > result.plan.efsClusterLimit) return ENOTSUP;
    }
    if (result.plan.bytes > size) return ENOSPC;
    error = CheckImageFileSource(&source, sourcePath);
    if (error != 0) return error;
    if (IsRufusJobCancelled()) return ECANCELED;

    char retainedTarget[64];
    snprintf(retainedTarget, sizeof(retainedTarget), "/proc/self/fd/%d", target);
    ApplyContext context = {source, sourcePath, result};
    wimlib_register_progress_function(wim, applyProgress, &context);
    result.started = true;
    constexpr int flags = WIMLIB_EXTRACT_FLAG_NTFS | WIMLIB_EXTRACT_FLAG_STRICT_ACLS
        | WIMLIB_EXTRACT_FLAG_STRICT_SHORT_NAMES | WIMLIB_EXTRACT_FLAG_STRICT_TIMESTAMPS;
    result.wimError = wimlib_extract_image(wim, image, retainedTarget, flags);
    wimlib_register_progress_function(wim, nullptr, nullptr);
    error = context.error != 0 ? context.error : nativeError(result.wimError);
    if (fsync(target) != 0)
        result.syncError = errno;
    if (error == 0) error = result.syncError;
    const int sourceError = CheckImageFileSource(&source, sourcePath);
    if (error == 0) error = sourceError;
    WRITABLE_TARGET after = {};
    const int targetError = InspectWritableTarget(target, device, &after);
    if (error == 0) error = targetError;
    if (error == 0 && after.size != size) error = ESTALE;
    if (error == 0 && IsRufusJobCancelled()) error = ECANCELED;
    return error;
}
}
