// SPDX-License-Identifier: GPL-3.0-or-later
// Fido's catalog layout is Copyright Pete Batard, GPL-3.0-or-later.
#include "download_catalog.h"
#include "download_urls.h"
extern "C" {
#include "compressed_lzma.h"
}
#include <QRegularExpression>
#include <QStringConverter>
#include <QTemporaryFile>
#include <sys/stat.h>
#include <cerrno>
#include <climits>
#include <QtEndian>

namespace rufus
{
namespace
{
constexpr qsizetype maximumScriptBytes=1024*1024,maximumStringUnits=512;
constexpr int maximumCatalogItems=2048,maximumArrayDepth=6;
struct Literal { QString text; int number=-1; QList<Literal> array; bool list=false; };
class CatalogReader
{
public:
    explicit CatalogReader(QStringView text) : text_(text) {}
    int read(Literal& value,int depth=0)
    {
        skip();
        if (++items_>maximumCatalogItems || depth>maximumArrayDepth || position_>=text_.size()) return EBADMSG;
        const QChar first=text_[position_++];
        if (first=='@') {
            if (position_>=text_.size() || text_[position_++]!='(') return EBADMSG;
            value.list=true;
            while (true) {
                skip(); if (position_>=text_.size()) return EBADMSG;
                if (text_[position_]==')') { ++position_; return 0; }
                Literal item; const int error=read(item,depth+1); if (error!=0) return error;
                value.array.append(std::move(item)); skip();
                if (position_<text_.size() && text_[position_]==',') ++position_;
            }
        }
        if (first=='"' || first=='\'') {
            bool closed=false;
            while (position_<text_.size()) {
                const QChar character=text_[position_++];
                if (character==first) { closed=true; break; }
                // Data literals only: no interpolation, escapes, code or controls.
                if (character=='$' || character=='`' || character.unicode()<32 || value.text.size()>=maximumStringUnits) return EBADMSG;
                value.text.append(character);
            }
            return closed && !value.text.isEmpty() ? 0 : EBADMSG;
        }
        if (first<'0' || first>'9') return EBADMSG;
        quint64 number=first.unicode()-'0';
        while (position_<text_.size() && text_[position_]>='0' && text_[position_]<='9') {
            number=number*10+text_[position_++].unicode()-'0'; if (number>INT_MAX) return ERANGE;
        }
        value.number=int(number); return 0;
    }
    bool atDataEnd() { skip(); return text_.sliced(position_).startsWith(QStringLiteral("#endregion")); }
private:
    void skip()
    {
        while (position_<text_.size()) {
            if (text_[position_].isSpace()) { ++position_; continue; }
            if (text_[position_]=='#' && !text_.sliced(position_).startsWith(QStringLiteral("#endregion"))) {
                while (position_<text_.size() && text_[position_]!='\n') ++position_;
                continue;
            }
            break;
        }
    }
    QStringView text_; qsizetype position_=0; int items_=0;
};
QString assignment(const QString& text,const QString& name)
{
    const QRegularExpression pattern(QStringLiteral("(?m)^\\$")+name+QStringLiteral("\\s*=\\s*\"([A-Za-z0-9-]{1,128})\"\\s*$"));
    auto matches=pattern.globalMatch(text); if (!matches.hasNext()) return {};
    const QString result=matches.next().captured(1); return matches.hasNext() ? QString() : result;
}
int collect(void* context,const uint8_t* data,size_t size,uint64_t offset)
{
    auto* bytes=static_cast<QByteArray*>(context);
    if (offset!=quint64(bytes->size()) || size>size_t(maximumScriptBytes-bytes->size())) return EFBIG;
    bytes->append(reinterpret_cast<const char*>(data),qsizetype(size)); return 0;
}
}
int ParseDownloadManifest(const QByteArray& bytes,QUrl& provider)
{
    provider.clear();
    if (bytes.isEmpty() || bytes.size()>=4096 || bytes.contains('\0')) return EBADMSG;
    QByteArray candidate;
    for (QByteArray line : bytes.split('\n')) {
        line=line.trimmed(); if (line.startsWith('#') || line.startsWith(';')) continue;
        const qsizetype equals=line.indexOf('=');
        if (equals<0 || line.left(equals).trimmed()!=FIDO_VERSION) continue;
        if (!candidate.isEmpty()) return EBADMSG;
        candidate=line.mid(equals+1).trimmed();
    }
    const QUrl url=QUrl::fromEncoded(candidate,QUrl::StrictMode);
    const QRegularExpression path(QStringLiteral("^/pbatard/Fido/releases/download/v[0-9]+(?:\\.[0-9]+)*/Fido\\.ps1\\.lzma$"));
    const bool endpoint=url.isValid() && url.scheme()=="https" && url.host()=="github.com"
        && url.userInfo().isEmpty() && !url.hasQuery() && !url.hasFragment() && url.port(-1)==-1;
    if (!endpoint || !path.match(url.path()).hasMatch()) return EBADMSG;
    provider=url; return 0;
}
int DecodeDownloadProvider(const QByteArray& compressed,QByteArray& script)
{
    script.clear(); if (compressed.size()<13 || compressed.size()>maximumScriptBytes) return EBADMSG;
    const quint32 dictionary=qFromLittleEndian<quint32>(compressed.constData()+1);
    const quint64 declared=qFromLittleEndian<quint64>(compressed.constData()+5);
    if (dictionary>64*1024*1024 || declared==0 || declared>quint64(maximumScriptBytes)) return EFBIG;
    QTemporaryFile file; if (!file.open() || file.write(compressed)!=compressed.size() || !file.flush()) return EIO;
    IMAGE_FILE_SOURCE source={}; source.fd=file.handle();
    if (fstat(source.fd,&source.info)!=0) return errno;
    COMPRESSED_IMAGE_RESULT result={}; result.source_bytes=quint64(compressed.size());
    const int error=ReadLzmaImageSource(&source,maximumScriptBytes,collect,&script,&result);
    if (error!=0) script.clear();
    return error;
}
int ParseDownloadCatalog(const QByteArray& script,DownloadCatalog& catalog)
{
    catalog={}; if (script.isEmpty() || script.size()>maximumScriptBytes) return EBADMSG;
    QStringDecoder decoder(QStringDecoder::Utf8); QString text=decoder(script);
    if (decoder.hasError() || text.contains(QChar::Null)) return EBADMSG;
    text.replace(QStringLiteral("\r\n"),QStringLiteral("\n"));
    DownloadCatalog result;
    result.version=QRegularExpression(QStringLiteral("(?m)^# Fido v([0-9.]+) -")).match(text).captured(1);
    // Native protocol adapter audited against this version. New code is not executed.
    if (result.version!=QStringLiteral("1.71")) return ENOTSUP;
    result.organization=assignment(text,"OrgId"); result.profile=assignment(text,"ProfileId"); result.instance=assignment(text,"InstanceId");
    if (result.organization.isEmpty() || result.profile.isEmpty() || result.instance.isEmpty()) return EBADMSG;
    const QString marker=QStringLiteral("$WindowsVersions = "); const qsizetype offset=text.indexOf(marker);
    if (offset<0 || text.indexOf(marker,offset+marker.size())>=0) return EBADMSG;
    CatalogReader reader(QStringView(text).sliced(offset+marker.size())); Literal table;
    const int error=reader.read(table); if (error!=0 || !table.list || !reader.atDataEnd()) return EBADMSG;
    for (const Literal& product : table.array) {
        if (!product.list || product.array.size()<2) return EBADMSG;
        const Literal& header=product.array.first();
        if (!header.list || header.array.size()!=2 || header.array[0].text.isEmpty() || header.array[1].text.isEmpty()) return EBADMSG;
        DownloadProduct parsed; parsed.name=header.array[0].text; parsed.id=header.array[1].text;
        const bool shell=parsed.id=="UEFI_SHELL 2.2" || parsed.id=="UEFI_SHELL 2.0";
        if (!shell && parsed.id!="windows11" && parsed.id!="Windows10ISO") return ENOTSUP;
        for (qsizetype index=1; index<product.array.size();++index) {
            const Literal& release=product.array[index];
            if (!release.list || release.array.size()<2 || release.array.first().text.isEmpty()) return EBADMSG;
            DownloadRelease named; named.name=release.array.first().text;
            for (qsizetype entry=1;entry<release.array.size();++entry) {
                const Literal& edition=release.array[entry];
                if (!edition.list || edition.array.size()!=2 || edition.array.first().text.isEmpty()) return EBADMSG;
                DownloadEdition choice; choice.name=edition.array.first().text;
                const Literal& ids=edition.array[1];
                const QList<Literal> numbers=ids.list ? ids.array : QList<Literal>{ids};
                if (numbers.isEmpty() || numbers.size()>2) return EBADMSG;
                for (const Literal& id : numbers) {
                    if (id.number<0 || (!shell && id.number==0) || id.list || !id.text.isEmpty()) return EBADMSG;
                    choice.ids.append(id.number);
                }
                named.editions.append(choice);
            }
            parsed.releases.append(named);
        }
        result.products.append(parsed);
    }
    if (result.products.isEmpty()) return EBADMSG;
    catalog=std::move(result); return 0;
}
}
