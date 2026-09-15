// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "platform/device.h"
#include "image_analysis_job.h"
#include "raw_write_controller.h"
#include "image_policy.h"

#include <QObject>
#include <QString>
#include <QThread>

namespace rufus
{

enum class ImageState
{
    None, Scanning, Cancelling, Complete, Partial, Failed, Cancelled
};

struct Selection
{
    QList<Device> devices;
    QString deviceId;
    QString imagePath;
    quint64 imageSize = 0;
    QString imageProblem;
    ImageState imageState = ImageState::None;
    ISO_ANALYSIS imageAnalysis = {};
    WINDOWS_IMAGE_ANALYSIS windowsAnalysis = {};
    bool discoveryAvailable = false;
    QString discoveryProblem;
    bool rawImage = false;
};

// Selection serializes analysis and raw writing; each operation owns its job.
class SelectionController : public QObject
{
    Q_OBJECT

public:
    explicit SelectionController(QObject* parent = nullptr);
    ~SelectionController() override;
    const Selection& selection() const;
    bool isAnalyzing() const;
    QString imageStatusText() const;
    void setDevices(const QList<Device>& devices);
    RawWriteController& writer() { return writer_; }
    bool canStartWrite() const;
    IMAGE_WRITE_CHOICES isoWriteChoices(quint64 persistenceSize, bool gpt, bool fat) const;

public slots:
    void setDiscovery(const QList<rufus::Device>& devices, const QString& problem);
    void selectDevice(const QString& id);
    void selectImage(const QString& path);
    void cancelAnalysis();
    void startWrite();

signals:
    void selectionChanged();
    void logMessage(QString message);

private:
    void startAnalysis();
    void acceptAnalysis(quint64 request, const rufus::ImageAnalysisResult& result);
    void acceptAnalysisLog(quint64 request, const QString& message);
    void analysisFinished();
    void logImageReport(bool ddRequired);
    void logWindowsMetadata();
    QString pendingChecks() const;

    Selection selection_;
    RawWriteController writer_;
    QDateTime imageModified_;
    QThread analysisThread_;
    bool analysisActive_ = false;
    quint64 imageRequest_ = 0;
    quint64 runningRequest_ = 0;
};

} // namespace rufus
