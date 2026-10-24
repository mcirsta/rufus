// SPDX-License-Identifier: GPL-3.0-or-later
#include "qt/signed_download.h"
#include "http_fixture.h"
using namespace rufus_test;
#include <QCryptographicHash>
#include <QFile>
#include <QMap>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QPointer>
#include <QProcess>
#include <QSet>
#include <QSignalSpy>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslKey>
#include <QSslSocket>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <algorithm>
#include <cerrno>

static int verificationError = 0;
extern "C" int __real_VerifyRufusDownloadSignature(const void*, size_t, const void*, size_t);
extern "C" int __wrap_VerifyRufusDownloadSignature(const void* data, size_t size, const void* signature, size_t signatureSize)
{
    if (verificationError != 0) return verificationError;
    return __real_VerifyRufusDownloadSignature(data, size, signature, signatureSize);
}

namespace
{
constexpr qsizetype payloadLimit = 1024 * 1024;
const QByteArray payloadPath = "/grub-2.12/core.img";
const QByteArray signaturePath = payloadPath + ".sig";


}

class TestSignedDownload : public QObject
{
    Q_OBJECT
    QByteArray payload_;
    QByteArray signature_;
    rufus::SignedDownload* callbackDownload_ = nullptr;
    HttpFixture* callbackServer_ = nullptr;
    QList<rufus::SignedDownloadResult> outcomes_;
    int restartError_ = -1;
    int deleted_ = 0;
    QSslConfiguration previousSsl_;
    bool sslChanged_ = false;

    void configure(HttpFixture& server)
    {
        server.responses[payloadPath] = response(payload_);
        server.responses[signaturePath] = response(signature_);
    }
    rufus::SignedDownloadResult result(const QSignalSpy& spy) const
    {
        return qvariant_cast<rufus::SignedDownloadResult>(spy.first().first());
    }
    void checkProgress(const QSignalSpy& progress)
    {
        QVERIFY(!progress.isEmpty());
        quint64 previous = 0;
        for (const QList<QVariant>& event : progress)
        {
            const quint64 current = event[0].toULongLong();
            QCOMPARE(event[1].toULongLong(), quint64(payload_.size()));
            QVERIFY(current >= previous && current <= quint64(payload_.size()));
            previous = current;
        }
        QCOMPARE(previous, quint64(payload_.size()));
    }
    bool configureTls(HttpFixture& server, const QTemporaryDir& files)
    {
        const QString certificatePath = files.filePath("certificate.pem");
        const QString keyPath = files.filePath("key.pem");
        QProcess openssl;
        openssl.start(QStringLiteral(RUFUS_OPENSSL), {"req", "-x509", "-newkey", "rsa:2048", "-nodes",
            "-keyout", keyPath, "-out", certificatePath, "-subj", "/CN=localhost", "-days", "1",
            "-addext", "subjectAltName=IP:127.0.0.1"});
        if (!openssl.waitForFinished(10000) || openssl.exitCode() != 0) return false;
        QFile certificate(certificatePath);
        QFile key(keyPath);
        if (!certificate.open(QIODevice::ReadOnly) || !key.open(QIODevice::ReadOnly)) return false;
        server.certificate = QSslCertificate(certificate.readAll());
        server.key = QSslKey(key.readAll(), QSsl::Rsa);
        return !server.certificate.isNull() && !server.key.isNull();
    }
    void trustFixture(const HttpFixture& server)
    {
        // Test-process trust only; production never adds or ignores certificates.
        previousSsl_ = QSslConfiguration::defaultConfiguration();
        QSslConfiguration trusted = previousSsl_;
        QList<QSslCertificate> certificates = trusted.caCertificates();
        certificates.append(server.certificate);
        trusted.setCaCertificates(certificates);
        QSslConfiguration::setDefaultConfiguration(trusted);
        sslChanged_ = true;
    }

private slots:
    void initTestCase()
    {
        qRegisterMetaType<rufus::SignedDownloadResult>();
        QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);
        QFile data(QStringLiteral(RUFUS_SIGNED_FIXTURE) + "/core.img");
        QFile signature(QStringLiteral(RUFUS_SIGNED_FIXTURE) + "/core.img.sig");
        QVERIFY(data.open(QIODevice::ReadOnly));
        QVERIFY(signature.open(QIODevice::ReadOnly));
        payload_ = data.readAll();
        signature_ = signature.readAll();
        QCOMPARE(payload_.size(), 35918);
        QCOMPARE(signature_.size(), 256);
        QCOMPARE(QCryptographicHash::hash(payload_, QCryptographicHash::Sha256).toHex(),
            QByteArray("15902895487c084c8f574666c31e7fa399d05b831fd61c1e3c3eee69d6576924"));
    }
    void init()
    {
        verificationError = 0;
        sslChanged_ = false;
    }
    void cleanup()
    {
        if (sslChanged_) QSslConfiguration::setDefaultConfiguration(previousSsl_);
        verificationError = 0;
    }

    void authenticAndFragmented_data()
    {
        QTest::addColumn<bool>("fragments");
        QTest::newRow("whole") << false;
        QTest::newRow("fragmented") << true;
    }
    void authenticAndFragmented()
    {
        QFETCH(bool, fragments);
        HttpFixture server;
        configure(server);
        if (fragments) server.fragmented = {payloadPath, signaturePath};
        QVERIFY(server.listen(QHostAddress::LocalHost));
        rufus::SignedDownload download;
        QSignalSpy done(&download, &rufus::SignedDownload::completed);
        QSignalSpy progress(&download, &rufus::SignedDownload::progress);
        QCOMPARE(download.start(server.url(), payload_.size()), 0); // Exact caller limit is allowed.
        QVERIFY(download.isActive());
        QTRY_COMPARE(done.count(), 1);
        const auto accepted = result(done);
        QCOMPARE(accepted.error, 0);
        QCOMPARE(accepted.httpStatus, 200);
        QVERIFY(accepted.signatureRequest);
        QCOMPARE(accepted.bytes, payload_);
        QCOMPARE(accepted.sha256, QCryptographicHash::hash(payload_, QCryptographicHash::Sha256));
        QVERIFY(!download.isActive());
        QCOMPARE(server.paths(), QList<QByteArray>({payloadPath, signaturePath}));
        for (const QByteArray& request : server.requests)
        {
            QVERIFY(request.startsWith("GET "));
            QVERIFY(request.toLower().contains("accept-encoding: identity\r\n"));
            QVERIFY(request.toLower().contains("user-agent: rufus/linux\r\n"));
        }
        checkProgress(progress);
        QTRY_VERIFY(download.findChildren<QNetworkReply*>().isEmpty());
        download.cancel();
        QCOMPARE(done.count(), 1);
    }

    void failures_data()
    {
        QTest::addColumn<QByteArray>("path");
        QTest::addColumn<QByteArray>("wire");
        QTest::addColumn<int>("error");
        QTest::addColumn<int>("requests");
        QTest::newRow("not-found") << payloadPath << response({}, {}, 0, 404) << ENOENT << 1;
        QTest::newRow("forbidden") << payloadPath << response({}, {}, 0, 403) << EACCES << 1;
        QTest::newRow("authentication") << payloadPath << response({}, "WWW-Authenticate: Basic realm=fixture\r\n", 0, 401) << EACCES << 1;
        QTest::newRow("server-error") << payloadPath << response({}, {}, 0, 500) << EPROTO << 1;
        QTest::newRow("partial-content") << payloadPath << response(payload_, {}, -1, 206) << EPROTO << 1;
        QTest::newRow("no-content") << payloadPath << response({}, {}, 0, 204) << EPROTO << 1;
        QTest::newRow("empty-payload") << payloadPath << response({}) << EBADMSG << 1;
        QTest::newRow("oversized-payload") << payloadPath << response({}, {}, payloadLimit + 1) << EFBIG << 1;
        QTest::newRow("huge-declared-size") << payloadPath << response({}, {}, qint64(8) * 1024 * 1024 * 1024) << EFBIG << 1;
        QTest::newRow("missing-length") << payloadPath << response(payload_, {}, -1, 200, false) << EBADMSG << 1;
        QTest::newRow("bad-length") << payloadPath << response(payload_, "Content-Length: invalid\r\n", -1, 200, false) << EBADMSG << 1;
        QTest::newRow("negative-length") << payloadPath << response(payload_, "Content-Length: -1\r\n", -1, 200, false) << EBADMSG << 1;
        QTest::newRow("encoded-content") << payloadPath << response(payload_, "Content-Encoding: gzip\r\n") << EBADMSG << 1;
        QTest::newRow("chunked-without-length") << payloadPath << response("3\r\nabc\r\n0\r\n\r\n", "Transfer-Encoding: chunked\r\n", -1, 200, false) << EBADMSG << 1;
        QTest::newRow("short-body") << payloadPath << response(payload_.left(100), {}, payload_.size()) << EIO << 1;
        QTest::newRow("tampered-body") << payloadPath << response(QByteArray(payload_.size(), 'x')) << EBADMSG << 2;
        QTest::newRow("signature-not-found") << signaturePath << response({}, {}, 0, 404) << ENOENT << 2;
        QTest::newRow("signature-forbidden") << signaturePath << response({}, {}, 0, 403) << EACCES << 2;
        QTest::newRow("empty-signature") << signaturePath << response({}) << EBADMSG << 2;
        QTest::newRow("short-signature") << signaturePath << response(signature_.left(255)) << EBADMSG << 2;
        QTest::newRow("long-signature") << signaturePath << response(signature_ + 'x') << EBADMSG << 2;
        QTest::newRow("truncated-signature-body") << signaturePath << response(signature_.left(100), {}, 256) << EIO << 2;
        QTest::newRow("bad-signature") << signaturePath << response(QByteArray(256, 'x')) << EBADMSG << 2;
        QTest::newRow("signature-missing-length") << signaturePath << response(signature_, {}, -1, 200, false) << EBADMSG << 2;
        QTest::newRow("redirect-without-location") << payloadPath << response({}, {}, 0, 302) << EPROTO << 1;
    }
    void failures()
    {
        QFETCH(QByteArray, path);
        QFETCH(QByteArray, wire);
        QFETCH(int, error);
        QFETCH(int, requests);
        HttpFixture server;
        configure(server);
        server.responses[path] = wire;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        rufus::SignedDownload download;
        QSignalSpy done(&download, &rufus::SignedDownload::completed);
        QCOMPARE(download.start(server.url(), payloadLimit), 0);
        QTRY_COMPARE(done.count(), 1);
        QCOMPARE(result(done).error, error);
        QCOMPARE(result(done).signatureRequest, requests == 2);
        QVERIFY(result(done).bytes.isEmpty() && result(done).sha256.isEmpty());
        QVERIFY(!download.isActive());
        QCOMPARE(server.requests.count(), requests);
        QTRY_VERIFY(download.findChildren<QNetworkReply*>().isEmpty());
    }

    void invalidStart_data()
    {
        QTest::addColumn<QString>("url");
        QTest::addColumn<qsizetype>("limit");
        QTest::addColumn<int>("timeout");
        for (const char* url : {"", "file:///tmp/core.img", "ftp://localhost/core.img", "http:///core.img",
            "http://localhost/", "http://user:pass@localhost/core.img", "http://localhost/core.img?x=1", "http://localhost/core.img#fragment"})
            QTest::newRow(url) << QString::fromLatin1(url) << payloadLimit << 100;
        QTest::newRow("zero-limit") << QString("http://localhost/core.img") << qsizetype(0) << 100;
        QTest::newRow("negative-limit") << QString("http://localhost/core.img") << qsizetype(-1) << 100;
        QTest::newRow("over-limit") << QString("http://localhost/core.img") << qsizetype(payloadLimit + 1) << 100;
        QTest::newRow("zero-timeout") << QString("http://localhost/core.img") << payloadLimit << 0;
    }
    void invalidStart()
    {
        QFETCH(QString, url);
        QFETCH(qsizetype, limit);
        QFETCH(int, timeout);
        rufus::SignedDownload download;
        QSignalSpy done(&download, &rufus::SignedDownload::completed);
        QCOMPARE(download.start(QUrl(url), limit, timeout), EINVAL);
        QVERIFY(!download.isActive());
        download.cancel();
        QCOMPARE(done.count(), 0);
    }

    void cancelAndTimeout_data()
    {
        QTest::addColumn<QByteArray>("path");
        QTest::addColumn<bool>("body");
        QTest::addColumn<bool>("timeout");
        QTest::newRow("cancel-payload-headers") << payloadPath << false << false;
        QTest::newRow("cancel-payload-body") << payloadPath << true << false;
        QTest::newRow("cancel-signature") << signaturePath << true << false;
        QTest::newRow("timeout-payload-headers") << payloadPath << false << true;
        QTest::newRow("timeout-payload-body") << payloadPath << true << true;
        QTest::newRow("timeout-signature") << signaturePath << true << true;
    }
    void cancelAndTimeout()
    {
        QFETCH(QByteArray, path);
        QFETCH(bool, body);
        QFETCH(bool, timeout);
        HttpFixture server;
        configure(server);
        if (body) server.holdBodies.insert(path);
        else server.holdHeaders.insert(path);
        QVERIFY(server.listen(QHostAddress::LocalHost));
        rufus::SignedDownload download;
        QSignalSpy done(&download, &rufus::SignedDownload::completed);
        QSignalSpy progress(&download, &rufus::SignedDownload::progress);
        QCOMPARE(download.start(server.url(), payloadLimit, timeout ? 100 : 3500), 0);
        QTRY_VERIFY(server.paths().contains(path));
        QCOMPARE(download.start(server.url(), payloadLimit), EBUSY);
        if (!timeout)
        {
            QCOMPARE(done.count(), 0);
            if (path == signaturePath)
            {
                QCOMPARE(progress.last()[0].toLongLong(), qint64(payload_.size()));
                QVERIFY(download.isActive()); // Payload 100% is not verified completion.
            }
            download.cancel();
            download.cancel();
        }
        QTRY_COMPARE(done.count(), 1);
        QCOMPARE(result(done).error, timeout ? ETIMEDOUT : ECANCELED);
        QCOMPARE(result(done).signatureRequest, path == signaturePath);
        QVERIFY(result(done).bytes.isEmpty() && result(done).sha256.isEmpty());
        QVERIFY(!download.isActive());
        QTRY_VERIFY(download.findChildren<QNetworkReply*>().isEmpty());
        QCOMPARE(server.paths().count(signaturePath), path == signaturePath ? 1 : 0);
    }

    void cancelOnProgress(quint64 received, quint64)
    {
        if (received > 0) callbackDownload_->cancel();
    }
    void cancelsDuringBody()
    {
        HttpFixture server;
        configure(server);
        server.fragmented.insert(payloadPath);
        QVERIFY(server.listen(QHostAddress::LocalHost));
        rufus::SignedDownload download;
        callbackDownload_ = &download;
        connect(&download, &rufus::SignedDownload::progress, this, &TestSignedDownload::cancelOnProgress);
        QSignalSpy done(&download, &rufus::SignedDownload::completed);
        QCOMPARE(download.start(server.url(), payloadLimit), 0);
        QTRY_COMPARE(done.count(), 1);
        QCOMPARE(result(done).error, ECANCELED);
        QCOMPARE(server.paths(), QList<QByteArray>({payloadPath}));
        QVERIFY(result(done).bytes.isEmpty());
        QTRY_VERIFY(download.findChildren<QNetworkReply*>().isEmpty());
        callbackDownload_ = nullptr;
    }

    void idleTimerResetsOnData()
    {
        HttpFixture server;
        configure(server);
        server.fragmented.insert(payloadPath);
        QVERIFY(server.listen(QHostAddress::LocalHost));
        rufus::SignedDownload download;
        QSignalSpy done(&download, &rufus::SignedDownload::completed);
        QCOMPARE(download.start(server.url(), payloadLimit, 100), 0);
        QTRY_COMPARE(done.count(), 1);
        QCOMPARE(result(done).error, 0); // Whole transfer exceeds 100 ms, but keeps delivering bytes.
    }

    void redirectsAndCookies()
    {
        HttpFixture original;
        HttpFixture mirror;
        configure(original);
        QVERIFY(original.listen(QHostAddress::LocalHost));
        QVERIFY(mirror.listen(QHostAddress::LocalHost));
        mirror.responses["/mirror/core.img"] = response(payload_, "Set-Cookie: session=secret; Path=/\r\n");
        original.responses[payloadPath] = response({}, "Location: " + mirror.url("/mirror/core.img").toEncoded() + "\r\n", 0, 302);
        rufus::SignedDownload download;
        QSignalSpy done(&download, &rufus::SignedDownload::completed);
        QCOMPARE(download.start(original.url(), payloadLimit), 0);
        QTRY_COMPARE(done.count(), 1);
        QCOMPARE(result(done).error, 0);
        QCOMPARE(original.paths(), QList<QByteArray>({payloadPath, signaturePath}));
        QCOMPARE(mirror.paths(), QList<QByteArray>({"/mirror/core.img"}));
        for (const QByteArray& request : original.requests) QVERIFY(!request.toLower().contains("cookie:"));
        for (const QByteArray& request : mirror.requests) QVERIFY(!request.toLower().contains("cookie:"));
    }

    void encodedUrlKeepsOriginalSignaturePath()
    {
        const QByteArray encoded = "/grub%2F2.12/core%20img";
        HttpFixture server;
        server.responses[encoded] = response(payload_);
        server.responses[encoded + ".sig"] = response(signature_);
        QVERIFY(server.listen(QHostAddress::LocalHost));
        rufus::SignedDownload download;
        QSignalSpy done(&download, &rufus::SignedDownload::completed);
        QCOMPARE(download.start(server.url(encoded), payloadLimit), 0);
        QTRY_COMPARE(done.count(), 1);
        QCOMPARE(result(done).error, 0);
        QCOMPARE(server.paths(), QList<QByteArray>({encoded, encoded + ".sig"}));
    }

    void callerGapLimit()
    {
        HttpFixture server;
        configure(server);
        QVERIFY(server.listen(QHostAddress::LocalHost));
        rufus::SignedDownload download;
        QSignalSpy done(&download, &rufus::SignedDownload::completed);
        QCOMPARE(download.start(server.url(), payload_.size() - 1), 0);
        QTRY_COMPARE(done.count(), 1);
        QCOMPARE(result(done).error, EFBIG);
        QCOMPARE(server.paths(), QList<QByteArray>({payloadPath}));
        QVERIFY(result(done).bytes.isEmpty());
    }

    void fullBoundStillNeedsAuthentication()
    {
        HttpFixture server;
        configure(server);
        server.responses[payloadPath] = response(QByteArray(payloadLimit, 'x'));
        QVERIFY(server.listen(QHostAddress::LocalHost));
        rufus::SignedDownload download;
        QSignalSpy done(&download, &rufus::SignedDownload::completed);
        QSignalSpy progress(&download, &rufus::SignedDownload::progress);
        QCOMPARE(download.start(server.url(), payloadLimit), 0);
        QTRY_COMPARE(done.count(), 1);
        QCOMPARE(result(done).error, EBADMSG);
        QVERIFY(result(done).bytes.isEmpty() && result(done).sha256.isEmpty());
        QCOMPARE(progress.last()[0].toLongLong(), qint64(payloadLimit));
        QCOMPARE(progress.last()[1].toLongLong(), qint64(payloadLimit));
        QCOMPARE(server.paths(), QList<QByteArray>({payloadPath, signaturePath}));
    }

    void cryptoFailure_data()
    {
        QTest::addColumn<int>("error");
        QTest::newRow("allocation") << ENOMEM;
        QTest::newRow("provider") << EIO;
    }
    void cryptoFailure()
    {
        QFETCH(int, error);
        HttpFixture server;
        configure(server);
        QVERIFY(server.listen(QHostAddress::LocalHost));
        rufus::SignedDownload download;
        QSignalSpy done(&download, &rufus::SignedDownload::completed);
        verificationError = error;
        QCOMPARE(download.start(server.url(), payloadLimit), 0);
        QTRY_COMPARE(done.count(), 1);
        QCOMPARE(result(done).error, error);
        QVERIFY(result(done).bytes.isEmpty() && result(done).sha256.isEmpty());
        QCOMPARE(server.paths(), QList<QByteArray>({payloadPath, signaturePath}));
    }

    void connectionRefused()
    {
        HttpFixture server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        const QUrl url = server.url();
        server.close();
        rufus::SignedDownload download;
        QSignalSpy done(&download, &rufus::SignedDownload::completed);
        QCOMPARE(download.start(url, payloadLimit), 0);
        QTRY_COMPARE(done.count(), 1);
        QCOMPARE(result(done).error, ECONNREFUSED);
        QVERIFY(result(done).bytes.isEmpty());
    }

    void redirectLoop()
    {
        HttpFixture server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        server.responses[payloadPath] = response({}, "Location: " + server.url().toEncoded() + "\r\n", 0, 302);
        rufus::SignedDownload download;
        QSignalSpy done(&download, &rufus::SignedDownload::completed);
        QCOMPARE(download.start(server.url(), payloadLimit), 0);
        QTRY_COMPARE(done.count(), 1);
        QCOMPARE(result(done).error, ELOOP);
        QVERIFY(server.requests.count() <= 51); // Qt's default redirect limit.
        QVERIFY(result(done).bytes.isEmpty());
    }

    void refusesUntrustedTls()
    {
        QVERIFY(QSslSocket::supportsSsl());
        QTemporaryDir files;
        QVERIFY(files.isValid());
        HttpFixture server;
        QVERIFY(configureTls(server, files));
        configure(server);
        QVERIFY(server.listen(QHostAddress::LocalHost));
        rufus::SignedDownload download;
        QSignalSpy done(&download, &rufus::SignedDownload::completed);
        QCOMPARE(download.start(server.url(), payloadLimit), 0);
        QTRY_COMPARE(done.count(), 1);
        QCOMPARE(result(done).error, EPROTO);
        QVERIFY(result(done).bytes.isEmpty() && server.requests.isEmpty());
    }

    void trustedTlsAndDowngrade_data()
    {
        QTest::addColumn<bool>("downgrade");
        QTest::newRow("trusted-https") << false;
        QTest::newRow("refuse-downgrade") << true;
    }
    void trustedTlsAndDowngrade()
    {
        QFETCH(bool, downgrade);
        QTemporaryDir files;
        QVERIFY(files.isValid());
        HttpFixture secure;
        HttpFixture plain;
        QVERIFY(configureTls(secure, files));
        configure(secure);
        configure(plain);
        QVERIFY(secure.listen(QHostAddress::LocalHost));
        QVERIFY(plain.listen(QHostAddress::LocalHost));
        trustFixture(secure);
        if (downgrade) secure.responses[payloadPath] = response({}, "Location: " + plain.url().toEncoded() + "\r\n", 0, 302);
        rufus::SignedDownload download;
        QSignalSpy done(&download, &rufus::SignedDownload::completed);
        QCOMPARE(download.start(secure.url(), payloadLimit), 0);
        QTRY_COMPARE(done.count(), 1);
        QCOMPARE(result(done).error, downgrade ? EPROTO : 0);
        QVERIFY(plain.requests.isEmpty());
        QCOMPARE(secure.requests.count(), downgrade ? 1 : 2);
        QCOMPARE(result(done).bytes, downgrade ? QByteArray() : payload_);
    }

    void restartAfterFailure(rufus::SignedDownloadResult accepted)
    {
        outcomes_.append(accepted);
        if (outcomes_.size() == 1)
        {
            callbackServer_->responses[signaturePath] = response(signature_);
            restartError_ = callbackDownload_->start(callbackServer_->url(), payloadLimit);
        }
    }
    void reentrantRestartAndOwnedBytes()
    {
        HttpFixture server;
        configure(server);
        server.responses[signaturePath] = response(QByteArray(256, 'x'));
        QVERIFY(server.listen(QHostAddress::LocalHost));
        rufus::SignedDownload download;
        callbackDownload_ = &download;
        callbackServer_ = &server;
        outcomes_.clear();
        connect(&download, &rufus::SignedDownload::completed, this, &TestSignedDownload::restartAfterFailure);
        QCOMPARE(download.start(server.url(), payloadLimit), 0);
        QTRY_COMPARE(outcomes_.size(), 2);
        QCOMPARE(outcomes_[0].error, EBADMSG);
        QVERIFY(outcomes_[0].bytes.isEmpty());
        QCOMPARE(restartError_, 0);
        QCOMPARE(outcomes_[1].error, 0);
        QCOMPARE(outcomes_[1].bytes, payload_);
        disconnect(&download, nullptr, this, nullptr);
        QSignalSpy done(&download, &rufus::SignedDownload::completed);
        server.responses[payloadPath] = response("changed");
        QCOMPARE(download.start(server.url(), payloadLimit), 0);
        QTRY_COMPARE(done.count(), 1);
        QCOMPARE(result(done).error, EBADMSG);
        QVERIFY(result(done).bytes.isEmpty());
        QCOMPARE(outcomes_[1].bytes, payload_);
        QCOMPARE(outcomes_[1].sha256, QCryptographicHash::hash(payload_, QCryptographicHash::Sha256));
        callbackDownload_ = nullptr;
        callbackServer_ = nullptr;
    }

    void deleteCompleted(rufus::SignedDownloadResult)
    {
        ++deleted_;
        delete qobject_cast<rufus::SignedDownload*>(sender());
    }
    void receiverCanDeleteDownloader()
    {
        HttpFixture server;
        configure(server);
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QPointer<rufus::SignedDownload> download = new rufus::SignedDownload;
        deleted_ = 0;
        connect(download, &rufus::SignedDownload::completed, this, &TestSignedDownload::deleteCompleted);
        QCOMPARE(download->start(server.url(), payloadLimit), 0);
        QTRY_COMPARE(deleted_, 1);
        QVERIFY(download.isNull());
    }

    void destructionDuringReply()
    {
        HttpFixture server;
        configure(server);
        server.holdBodies.insert(payloadPath);
        QVERIFY(server.listen(QHostAddress::LocalHost));
        auto* download = new rufus::SignedDownload;
        QSignalSpy done(download, &rufus::SignedDownload::completed);
        QCOMPARE(download->start(server.url(), payloadLimit), 0);
        QTRY_COMPARE(server.requests.count(), 1);
        delete download;
        QCOMPARE(done.count(), 0);
        QTRY_VERIFY(server.findChildren<QTcpSocket*>().isEmpty());
    }
};
QTEST_GUILESS_MAIN(TestSignedDownload)
#include "test_signed_download.moc"
