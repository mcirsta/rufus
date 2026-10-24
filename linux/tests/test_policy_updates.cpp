// SPDX-License-Identifier: GPL-3.0-or-later
#include "qt/policy_updates.h"
#include "http_fixture.h"
#include "secure_boot_fixture.h"
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkProxy>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <unistd.h>
#include <cerrno>
using namespace rufus_test;
namespace
{
const QByteArray changedSbat = "shim,999\nBOOTMGRSECURITYVERSIONNUMBER,0x70000";
QByteArray commits(const char* date)
{
    const QJsonObject author{{"date",QString::fromLatin1(date)}};
    return QJsonDocument(QJsonArray{QJsonObject{{"commit",QJsonObject{{"author",author}}}}}).toJson();
}
void configure(HttpFixture& server)
{
    server.responses["/sbat_level.txt"] = response(changedSbat);
    server.responses["/sb_active.txt"] = response(QByteArray(40,'a'));
    server.responses["/sb_revoked.txt"] = response(QByteArray(40,'b'));
}
}
class TestPolicyUpdates : public QObject
{
    Q_OBJECT
    rufus::SecureBootPolicy policy_;
    void cancelOnProgress()
    {
        auto* update = qobject_cast<rufus::PolicyUpdates*>(sender());
        if (update) update->cancel();
    }
private slots:
    void initTestCase()
    {
        QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);
        QCOMPARE(rufus::LoadBundledSecureBootPolicy(policy_),0);
    }
    void listValidation_data()
    {
        QTest::addColumn<int>("kind"); QTest::addColumn<QByteArray>("bytes"); QTest::addColumn<bool>("valid");
        QTest::newRow("sbat") << 0 << changedSbat << true;
        QTest::newRow("thumbprints") << 1 << QByteArray(40,'a') << true;
        QTest::newRow("comments") << 2 << QByteArray("# origin\r\n")+QByteArray(40,'B')+"\r\n" << true;
        QTest::newRow("empty") << 0 << QByteArray() << false;
        QTest::newRow("comment-only") << 1 << QByteArray("# no entries") << false;
        QTest::newRow("missing-generation") << 0 << QByteArray("shim,wrong") << false;
        QTest::newRow("short-hash") << 1 << QByteArray(39,'a') << false;
        QTest::newRow("bad-hash") << 2 << QByteArray(40,'x') << false;
        QTest::newRow("bound") << 1 << QByteArray(1024,'a') << false;
        QTest::newRow("nul") << 0 << QByteArray("shim,5\0junk",11) << false;
    }
    void listValidation()
    {
        QFETCH(int,kind);QFETCH(QByteArray,bytes);QFETCH(bool,valid);
        QCOMPARE(rufus::ValidateSecureBootList(kind,bytes)==0,valid);
    }
    void cacheAndTimestamps()
    {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto sources = rufus::BundledDbxSources(); QCOMPARE(sources.size(),7);
        const auto bytes = secureboot_fixture::dbxList(QByteArray(32,'z')); const quint64 time = sources[1].timestamp+1;
        QCOMPARE(rufus::StoreSecureBootDbxCache(directory.path(),2,bytes,time),0);
        QByteArray read;quint64 saved = 0;
        QCOMPARE(rufus::ReadSecureBootDbxCache(directory.path(),2,read,saved),0);QCOMPARE(read,bytes);QCOMPARE(saved,time);
        QCOMPARE(rufus::StoreSecureBootDbxCache(directory.path(),2,bytes,time),EALREADY);
        rufus::SecureBootPolicy policy;QCOMPARE(rufus::LoadCachedSecureBootPolicy(directory.path(),policy),0);QCOMPARE(policy.dbx.value(2),bytes);
        QVERIFY(QFile::setPermissions(directory.path(),QFile::ReadOwner|QFile::ExeOwner));
        const int refused = rufus::StoreSecureBootDbxCache(directory.path(),2,secureboot_fixture::dbxList(QByteArray(32,'x')),time+1);
        QVERIFY(QFile::setPermissions(directory.path(),QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));
        QVERIFY(refused!=0);QCOMPARE(rufus::ReadSecureBootDbxCache(directory.path(),2,read,saved),0);QCOMPARE(read,bytes);QCOMPARE(saved,time);
        const QString path = directory.filePath(QStringLiteral("dbx_x64.json"));
        QFile corrupt(path);QVERIFY(corrupt.open(QIODevice::WriteOnly));QCOMPARE(corrupt.write("truncated"),9);corrupt.close();
        QVERIFY(rufus::ReadSecureBootDbxCache(directory.path(),2,read,saved)!=0);QVERIFY(read.isEmpty());
        QCOMPARE(rufus::LoadCachedSecureBootPolicy(directory.path(),policy),0);QCOMPARE(policy.dbx.value(2),policy_.dbx.value(2));
        QVERIFY(QFile::remove(path));QVERIFY(::symlink("/dev/null",QFile::encodeName(path).constData())==0);
        QVERIFY(rufus::StoreSecureBootDbxCache(directory.path(),2,bytes,time)!=0);
        QCOMPARE(rufus::ValidateSecureBootDbx(QByteArray(40,'\0')),EBADMSG);
        QCOMPARE(rufus::DbxCommitTimestamp(commits("2026-09-01T22:05:39Z")),quint64(1788300339));
        QCOMPARE(rufus::DbxCommitTimestamp(commits("2026-02-31T22:05:39Z")),quint64(0));
    }
    void textOnlyAndUnsupportedArchitectures()
    {
        for (bool textOnly : {true,false}) {
            QTemporaryDir directory; HttpFixture server; QVERIFY(server.listen(QHostAddress::LocalHost)); configure(server);
            rufus::PolicyUpdateSources sources; sources.textRoot=server.url("/"); sources.dbx.clear();
            if (!textOnly) sources.dbx.append({5,server.url("/repos/policy/contents/dbx_ia64"),0});
            rufus::PolicyUpdates update; QSignalSpy done(&update,&rufus::PolicyUpdates::completed), ask(&update,&rufus::PolicyUpdates::dbxAvailable);
            QCOMPARE(update.start(directory.path(),policy_,sources),0);
            QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,3000); QCOMPARE(ask.size(),0);
            const auto result=qvariant_cast<rufus::PolicyUpdateResult>(done.first().first());
            QCOMPARE(result.error,0); QCOMPARE(result.failures,0); QCOMPARE(result.listUpdates,3); QCOMPARE(result.dbxUpdates,0);
            QCOMPARE(server.paths().size(),textOnly?3:4);
        }
    }
    void cancelDuringProgress()
    {
        QTemporaryDir directory; HttpFixture server; QVERIFY(server.listen(QHostAddress::LocalHost)); configure(server);
        rufus::PolicyUpdates update; rufus::PolicyUpdateSources sources; sources.textRoot=server.url("/"); sources.dbx.clear();
        QSignalSpy done(&update,&rufus::PolicyUpdates::completed);
        connect(&update,&rufus::PolicyUpdates::progress,this,&TestPolicyUpdates::cancelOnProgress);
        QCOMPARE(update.start(directory.path(),policy_,sources),0);
        QCOMPARE(done.size(),1); QVERIFY(!update.isActive());
        auto* transport=update.findChild<rufus::HttpDownload*>(); QVERIFY(transport && !transport->isActive());
        QCOMPARE(qvariant_cast<rufus::PolicyUpdateResult>(done.first().first()).error,ECANCELED);
    }
    void update_data()
    {
        QTest::addColumn<QString>("mode");
        for (const char* mode : {"accept","decline","cancel","invalid-list","invalid-dbx","no-newer","offline","chunked","oversize-list"}) QTest::newRow(mode)<<QString::fromLatin1(mode);
    }
    void update()
    {
        QFETCH(QString,mode); QTemporaryDir directory; HttpFixture server; QVERIFY(server.listen(QHostAddress::LocalHost));configure(server);
        rufus::PolicyUpdateSources sources; sources.textRoot=server.url("/");
        sources.dbx={{2,server.url("/repos/example/contents/dbx_x64.efiauth2"),1}, {1,server.url("/repos/example/contents/dbx_ia32.efiauth2"),1}};
        const QByteArray date = mode=="no-newer"?commits("1970-01-01T00:00:01Z"):commits("2026-09-01T22:05:39Z");
        const auto dbx=secureboot_fixture::dbxList(QByteArray(32,'z'));
        const QByteArray query="/repos/example/commits?path=dbx_x64.efiauth2&page=1&per_page=1";
        server.responses[query]=response(date);
        server.responses["/repos/example/commits?path=dbx_ia32.efiauth2&page=1&per_page=1"]=response(date);
        server.responses["/repos/example/contents/dbx_x64.efiauth2"]=response(mode=="invalid-dbx"?QByteArray("bad DBX"):dbx);
        server.responses["/repos/example/contents/dbx_ia32.efiauth2"]=response(dbx);
        if(mode=="invalid-list")server.responses["/sbat_level.txt"]=response(QByteArray("bad policy"));
        if(mode=="oversize-list")server.responses["/sbat_level.txt"]=response(QByteArray(1024,'a'));
        if(mode=="chunked") {
            const QByteArray chunk = QByteArray::number(changedSbat.size(),16)+"\r\n"+changedSbat+"\r\n0\r\n\r\n";
            server.responses["/sbat_level.txt"]=response(chunk,"Transfer-Encoding: chunked\r\n",-1,200,false);
        }
        if(mode=="offline")server.responses.clear();
        rufus::PolicyUpdates update;QSignalSpy done(&update,&rufus::PolicyUpdates::completed),ask(&update,&rufus::PolicyUpdates::dbxAvailable);
        QCOMPARE(update.start(directory.path(),policy_,sources,100),0);
        if(mode!="offline"&&mode!="no-newer"){
            QTRY_COMPARE_WITH_TIMEOUT(ask.size(),1,3000);QCOMPARE(done.size(),0);
            QVERIFY(!server.paths().contains("/repos/example/contents/dbx_x64.efiauth2"));
            if(mode=="cancel")update.cancel();else update.answerDbx(mode!="decline");
        }
        QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,3000);QVERIFY(!update.isActive());
        const auto result=qvariant_cast<rufus::PolicyUpdateResult>(done.first().first());
        QCOMPARE(result.error,mode=="cancel"?ECANCELED:0);
        QCOMPARE(ask.size(),mode=="offline"||mode=="no-newer"?0:1);
        QCOMPARE(result.listUpdates,mode=="offline"?0:(mode=="invalid-list"||mode=="oversize-list")?2:3);
        QCOMPARE(result.dbxUpdates,mode=="accept"||mode=="invalid-list"||mode=="oversize-list"||mode=="chunked"?2:mode=="invalid-dbx"?1:0);
        QCOMPARE(result.policy.sbat,mode=="offline"||mode=="invalid-list"||mode=="oversize-list"?policy_.sbat:changedSbat);
        if(mode=="decline")QVERIFY(result.dbxDeclined);
        if(result.dbxUpdates==2){
            rufus::SecureBootPolicy cached;QCOMPARE(rufus::LoadCachedSecureBootPolicy(directory.path(),cached),0);
            // The test's synthetic base timestamp is older than production;
            // inspect the actual cache metadata independently instead.
            QByteArray saved;quint64 timestamp=0;QCOMPARE(rufus::ReadSecureBootDbxCache(directory.path(),2,saved,timestamp),0);QCOMPARE(saved,dbx);
        }
        if(mode=="accept"){
            ask.clear();done.clear();server.requests.clear();
            QCOMPARE(update.start(directory.path(),result.policy,sources,100),0);
            QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,3000);QCOMPARE(ask.size(),0);
            QVERIFY(!server.paths().contains("/repos/example/contents/dbx_x64.efiauth2"));
        }
    }
};
QTEST_GUILESS_MAIN(TestPolicyUpdates)
#include "test_policy_updates.moc"
