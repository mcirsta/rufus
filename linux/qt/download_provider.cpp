// SPDX-License-Identifier: GPL-3.0-or-later
// Native adaptation of Fido v1.71, Copyright Pete Batard, GPL-3.0-or-later.
#include "download_provider.h"
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QUrlQuery>
#include <QUuid>
#include <QXmlStreamReader>
#include <cerrno>
#include <cstring>
#include <climits>
namespace rufus
{
namespace
{
constexpr int metadataTimeoutMs=30000,metadataBytes=4*1024*1024,maximumChoices=512;
QUrl query(QUrl url,const QList<QPair<QString,QString>>& fields)
{
    QUrlQuery values; for (const auto& field : fields) values.addQueryItem(field.first,field.second);
    url.setQuery(values); return url;
}
bool textValue(const QString& value)
{
    if (value.isEmpty() || value.size()>512) return false;
    for (QChar character : value) if (character.unicode()<32 || character.unicode()==127) return false;
    return true;
}
QString idValue(const QJsonValue& value)
{
    if (value.isString()) {
        const QString id=value.toString(); bool valid=false; const uint number=id.toUInt(&valid);
        return valid && number>0 && number<=INT_MAX && id==QString::number(number) ? id : QString();
    }
    if (value.isDouble() && value.toDouble()>0 && value.toDouble()<=INT_MAX && value.toDouble()==value.toInt()) return QString::number(value.toInt());
    return {};
}
QString responseError(const QJsonObject& object)
{
    const QJsonArray errors=object.value("Errors").toArray();
    if (errors.isEmpty()) return {};
    const QString message=errors.first().toObject().value("Value").toString();
    return textValue(message) ? message : DownloadProvider::tr("The download service rejected this request.");
}
bool downloadUrl(const QUrl& url)
{
    const bool secure=url.scheme()=="https";
    const bool fixture=url.scheme()=="http" && url.host()=="127.0.0.1";
    return url.isValid() && (secure || fixture) && !url.host().isEmpty() && url.userInfo().isEmpty() && !url.hasFragment();
}
}
DownloadProvider::DownloadProvider(QObject* parent,const DownloadProviderSources& sources)
    : QObject(parent),http_(this),signed_(this),sources_(sources)
{
    connect(&http_,&HttpDownload::completed,this,&DownloadProvider::downloaded);
    connect(&signed_,&SignedDownload::completed,this,&DownloadProvider::providerDownloaded);
    retry_.setSingleShot(true); connect(&retry_,&QTimer::timeout,this,&DownloadProvider::requestSkus);
}
void DownloadProvider::get(Stage stage,const QUrl& url,const QByteArray& referer)
{
    stage_=stage;
    const bool emptyAllowed=stage==Stage::Tags || stage==Stage::DeviceReply;
    const int error=http_.start(url,stage==Stage::Manifest ? 4095 : metadataBytes,metadataTimeoutMs,false,{}, {},referer,emptyAllowed);
    if (error!=0) fail(error);
}
QUrl DownloadProvider::asset(const QUrl& original) const
{
    QUrl base=sources_.assets; base.setPath(original.path()); return base;
}
int DownloadProvider::load()
{
    if (active_) return EBUSY;
    if (!catalog_.products.isEmpty()) { emit catalogReady(); return 0; }
    active_=true; catalog_={}; languages_.clear(); links_.clear();
    get(Stage::Manifest,sources_.manifest); return 0;
}
void DownloadProvider::providerDownloaded(SignedDownloadResult result)
{
    if (!active_ || stage_!=Stage::Provider) return;
    QByteArray script;
    int error=result.error;
    if (error==0) error=DecodeDownloadProvider(result.bytes,script);
    if (error==0) error=ParseDownloadCatalog(script,catalog_);
    if (error!=0) { fail(error,error==ENOTSUP ? tr("This download provider requires a newer Linux Rufus adapter.") : QString()); return; }
    active_=false; stage_=Stage::Idle; emit catalogReady();
}
int DownloadProvider::languages(int product,int release,int edition)
{
    if (active_) return EBUSY;
    if (product<0 || product>=catalog_.products.size()) return EINVAL;
    const auto& selected=catalog_.products[product];
    if (release<0 || release>=selected.releases.size()) return EINVAL;
    if (edition<0 || edition>=selected.releases[release].editions.size()) return EINVAL;
    product_=product; release_=release; edition_=edition; languages_.clear(); links_.clear(); sessions_.clear();
    editions_=selected.releases[release].editions[edition].ids;
    if (selected.id.startsWith("UEFI_SHELL")) {
        languages_.append({"en-us",tr("English (US)"),{}}); emit languagesReady(); return 0;
    }
    active_=true; session_=0; nextSession(); return 0;
}
void DownloadProvider::nextSession()
{
    sessions_.append(QUuid::createUuid().toString(QUuid::WithoutBraces)); attempt_=0;
    get(Stage::Tags,query(sources_.tags,{{"org_id",catalog_.organization},{"session_id",sessions_[session_]}}));
}
void DownloadProvider::requestSkus()
{
    if (!active_) return;
    ++attempt_;
    get(Stage::Skus,query(sources_.microsoft.resolved(QUrl("software-download-connector/api/getskuinformationbyproductedition")),
        {{"profile",catalog_.profile},{"productEditionId",QString::number(editions_[session_])},{"SKU","undefined"},
         {"friendlyFileName","undefined"},{"Locale","en-US"},{"sessionID",sessions_[session_]}}));
}
int DownloadProvider::links(int language)
{
    if (active_) return EBUSY;
    if (language<0 || language>=languages_.size()) return EINVAL;
    language_=language; entry_=0; links_.clear(); active_=true;
    const auto& product=catalog_.products[product_];
    if (product.id.startsWith("UEFI_SHELL")) {
        const QString tag=product.releases[release_].name.section(' ',0,0);
        const QRegularExpression safeTag(QStringLiteral("^[A-Za-z0-9.]+$"));
        if (!safeTag.match(tag).hasMatch()) { active_=false; return EBADMSG; }
        get(Stage::Shell,sources_.assets.resolved(QUrl("pbatard/UEFI-Shell/releases/download/"+tag+"/Version.xml")));
    } else requestLinks();
    return 0;
}
void DownloadProvider::requestLinks()
{
    const auto& selected=languages_[language_].entries[entry_];
    get(Stage::Links,query(sources_.microsoft.resolved(QUrl("software-download-connector/api/GetProductDownloadLinksBySku")),
        {{"profile",catalog_.profile},{"productEditionId","undefined"},{"SKU",selected.sku},{"friendlyFileName","undefined"},
         {"Locale","en-US"},{"sessionID",sessions_[selected.session]}}),"https://www.microsoft.com/software-download/windows11");
}
void DownloadProvider::downloaded(HttpDownloadResult result)
{
    if (!active_) return;
    if (result.error!=0) { fail(result.error); return; }
    switch (stage_) {
    case Stage::Manifest: {
        int error=ParseDownloadManifest(result.bytes,providerUrl_);
        if (error==0) { stage_=Stage::Provider; error=signed_.start(asset(providerUrl_),1024*1024,metadataTimeoutMs); }
        if (error!=0) fail(error);
        return;
    }
    case Stage::Tags:
        get(Stage::Device,query(sources_.device.resolved(QUrl("mdt.js")),{{"instanceId",catalog_.instance},{"PageId","si"},{"session_id",sessions_[session_]}})); return;
    case Stage::Device: {
        const QString text=QString::fromUtf8(result.bytes);
        w_=QRegularExpression(QStringLiteral("[?&]w=([A-F0-9]{1,512})")).match(text).captured(1);
        ticks_=QRegularExpression(QStringLiteral("rticks=\"\\+?([0-9]{1,32})")).match(text).captured(1);
        if (w_.isEmpty() || ticks_.isEmpty()) { fail(EBADMSG); return; }
        get(Stage::DeviceReply,query(sources_.device,{{"session_id",sessions_[session_]},{"CustomerId",catalog_.instance},
            {"PageId","si"},{"w",w_},{"mdt",QString::number(QDateTime::currentMSecsSinceEpoch())},{"rticks",ticks_}})); return;
    }
    case Stage::DeviceReply: requestSkus(); return;
    case Stage::Skus:
    case Stage::Links: {
        QJsonParseError parse={}; const auto document=QJsonDocument::fromJson(result.bytes,&parse);
        if (parse.error!=QJsonParseError::NoError || !document.isObject()) { fail(EBADMSG); return; }
        const auto object=document.object(); const QString error=responseError(object);
        if (stage_==Stage::Skus) {
            const auto skus=object.value("Skus").toArray();
            if (!error.isEmpty() || skus.isEmpty()) {
                if (attempt_<3) retry_.start(2000); else fail(EBADMSG,error);
                return;
            }
            if (skus.size()>maximumChoices) { fail(EFBIG); return; }
            for (const auto& value : skus) {
                const auto sku=value.toObject(); const QString name=sku.value("Language").toString(),display=sku.value("LocalizedLanguage").toString(),id=idValue(sku.value("Id"));
                if (!textValue(name) || !textValue(display) || id.isEmpty()) { fail(EBADMSG); return; }
                int index=0; while (index<languages_.size() && languages_[index].name!=name) ++index;
                if (index==languages_.size()) languages_.append({name,display,{}});
                languages_[index].entries.append({session_,id});
            }
            if (++session_<editions_.size()) nextSession();
            else { active_=false; stage_=Stage::Idle; emit languagesReady(); }
        } else {
            if (!error.isEmpty()) { fail(EACCES,error); return; }
            const auto options=object.value("ProductDownloadOptions").toArray();
            if (options.isEmpty() || options.size()>maximumChoices) { fail(EBADMSG); return; }
            for (const auto& value : options) {
                const auto item=value.toObject(); const QString archs[]={"x86","x64","ARM64"};
                const int type=item.value("DownloadType").toInt(-1); const QUrl url(item.value("Uri").toString(),QUrl::StrictMode);
                if (type<0 || type>2 || !downloadUrl(url)) { fail(EBADMSG); return; }
                links_.append({archs[type],url});
            }
            if (++entry_<languages_[language_].entries.size()) requestLinks();
            else { active_=false; stage_=Stage::Idle; emit linksReady(); }
        }
        return;
    }
    case Stage::Shell: {
        QXmlStreamReader xml(result.bytes); QStringList architectures;
        while (!xml.atEnd()) {
            xml.readNext();
            if (xml.isDTD() || xml.isEntityReference()) { fail(EBADMSG); return; }
            if (xml.isStartElement() && xml.name()==QLatin1String("arch")) {
                const QString arch=xml.readElementText();
                if (!textValue(arch) || architectures.size()>=32) { fail(EBADMSG); return; }
                architectures.append(arch);
            }
        }
        if (xml.hasError() || architectures.isEmpty()) { fail(EBADMSG); return; }
        const auto& product=catalog_.products[product_]; const QString tag=product.releases[release_].name.section(' ',0,0);
        const QString build=editions_.first()==0 ? "RELEASE" : "DEBUG";
        const QString file="UEFI-Shell-"+product.id.section(' ',1,1)+"-"+tag+"-"+build+".iso";
        links_.append({architectures.join(", "),sources_.assets.resolved(QUrl("pbatard/UEFI-Shell/releases/download/"+tag+"/"+file))});
        active_=false; stage_=Stage::Idle; emit linksReady(); return;
    }
    default: fail(EPROTO); return;
    }
}
void DownloadProvider::cancel()
{
    if (!active_) return;
    active_=false; retry_.stop(); http_.cancel(); signed_.cancel(); stage_=Stage::Idle;
    emit failed(ECANCELED,tr("Download cancelled."));
}
void DownloadProvider::fail(int error,const QString& message)
{
    if (!active_) return;
    active_=false; stage_=Stage::Idle; retry_.stop();
    emit failed(error,message.isEmpty() ? tr("Could not obtain download choices: %1").arg(QString::fromLocal8Bit(std::strerror(error))) : message);
}
}
