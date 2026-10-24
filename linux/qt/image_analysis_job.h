// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "rufus_job.h"
#include "platform/windows_image.h"
#include "prepare_image_protocol.h"
#include "platform/secure_boot.h"

#include <QDateTime>
#include <QMetaType>

namespace rufus
{

struct ImageAnalysisResult
{
    int status = ISO_SCAN_ERROR;
    ISO_ANALYSIS analysis = {};
    WINDOWS_IMAGE_ANALYSIS windows = {};
    PrepareImageRequest source;
    bool ddRequired = false;
    bool rawImage = false;
    QStringList ffuStores;
    QString problem;
};

// One read-only operation. Results and log messages carry the selection's
// request number so queued delivery remains safe after changing the image.
class ImageAnalysisJob final : public RufusJob
{
    Q_OBJECT

public:
    ImageAnalysisJob(quint64 request, const QString& path, quint64 size,
                     const QDateTime& modified, bool ignoreBootMarker = false,
                     const PrepareImageRequest& selectedSource = {}, bool expertMode = false, const SecureBootPolicy& policy = {}, bool isoSupport = true);

signals:
    void analysisReady(quint64 request, rufus::ImageAnalysisResult result);
    void analysisLog(quint64 request, QString message);

protected:
    void execute() override;

private:
    void forwardLog(QString message);
    void reportFailure(QString message);
    bool imageUnchanged() const;

    quint64 request_;
    QString path_;
    quint64 size_;
    QDateTime modified_;
    int wimInitError_;
    bool ignoreBootMarker_, expertMode_, isoSupport_;
    PrepareImageRequest selectedSource_;
    SecureBootPolicy policy_;
};

} // namespace rufus

Q_DECLARE_METATYPE(rufus::ImageAnalysisResult)
