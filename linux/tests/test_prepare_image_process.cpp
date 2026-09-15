// SPDX-License-Identifier: GPL-3.0-or-later
#include "qt/prepare_image_process.h"

#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <cerrno>
#include <memory>
#include <sys/stat.h>

class TestPrepareImageProcess : public QObject
{
    Q_OBJECT
    QTemporaryDir directory_;
    rufus::PrepareImageRequest request_;

private slots:
    void init()
    {
        QVERIFY(directory_.isValid());
        request_ = {};
        request_.path = directory_.filePath(QStringLiteral("target.img"));
        request_.size = 64 * 1024 * 1024;
        request_.volumeId = 0x1234abcd;
        request_.label = QStringLiteral("RUFUS TEST");
        QFile file(request_.path);
        QVERIFY(file.open(QIODevice::ReadWrite | QIODevice::Truncate));
        QVERIFY(file.resize(qint64(request_.size)));
        struct stat info;
        QCOMPARE(fstat(file.handle(), &info), 0);
        request_.device = quint64(info.st_dev);
        request_.inode = quint64(info.st_ino);
    }

    void nativeCompletionAndReuse()
    {
        rufus::PrepareImageProcess process(QString::fromLocal8Bit(RUFUS_PREPARE_HELPER));
        QSignalSpy completed(&process, &rufus::PrepareImageProcess::finished);
        QSignalSpy phases(&process, &rufus::PrepareImageProcess::phaseChanged);
        QVERIFY(process.start(request_));
        QVERIFY(!process.start(request_));
        QTRY_COMPARE_WITH_TIMEOUT(completed.size(), 1, 10000);
        QCOMPARE(process.state(), rufus::PrepareImageProcess::State::Complete);
        QVERIFY(!process.isActive());
        QCOMPARE(process.outcome().error, 0);
        QVERIFY(process.outcome().started);
        QVERIFY(process.outcome().filesystemBytes > 0);
        QCOMPARE(phases.size(), 3);
        QCOMPARE(phases[0][0].toString(), QStringLiteral("checking"));
        QCOMPARE(phases[1][0].toString(), QStringLiteral("partitioning"));
        QCOMPARE(phases[2][0].toString(), QStringLiteral("formatting"));
        QVERIFY(process.start(request_));
        QTRY_COMPARE_WITH_TIMEOUT(completed.size(), 2, 10000);
        QCOMPARE(process.state(), rufus::PrepareImageProcess::State::Complete);
    }

    void cancelBeforeRequest()
    {
        rufus::PrepareImageProcess process(QString::fromLocal8Bit(RUFUS_PREPARE_HELPER));
        QSignalSpy completed(&process, &rufus::PrepareImageProcess::finished);
        QVERIFY(process.start(request_));
        process.cancel();
        process.cancel();
        QTRY_COMPARE_WITH_TIMEOUT(completed.size(), 1, 5000);
        QCOMPARE(process.state(), rufus::PrepareImageProcess::State::Cancelled);
        QVERIFY(!process.outcome().started);
        QFile file(request_.path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray(qsizetype(request_.size), '\0'));
    }

    void changedTargetFailsBeforeWrites()
    {
        rufus::PrepareImageProcess process(QString::fromLocal8Bit(RUFUS_PREPARE_HELPER));
        QSignalSpy completed(&process, &rufus::PrepareImageProcess::finished);
        ++request_.inode;
        QVERIFY(process.start(request_));
        QTRY_COMPARE_WITH_TIMEOUT(completed.size(), 1, 5000);
        QCOMPARE(process.state(), rufus::PrepareImageProcess::State::Failed);
        QCOMPARE(process.outcome().error, ESTALE);
        QVERIFY(!process.outcome().started);
        QVERIFY(!process.problem().isEmpty());
    }

    void failedLaunchDoesNotRetry()
    {
        rufus::PrepareImageProcess process(directory_.filePath(QStringLiteral("missing")));
        QSignalSpy completed(&process, &rufus::PrepareImageProcess::finished);
        QVERIFY(process.start(request_));
        QTRY_COMPARE_WITH_TIMEOUT(completed.size(), 1, 5000);
        QCOMPARE(process.state(), rufus::PrepareImageProcess::State::Failed);
        QVERIFY(!process.isActive());
        QVERIFY(!process.outcome().started);
    }

    void invalidHelperResponse_data()
    {
        QTest::addColumn<QByteArray>("reply");
        QTest::newRow("invalid-json") << QByteArray("not json\n");
        QTest::newRow("second-ready") << QByteArray("{\"event\":\"ready\"}\n");
        QTest::newRow("missing-result") << QByteArray();
        QTest::newRow("oversized") << QByteArray(8192, 'x');
        QTest::newRow("incomplete") << QByteArray("{\"event\":");
    }

    void flushFailureReportsPartialImage()
    {
        qputenv("RUFUS_TEST_SYNC_ERROR", "1");
        rufus::PrepareImageProcess process(QString::fromLocal8Bit(RUFUS_PREPARE_PROBE));
        QSignalSpy completed(&process, &rufus::PrepareImageProcess::finished);
        QVERIFY(process.start(request_));
        QTRY_COMPARE_WITH_TIMEOUT(completed.size(), 1, 5000);
        QCOMPARE(process.state(), rufus::PrepareImageProcess::State::Failed);
        QVERIFY(process.outcome().started);
        QCOMPARE(process.outcome().syncError, EIO);
        QVERIFY(process.problem().contains(QStringLiteral("flush")));
    }

    void invalidHelperResponse()
    {
        QFETCH(QByteArray, reply);
        const QString name = directory_.filePath(QStringLiteral("invalid-helper"));
        QFile script(name);
        QVERIFY(script.open(QIODevice::WriteOnly | QIODevice::Truncate));
        const QByteArray code = "#!/usr/bin/python3\nimport sys\n"
            "print('{\"event\":\"ready\"}', flush=True)\nsys.stdin.readline()\n"
            "sys.stdout.buffer.write(bytes.fromhex('" + reply.toHex() + "'))\n"
            "sys.stdout.flush()\n";
        QCOMPARE(script.write(code), code.size());
        QVERIFY(script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        script.close();
        rufus::PrepareImageProcess process(name);
        QSignalSpy completed(&process, &rufus::PrepareImageProcess::finished);
        QVERIFY(process.start(request_));
        QTRY_COMPARE_WITH_TIMEOUT(completed.size(), 1, 5000);
        QCOMPARE(process.state(), rufus::PrepareImageProcess::State::Failed);
        QVERIFY(process.outcome().started); // A lost result cannot promise unchanged media.
        QVERIFY(!process.problem().isEmpty());
    }

    void activeCancellationAndDestruction_data()
    {
        QTest::addColumn<bool>("destroy");
        QTest::newRow("cancel") << false;
        QTest::newRow("destroy-controller") << true;
    }

    void activeCancellationAndDestruction()
    {
        QFETCH(bool, destroy);
        const QString marker = directory_.filePath(QStringLiteral("child.pid"));
        const QString name = directory_.filePath(QStringLiteral("formatter"));
        QFile::remove(marker);
        QFile script(name);
        QVERIFY(script.open(QIODevice::WriteOnly | QIODevice::Truncate));
        const QByteArray code = "#!/usr/bin/python3\nimport os, signal, time\n"
            "from pathlib import Path\nsignal.signal(signal.SIGTERM, signal.SIG_IGN)\n"
            "Path(os.environ['RUFUS_TEST_PID']).write_text(str(os.getpid()))\ntime.sleep(60)\n";
        QCOMPARE(script.write(code), code.size());
        QVERIFY(script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        script.close();
        qputenv("RUFUS_TEST_FORMATTER", QFile::encodeName(name));
        qputenv("RUFUS_TEST_PID", QFile::encodeName(marker));
        std::unique_ptr<rufus::PrepareImageProcess> process = std::make_unique<rufus::PrepareImageProcess>(
            QString::fromLocal8Bit(RUFUS_PREPARE_PROBE));
        QSignalSpy completed(process.get(), &rufus::PrepareImageProcess::finished);
        QVERIFY(process->start(request_));
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo(marker).size() > 0, 5000);
        QFile pidFile(marker);
        QVERIFY(pidFile.open(QIODevice::ReadOnly));
        const QByteArray pid = pidFile.readAll();
        QVERIFY(pid.toInt() > 0);
        if (destroy)
            process.reset();
        else
        {
            process->cancel();
            QTRY_COMPARE_WITH_TIMEOUT(completed.size(), 1, 5000);
            QCOMPARE(process->state(), rufus::PrepareImageProcess::State::Cancelled);
            QVERIFY(process->outcome().started);
            QCOMPARE(process->outcome().syncError, 0);
        }
        QVERIFY(!QFileInfo::exists(QStringLiteral("/proc/") + QString::fromLatin1(pid)));
    }

    void cleanup()
    {
        qunsetenv("RUFUS_TEST_FORMATTER");
        qunsetenv("RUFUS_TEST_PID");
        qunsetenv("RUFUS_TEST_SYNC_ERROR");
    }
};

QTEST_GUILESS_MAIN(TestPrepareImageProcess)
#include "test_prepare_image_process.moc"
