// SPDX-License-Identifier: GPL-3.0-or-later
#include "image_save_job.h"
#include "save_source_process.h"
#include "platform/iso_export.h"
#include <QFileInfo>
#include "platform/image_save_destination.h"
#include "platform/device_selection.h"
#include "platform/native_disk_facts.h"
#include "rufus_reporting.h"
#include <QDBusMessage>
#include <QDBusReply>
#include <QDBusUnixFileDescriptor>
#include <QFile>
#include <cerrno>
#include <fcntl.h>
#include <linux/magic.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace rufus
{
namespace
{
struct CaptureDescriptor
{
    int value = -1;
    ~CaptureDescriptor() { close(); }
    int close()
    {
        if (value < 0) return 0;
        const int descriptor = value; value = -1;
        return ::close(descriptor) == 0 ? 0 : errno;
    }
};
}
ImageSaveJob::ImageSaveJob(const QDBusConnection& connection, const PrepareImageRequest& source, const QString& path, int format, const QString& helper)
    : connection_(connection), source_(source), path_(path), format_(format), helper_(helper)
{
    qRegisterMetaType<ImageSaveResult>(); qRegisterMetaType<ImageSaveState>(); qRegisterMetaType<ImageSavePhase>();
    connect(this, &RufusJob::failed, this, &ImageSaveJob::reportFailure, Qt::DirectConnection);
}

void ImageSaveJob::reportFailure(const QString&)
{
    ImageSaveResult result; result.error = EIO; emit saved(result);
}

void ImageSaveJob::progress(void* context, ImageSavePhase phase, quint64 processed, quint64 total)
{
    auto* job = static_cast<ImageSaveJob*>(context);
    emit job->saveProgress(phase, processed, total);
}

void ImageSaveJob::execute()
{
    ImageSaveResult result;
    ImageSaveDestination destination(path_);
    SaveSourceProcess sourceProcess;
    CaptureDescriptor descriptor;
    const bool optical = source_.deviceClass == DeviceClass::Optical;
    const bool iso = format_ == ImageSaveIso && !optical;
    bool helperSource = false;
    QJsonObject metadata;
    result.error = getuid() == 0 || geteuid() == 0 ? EPERM : destination.open();
    source_.imageDevice = destination.filesystemDevice();
    source_.imageInode = destination.originalInode();
    if (result.error == 0 && iso && !QFileInfo(QStringLiteral(RUFUS_MKISOFS_PATH)).isExecutable()) result.error = ENOENT;
    UDisksObjects objects;
    Device current;
    QStringList mounts;
    if (result.error == 0) result.error = ReadSaveDeviceSnapshot(connection_, source_, objects, current);
    if (result.error == 0 && !ListUDisksTargetMounts(objects, current, source_.imageDevice, mounts, source_.allowFixedUsb, DeviceAccess::Read).isEmpty()) result.error = EBUSY;
    NativeDiskFacts facts;
    if (result.error == 0) result.error = ReadNativeDiskFacts(source_.device, source_.size, facts);
    const bool fileBacked = !facts.backingIdentity.isEmpty();
    helperSource = !optical && (iso || !mounts.isEmpty() || fileBacked);
    if (result.error == 0 && (source_.diskSequence == 0 || ReadDiskSequence(source_.device) != source_.diskSequence)) result.error = ESTALE;
    if (result.error == 0 && IsRufusJobCancelled()) result.error = ECANCELED;
    if (result.error == 0) {
        emit stageChanged(ImageSaveState::Authorizing);
        if (helperSource) result.error = sourceProcess.acquire(source_, iso, helper_, descriptor.value, metadata);
        else {
            QDBusMessage message = QDBusMessage::createMethodCall(source_.serviceOwner, source_.objectPath,
                QStringLiteral("org.freedesktop.UDisks2.Block"), QStringLiteral("OpenDevice"));
            QVariantMap options;
            options.insert(QStringLiteral("flags"), (optical || !source_.lockDevice ? 0 : O_EXCL) | O_CLOEXEC);
            options.insert(QStringLiteral("auth.no_user_interaction"), false);
            message.setArguments({QStringLiteral("r"), options});
            message.setInteractiveAuthorizationAllowed(true);
            const QDBusReply<QDBusUnixFileDescriptor> reply = connection_.call(message, QDBus::Block, 120000);
            if (!reply.isValid()) {
                const QString name = reply.error().name();
                result.error = name.endsWith(QLatin1String("NotAuthorizedDismissed")) ? ECANCELED
                    : name.endsWith(QLatin1String("NotAuthorized")) ? EACCES
                    : name.endsWith(QLatin1String("DeviceBusy")) ? EBUSY : EIO;
                uprintfs(reply.error().message().toUtf8().constData());
            } else if (!reply.value().isValid()) result.error = EBADF;
            else {
                descriptor.value = fcntl(reply.value().fileDescriptor(), F_DUPFD_CLOEXEC, 3);
                if (descriptor.value < 0) result.error = errno;
            }
        }
    }
    if (IsRufusJobCancelled()) result.error = ECANCELED;
    unsigned sector = 0;
    if (result.error == 0 && !iso) result.error = CheckImageSaveDevice(descriptor.value, source_.device, source_.size, source_.diskSequence, sector);
    if (result.error == 0 && iso) {
        struct stat volume;
        struct statvfs filesystem;
        if (fstat(descriptor.value, &volume) != 0 || fstatvfs(descriptor.value, &filesystem) != 0) result.error = errno;
        else if (!S_ISDIR(volume.st_mode) || quint64(volume.st_dev) != metadata.value(QStringLiteral("volumeDevice")).toString().toULongLong()
            || !(filesystem.f_flag & ST_RDONLY)) result.error = ESTALE;
    }
    if (result.error == 0) result.error = CheckSaveDeviceSelection(connection_, source_);
    quint64 maximum = 0;
    if (result.error == 0 && !iso) result.error = ImageSaveMaximumBytes(source_.size, sector, format_, maximum);
    if (result.error == 0 && !iso) {
        struct statvfs space;
        struct statfs filesystem;
        if (fstatvfs(destination.directoryDescriptor(), &space) != 0 || fstatfs(destination.directoryDescriptor(), &filesystem) != 0) result.error = errno;
        else if (filesystem.f_type == MSDOS_SUPER_MAGIC && maximum > UINT32_MAX) result.error = EFBIG;
        else if (format_ != IMG_COMPRESSION_VHDX && (space.f_frsize == 0 || (maximum + space.f_frsize - 1) / space.f_frsize > space.f_bavail)) result.error = ENOSPC;
    }
    if (result.error == 0) result.error = destination.create(format_);
    if (result.error == 0) {
        emit stageChanged(ImageSaveState::Saving);
        if (iso) result.error = ExportIsoImage(descriptor.value, destination.descriptor(), destination.temporaryPath(),
            metadata.value(QStringLiteral("label")).toString(), QStringLiteral(RUFUS_MKISOFS_PATH), progress, this, result);
        else result.error = SaveDeviceImage(descriptor.value, source_.size, sector, destination.descriptor(),
            destination.temporaryPath(), format_, progress, this, result);
    }
    if (result.error == 0) result.error = CheckSaveDeviceSelection(connection_, source_);
    const int sourceClosed = descriptor.close();
    if (sourceClosed != 0 && (result.error == 0 || result.error == ECANCELED)) result.error = sourceClosed;
    if (helperSource) {
        const int helperClosed = sourceProcess.finish();
        if (helperClosed != 0 && (result.error == 0 || result.error == ECANCELED)) result.error = helperClosed;
    }
    if (result.error == 0 && IsRufusJobCancelled()) result.error = ECANCELED;
    if (result.error == 0) {
        emit stageChanged(ImageSaveState::Finalizing);
        result.error = destination.commit();
    }
    result.committed = destination.committed();
    const int cleanup = destination.close();
    if (!destination.recoveryPath().isEmpty())
        emit logMessage(tr("The previous file is preserved at: %1").arg(destination.recoveryPath()));
    if (cleanup != 0 && (result.error == 0 || result.error == ECANCELED)) result.error = cleanup;
    emit saved(result);
}
}
