// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "http_download.h"
#include "platform/msdos_image.h"

namespace rufus
{
struct MsDosSelectionResult
{
    int error = 0;
    QString path;
    QByteArray bytes;
};
class MsDosSelection final : public QObject
{
    Q_OBJECT
public:
    explicit MsDosSelection(QObject* parent = nullptr);
    int start(const QString& directory, const QUrl& url = QUrl(QStringLiteral(DISKCOPY_URL)));
    void choose(bool accept);
    void cancel();
    bool isActive() const { return active_; }
signals:
    void downloadRequired();
    void progress(quint64 received, quint64 total);
    void completed(rufus::MsDosSelectionResult result);
private:
    void downloaded(rufus::HttpDownloadResult result);
    void finish(int error, QByteArray bytes = {});
    HttpDownload download_;
    QString directory_;
    QUrl url_;
    bool active_ = false, waiting_ = false;
};
}
Q_DECLARE_METATYPE(rufus::MsDosSelectionResult)
