// SPDX-License-Identifier: GPL-3.0-or-later
#include <QApplication>
#include <QFile>
#include <QProcess>
#include <QScreen>
#include <QTest>
#include <fcntl.h>
#include <unistd.h>

namespace
{
constexpr int callerUid = 1000;
constexpr int processTimeoutMs = 3000;
constexpr int authenticationTimeoutMs = 15000;
constexpr int jobTimeoutMs = 30000;
constexpr int pkexecDismissed = 126;
constexpr int pkexecFailed = 127;

QByteArray readFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

bool fixtureGuest()
{
    const bool ordinaryUser = getuid() == callerUid && geteuid() == callerUid;
    const bool marker = readFile(QStringLiteral("/etc/rufus-gpt-fixture")) == "RUFUS-GPT-TEST\n";
    const bool machine = readFile(QStringLiteral("/sys/class/dmi/id/product_name")).trimmed() == "RufusUSBFixture";
    return ordinaryUser && marker && machine;
}

QByteArray xdo(const QStringList& arguments)
{
    QProcess process;
    process.start(QStringLiteral("/usr/bin/xdotool"), arguments);
    if (!process.waitForFinished(processTimeoutMs))
    {
        process.kill();
        process.waitForFinished(processTimeoutMs);
        return {};
    }
    return process.exitCode() == 0 ? process.readAllStandardOutput().trimmed() : QByteArray();
}
}

class TestHelperGuest : public QObject
{
    Q_OBJECT
    QProcess agent_;
    QProcess helper_;

    QByteArray authenticationWindow()
    {
        return xdo({QStringLiteral("search"), QStringLiteral("--onlyvisible"),
            QStringLiteral("--name"), QStringLiteral("^Authentication Required$")}).split('\n').value(0);
    }

    int authorizationCount()
    {
        return readFile(QStringLiteral("/home/rufus/authorization.log"))
            .count("string \"org.rufus.TestCreate\"");
    }

    void launch()
    {
        QCOMPARE(helper_.state(), QProcess::NotRunning);
        helper_.start(QStringLiteral("/usr/bin/pkexec"),
            {QStringLiteral("--disable-internal-agent"), QStringLiteral("/opt/rufus/test_gpt_guest")});
        QVERIFY(helper_.waitForStarted(processTimeoutMs));
    }

    void checkExit(int expected)
    {
        QTRY_COMPARE_WITH_TIMEOUT(helper_.state(), QProcess::NotRunning, jobTimeoutMs);
        QCOMPARE(helper_.exitStatus(), QProcess::NormalExit);
        const QByteArray output = helper_.readAllStandardOutput();
        const QByteArray errors = helper_.readAllStandardError();
        if (!output.isEmpty())
            fwrite(output.constData(), 1, size_t(output.size()), stdout);
        if (!errors.isEmpty())
            fwrite(errors.constData(), 1, size_t(errors.size()), stderr);
        // A desktop agent can report Escape as denial rather than setting the
        // polkit "dismissed" flag. Both outcomes must stop before helper entry.
        const int actual = helper_.exitCode();
        const bool cancelledAsDenial = expected == pkexecDismissed && actual == pkexecFailed;
        if (!cancelledAsDenial)
            QCOMPARE(actual, expected);
        QCOMPARE(getuid(), uid_t(callerUid));
        QCOMPARE(geteuid(), uid_t(callerUid));
        if (expected == 0)
        {
            QVERIFY(output.contains("RUFUS_HELPER_UID=0 EUID=0 CALLER=1000"));
            QVERIFY(output.contains("RUFUS_GPT_FORMATTER_HANDOFF"));
            QVERIFY(output.contains("RUFUS_GPT_FORMATTER_MOUNTED_REFUSED"));
            QVERIFY(output.contains("RUFUS_GPT_REACQUIRED"));
            QVERIFY(output.contains("RUFUS_GPT_DONE"));
        }
        else
        {
            QVERIFY(!output.contains("RUFUS_HELPER_UID"));
            QVERIFY(!output.contains("RUFUS_GPT_CASE"));
        }
    }

private slots:
    void initTestCase()
    {
        QVERIFY(fixtureGuest());
        const int fd = open("/dev/sda", O_RDWR | O_CLOEXEC);
        const int error = errno;
        if (fd >= 0)
            close(fd);
        QVERIFY(fd < 0 && error == EACCES);
        agent_.setProcessChannelMode(QProcess::ForwardedChannels);
        agent_.start(QStringLiteral("/usr/lib/polkit-kde-authentication-agent-1"), {});
        QVERIFY(agent_.waitForStarted(processTimeoutMs));
        // Let the native agent finish registering on the system bus.
        QTest::qWait(1000);
        QCOMPARE(authorizationCount(), 0);
    }

    void dismissed()
    {
        const int before = authorizationCount();
        launch();
        QTRY_VERIFY_WITH_TIMEOUT(!authenticationWindow().isEmpty(), authenticationTimeoutMs);
        const QString window = QString::fromLatin1(authenticationWindow());
        xdo({QStringLiteral("windowfocus"), window});
        xdo({QStringLiteral("key"), QStringLiteral("Escape")});
        checkExit(pkexecDismissed);
        QTRY_COMPARE_WITH_TIMEOUT(authorizationCount(), before + 1, processTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(authenticationWindow().isEmpty(), processTimeoutMs);
        qInfo("RUFUS_HELPER_DISMISSED REQUESTS=1");
    }

    void oneAuthorizationForAllPhases()
    {
        const int before = authorizationCount();
        launch();
        QTRY_VERIFY_WITH_TIMEOUT(!authenticationWindow().isEmpty(), authenticationTimeoutMs);
        const QString window = QString::fromLatin1(authenticationWindow());
        xdo({QStringLiteral("windowraise"), window});
        xdo({QStringLiteral("windowfocus"), window});
        QVERIFY(QApplication::primaryScreen()->grabWindow(0)
            .save(QStringLiteral("/home/rufus/helper-authentication.png")));
        // Public fixture password, entered only into the separate native agent.
        xdo({QStringLiteral("type"), QStringLiteral("--clearmodifiers"), QStringLiteral("rufus-fixture")});
        xdo({QStringLiteral("key"), QStringLiteral("Return")});
        checkExit(0);
        QTRY_COMPARE_WITH_TIMEOUT(authorizationCount(), before + 1, processTimeoutMs);
        QVERIFY(authenticationWindow().isEmpty());
        qInfo("RUFUS_HELPER_APPROVED REQUESTS=1");
    }

    void noAgentDoesNotRetry()
    {
        agent_.terminate();
        QVERIFY(agent_.waitForFinished(processTimeoutMs));
        const int before = authorizationCount();
        launch();
        checkExit(pkexecFailed);
        QTRY_COMPARE_WITH_TIMEOUT(authorizationCount(), before + 1, processTimeoutMs);
        qInfo("RUFUS_HELPER_NO_AGENT REQUESTS=1");
    }

    void cleanupTestCase()
    {
        if (helper_.state() != QProcess::NotRunning)
        {
            // The VM supervisor stops the isolated guest if a root job stalls.
            helper_.closeWriteChannel();
            helper_.waitForFinished(processTimeoutMs);
        }
        if (agent_.state() != QProcess::NotRunning)
        {
            agent_.terminate();
            agent_.waitForFinished(processTimeoutMs);
        }
    }
};

int main(int argc, char** argv)
{
    if (!fixtureGuest())
        return 77;
    QApplication application(argc, argv);
    TestHelperGuest test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_helper_guest.moc"
