// SPDX-License-Identifier: GPL-3.0-or-later
#include "qt/grub2_core_selection.h"
#include "platform/download_signature.h"
#include "../../res/grub2/grub2_version.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QSet>
#include <QNetworkProxy>
#include <QPointer>
#include <QSaveFile>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <cerrno>
#include <cstdarg>
#include <cstring>
#include <fcntl.h>

namespace
{
constexpr quint64 partitionOffset = 1024 * 1024;
bool failCommit = false;
bool failReadback = false;
bool substituteCache = false;
int commits = 0;
QByteArray cachePath;
QByteArray readFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}
bool writeFile(const QString& path, const QByteArray& bytes)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size();
}
QByteArray response(const QByteArray& bytes, int status = 200)
{
    return "HTTP/1.1 " + QByteArray::number(status) + " Fixture\r\nConnection: close\r\nContent-Length: "
        + QByteArray::number(bytes.size()) + "\r\n\r\n" + bytes;
}
}

extern "C" bool __real__ZN9QSaveFile6commitEv(QSaveFile*);
extern "C" bool __wrap__ZN9QSaveFile6commitEv(QSaveFile* file)
{
    ++commits;
    if (failCommit) file->cancelWriting();
    return __real__ZN9QSaveFile6commitEv(file);
}
extern "C" int __real_open64(const char*, int, ...);
extern "C" int __wrap_open64(const char* path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & O_CREAT)
    {
        va_list arguments;
        va_start(arguments, flags);
        mode = va_arg(arguments, int);
        va_end(arguments);
    }
    const bool readback = commits > 0 && cachePath == path && (flags & O_ACCMODE) == O_RDONLY;
    if (readback && failReadback) { errno = EACCES; return -1; }
    if (readback && substituteCache)
    {
        substituteCache = false;
        if (!writeFile(QFile::decodeName(cachePath), QByteArray(512, 'x'))) { errno = EIO; return -1; }
    }
    return __real_open64(path, flags, mode);
}

class SelectionServer final : public QTcpServer
{
    Q_OBJECT
public:
    QMap<QByteArray, QByteArray> responses;
    QList<QByteArray> paths;
    QSet<QByteArray> heldPaths;
    QUrl root() const { return QUrl(QStringLiteral("http://127.0.0.1:%1/files").arg(serverPort())); }
    void release()
    {
        const auto sockets = pending_.keys();
        for (QTcpSocket* socket : sockets)
        {
            const QByteArray wire = pending_.take(socket);
            if (socket->state() != QAbstractSocket::ConnectedState) continue;
            socket->write(wire);
            socket->disconnectFromHost();
        }
    }
protected:
    void incomingConnection(qintptr descriptor) override
    {
        auto* socket = new QTcpSocket(this);
        socket->setSocketDescriptor(descriptor);
        connect(socket, &QTcpSocket::readyRead, this, &SelectionServer::requestReady);
        connect(socket, &QTcpSocket::disconnected, this, &SelectionServer::socketClosed);
    }
private:
    QMap<QTcpSocket*, QByteArray> input_;
    QMap<QTcpSocket*, QByteArray> pending_;
    void requestReady()
    {
        auto* socket = qobject_cast<QTcpSocket*>(sender());
        QByteArray& wire = input_[socket];
        wire += socket->readAll();
        if (!wire.contains("\r\n\r\n")) return;
        const QByteArray path = wire.split(' ').value(1);
        input_.remove(socket);
        paths.append(path);
        disconnect(socket, &QTcpSocket::readyRead, this, &SelectionServer::requestReady);
        const QByteArray reply = responses.value(path, response({}, 404));
        if (heldPaths.contains(path)) pending_.insert(socket, reply);
        else
        {
            socket->write(reply);
            socket->disconnectFromHost();
        }
    }
    void socketClosed()
    {
        auto* socket = qobject_cast<QTcpSocket*>(sender());
        input_.remove(socket);
        pending_.remove(socket);
        socket->deleteLater();
    }
};

class TestGrub2CoreSelection : public QObject
{
    Q_OBJECT
    QByteArray core_, signature_, loader_, loaderSignature_;
    RUFUS_IMG_REPORT report_ = {};
    rufus::PrepareImageRequest source_;
    std::unique_ptr<QTemporaryDir> directory_;
    SelectionServer server_;
    rufus::Grub2CoreSelection* callback_ = nullptr;
    int callbacks_ = 0;
    int restartError_ = -1;
    QList<rufus::Grub2CoreSelectionResult> outcomes_;
    static QByteArray path(const QByteArray& version) { return "/files/grub-" + version + "/core.img"; }
    QString files() const { return directory_->filePath("files"); }
    void candidate(const QByteArray& version, bool valid = true)
    {
        server_.responses[path(version)] = response(core_);
        server_.responses[path(version) + ".sig"] = response(valid ? signature_ : QByteArray(256, 'x'));
    }
    int start(rufus::Grub2CoreSelection& selection, bool mbr = true, bool bios = true)
    {
        RUFUS_IMG_REPORT before;
        memcpy(&before, &report_, sizeof(before));
        const int error = selection.start(source_, report_, files(), mbr, bios, partitionOffset, server_.root());
        if (memcmp(&before, &report_, sizeof(before)) != 0) return EPROTO;
        return error;
    }
    rufus::Grub2CoreSelectionResult result(const QSignalSpy& spy) const
    {
        return qvariant_cast<rufus::Grub2CoreSelectionResult>(spy.first().first());
    }
    void checkEmpty(const rufus::Grub2CoreSelectionResult& result)
    {
        QVERIFY(result.bytes.isEmpty() && result.sha256.isEmpty());
        QCOMPARE(result.origin, rufus::Grub2CoreOrigin::Embedded);
    }
private slots:
    void initTestCase()
    {
        qRegisterMetaType<rufus::Grub2CoreSelectionResult>();
        QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);
        core_ = readFile(QStringLiteral(RUFUS_SIGNED_FIXTURE) + "/core.img");
        signature_ = readFile(QStringLiteral(RUFUS_SIGNED_FIXTURE) + "/core.img.sig");
        QVERIFY(!core_.isEmpty());
        loader_ = readFile(QStringLiteral(RUFUS_GRUB4DOS_SIGNED_FIXTURE) + "/grldr");
        loaderSignature_ = readFile(QStringLiteral(RUFUS_GRUB4DOS_SIGNED_FIXTURE) + "/grldr.sig");
        QCOMPARE(loader_.size(), 322965);
        QCOMPARE(QCryptographicHash::hash(loader_, QCryptographicHash::Sha256).toHex(),
            QByteArray("19ba57329be745ceae46354c5d411b7d8efec6802cfd7f6fee986df4b763f6fe"));
        QCOMPARE(VerifyRufusDownloadSignature(loader_.constData(), size_t(loader_.size()),
            loaderSignature_.constData(), size_t(loaderSignature_.size())), 0);
    }
    void init()
    {
        directory_ = std::make_unique<QTemporaryDir>();
        QVERIFY(directory_->isValid());
        source_ = {};
        const QString image = directory_->filePath("source.iso");
        QVERIFY(writeFile(image, "identity fixture"));
        QCOMPARE(rufus::CapturePrepareSource(image, source_), 0);
        report_ = {};
        report_.has_grub2 = true;
        strcpy(report_.grub2_version, "2.12-nonstandard-demo");
        cachePath = QFile::encodeName(files() + "/grub-2.12-nonstandard-demo/core.img");
        server_.responses.clear();
        server_.paths.clear();
        server_.heldPaths.clear();
        QVERIFY(server_.listen(QHostAddress::LocalHost));
        failCommit = failReadback = substituteCache = false;
        commits = 0;
        callbacks_ = 0;
    }
    void cleanup()
    {
        server_.release();
        QTRY_VERIFY(server_.findChildren<QTcpSocket*>().isEmpty());
        server_.close();
        directory_.reset();
        callback_ = nullptr;
    }
    void cacheAndSkip_data()
    {
        QTest::addColumn<QString>("mode");
        for (const char* mode : {"cache", "unsigned-cache", "empty-cache", "short-cache", "unknown", "embedded", "not-grub", "gpt", "uefi", "unterminated"})
            QTest::newRow(mode) << QString::fromLatin1(mode);
    }
    void cacheAndSkip()
    {
        QFETCH(QString, mode);
        QByteArray bytes;
        if (mode == "cache") bytes = core_;
        if (mode == "unsigned-cache") bytes = QByteArray(512, 'u');
        const bool usable = !bytes.isEmpty();
        if (usable) QVERIFY(writeFile(QFile::decodeName(cachePath), bytes));
        if (mode == "empty-cache") QVERIFY(writeFile(QFile::decodeName(cachePath), {}));
        if (mode == "short-cache") QVERIFY(writeFile(QFile::decodeName(cachePath), "short"));
        if (mode == "unknown") report_.grub2_version[0] = 0;
        if (mode == "embedded") strcpy(report_.grub2_version, GRUB2_PACKAGE_VERSION);
        if (mode == "not-grub") report_.has_grub2 = false;
        if (mode == "unterminated") memset(report_.grub2_version, 'x', sizeof(report_.grub2_version));
        rufus::Grub2CoreSelection selection;
        QSignalSpy prompt(&selection, &rufus::Grub2CoreSelection::downloadRequired);
        QSignalSpy done(&selection, &rufus::Grub2CoreSelection::completed);
        QCOMPARE(start(selection, mode != "gpt", mode != "uefi"), 0);
        QCOMPARE(prompt.count(), 0);
        QCOMPARE(done.count(), 1);
        QCOMPARE(result(done).error, 0);
        QVERIFY(!selection.isActive());
        QVERIFY(server_.paths.isEmpty());
        QCOMPARE(commits, 0);
        if (usable)
        {
            QCOMPARE(result(done).origin, rufus::Grub2CoreOrigin::Cache);
            QCOMPARE(result(done).bytes, bytes);
            QVERIFY(QFile::remove(QFile::decodeName(cachePath)));
            QCOMPARE(result(done).bytes, bytes);
        }
        else checkEmpty(result(done));
    }
    void choices_data()
    {
        QTest::addColumn<bool>("cancel");
        QTest::newRow("no") << false;
        QTest::newRow("cancel") << true;
    }
    void choices()
    {
        QFETCH(bool, cancel);
        // Shortened caches never bypass the full-name prompt.
        QVERIFY(writeFile(files() + "/grub-2.12/core.img", core_));
        report_.has_syslinux = true; // The lookup precedes SBR precedence.
        rufus::Grub2CoreSelection selection;
        QSignalSpy prompt(&selection, &rufus::Grub2CoreSelection::downloadRequired);
        QSignalSpy done(&selection, &rufus::Grub2CoreSelection::completed);
        QCOMPARE(selection.choose(rufus::Grub2DownloadChoice::Yes), EALREADY);
        QCOMPARE(start(selection), 0);
        QCOMPARE(prompt.count(), 1);
        QCOMPARE(prompt.first().first().toString(), QString("2.12-nonstandard-demo"));
        QVERIFY(selection.isAwaitingChoice());
        QCOMPARE(start(selection), EBUSY);
        QCOMPARE(selection.choose(static_cast<rufus::Grub2DownloadChoice>(99)), EINVAL);
        QVERIFY(selection.isAwaitingChoice());
        QCOMPARE(selection.choose(cancel ? rufus::Grub2DownloadChoice::Cancel : rufus::Grub2DownloadChoice::No), 0);
        QCOMPARE(done.count(), 1);
        QCOMPARE(result(done).error, cancel ? ECANCELED : 0);
        checkEmpty(result(done));
        QCOMPARE(selection.choose(rufus::Grub2DownloadChoice::Yes), EALREADY);
        selection.cancel();
        QCOMPARE(done.count(), 1);
        QVERIFY(server_.paths.isEmpty());
        QVERIFY(!QFile::exists(QFile::decodeName(cachePath)));
    }
    void downloadOrder_data()
    {
        QTest::addColumn<QByteArray>("success");
        for (const char* version : {"2.12-nonstandard-demo", "2.12-nonstandard", "2.12", "2", ""})
            QTest::newRow(*version ? version : "exhausted") << QByteArray(version);
    }
    void downloadOrder()
    {
        QFETCH(QByteArray, success);
        if (!success.isEmpty()) candidate(success);
        rufus::Grub2CoreSelection selection;
        QSignalSpy prompt(&selection, &rufus::Grub2CoreSelection::downloadRequired);
        QSignalSpy progress(&selection, &rufus::Grub2CoreSelection::progress);
        QSignalSpy done(&selection, &rufus::Grub2CoreSelection::completed);
        QCOMPARE(start(selection), 0);
        QCOMPARE(selection.choose(rufus::Grub2DownloadChoice::Yes), 0);
        QTRY_COMPARE(done.count(), 1);
        QCOMPARE(prompt.count(), 1);
        QCOMPARE(result(done).error, 0);
        QList<QByteArray> expected;
        for (const char* version : {"2.12-nonstandard-demo", "2.12-nonstandard", "2.12", "2"})
        {
            expected.append(path(version));
            if (success == version) { expected.append(path(version) + ".sig"); break; }
        }
        QCOMPARE(server_.paths, expected);
        if (success.isEmpty())
        {
            checkEmpty(result(done));
            QVERIFY(!QFile::exists(QFile::decodeName(cachePath)));
            QCOMPARE(result(done).candidateError, ENOENT);
        }
        else
        {
            QCOMPARE(result(done).origin, rufus::Grub2CoreOrigin::Download);
            QCOMPARE(result(done).downloadVersion, QString::fromLatin1(success));
            QCOMPARE(result(done).cachePath, QFile::decodeName(cachePath));
            QCOMPARE(result(done).bytes, core_);
            QCOMPARE(result(done).sha256, QCryptographicHash::hash(core_, QCryptographicHash::Sha256));
            QCOMPARE(readFile(QFile::decodeName(cachePath)), core_);
            QCOMPARE(progress.last()[0].toLongLong(), qint64(core_.size()));
            QCOMPARE(commits, 1);
            QVERIFY(QFile::remove(QFile::decodeName(cachePath)));
            QCOMPARE(result(done).bytes, core_);
        }
    }
    void badSignatureThenValid()
    {
        candidate("2.12-nonstandard-demo", false);
        candidate("2.12");
        rufus::Grub2CoreSelection selection;
        QSignalSpy done(&selection, &rufus::Grub2CoreSelection::completed);
        QCOMPARE(start(selection), 0);
        QCOMPARE(selection.choose(rufus::Grub2DownloadChoice::Yes), 0);
        QTRY_COMPARE(done.count(), 1);
        QCOMPARE(result(done).error, 0);
        QCOMPARE(result(done).bytes, core_);
        QCOMPARE(result(done).downloadVersion, QString("2.12"));
        QCOMPARE(commits, 1); // The bad signature was never installed.
        QCOMPARE(server_.paths, QList<QByteArray>({path("2.12-nonstandard-demo"), path("2.12-nonstandard-demo") + ".sig",
            path("2.12-nonstandard"), path("2.12"), path("2.12") + ".sig"}));
    }
    void finalSignatureRejection_data()
    {
        QTest::addColumn<bool>("missing");
        QTest::addColumn<QByteArray>("version");
        QTest::newRow("last-missing-signature") << true << QByteArray("2");
        QTest::newRow("last-invalid-signature") << false << QByteArray("2");
        QTest::newRow("bad-first-then-not-found") << false << QByteArray("2.12-nonstandard-demo");
    }
    void finalSignatureRejection()
    {
        QFETCH(bool, missing);
        QFETCH(QByteArray, version);
        candidate(version, false);
        if (missing) server_.responses[path(version) + ".sig"] = response({}, 404);
        rufus::Grub2CoreSelection selection;
        QSignalSpy done(&selection, &rufus::Grub2CoreSelection::completed);
        QCOMPARE(start(selection), 0);
        QCOMPARE(selection.choose(rufus::Grub2DownloadChoice::Yes), 0);
        QTRY_COMPARE(done.count(), 1);
        const bool rejectedLast = version == "2";
        QCOMPARE(result(done).error, rejectedLast ? EBADMSG : 0);
        QCOMPARE(result(done).candidateError, missing || !rejectedLast ? ENOENT : EBADMSG);
        checkEmpty(result(done));
        QCOMPARE(commits, 0);
        QVERIFY(!QFile::exists(QFile::decodeName(cachePath)));
        QCOMPARE(server_.paths.last(), rejectedLast ? path("2") + ".sig" : path("2"));
    }
    void installedReadback_data()
    {
        QTest::addColumn<QString>("mode");
        for (const char* mode : {"save-error", "readback-error", "substituted-cache"})
            QTest::newRow(mode) << QString::fromLatin1(mode);
    }
    void installedReadback()
    {
        QFETCH(QString, mode);
        candidate("2.12-nonstandard-demo");
        failCommit = mode == "save-error";
        failReadback = mode == "readback-error";
        substituteCache = mode == "substituted-cache";
        rufus::Grub2CoreSelection selection;
        QSignalSpy done(&selection, &rufus::Grub2CoreSelection::completed);
        QCOMPARE(start(selection), 0);
        QCOMPARE(selection.choose(rufus::Grub2DownloadChoice::Yes), 0);
        QTRY_COMPARE(done.count(), 1);
        QCOMPARE(result(done).error, mode == "substituted-cache" ? ESTALE : 0);
        checkEmpty(result(done));
        QCOMPARE(commits, 1);
        if (mode == "save-error")
        {
            QVERIFY(!QFile::exists(QFile::decodeName(cachePath)));
            QCOMPARE(server_.paths.last(), path("2"));
        }
        else QCOMPARE(server_.paths.count(), 2); // Opened-install failure falls back without another search.
    }
    void staleSource_data()
    {
        QTest::addColumn<QString>("when");
        for (const char* when : {"before-start", "prompt-yes", "prompt-no", "downloading", "invalidate-prompt", "invalidate-download"})
            QTest::newRow(when) << QString::fromLatin1(when);
    }
    void staleSource()
    {
        QFETCH(QString, when);
        candidate("2.12-nonstandard-demo");
        server_.heldPaths.insert(path("2.12-nonstandard-demo"));
        if (when == "before-start") QVERIFY(writeFile(source_.imagePath, "changed source"));
        rufus::Grub2CoreSelection selection;
        QSignalSpy done(&selection, &rufus::Grub2CoreSelection::completed);
        QCOMPARE(start(selection), 0);
        const bool duringDownload = when == "downloading" || when == "invalidate-download";
        if (duringDownload)
        {
            QCOMPARE(selection.choose(rufus::Grub2DownloadChoice::Yes), 0);
            QTRY_COMPARE(server_.paths.count(), 1);
        }
        if (when.startsWith("invalidate")) selection.invalidate();
        else if (when != "before-start")
        {
            QVERIFY(writeFile(source_.imagePath, "changed source"));
            if (duringDownload) server_.release();
            else QCOMPARE(selection.choose(when == "prompt-no" ? rufus::Grub2DownloadChoice::No : rufus::Grub2DownloadChoice::Yes), 0);
        }
        QTRY_COMPARE(done.count(), 1);
        QCOMPARE(result(done).error, ESTALE);
        checkEmpty(result(done));
        QVERIFY(!QFile::exists(QFile::decodeName(cachePath)));
        QCOMPARE(commits, 0);
        if (!duringDownload) QVERIFY(server_.paths.isEmpty());
    }
    void cancelPending_data()
    {
        QTest::addColumn<QString>("when");
        for (const char* when : {"prompt", "payload", "signature"})
            QTest::newRow(when) << QString::fromLatin1(when);
    }
    void cancelPending()
    {
        QFETCH(QString, when);
        candidate("2.12-nonstandard-demo");
        const QByteArray held = path("2.12-nonstandard-demo") + (when == "signature" ? ".sig" : "");
        server_.heldPaths.insert(held);
        rufus::Grub2CoreSelection selection;
        QSignalSpy done(&selection, &rufus::Grub2CoreSelection::completed);
        QCOMPARE(start(selection), 0);
        if (when != "prompt")
        {
            QCOMPARE(selection.choose(rufus::Grub2DownloadChoice::Yes), 0);
            QTRY_VERIFY(server_.paths.contains(held));
        }
        selection.cancel();
        selection.cancel();
        QCOMPARE(done.count(), 1);
        QCOMPARE(result(done).error, ECANCELED);
        checkEmpty(result(done));
        QCOMPARE(commits, 0);
        server_.release();
        QVERIFY(!QFile::exists(QFile::decodeName(cachePath)));
    }
    void cancelInPrompt(QString)
    {
        ++callbacks_;
        callback_->cancel();
    }
    void synchronousPromptCancellation()
    {
        rufus::Grub2CoreSelection selection;
        callback_ = &selection;
        connect(&selection, &rufus::Grub2CoreSelection::downloadRequired, this, &TestGrub2CoreSelection::cancelInPrompt);
        QSignalSpy done(&selection, &rufus::Grub2CoreSelection::completed);
        QCOMPARE(start(selection), 0);
        QCOMPARE(callbacks_, 1);
        QCOMPARE(done.count(), 1);
        QCOMPARE(result(done).error, ECANCELED);
    }
    void deleteOnComplete(rufus::Grub2CoreSelectionResult)
    {
        ++callbacks_;
        delete qobject_cast<rufus::Grub2CoreSelection*>(sender());
    }
    void receiverCanDelete()
    {
        QPointer<rufus::Grub2CoreSelection> selection = new rufus::Grub2CoreSelection;
        connect(selection, &rufus::Grub2CoreSelection::completed, this, &TestGrub2CoreSelection::deleteOnComplete);
        QCOMPARE(start(*selection), 0);
        QCOMPARE(selection->choose(rufus::Grub2DownloadChoice::No), 0);
        QCOMPARE(callbacks_, 1);
        QVERIFY(selection.isNull());
    }
    void reusesAfterInvalidation()
    {
        candidate("2.12-nonstandard-demo");
        server_.heldPaths.insert(path("2.12-nonstandard-demo"));
        rufus::Grub2CoreSelection selection;
        QSignalSpy done(&selection, &rufus::Grub2CoreSelection::completed);
        QCOMPARE(start(selection), 0);
        QCOMPARE(selection.choose(rufus::Grub2DownloadChoice::Yes), 0);
        QTRY_COMPARE(server_.paths.count(), 1);
        selection.invalidate();
        QCOMPARE(result(done).error, ESTALE);
        server_.heldPaths.clear();
        server_.release();
        done.clear();
        QCOMPARE(start(selection), 0);
        QCOMPARE(selection.choose(rufus::Grub2DownloadChoice::Yes), 0);
        QTRY_COMPARE(done.count(), 1);
        QCOMPARE(result(done).error, 0);
        QCOMPARE(result(done).bytes, core_);
    }
    void restartAfterComplete(rufus::Grub2CoreSelectionResult result)
    {
        outcomes_.append(result);
        if (outcomes_.size() == 1) restartError_ = start(*callback_);
    }
    void completionCanRestart()
    {
        candidate("2.12-nonstandard-demo");
        rufus::Grub2CoreSelection selection;
        callback_ = &selection;
        outcomes_.clear();
        restartError_ = -1;
        connect(&selection, &rufus::Grub2CoreSelection::completed, this, &TestGrub2CoreSelection::restartAfterComplete);
        QCOMPARE(start(selection), 0);
        QCOMPARE(selection.choose(rufus::Grub2DownloadChoice::Yes), 0);
        QTRY_COMPARE(outcomes_.size(), 2);
        QCOMPARE(restartError_, 0);
        QCOMPARE(outcomes_[0].origin, rufus::Grub2CoreOrigin::Download);
        QCOMPARE(outcomes_[1].origin, rufus::Grub2CoreOrigin::Cache);
        QCOMPARE(outcomes_[0].bytes, core_);
        QCOMPARE(outcomes_[1].bytes, core_);
        QVERIFY(!selection.isActive());
        QCOMPARE(server_.paths.count(), 2);
        QCOMPARE(commits, 1);
    }
    void invalidStart_data()
    {
        QTest::addColumn<QString>("reason");
        for (const char* reason : {"relative-source", "nul-source", "relative-cache", "nul-cache", "zero-gap", "no-gap",
            "misaligned-gap", "huge-gap", "file-server", "missing-host", "credentials", "query", "fragment"})
            QTest::newRow(reason) << QString::fromLatin1(reason);
    }
    void invalidStart()
    {
        QFETCH(QString, reason);
        QString cache = files();
        QUrl server = server_.root();
        quint64 offset = partitionOffset;
        if (reason == "relative-source") source_.imagePath = "relative.iso";
        if (reason == "nul-source") source_.imagePath += QChar(u'\0');
        if (reason == "relative-cache") cache = "relative-cache";
        if (reason == "nul-cache") cache += QChar(u'\0');
        if (reason == "zero-gap") offset = 0;
        if (reason == "no-gap") offset = 512;
        if (reason == "misaligned-gap") offset = 1025;
        if (reason == "huge-gap") offset += 512;
        if (reason == "file-server") server = QUrl("file:///tmp/files");
        if (reason == "missing-host") server = QUrl("http:///files");
        if (reason == "credentials") server.setUserInfo("user:pass");
        if (reason == "query") server.setQuery("query=1");
        if (reason == "fragment") server.setFragment("fragment");
        rufus::Grub2CoreSelection selection;
        QSignalSpy prompt(&selection, &rufus::Grub2CoreSelection::downloadRequired);
        QSignalSpy done(&selection, &rufus::Grub2CoreSelection::completed);
        QCOMPARE(selection.start(source_, report_, cache, true, true, offset, server), EINVAL);
        QCOMPARE(prompt.count(), 0);
        QCOMPARE(done.count(), 0);
        QVERIFY(!selection.isActive());
        QVERIFY(server_.paths.isEmpty());
        QVERIFY(!QDir(files()).exists());
        selection.cancel();
        QCOMPARE(done.count(), 0);
    }
    void grub4dosCacheAndChoices_data()
    {
        QTest::addColumn<QString>("mode");
        for (const char* mode : {"cache", "unsigned-cache", "empty-cache", "short-cache", "directory", "other-version", "no", "cancel"})
            QTest::newRow(mode) << QString::fromLatin1(mode);
    }
    void grub4dosCacheAndChoices()
    {
        QFETCH(QString, mode);
        cachePath = QFile::encodeName(files() + "/grub4dos-0.4.6a/grldr");
        const QString cache = QFile::decodeName(cachePath);
        if (mode == "cache") QVERIFY(writeFile(cache, loader_));
        if (mode == "unsigned-cache") QVERIFY(writeFile(cache, QByteArray(512, 'u')));
        if (mode == "empty-cache") QVERIFY(writeFile(cache, {}));
        if (mode == "short-cache") QVERIFY(writeFile(cache, QByteArray(511, 's')));
        if (mode == "directory") QVERIFY(QDir().mkpath(cache));
        if (mode == "other-version") QVERIFY(writeFile(files() + "/grub4dos-0.4.5/grldr", loader_));
        // Standalone selection has no ISO dependency, including after GRUB2 use.
        QVERIFY(QFile::remove(source_.imagePath));
        rufus::Grub2CoreSelection selection;
        QSignalSpy prompt(&selection, &rufus::Grub2CoreSelection::downloadRequired);
        QSignalSpy done(&selection, &rufus::Grub2CoreSelection::completed);
        QCOMPARE(selection.startGrub4dos(files(), server_.root()), 0);
        const bool usable = mode == "cache" || mode == "unsigned-cache";
        const bool malformed = mode == "empty-cache" || mode == "short-cache" || mode == "directory";
        if (!usable && !malformed)
        {
            QCOMPARE(prompt.count(), 1);
            QCOMPARE(prompt.first().first().toString(), QString("0.4.6a"));
            QCOMPARE(selection.startGrub4dos(files(), server_.root()), EBUSY);
            QCOMPARE(start(selection), EBUSY);
            QCOMPARE(selection.choose(static_cast<rufus::Grub2DownloadChoice>(99)), EINVAL);
            QVERIFY(selection.isAwaitingChoice());
            QCOMPARE(selection.choose(mode == "cancel" ? rufus::Grub2DownloadChoice::Cancel : rufus::Grub2DownloadChoice::No), 0);
        }
        QCOMPARE(done.count(), 1);
        const int error = usable ? 0 : mode == "cancel" ? ECANCELED : mode == "directory" ? EINVAL
            : malformed ? EBADMSG : ENOENT;
        QCOMPARE(result(done).error, error);
        QCOMPARE(result(done).cachePath, cache);
        QVERIFY(!selection.isActive() && !selection.isAwaitingChoice());
        QCOMPARE(selection.choose(rufus::Grub2DownloadChoice::Yes), EALREADY);
        selection.cancel();
        QCOMPARE(done.count(), 1);
        QVERIFY(server_.paths.isEmpty());
        QCOMPARE(commits, 0);
        if (usable)
        {
            const QByteArray expected = mode == "cache" ? loader_ : QByteArray(512, 'u');
            QCOMPARE(result(done).origin, rufus::Grub2CoreOrigin::Cache);
            QCOMPARE(result(done).bytes, expected);
            QCOMPARE(result(done).sha256, QCryptographicHash::hash(expected, QCryptographicHash::Sha256));
            QVERIFY(QFile::remove(cache));
            QCOMPARE(result(done).bytes, expected);
        }
        else checkEmpty(result(done));
    }

    void grub4dosDownload_data()
    {
        QTest::addColumn<QString>("mode");
        for (const char* mode : {"success", "payload-missing", "signature-missing", "bad-signature", "changed-payload",
            "oversized", "save-error", "readback-error", "substituted-cache"})
            QTest::newRow(mode) << QString::fromLatin1(mode);
    }
    void grub4dosDownload()
    {
        QFETCH(QString, mode);
        const QByteArray payloadPath = "/files/grub4dos-0.4.6a/grldr";
        cachePath = QFile::encodeName(files() + "/grub4dos-0.4.6a/grldr");
        server_.responses[payloadPath] = response(loader_);
        server_.responses[payloadPath + ".sig"] = response(loaderSignature_);
        int expected = 0;
        if (mode == "payload-missing") { server_.responses.remove(payloadPath); expected = ENOENT; }
        if (mode == "signature-missing") { server_.responses.remove(payloadPath + ".sig"); expected = ENOENT; }
        if (mode == "bad-signature") { server_.responses[payloadPath + ".sig"] = response(QByteArray(256, 'x')); expected = EBADMSG; }
        if (mode == "changed-payload")
        {
            QByteArray changed = loader_; changed[changed.size() - 1] ^= 1;
            server_.responses[payloadPath] = response(changed); expected = EBADMSG;
        }
        if (mode == "oversized") { server_.responses[payloadPath] = response(QByteArray(1024 * 1024 - 511, 'x')); expected = EFBIG; }
        if (mode == "save-error") { failCommit = true; expected = EIO; }
        if (mode == "readback-error") { failReadback = true; expected = EACCES; }
        if (mode == "substituted-cache") { substituteCache = true; expected = ESTALE; }
        rufus::Grub2CoreSelection selection;
        QSignalSpy prompt(&selection, &rufus::Grub2CoreSelection::downloadRequired);
        QSignalSpy progress(&selection, &rufus::Grub2CoreSelection::progress);
        QSignalSpy done(&selection, &rufus::Grub2CoreSelection::completed);
        QCOMPARE(selection.startGrub4dos(files(), server_.root()), 0);
        QCOMPARE(selection.choose(rufus::Grub2DownloadChoice::Yes), 0);
        QTRY_COMPARE(done.count(), 1);
        QCOMPARE(prompt.count(), 1);
        QCOMPARE(result(done).error, expected);
        QCOMPARE(result(done).candidateError, expected);
        QCOMPARE(result(done).downloadVersion, QString("0.4.6a"));
        const bool noSignature = mode == "payload-missing" || mode == "oversized";
        const QList<QByteArray> expectedPaths = noSignature ? QList<QByteArray>({payloadPath})
            : QList<QByteArray>({payloadPath, payloadPath + ".sig"});
        QCOMPARE(server_.paths, expectedPaths); // No prefix retries or GRUB2 fallback.
        const bool saved = mode == "success" || mode == "save-error" || mode == "readback-error" || mode == "substituted-cache";
        QCOMPARE(commits, int(saved));
        if (expected == 0)
        {
            QCOMPARE(result(done).origin, rufus::Grub2CoreOrigin::Download);
            QCOMPARE(result(done).bytes, loader_);
            QCOMPARE(result(done).sha256, QCryptographicHash::hash(loader_, QCryptographicHash::Sha256));
            QCOMPARE(readFile(QFile::decodeName(cachePath)), loader_);
            QCOMPARE(progress.last()[0].toLongLong(), qint64(loader_.size()));
            QVERIFY(writeFile(QFile::decodeName(cachePath), QByteArray(512, 'x')));
            QCOMPARE(result(done).bytes, loader_); // Accepted bytes are frozen.
        }
        else
        {
            checkEmpty(result(done));
            if (!saved || mode == "save-error") QVERIFY(!QFile::exists(QFile::decodeName(cachePath)));
        }
    }

    void grub4dosPending_data()
    {
        QTest::addColumn<QString>("when"); QTest::addColumn<bool>("invalidate");
        for (const char* when : {"prompt", "payload", "signature"})
        for (bool invalidate : {false, true})
            QTest::newRow(qPrintable(QString::fromLatin1(when) + (invalidate ? "-invalidate" : "-cancel")))
                << QString::fromLatin1(when) << invalidate;
    }
    void grub4dosPending()
    {
        QFETCH(QString, when); QFETCH(bool, invalidate);
        const QByteArray payloadPath = "/files/grub4dos-0.4.6a/grldr";
        server_.responses[payloadPath] = response(loader_);
        server_.responses[payloadPath + ".sig"] = response(loaderSignature_);
        const QByteArray held = payloadPath + (when == "signature" ? ".sig" : "");
        server_.heldPaths.insert(held);
        rufus::Grub2CoreSelection selection;
        QSignalSpy done(&selection, &rufus::Grub2CoreSelection::completed);
        QCOMPARE(selection.startGrub4dos(files(), server_.root()), 0);
        if (when != "prompt")
        {
            QCOMPARE(selection.choose(rufus::Grub2DownloadChoice::Yes), 0);
            QTRY_VERIFY(server_.paths.contains(held));
        }
        if (invalidate) selection.invalidate();
        else selection.cancel();
        selection.cancel();
        QCOMPARE(done.count(), 1);
        QCOMPARE(result(done).error, invalidate ? ESTALE : ECANCELED);
        checkEmpty(result(done));
        server_.release();
        QCOMPARE(commits, 0);
        QVERIFY(!QFile::exists(files() + "/grub4dos-0.4.6a/grldr"));
    }

    void grub4dosInvalidStart_data()
    {
        QTest::addColumn<QString>("reason");
        for (const char* reason : {"relative-cache", "nul-cache", "file-server", "missing-host", "credentials", "query", "fragment"})
            QTest::newRow(reason) << QString::fromLatin1(reason);
    }
    void grub4dosInvalidStart()
    {
        QFETCH(QString, reason);
        QString cache = files(); QUrl server = server_.root();
        if (reason == "relative-cache") cache = "relative";
        if (reason == "nul-cache") cache += QChar(u'\0');
        if (reason == "file-server") server = QUrl("file:///tmp/files");
        if (reason == "missing-host") server = QUrl("http:///files");
        if (reason == "credentials") server.setUserInfo("user:pass");
        if (reason == "query") server.setQuery("query=1");
        if (reason == "fragment") server.setFragment("fragment");
        rufus::Grub2CoreSelection selection;
        QSignalSpy done(&selection, &rufus::Grub2CoreSelection::completed);
        QCOMPARE(selection.startGrub4dos(cache, server), EINVAL);
        QCOMPARE(done.count(), 0);
        QVERIFY(!selection.isActive() && server_.paths.isEmpty());
        QVERIFY(!QDir(files()).exists());
    }

    void grub4dosProviderReuse()
    {
        QVERIFY(writeFile(QFile::decodeName(cachePath), core_));
        QVERIFY(writeFile(files() + "/grub4dos-0.4.6a/grldr", loader_));
        rufus::Grub2CoreSelection selection;
        QSignalSpy done(&selection, &rufus::Grub2CoreSelection::completed);
        QCOMPARE(start(selection), 0);
        QCOMPARE(result(done).bytes, core_);
        done.clear();
        QVERIFY(QFile::remove(source_.imagePath));
        QCOMPARE(selection.startGrub4dos(files(), server_.root()), 0);
        QCOMPARE(result(done).bytes, loader_);
        done.clear();
        QCOMPARE(start(selection), 0); // ISO identity checks are restored for GRUB2.
        QCOMPARE(result(done).error, ENOENT);
        checkEmpty(result(done));
        QCOMPARE(commits, 0);
        QVERIFY(server_.paths.isEmpty());
        done.clear();
        QVERIFY(writeFile(source_.imagePath, "replacement ISO identity"));
        QCOMPARE(rufus::CapturePrepareSource(source_.imagePath, source_), 0);
        QVERIFY(QFile::remove(QFile::decodeName(cachePath)));
        candidate("2.12-nonstandard-demo");
        QCOMPARE(start(selection), 0);
        QCOMPARE(selection.choose(rufus::Grub2DownloadChoice::Yes), 0);
        QTRY_COMPARE(done.count(), 1);
        QCOMPARE(result(done).error, 0);
        QCOMPARE(result(done).bytes, core_);
        QCOMPARE(server_.paths, QList<QByteArray>({path("2.12-nonstandard-demo"), path("2.12-nonstandard-demo") + ".sig"}));
        QCOMPARE(commits, 1);
    }

    void grub4dosPromptCancellation()
    {
        rufus::Grub2CoreSelection selection;
        callback_ = &selection;
        connect(&selection, &rufus::Grub2CoreSelection::downloadRequired, this, &TestGrub2CoreSelection::cancelInPrompt);
        QSignalSpy done(&selection, &rufus::Grub2CoreSelection::completed);
        QCOMPARE(selection.startGrub4dos(files(), server_.root()), 0);
        QCOMPARE(callbacks_, 1);
        QCOMPARE(done.count(), 1);
        QCOMPARE(result(done).error, ECANCELED);
    }
    void restartGrub4dos(rufus::Grub2CoreSelectionResult result)
    {
        outcomes_.append(result);
        if (outcomes_.size() == 1) restartError_ = callback_->startGrub4dos(files(), server_.root());
    }
    void grub4dosCompletionCanRestart()
    {
        const QByteArray payloadPath = "/files/grub4dos-0.4.6a/grldr";
        server_.responses[payloadPath] = response(loader_);
        server_.responses[payloadPath + ".sig"] = response(loaderSignature_);
        rufus::Grub2CoreSelection selection;
        callback_ = &selection;
        outcomes_.clear();
        restartError_ = -1;
        connect(&selection, &rufus::Grub2CoreSelection::completed, this, &TestGrub2CoreSelection::restartGrub4dos);
        QCOMPARE(selection.startGrub4dos(files(), server_.root()), 0);
        QCOMPARE(selection.choose(rufus::Grub2DownloadChoice::Yes), 0);
        QTRY_COMPARE(outcomes_.size(), 2);
        QCOMPARE(restartError_, 0);
        QCOMPARE(outcomes_[0].origin, rufus::Grub2CoreOrigin::Download);
        QCOMPARE(outcomes_[1].origin, rufus::Grub2CoreOrigin::Cache);
        QCOMPARE(outcomes_[0].bytes, loader_);
        QCOMPARE(outcomes_[1].bytes, loader_);
        QVERIFY(!selection.isActive());
        QCOMPARE(server_.paths, QList<QByteArray>({payloadPath, payloadPath + ".sig"}));
        QCOMPARE(commits, 1);
    }
    void grub4dosReceiverCanDelete()
    {
        QPointer<rufus::Grub2CoreSelection> selection = new rufus::Grub2CoreSelection;
        connect(selection, &rufus::Grub2CoreSelection::completed, this, &TestGrub2CoreSelection::deleteOnComplete);
        QCOMPARE(selection->startGrub4dos(files(), server_.root()), 0);
        QCOMPARE(selection->choose(rufus::Grub2DownloadChoice::No), 0);
        QCOMPARE(callbacks_, 1);
        QVERIFY(selection.isNull());
    }
    void grub4dosDestructionAborts()
    {
        const QByteArray payloadPath = "/files/grub4dos-0.4.6a/grldr";
        server_.responses[payloadPath] = response(loader_);
        server_.heldPaths.insert(payloadPath);
        auto* selection = new rufus::Grub2CoreSelection;
        QSignalSpy done(selection, &rufus::Grub2CoreSelection::completed);
        QCOMPARE(selection->startGrub4dos(files(), server_.root()), 0);
        QCOMPARE(selection->choose(rufus::Grub2DownloadChoice::Yes), 0);
        QTRY_COMPARE(server_.paths.count(), 1);
        delete selection;
        QCOMPARE(done.count(), 0);
        QCOMPARE(commits, 0);
    }

    void destructionAborts()
    {
        candidate("2.12-nonstandard-demo");
        server_.heldPaths.insert(path("2.12-nonstandard-demo"));
        auto* selection = new rufus::Grub2CoreSelection;
        QSignalSpy done(selection, &rufus::Grub2CoreSelection::completed);
        QCOMPARE(start(*selection), 0);
        QCOMPARE(selection->choose(rufus::Grub2DownloadChoice::Yes), 0);
        QTRY_COMPARE(server_.paths.count(), 1);
        delete selection;
        QCOMPARE(done.count(), 0);
        QCOMPARE(commits, 0);
    }
};
QTEST_GUILESS_MAIN(TestGrub2CoreSelection)
#include "test_grub2_core_selection.moc"
