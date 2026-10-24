// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "rufus_job.h"
#include "platform/image_save.h"
#include "prepare_image_protocol.h"
#include <QDBusConnection>

namespace rufus
{
enum class ImageSaveState { Idle, Checking, Authorizing, Saving, Verifying, Finalizing, Cancelling, Complete, Cancelled, Failed };

class ImageSaveJob final : public RufusJob
{
    Q_OBJECT
public:
    ImageSaveJob(const QDBusConnection& connection, const PrepareImageRequest& source, const QString& path, int format, const QString& helper);
signals:
    void stageChanged(rufus::ImageSaveState stage);
    void saveProgress(rufus::ImageSavePhase phase, quint64 processed, quint64 total);
    void saved(rufus::ImageSaveResult result);
protected:
    void execute() override;
private:
    static void progress(void* context, ImageSavePhase phase, quint64 processed, quint64 total);
    void reportFailure(const QString& problem);
    const QDBusConnection connection_;
    PrepareImageRequest source_;
    const QString path_;
    const int format_;
    const QString helper_;
};
}
Q_DECLARE_METATYPE(rufus::ImageSaveResult)
Q_DECLARE_METATYPE(rufus::ImageSaveState)
Q_DECLARE_METATYPE(rufus::ImageSavePhase)
