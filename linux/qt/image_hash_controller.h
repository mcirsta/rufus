// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "image_hash_job.h"
#include <QThread>
#include <optional>

namespace rufus
{

enum class ImageHashState { Idle, Running, Cancelling, Complete, Cancelled, Failed };

class ImageHashController final : public QObject
{
    Q_OBJECT

public:
    explicit ImageHashController(QObject* parent = nullptr);
    ~ImageHashController() override;
    bool start(const PrepareImageRequest& source, bool extra);
    bool isActive() const;
    ImageHashState state() const { return state_; }
    const ImageHashResult& result() const { return result_; }
    quint64 processed() const { return processed_; }
    quint64 total() const { return total_; }
    int progressPosition() const;
    QString statusText() const;

public slots:
    void cancel();
    void reset();

signals:
    void changed();
    void logMessage(QString message);
    // Exactly once, after the worker has exited and its cleanup is joined.
    void finished();

private:
    void acceptResult(quint64 request, const rufus::ImageHashResult& result);
    void acceptProgress(quint64 request, quint64 processed, quint64 total);
    void threadFinished();

    QThread thread_;
    ImageHashState state_ = ImageHashState::Idle;
    ImageHashResult result_;
    std::optional<ImageHashResult> pending_;
    quint64 request_ = 0;
    quint64 processed_ = 0;
    quint64 total_ = 0;
};

} // namespace rufus
