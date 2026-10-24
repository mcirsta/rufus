// SPDX-License-Identifier: GPL-3.0-or-later
#include "raw_device.h"
#include "prepare_badblocks.h"
#include "raw_write_plan.h"
#include "prepare_device.h"
#include "prepare_source.h"
#include "rufus_reporting.h"
#include "compressed_write.h"
#include "compressed_ffu.h"
#include "ffu_finalize.h"

#include <QFile>
#include <cerrno>
#include <cstring>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/fs.h>

namespace rufus
{
static int CollectFfuPartition(void* context, const FFU_DEPLOYMENT_PARTITION* partition)
{
    static_cast<QVector<FFU_DEPLOYMENT_PARTITION>*>(context)->append(*partition);
    return 0;
}

static int ZeroRawDevice(const PrepareImageRequest& request, uid_t caller, RAW_WRITE_RESULT& result)
{
    PrepareImageRequest checked;
    if (!DecodePrepareRequest(EncodePrepareRequest(request), checked)) return EINVAL;
    PrepareDeviceTarget target(request, PrepareDeviceTarget::Table::Raw);
    int error = target.open(caller, nullptr);
    if (error == 0) error = RunPrepareBadBlocks(request, target.descriptor());
    if (error == 0)
        error = ZeroDeviceData(target.descriptor(), request.device, request.size, request.zeroMode, &result);
    if (error == 0 && ioctl(target.descriptor(), BLKRRPART) != 0) error = errno;
    if (error == 0) error = target.check();
    const int cleanup = target.finishRaw();
    if (cleanup != 0 && (error == 0 || error == ECANCELED)) error = cleanup;
    return error;
}

int WriteRawDevice(const PrepareImageRequest& request, uid_t caller, RAW_WRITE_RESULT& result)
{
    result = {};
    const bool authorized = getuid() == 0 && geteuid() == 0 && caller != 0;
    if (!authorized || !request.isDevice)
        return EPERM;
    if (request.zeroMode != ZERO_NONE)
        return ZeroRawDevice(request, caller, result);
    if (request.imagePath.isEmpty())
        return EINVAL;
    const bool compressed = request.hasCompressedImage();
    if (compressed)
    {
        PrepareImageRequest checked;
        if (!DecodePrepareRequest(EncodePrepareRequest(request), checked))
            return EINVAL;
    }

    PrepareIsoSource source(request);
    PrepareDeviceTarget target(request, PrepareDeviceTarget::Table::Raw);
    int error = source.open(caller);
    COMPRESSED_WRITE_PLAN plan = {};
    if (error == 0 && compressed)
    {
        const QByteArray path = QFile::encodeName(request.imagePath);
        error = PrepareCompressedWrite(&source.retainedSource(), path.constData(),
            request.decodedImageSize, request.imageSectorSize, request.ignoreBootMarker, &plan);
        if (error == EFBIG)
            error = ESTALE;
        if (error == 0)
        {
            const bool sameSize = plan.image.decoded_bytes == request.decodedImageSize;
            const bool sameMinimum = plan.image.ffu_minimum_bytes == request.ffuMinimumSize;
            const bool sameHash = memcmp(plan.decoded_sha256, request.decodedImageSha256.constData(),
                sizeof(plan.decoded_sha256)) == 0;
            const bool sameLayout = (request.imageCompression != BLED_COMPRESSION_VTSI && request.imageCompression != IMG_COMPRESSION_FFU)
                || memcmp(plan.layout_sha256, request.imageLayoutSha256.constData(),
                    sizeof(plan.layout_sha256)) == 0;
            if (!sameSize || !sameMinimum || !sameHash || !sameLayout)
                error = ESTALE;
        }
    }
    if (error == 0 && !compressed)
    {
        // Inspect only the retained descriptor after ownership/readability checks.
        // The duplicate is read-only and closed before target acquisition.
        IMAGE_FILE_SOURCE inspected = {-1, {}, false, false};
        error = OpenImageFileSource(source.retainedPath(), &inspected);
        if (error == 0)
        {
            const QString problem = CheckRawImageSource(inspected, request.imagePath, request.ignoreBootMarker);
            if (!problem.isEmpty())
            {
                uprintfs(problem.toUtf8().constData());
                error = ENOTSUP;
            }
        }
        CloseImageFileSource(&inspected);
    }
    if (error == 0 && request.badBlocksPattern >= 0 && request.imageCompression == IMG_COMPRESSION_FFU)
    {
        bool requiresBase = false;
        error = FfuRequiresBase(&source.retainedSource(), &requiresBase);
        if (error == 0 && requiresBase)
        {
            uprintf("Clear the bad-block test option to apply this partial FFU: it requires the existing device contents.");
            error = ENOTSUP;
        }
    }
    const quint64 imageBytes = compressed ? PrepareRawWriteBytes(request) : request.imageSize;
    if (error == 0 && imageBytes > request.size)
        error = ENOSPC;
    if (error == 0)
        error = source.check();
    FFU_DEPLOYMENT_INFO ffu = {};
    QVector<FFU_DEPLOYMENT_PARTITION> ffuPartitions;
    if (error == 0 && request.ffuMinimumSize != 0)
    {
        error = PlanFfuDeployment(&source.retainedSource(), request.size, request.imageSectorSize,
            plan.layout_sha256, &ffu, CollectFfuPartition, &ffuPartitions);
        if (error == 0 && (!ffu.optimized || ffu.minimum_bytes != request.ffuMinimumSize)) error = ESTALE;
        if (error == 0) error = CheckFfuFinalization(target, ffu, ffuPartitions);
    }

    // Acquisition uses the same identity, source/home and native mount checks
    // as preparation. Optimized FFU finalization uses checked restored partitions.
    if (error == 0)
        error = target.open(caller, &source);
    if (error == 0) error = RunPrepareBadBlocks(request, target.descriptor());
    if (error == 0) error = source.check();
    if (error == 0)
    {
        const QByteArray path = QFile::encodeName(request.imagePath);
        if (compressed)
            error = WriteCompressedImageToDevice(&source.retainedSource(), path.constData(), &plan,
                target.descriptor(), request.device, request.size, &result);
        else
            error = WriteRawImageToDevice(&source.retainedSource(), path.constData(),
                target.descriptor(), request.device, request.size, &result);
    }
    if (error == 0)
        error = target.check();
    if (error == 0 && ffu.optimized)
        error = FinishFfuDeployment(target, ffu, ffuPartitions, request.imageSectorSize);
    if (error == 0) error = source.check();
    const int closedTarget = target.finishRaw();
    const int closedSource = source.close();
    const int cleanupError = closedTarget != 0 ? closedTarget : closedSource;
    if (cleanupError != 0)
    {
        uprintf("Cannot close raw image job: %s", strerror(cleanupError));
        if (error == 0 || error == ECANCELED)
            error = cleanupError;
    }
    return error;
}
}
