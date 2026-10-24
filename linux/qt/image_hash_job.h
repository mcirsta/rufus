// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "rufus_job.h"
#include "prepare_image_protocol.h"

#include <QStringList>
#include <cerrno>

namespace rufus
{

struct ImageHashResult
{
    QString path;
    QStringList digests; // Original order: MD5, SHA-1, SHA-256, optional SHA-512.
    QString problem;
    int error = EIO;
};

// One read-only operation. Source identity and optional algorithms are frozen
// before moving to the worker. The controller publishes only after joining it.
class ImageHashJob final : public RufusJob
{
    Q_OBJECT

public:
    ImageHashJob(quint64 request, const PrepareImageRequest& source, bool extra);

signals:
    void hashesReady(quint64 request, rufus::ImageHashResult result);
    void hashProgress(quint64 request, quint64 processed, quint64 total);

protected:
    void execute() override;

private:
    void forwardProgress(int operation, int message, quint64 processed, quint64 total, bool forced);
    void reportFailure(const QString& problem);

    const quint64 request_;
    const PrepareImageRequest source_;
    const bool extra_;
};

} // namespace rufus

Q_DECLARE_METATYPE(rufus::ImageHashResult)
