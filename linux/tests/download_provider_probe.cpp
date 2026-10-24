// SPDX-License-Identifier: GPL-3.0-or-later
// Explicit opt-in metadata smoke check; never run by CTest, never downloads an ISO.
#include "qt/download_provider.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include <QTimer>
class ProviderProbe : public QObject
{
    Q_OBJECT
public:
    rufus::DownloadProvider provider;
    bool requestLinks=false;
    ProviderProbe()
    {
        connect(&provider,&rufus::DownloadProvider::catalogReady,this,&ProviderProbe::loaded);
        connect(&provider,&rufus::DownloadProvider::languagesReady,this,&ProviderProbe::languages);
        connect(&provider,&rufus::DownloadProvider::linksReady,this,&ProviderProbe::links);
        connect(&provider,&rufus::DownloadProvider::failed,this,&ProviderProbe::failed);
    }
    void begin() { const int error=provider.load(); if (error!=0) failed(error,"start"); }
    void loaded()
    {
        QJsonArray products; for (const auto& product : provider.catalog().products) products.append(product.name);
        QTextStream(stdout)<<QJsonDocument(QJsonObject{{"signed_provider",provider.catalog().version},{"products",products}}).toJson(QJsonDocument::Compact)<<'\n';
        if (!requestLinks) { QCoreApplication::exit(0); return; }
        int index=0; while (index<provider.catalog().products.size() && provider.catalog().products[index].id!="Windows10ISO") ++index;
        const int error=provider.languages(index,0,0); if (error!=0) failed(error,"languages");
    }
    void languages()
    {
        int english=0; while (english<provider.languageChoices().size() && provider.languageChoices()[english].name!="English") ++english;
        const int error=provider.links(english); if (error!=0) failed(error,"links");
    }
    void links()
    {
        QJsonArray links;
        for (const auto& link : provider.linkChoices()) links.append(QJsonObject{{"architecture",link.architecture},{"scheme",link.url.scheme()},{"host",link.url.host()},{"file",link.url.fileName()}});
        QTextStream(stdout)<<QJsonDocument(QJsonObject{{"language_count",provider.languageChoices().size()},{"download_options",links},{"iso_downloaded",false}}).toJson(QJsonDocument::Compact)<<'\n'; QCoreApplication::exit(0);
    }
    void failed(int error,const QString& problem)
    {
        QTextStream(stderr)<<"Provider error "<<error<<": "<<problem<<'\n'; QCoreApplication::exit(1);
    }
    void timeout() { provider.cancel(); QCoreApplication::exit(2); }
};
int main(int argc,char** argv)
{
    QCoreApplication app(argc,argv);
    if (app.arguments().size()!=2 || (app.arguments()[1]!="--catalog" && app.arguments()[1]!="--windows10-links")) return 64;
    ProviderProbe probe; probe.requestLinks=app.arguments()[1]=="--windows10-links";
    QTimer::singleShot(0,&probe,&ProviderProbe::begin); QTimer::singleShot(120000,&probe,&ProviderProbe::timeout); return app.exec();
}
#include "download_provider_probe.moc"
