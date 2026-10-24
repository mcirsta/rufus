// SPDX-License-Identifier: GPL-3.0-or-later
#include "qt/download_provider.h"
#include "qt/image_download.h"
#include "qt/image_download_dialog.h"
#include "qt/image_select_button.h"
#include "qt/selection_controller.h"
#include "qt/main_window.h"
#include "platform/download_catalog.h"
#include "http_fixture.h"
#include <QAction>
#include <QComboBox>
#include <QCheckBox>
#include <QFileDialog>
#include <QLabel>
#include <QPushButton>
#include <QNetworkProxy>
#include <QProgressBar>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <fcntl.h>
#include <unistd.h>
#include <atomic>
#include <cstring>
#include <QElapsedTimer>
#include <QStorageInfo>
#include <linux/fs.h>
#include <sys/stat.h>
static std::atomic<int> outputFault{0};
enum class RenameFault { None,NoExchange,Publish,BeforeSync,AfterSync,Concurrent };
static std::atomic<RenameFault> renameFault{RenameFault::None};
static std::atomic_bool backupMoved{false},published{false},renameFaultUsed{false};

static bool downloadOutput(int fd)
{
    char path[4096]; const QByteArray descriptor="/proc/self/fd/"+QByteArray::number(fd);
    const ssize_t size=readlink(descriptor.constData(),path,sizeof(path)-1);
    if (size<0) return false;
    path[size]=0; return std::strstr(path,"/.rufus-image-")!=nullptr;
}
extern "C" ssize_t __real_write(int,const void*,size_t);
extern "C" int __real_fsync(int);
extern "C" ssize_t __wrap_write(int fd,const void* bytes,size_t size)
{
    if (outputFault.load()==ENOSPC && downloadOutput(fd)) { outputFault=0; errno=ENOSPC; return -1; }
    return __real_write(fd,bytes,size);
}
extern "C" int __wrap_fsync(int fd)
{
    struct stat facts={};
    const bool directory=fstat(fd,&facts)==0 && S_ISDIR(facts.st_mode);
    const bool before=renameFault==RenameFault::BeforeSync && backupMoved;
    const bool after=renameFault==RenameFault::AfterSync && published;
    if (directory && (before || after) && !renameFaultUsed.exchange(true)) { errno=EIO; return -1; }
    if (outputFault.load()==EIO && downloadOutput(fd)) { outputFault=0; errno=EIO; return -1; }
    return __real_fsync(fd);
}
extern "C" int __real_renameat2(int,const char*,int,const char*,unsigned);
extern "C" int __wrap_renameat2(int fromDir,const char* from,int toDir,const char* to,unsigned flags)
{
    if (flags==RENAME_EXCHANGE && renameFault!=RenameFault::None) { errno=EINVAL; return -1; }
    const bool staging=std::strncmp(from,".rufus-image-",13)==0 && std::strncmp(to,".rufus-",7)!=0;
    if (staging && renameFault==RenameFault::Publish && !renameFaultUsed.exchange(true)) { errno=EIO; return -1; }
    if (staging && renameFault==RenameFault::Concurrent && !renameFaultUsed.exchange(true)) {
        const int fd=openat(toDir,to,O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC,0600);
        if (fd>=0) { __real_write(fd,"concurrent entry",16); close(fd); }
    }
    const int result=__real_renameat2(fromDir,from,toDir,to,flags);
    const int savedError=errno;
    if (result!=0 && flags==RENAME_EXCHANGE) qInfo("RUFUS_TEST_EXCHANGE_UNAVAILABLE error=%d",savedError);
    errno=savedError;
    if (result==0 && std::strncmp(to,".rufus-backup-",14)==0) backupMoved=true;
    if (result==0 && staging) published=true;
    return result;
}
class LargeDownloadServer final : public QTcpServer
{
public:
    static constexpr quint64 imageBytes=5ULL*1024*1024*1024+17;
    QByteArray pattern=QByteArray(64*1024,char(0x73));
    LargeDownloadServer() { connect(this,&QTcpServer::newConnection,this,&LargeDownloadServer::connected); }
private:
    void connected()
    {
        socket_=nextPendingConnection();
        connect(socket_,&QTcpSocket::readyRead,this,&LargeDownloadServer::requested);
        connect(socket_,&QTcpSocket::bytesWritten,this,&LargeDownloadServer::sendMore);
    }
    void requested()
    {
        request_+=socket_->readAll(); if (!request_.contains("\r\n\r\n")) return;
        disconnect(socket_,&QTcpSocket::readyRead,this,&LargeDownloadServer::requested);
        socket_->write("HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: "+QByteArray::number(imageBytes)+"\r\n\r\n");
        started_=true; sendMore();
    }
    void sendMore()
    {
        if (!started_ || socket_->state()!=QTcpSocket::ConnectedState) return;
        while (queued_<imageBytes && socket_->bytesToWrite()<256*1024) {
            const qint64 count=qint64(qMin(quint64(pattern.size()),imageBytes-queued_));
            const qint64 accepted=socket_->write(pattern.constData(),count);
            if (accepted<=0) return;
            queued_+=quint64(accepted);
        }
        if (queued_==imageBytes && socket_->bytesToWrite()==0) socket_->disconnectFromHost();
    }
    QTcpSocket* socket_=nullptr; QByteArray request_; quint64 queued_=0; bool started_=false;
};
using namespace rufus;
using namespace rufus_test;
namespace
{
constexpr int testTimeoutMs=6000;
const QByteArray providerPath="/pbatard/Fido/releases/download/v1.71/Fido.ps1.lzma";
QByteArray fixture(const QString& name)
{
    QFile file(QStringLiteral(RUFUS_FIDO_FIXTURE)+QChar('/')+name); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
QByteArray readFile(const QString& path)
{
    QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
bool writeFile(const QString& path,const QByteArray& bytes)
{
    QFile file(path); return file.open(QIODevice::WriteOnly|QIODevice::Truncate) && file.write(bytes)==bytes.size();
}
DownloadProviderSources sources(HttpFixture& server)
{
    DownloadProviderSources source; source.manifest=server.url("/Fido.ver"); source.assets=server.url("/"); source.microsoft=server.url("/"); source.tags=server.url("/tags"); source.device=server.url("/"); return source;
}
void setup(HttpFixture& server)
{
    server.responses["/Fido.ver"]=response(fixture("Fido.ver"));
    server.responses[providerPath]=response(fixture("Fido.ps1.lzma")); server.responses[providerPath+".sig"]=response(fixture("Fido.ps1.lzma.sig"));
    server.prefixResponses["/tags?"]=response({}, {}, 0, 204);
    server.prefixResponses["/mdt.js?"]=response("?w=ABCDEF123&foo=bar; rticks=\"+12345678");
    server.prefixResponses["/?session_id="]=response({});
    server.prefixResponses["/software-download-connector/api/getskuinformationbyproductedition?"]=response(R"JSON({"Skus":[{"Id":101,"Language":"English","LocalizedLanguage":"English (US)"},{"Id":"102","Language":"Romanian","LocalizedLanguage":"Romanian"}]})JSON");
    const QByteArray url=server.url("/windows.iso?token=fixture").toEncoded();
    server.prefixResponses["/software-download-connector/api/GetProductDownloadLinksBySku?"]=response("{\"ProductDownloadOptions\":[{\"DownloadType\":1,\"Uri\":\""+url+"\"},{\"DownloadType\":2,\"Uri\":\""+url+"\"}]}");
    server.responses["/pbatard/UEFI-Shell/releases/download/26H1/Version.xml"]=response("<release><supported_archs><arch>X64</arch><arch>AA64</arch></supported_archs></release>");
}
}
class TestDownloadWorkflow : public QObject
{
    Q_OBJECT
    QTemporaryDir settings_;
private slots:
    void initTestCase()
    {
        QVERIFY(settings_.isValid()); QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settings_.path());
        qputenv("XDG_DATA_HOME",settings_.filePath("data").toUtf8()); QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);
        QCOMPARE(fixture("Fido.ps1.lzma").size(),20569);
    }
    void init() { outputFault=0; renameFault=RenameFault::None; backupMoved=false; published=false; renameFaultUsed=false; QSettings(QSettings::IniFormat,QSettings::UserScope,"Rufus","Linux").clear(); }
    void signedCatalogAndNativeProtocol()
    {
        HttpFixture server; QVERIFY(server.listen(QHostAddress::LocalHost)); setup(server);
        DownloadProvider provider(nullptr,sources(server)); QSignalSpy loaded(&provider,&DownloadProvider::catalogReady),failure(&provider,&DownloadProvider::failed);
        QCOMPARE(provider.load(),0); QTRY_COMPARE_WITH_TIMEOUT(loaded.size(),1,testTimeoutMs); QVERIFY(failure.isEmpty());
        const auto& catalog=provider.catalog(); QCOMPARE(catalog.version,QStringLiteral("1.71")); QCOMPARE(catalog.products.size(),4);
        QCOMPARE(catalog.products[0].releases[0].editions[0].ids,(QList<int>{3813,3816}));
        QSignalSpy languages(&provider,&DownloadProvider::languagesReady); QCOMPARE(provider.languages(0,0,0),0);
        QTRY_COMPARE_WITH_TIMEOUT(languages.size(),1,testTimeoutMs); QVERIFY(failure.isEmpty());
        QCOMPARE(provider.languageChoices().size(),2); QCOMPARE(provider.languageChoices()[0].entries.size(),2);
        QSignalSpy links(&provider,&DownloadProvider::linksReady); QCOMPARE(provider.links(1),0);
        QTRY_COMPARE_WITH_TIMEOUT(links.size(),1,testTimeoutMs); QCOMPARE(provider.linkChoices().size(),4);
        QCOMPARE(provider.linkChoices()[1].architecture,QStringLiteral("ARM64"));
        const auto paths=server.paths(); int sessions=0,referers=0;
        for (const QByteArray& request : server.requests) {
            if (request.contains("/tags?")) ++sessions;
            if (request.contains("GetProductDownloadLinksBySku?")) {
                QVERIFY(request.contains("SKU=102")); QVERIFY(request.contains("Locale=en-US"));
                QVERIFY(request.contains("Referer: https://www.microsoft.com/software-download/windows11")); ++referers;
            }
        }
        QCOMPARE(sessions,2); QCOMPARE(referers,2);
        QVERIFY(paths.contains(providerPath+".sig"));
        const int requests=server.requests.size(); QCOMPARE(provider.load(),0); QCOMPARE(loaded.size(),2); QCOMPARE(server.requests.size(),requests);
    }
    void providerErrors_data()
    {
        QTest::addColumn<int>("kind");
        QTest::newRow("manifest-origin")<<0; QTest::newRow("bad-signature")<<1;
        QTest::newRow("invalid-device-reply")<<2; QTest::newRow("api-http-refusal")<<3;
        QTest::newRow("api-empty-language")<<4; QTest::newRow("unsafe-image-url")<<5;
        QTest::newRow("bad-sku-id")<<6;
    }
    void providerErrors()
    {
        QFETCH(int,kind); HttpFixture server; QVERIFY(server.listen(QHostAddress::LocalHost)); setup(server);
        if (kind==0) server.responses["/Fido.ver"]=response("z1=https://github.com.invalid/pbatard/Fido/releases/download/v1.71/Fido.ps1.lzma");
        if (kind==1) server.responses[providerPath+".sig"]=response(QByteArray(256,'x'));
        if (kind==2) server.prefixResponses["/mdt.js?"]=response("not protocol data");
        if (kind==3) server.prefixResponses["/tags?"]=response({}, {}, 0, 403);
        if (kind==4) server.prefixResponses["/software-download-connector/api/getskuinformationbyproductedition?"]=response("{\"Skus\":[]}");
        if (kind==5) server.prefixResponses["/software-download-connector/api/GetProductDownloadLinksBySku?"]=response("{\"ProductDownloadOptions\":[{\"DownloadType\":1,\"Uri\":\"file:///etc/passwd\"}]}");
        if (kind==6) server.prefixResponses["/software-download-connector/api/getskuinformationbyproductedition?"]=response("{\"Skus\":[{\"Id\":\"bad\",\"Language\":\"English\",\"LocalizedLanguage\":\"English\"}]}");
        DownloadProvider provider(nullptr,sources(server)); QSignalSpy loaded(&provider,&DownloadProvider::catalogReady),failure(&provider,&DownloadProvider::failed);
        QCOMPARE(provider.load(),0);
        if (kind>=2) {
            QTRY_COMPARE_WITH_TIMEOUT(loaded.size(),1,testTimeoutMs); QCOMPARE(provider.languages(1,0,0),0);
            if (kind==5) {
                QSignalSpy languages(&provider,&DownloadProvider::languagesReady); QTRY_COMPARE_WITH_TIMEOUT(languages.size(),1,testTimeoutMs);
                QCOMPARE(provider.links(0),0);
            }
        }
        QTRY_COMPARE_WITH_TIMEOUT(failure.size(),1,testTimeoutMs); QVERIFY(!provider.isActive());
        QVERIFY(failure.first()[0].toInt()!=0);
    }
    void originalCatalogBounds()
    {
        QByteArray script; QCOMPARE(DecodeDownloadProvider(fixture("Fido.ps1.lzma"),script),0);
        DownloadCatalog catalog; QCOMPARE(ParseDownloadCatalog(script,catalog),0);
        QByteArray changed=script; changed.replace("# Fido v1.71 -","# Fido v9.99 -"); QCOMPARE(ParseDownloadCatalog(changed,catalog),ENOTSUP); QVERIFY(catalog.products.isEmpty());
        changed=script; changed.replace("@(3813, 3816)","@(3813, $(Invoke-Code))"); QCOMPARE(ParseDownloadCatalog(changed,catalog),EBADMSG);
        changed=script; changed.append('\0'); QCOMPARE(ParseDownloadCatalog(changed,catalog),EBADMSG);
        changed=fixture("Fido.ps1.lzma"); changed[5]=char(255); changed[12]=char(255);
        QByteArray output; QVERIFY(DecodeDownloadProvider(changed,output)!=0); QVERIFY(output.isEmpty());
        QUrl url; QCOMPARE(ParseDownloadManifest("z1 = https://github.com/pbatard/Fido/releases/download/v1.71/Fido.ps1.lzma\nz1=bad",url),EBADMSG);
    }
    void shellAndChooserBacktracking()
    {
        HttpFixture server; QVERIFY(server.listen(QHostAddress::LocalHost)); setup(server);
        ImageDownloadDialog dialog(nullptr,sources(server)); dialog.show(); dialog.begin();
        auto* next=dialog.findChild<QPushButton*>("downloadContinue"); auto* back=dialog.findChild<QPushButton*>("downloadBack");
        auto* version=dialog.findChild<QComboBox*>("versionChoice"); QVERIFY(next && back && version);
        QTRY_COMPARE_WITH_TIMEOUT(version->count(),4,testTimeoutMs); version->setCurrentIndex(2);
        QTest::mouseClick(next,Qt::LeftButton); auto* release=dialog.findChild<QComboBox*>("releaseChoice"); QCOMPARE(release->count(),12);
        QTest::mouseClick(next,Qt::LeftButton); QTest::mouseClick(back,Qt::LeftButton); QVERIFY(release->isEnabled());
        QTest::mouseClick(next,Qt::LeftButton); QTest::mouseClick(next,Qt::LeftButton);
        auto* language=dialog.findChild<QComboBox*>("languageChoice"); QCOMPARE(language->count(),1);
        QTest::mouseClick(next,Qt::LeftButton); auto* arch=dialog.findChild<QComboBox*>("architectureChoice");
        QTRY_COMPARE_WITH_TIMEOUT(arch->count(),1,testTimeoutMs); QCOMPARE(arch->currentText(),QStringLiteral("X64, AA64"));
        QVERIFY(dialog.selectedUrl().path().endsWith("UEFI-Shell-2.2-26H1-RELEASE.iso"));
        auto* browser=dialog.findChild<QCheckBox*>("downloadUsingBrowser"); QVERIFY(browser->isVisible()); browser->setChecked(true); QVERIFY(dialog.useBrowser());
        QSignalSpy accepted(&dialog,&QDialog::accepted); QTest::mouseClick(next,Qt::LeftButton); QCOMPARE(accepted.size(),1);
        dialog.begin(); QCOMPARE(version->currentIndex(),0); QVERIFY(!browser->isVisible());
    }
    void streamAndPublish()
    {
        HttpFixture server; QVERIFY(server.listen(QHostAddress::LocalHost));
        const QByteArray data(1024*1024+17,'Z'); server.responses["/large.iso"]=response(data);
        QTemporaryDir directory; const QString path=directory.filePath("saved.iso"); QVERIFY(writeFile(path,"old image"));
        ImageDownload download; QSignalSpy finished(&download,&ImageDownload::finished);
        QVERIFY(download.start(server.url("/large.iso"),path)); QVERIFY(download.isActive());
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(),1,testTimeoutMs);
        QVERIFY2(download.result().error==0,qPrintable(QString::number(download.result().error)+" "+download.result().problem));
        QCOMPARE(download.state(),ImageDownloadState::Complete); QCOMPARE(readFile(path),data);
        QCOMPARE(download.result().sha256,QCryptographicHash::hash(data,QCryptographicHash::Sha256));
        QCOMPARE(QDir(directory.path()).entryList(QDir::Files|QDir::Hidden|QDir::NoDotAndDotDot),QStringList{"saved.iso"});
    }
    void transferRefusals_data()
    {
        QTest::addColumn<int>("kind");
        QTest::newRow("http-forbidden")<<0; QTest::newRow("short-body")<<1; QTest::newRow("missing-length")<<2;
        QTest::newRow("encoded-body")<<3; QTest::newRow("zero-length")<<4; QTest::newRow("timeout")<<5;
        QTest::newRow("cancel")<<6; QTest::newRow("destination-symlink")<<7; QTest::newRow("changed-destination")<<8;
        QTest::newRow("disk-full")<<9; QTest::newRow("sync-failure")<<10;
    }
    void transferRefusals()
    {
        QFETCH(int,kind); HttpFixture server; QVERIFY(server.listen(QHostAddress::LocalHost));
        QByteArray wire=response(QByteArray(4096,'A'));
        if (kind==0) wire=response({}, {}, 0, 403);
        if (kind==1) wire=response("short",{},5000);
        if (kind==2) wire=response("bytes",{},-1,200,false);
        if (kind==3) wire=response("bytes","Content-Encoding: gzip\r\n");
        if (kind==4) wire=response({});
        server.responses["/image.iso"]=wire;
        if (kind==5 || kind==6 || kind==8) server.holdBodies.insert("/image.iso");
        QTemporaryDir directory; const QString path=directory.filePath("existing.iso"),other=directory.filePath("other.iso");
        QVERIFY(writeFile(path,"existing bytes"));
        if (kind==7) { QVERIFY(QFile::rename(path,other)); QVERIFY(QFile::link(other,path)); }
        if (kind==9) outputFault=ENOSPC;
        if (kind==10) outputFault=EIO;
        ImageDownload download; QSignalSpy finished(&download,&ImageDownload::finished);
        QVERIFY(download.start(server.url("/image.iso"),path,kind==5 || kind==8 ? 150 : 3000));
        if (kind==6 || kind==8) {
            QTRY_VERIFY_WITH_TIMEOUT(!server.requests.isEmpty(),testTimeoutMs);
            if (kind==8) QVERIFY(writeFile(path,"changed by another operation"));
            else download.cancel();
            if (kind==8) server.releaseBodies();
        }
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(),1,testTimeoutMs);
        QVERIFY(download.state()==ImageDownloadState::Failed || download.state()==ImageDownloadState::Cancelled);
        if (kind==8) QCOMPARE(download.result().error,ESTALE);
        if (kind==9) QCOMPARE(download.result().error,ENOSPC);
        if (kind==10) QCOMPARE(download.result().error,EIO);
        QCOMPARE(readFile(path),kind==8 ? QByteArray("changed by another operation") : QByteArray("existing bytes"));
        const auto hidden=QDir(directory.path()).entryList(QStringList{".rufus-image-*"},QDir::Files|QDir::Hidden); QVERIFY(hidden.isEmpty());
    }
    void filesystemWithoutExchange_data()
    {
        QTest::addColumn<int>("fault");
        QTest::newRow("success")<<int(RenameFault::NoExchange);
        QTest::newRow("publication-failure")<<int(RenameFault::Publish);
        QTest::newRow("before-publication-sync")<<int(RenameFault::BeforeSync);
        QTest::newRow("after-publication-sync")<<int(RenameFault::AfterSync);
        QTest::newRow("concurrent-destination")<<int(RenameFault::Concurrent);
    }
    void filesystemWithoutExchange()
    {
        QFETCH(int,fault); renameFault=RenameFault(fault);
        HttpFixture server; QVERIFY(server.listen(QHostAddress::LocalHost)); server.responses["/image.iso"]=response("new image");
        QTemporaryDir directory; const QString path=directory.filePath("existing.iso"); QVERIFY(writeFile(path,"original image"));
        ImageDownload download; QSignalSpy finished(&download,&ImageDownload::finished);
        QVERIFY(download.start(server.url("/image.iso"),path)); QTRY_COMPARE_WITH_TIMEOUT(finished.size(),1,testTimeoutMs);
        QVERIFY(backupMoved);
        if (renameFault==RenameFault::NoExchange) { QCOMPARE(download.state(),ImageDownloadState::Complete); QCOMPARE(readFile(path),QByteArray("new image")); }
        else {
            QCOMPARE(download.state(),ImageDownloadState::Failed);
            QCOMPARE(readFile(path),renameFault==RenameFault::Concurrent ? QByteArray("concurrent entry") : QByteArray("original image"));
        }
        const auto backups=QDir(directory.path()).entryList(QStringList{".rufus-backup-*"},QDir::Files|QDir::Hidden);
        if (renameFault==RenameFault::Concurrent) {
            QCOMPARE(backups.size(),1); QCOMPARE(readFile(directory.filePath(backups.first())),QByteArray("original image"));
            QVERIFY(download.result().problem.contains(backups.first()));
        } else QVERIFY(backups.isEmpty());
        QVERIFY(QDir(directory.path()).entryList(QStringList{".rufus-image-*"},QDir::Files|QDir::Hidden).isEmpty());
    }
    void actualFiveGiBStream()
    {
        LargeDownloadServer server; QVERIFY(server.listen(QHostAddress::LocalHost)); QTemporaryDir directory; QVERIFY(directory.isValid());
        QVERIFY(QStorageInfo(directory.path()).bytesAvailable()>qint64(LargeDownloadServer::imageBytes+1024*1024));
        const QString path=directory.filePath("large.iso"); ImageDownload download; QSignalSpy finished(&download,&ImageDownload::finished);
        const QUrl url(QStringLiteral("http://127.0.0.1:%1/large.iso").arg(server.serverPort()));
        QVERIFY(download.start(url,path)); QTRY_COMPARE_WITH_TIMEOUT(finished.size(),1,90000);
        QCOMPARE(download.state(),ImageDownloadState::Complete); QCOMPARE(download.result().bytes,LargeDownloadServer::imageBytes);
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(quint64(file.size()),LargeDownloadServer::imageBytes);
        QCryptographicHash actual(QCryptographicHash::Sha256),expected(QCryptographicHash::Sha256);
        while (!file.atEnd()) { const QByteArray chunk=file.read(1024*1024); QVERIFY(!chunk.isEmpty()); actual.addData(chunk); }
        QCOMPARE(file.error(),QFileDevice::NoError);
        quint64 remaining=LargeDownloadServer::imageBytes;
        while (remaining>0) { const qsizetype count=qsizetype(qMin(remaining,quint64(server.pattern.size()))); expected.addData(server.pattern.constData(), int(count)); remaining-=quint64(count); }
        QCOMPARE(actual.result(),expected.result()); QCOMPARE(download.result().sha256,actual.result());
        file.close(); QVERIFY(QFile::remove(path));
    }
    void destructorCancelsAndJoins()
    {
        HttpFixture server; QVERIFY(server.listen(QHostAddress::LocalHost)); server.responses["/held.iso"]=response("held"); server.holdHeaders.insert("/held.iso");
        QTemporaryDir directory; const QString path=directory.filePath("existing.iso"); QVERIFY(writeFile(path,"original"));
        auto download=std::make_unique<ImageDownload>(); QVERIFY(download->start(server.url("/held.iso"),path));
        QTRY_VERIFY_WITH_TIMEOUT(!server.requests.isEmpty(),testTimeoutMs); QElapsedTimer elapsed; elapsed.start(); download.reset();
        QVERIFY(elapsed.elapsed()<1000); QCOMPARE(readFile(path),QByteArray("original"));
        QVERIFY(QDir(directory.path()).entryList(QStringList{".rufus-image-*"},QDir::Files|QDir::Hidden).isEmpty());
    }
    void splitButtonAndAutomaticSelection()
    {
        SelectionController controller; MainWindow window(controller); window.show();
        auto* select=window.findChild<ImageSelectButton*>("selectImageButton"); QVERIFY(select);
        auto* download=select->findChild<QAction*>("downloadImageAction"); auto* local=select->findChild<QAction*>("selectImageAction"); QVERIFY(download && local);
        download->trigger(); QVERIFY(select->downloadMode()); QCOMPARE(select->text(),QStringLiteral("DOWNLOAD"));
        // Ctrl+SELECT still opens the supplementary archive, even in DOWNLOAD mode.
        QTest::keyPress(select,Qt::Key_Control); QTest::mouseClick(select,Qt::LeftButton,Qt::ControlModifier,QPoint(8,select->height()/2)); QTest::keyRelease(select,Qt::Key_Control);
        auto* archive=window.findChild<QFileDialog*>("archiveDialog"); QVERIFY(archive && archive->isVisible()); archive->reject();
        local->trigger(); QVERIFY(!select->downloadMode());
        HttpFixture server; QVERIFY(server.listen(QHostAddress::LocalHost));
        QByteArray image(2*1024*1024,'X'); image[510]=char(0x55); image[511]=char(0xaa); server.responses["/image.iso"]=response(image);
        QTemporaryDir directory; const QString path=directory.filePath("download.iso");
        QVERIFY(controller.downloadImage(server.url("/image.iso"),path));
        QVERIFY(controller.isBusy()); QVERIFY(!window.findChild<QPushButton*>("startButton")->isEnabled());
        QTRY_COMPARE_WITH_TIMEOUT(controller.selection().imagePath,path,testTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.isBusy(),testTimeoutMs);
        QCOMPARE(readFile(path),image); QCOMPARE(controller.downloader().state(),ImageDownloadState::Idle);
    }
};
QTEST_MAIN(TestDownloadWorkflow)
#include "test_download_workflow.moc"
