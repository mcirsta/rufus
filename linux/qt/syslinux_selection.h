// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "signed_download.h"
#include "grub2_core_selection.h"
#include "platform/syslinux_resources.h"
#include "platform/prepare_source.h"
#include <memory>
namespace rufus {
class SyslinuxSelection final : public QObject {
    Q_OBJECT
public:
    explicit SyslinuxSelection(QObject* parent = nullptr);
    int start(const PrepareImageRequest& source, const RUFUS_IMG_REPORT& report, const SyslinuxResources& initial,
        const QString& directory, const QUrl& server);
    void choose(Grub2DownloadChoice choice);
    void cancel();
    bool isActive() const { return active_; }
signals:
    void downloadRequired(QString version);
    void progress(quint64 received, quint64 total);
    void completed(int error, QByteArray resources);
private:
    struct Item { QString full, base; int part; bool optional; };
    void next();
    void downloaded(SignedDownloadResult result);
    void finish(int error);
    int readCache(const QString& path,QByteArray& bytes);
    SignedDownload download_;
    PrepareImageRequest request_;
    RUFUS_IMG_REPORT report_ = {};
    SyslinuxResources resources_;
    QList<Item> missing_;
    QString directory_;
    QUrl server_;
    std::unique_ptr<PrepareIsoSource> source_;
    bool active_ = false, waiting_ = false, base_ = false;
};
}
