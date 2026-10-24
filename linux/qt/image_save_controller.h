// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "image_save_job.h"
#include "platform/device.h"
#include <QThread>
#include <optional>

namespace rufus
{
class ImageSaveController : public QObject
{
    Q_OBJECT
public:
    explicit ImageSaveController(QObject* parent = nullptr);
    ~ImageSaveController() override;
    bool start(const Device& device, const QString& path, bool allowFixedUsb);
    void setConnection(const QDBusConnection& connection);
    void setHelper(const QString& helper) { if (!isActive()) helper_ = helper; }
    void cancel();
    void reset();
    bool isActive() const;
    ImageSaveState state() const { return state_; }
    const ImageSaveResult& result() const { return result_; }
    const QString& path() const { return path_; }
    QString statusText() const;
    int progressPosition() const;
    quint64 processedBytes() const { return processed_; }
    quint64 totalBytes() const { return total_; }
signals:
    void changed();
    void logMessage(QString message);
    void finished();
private:
    void stageChanged(ImageSaveState state);
    void progress(ImageSavePhase phase, quint64 processed, quint64 total);
    void acceptResult(const ImageSaveResult& result);
    void threadFinished();
    QDBusConnection connection_;
    QThread thread_;
    ImageSaveState state_ = ImageSaveState::Idle;
    ImageSavePhase phase_ = ImageSavePhase::Copy;
    ImageSaveResult result_;
    std::optional<ImageSaveResult> pending_;
    QString path_;
    QString helper_;
    bool iso_ = false;
    quint64 processed_ = 0, total_ = 0;
};
}
