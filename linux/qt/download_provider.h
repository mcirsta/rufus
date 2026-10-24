// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "http_download.h"
#include "download_urls.h"
#include "signed_download.h"
#include "platform/download_catalog.h"
#include <QObject>
#include <QTimer>
namespace rufus
{
struct DownloadLanguageEntry { int session=0; QString sku; };
struct DownloadLanguage { QString name,display; QList<DownloadLanguageEntry> entries; };
struct DownloadLink { QString architecture; QUrl url; };
struct DownloadProviderSources
{
    QUrl manifest=QUrl(QStringLiteral(RUFUS_URL "/Fido.ver"));
    QUrl assets=QUrl(QStringLiteral("https://github.com/"));
    QUrl microsoft=QUrl(QStringLiteral("https://www.microsoft.com/"));
    QUrl tags=QUrl(QStringLiteral("https://vlscppe.microsoft.com/tags"));
    QUrl device=QUrl(QStringLiteral("https://ov-df.microsoft.com/"));
};
// Native adapter for Fido171: signed data + original API sequence, no script execution.
class DownloadProvider final : public QObject
{
    Q_OBJECT
public:
    explicit DownloadProvider(QObject* parent=nullptr,const DownloadProviderSources& sources={});
    int load();
    int languages(int product,int release,int edition);
    int links(int language);
    void cancel();
    bool isActive() const { return active_; }
    const DownloadCatalog& catalog() const { return catalog_; }
    const QList<DownloadLanguage>& languageChoices() const { return languages_; }
    const QList<DownloadLink>& linkChoices() const { return links_; }
signals:
    void catalogReady();
    void languagesReady();
    void linksReady();
    void failed(int error,QString problem);
private:
    enum class Stage { Idle,Manifest,Provider,Tags,Device,DeviceReply,Skus,Links,Shell };
    void downloaded(rufus::HttpDownloadResult result);
    void providerDownloaded(rufus::SignedDownloadResult result);
    void requestSkus();
    void nextSession();
    void requestLinks();
    void fail(int error,const QString& message={});
    void get(Stage stage,const QUrl& url,const QByteArray& referer={});
    QUrl asset(const QUrl& original) const;
    HttpDownload http_;
    SignedDownload signed_;
    DownloadProviderSources sources_;
    DownloadCatalog catalog_;
    QList<DownloadLanguage> languages_;
    QList<DownloadLink> links_;
    QStringList sessions_;
    QList<int> editions_;
    QUrl providerUrl_;
    QTimer retry_;
    Stage stage_=Stage::Idle;
    int product_=0,release_=0,edition_=0,session_=0,language_=0,entry_=0,attempt_=0;
    QString w_,ticks_;
    bool active_=false;
};
}
