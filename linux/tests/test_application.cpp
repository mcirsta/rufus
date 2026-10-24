// SPDX-License-Identifier: GPL-3.0-or-later
#include "qt/application_instance.h"
#include "qt/application_log.h"
#include "qt/application_storage.h"
#include "qt/device_size.h"
#include "qt/startup_options.h"
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>
#include <QTextStream>
#include <QThread>

class TestApplication : public QObject
{
    Q_OBJECT
    QTemporaryDir directory_;
private slots:
    void initTestCase()
    {
        QVERIFY(directory_.isValid());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory_.filePath(QStringLiteral("settings")));
        qputenv("XDG_DATA_HOME", directory_.filePath(QStringLiteral("data")).toUtf8());
    }
    void cleanup()
    {
        QString problem;
        QCOMPARE(rufus::ConfigureApplicationStorage(directory_.filePath(QStringLiteral("rufus-linux")), false, problem), 0);
    }
    void startup()
    {
        rufus::StartupOptions options;
        QString problem;
        QVERIFY(rufus::ParseStartupOptions({"rufus", "-x", "--gui", "--portable", "-f", "nTfS", "-w", "15", "--iso", "image.iso"}, options, problem));
        QCOMPARE(options.fileSystem, int(FS_NTFS)); QCOMPARE(options.waitTicks, 15);
        QCOMPARE(options.image, QStringLiteral("image.iso")); QVERIFY(options.extraDevices && options.portable);
        QVERIFY(rufus::ParseStartupOptions({"rufus", "/W", "-f", "0x2", "image.img"}, options, problem));
        QCOMPARE(options.fileSystem, int(FS_NTFS)); QCOMPARE(options.waitTicks, 150);
        QVERIFY(rufus::ParseStartupOptions({"rufus", "--iso", "/W"}, options, problem));
        QCOMPARE(options.image, QStringLiteral("/W")); QCOMPARE(options.waitTicks, 0);
        QVERIFY(rufus::ParseStartupOptions({"rufus", "--", "/W"}, options, problem));
        QCOMPARE(options.image, QStringLiteral("/W")); QCOMPARE(options.waitTicks, 0);
        QVERIFY(rufus::ParseStartupOptions({"rufus", "--filesystem=unsupported"}, options, problem));
        QCOMPARE(options.fileSystem, int(FS_UNKNOWN));
        QVERIFY(!rufus::ParseStartupOptions({"rufus", "--wait=-1"}, options, problem));
        QVERIFY(!rufus::ParseStartupOptions({"rufus", "--wait=2147483647"}, options, problem));
        QVERIFY(!rufus::ParseStartupOptions({"rufus", "--iso", "a", "b"}, options, problem));
        QVERIFY(!rufus::ParseStartupOptions({"rufus", "--unknown"}, options, problem));
        QVERIFY(rufus::ParseStartupOptions({"rufus", "--help"}, options, problem));
        QCOMPARE(options.action, rufus::StartupAction::Help);
        QVERIFY(rufus::ParseStartupOptions({"rufus", "--help-all"}, options, problem));
        QCOMPARE(options.action, rufus::StartupAction::Help);
        QVERIFY(rufus::ParseStartupOptions({"rufus", "--version"}, options, problem));
        QCOMPARE(options.action, rufus::StartupAction::Version);
        QVERIFY(rufus::StartupRequestsText({"rufus", "--help"}));
        QVERIFY(!rufus::StartupRequestsText({"rufus", "--iso", "--help"}));
        QVERIFY(!rufus::StartupRequestsText({"rufus", "-platform", "--help"}));
        QVERIFY(!rufus::StartupRequestsText({"rufus", "--", "--version"}));
        QVERIFY(rufus::StartupRequestsText({"rufus", "--iso=a", "--version"}));
    }
    void portableNames_data()
    {
        QTest::addColumn<QString>("name"); QTest::addColumn<bool>("portable");
        QTest::newRow("ordinary") << QStringLiteral("rufus-linux") << false;
        QTest::newRow("lowercase") << QStringLiteral("rufus-4.15p") << true;
        QTest::newRow("capital") << QStringLiteral("rufus-Portable") << true;
        QTest::newRow("alpha") << QStringLiteral("rufus-ALPHA") << false;
        QTest::newRow("alpha-and-lowercase") << QStringLiteral("rufus-ALPHAp") << true;
    }
    void portableNames()
    {
        QFETCH(QString, name); QFETCH(bool, portable);
        QTemporaryDir location(directory_.filePath(QStringLiteral("portable-parent-XXXXXX")));
        QVERIFY(location.isValid()); QString problem;
        QCOMPARE(rufus::ConfigureApplicationStorage(location.filePath(name), false, problem), 0);
        QCOMPARE(rufus::IsPortableApplication(), portable);
        QCOMPARE(QFile::exists(location.filePath(QStringLiteral("rufus.ini"))), portable);
    }
    void windowsIniAndUserSettings()
    {
        QString problem;
        QCOMPARE(rufus::ConfigureApplicationStorage(directory_.filePath(QStringLiteral("rufus-linux")), false, problem), 0);
        {
            rufus::ApplicationSettings normal;
            normal.setValue(QStringLiteral("ExpertMode"), false);
            normal.setValue(QStringLiteral("DisableRufusMbr"), true);
        }
        QTemporaryDir portable;
        QVERIFY(portable.isValid());
        QFile ini(portable.filePath(QStringLiteral("rufus.ini")));
        QVERIFY(ini.open(QIODevice::WriteOnly));
        const QByteArray windows = "ExpertMode=1\nPersistentLog=0\ndisablerufusmbr=0\nWindowsUserExperienceOptions=0x001f0002\nUseExtVersion=0x4\nUnknownWindowsOption=7\n";
        QCOMPARE(ini.write(windows), windows.size()); ini.close();
        QCOMPARE(rufus::ConfigureApplicationStorage(portable.filePath(QStringLiteral("rufus-linux")), false, problem), 0);
        QVERIFY(rufus::IsPortableApplication());
        QCOMPARE(rufus::ApplicationFilesDirectory(), portable.filePath(QStringLiteral("rufus_files")));
        QCOMPARE(rufus::ApplicationReportDirectory(), rufus::ApplicationFilesDirectory());
        {
            rufus::ApplicationSettings settings;
            QVERIFY(settings.value(QStringLiteral("ExpertMode"), false).toBool());
            QVERIFY(!settings.value(QStringLiteral("PersistentLog"), false).toBool());
            QVERIFY(!settings.value(QStringLiteral("DisableRufusMbr"), true).toBool());
            QCOMPARE(settings.value(QStringLiteral("WindowsUserExperience"), quint32(0)).toUInt(), quint32(0x001f0002));
            QCOMPARE(settings.value(QStringLiteral("UseExtVersion"), 3).toInt(), 4);
            settings.setValue(QStringLiteral("DisableRufusMbr"), true);
            settings.setValue(QStringLiteral("PersistentLog"), true);
        }
        QVERIFY(ini.open(QIODevice::ReadOnly)); const QByteArray written = ini.readAll(); ini.close();
        QVERIFY(written.contains("DisableRufusMBR=1")); QVERIFY(written.contains("PersistentLog=1"));
        QVERIFY(written.contains("UnknownWindowsOption=7"));
        QCOMPARE(rufus::ResetUserApplicationSettings(), 0);
        QVERIFY(rufus::ApplicationSettings().value(QStringLiteral("ExpertMode"), false).toBool());
        QCOMPARE(rufus::ConfigureApplicationStorage(directory_.filePath(QStringLiteral("rufus-linux")), false, problem), 0);
        QVERIFY(!rufus::ApplicationSettings().contains(QStringLiteral("ExpertMode")));
        QVERIFY(!rufus::ApplicationSettings().contains(QStringLiteral("DisableRufusMbr")));
        // Old Linux key spellings/types are retained in the user profile.
        { rufus::ApplicationSettings normal; normal.setValue(QStringLiteral("DisableRufusMbr"), true); }
        QSettings original(QSettings::IniFormat, QSettings::UserScope, "Rufus", "Linux");
        QVERIFY(original.value(QStringLiteral("DisableRufusMbr"), false).toBool());
    }
    void portableFailure()
    {
        QTemporaryDir location; QVERIFY(location.isValid());
        QVERIFY(QDir().mkdir(location.filePath(QStringLiteral("rufus.ini"))));
        QString problem;
        QVERIFY(rufus::ConfigureApplicationStorage(location.filePath(QStringLiteral("rufus-linux")), true, problem) != 0);
        QVERIFY(!problem.isEmpty()); QVERIFY(!rufus::IsPortableApplication());
    }
    void logsKeepCompleteSessions()
    {
        QTemporaryDir location; QVERIFY(location.isValid());
        const QString path = location.filePath(QStringLiteral("rufus.log"));
        QString problem;
        rufus::ApplicationLog first;
        QVERIFY(first.append(QStringLiteral("first")));
        for (int index = 0; index < 2300; ++index) QVERIFY(first.append(QStringLiteral("line %1").arg(index)));
        QVERIFY2(first.save(path, false, problem), qPrintable(problem));
        QFile saved(path); QVERIFY(saved.open(QIODevice::ReadOnly));
        const QByteArray initial = saved.readAll(); saved.close();
        QVERIFY(initial.startsWith("first\nline 0\n")); QVERIFY(initial.endsWith("line 2299\n"));
        rufus::ApplicationLog second; QVERIFY(second.append(QStringLiteral("second")));
        QVERIFY2(second.save(path, true, problem), qPrintable(problem));
        QVERIFY(saved.open(QIODevice::ReadOnly)); QCOMPARE(saved.readAll(), initial + "\nsecond\n"); saved.close();
        QVERIFY(!second.save(location.path(), true, problem)); QVERIFY(!problem.isEmpty());
        QVERIFY(saved.open(QIODevice::ReadOnly)); QCOMPARE(saved.readAll(), initial + "\nsecond\n"); saved.close();
        QVERIFY(second.clear()); QVERIFY(second.append(QStringLiteral("replacement")));
        QVERIFY2(second.save(path, false, problem), qPrintable(problem));
        QVERIFY(saved.open(QIODevice::ReadOnly)); QCOMPARE(saved.readAll(), QByteArray("replacement\n"));
    }
    void instanceWaitAndRelease()
    {
        QTemporaryDir runtime; QVERIFY(runtime.isValid());
        QProcess owner;
        owner.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--hold-instance"), runtime.path()});
        QVERIFY(owner.waitForStarted()); QVERIFY(owner.waitForReadyRead());
        QCOMPARE(owner.readLine(), QByteArray("locked\n"));
        QString problem;
        rufus::ApplicationInstance second(runtime.path());
        QVERIFY(!second.acquire(0, problem)); QVERIFY(problem.contains(QStringLiteral("Another Rufus")));
        QElapsedTimer wait; wait.start();
        QVERIFY2(second.acquire(20, problem), qPrintable(problem));
        QVERIFY(wait.elapsed() >= 100);
        QVERIFY(owner.waitForFinished()); QCOMPARE(owner.exitCode(), 0);
        QTemporaryDir alias;
        QVERIFY(alias.isValid());
        const QString link = alias.filePath(QStringLiteral("runtime"));
        QVERIFY(QFile::link(runtime.path(), link));
        rufus::ApplicationInstance symlink(link);
        QVERIFY(!symlink.acquire(0, problem));
    }
    void deviceSizeUnits()
    {
        QCOMPARE(rufus::DeviceSizeLabel(32000000000ULL), QStringLiteral("32 GB"));
        QCOMPARE(rufus::DeviceSizeLabel(32000000000ULL, true), QStringLiteral("29.8 GB"));
        QCOMPARE(rufus::DeviceSizeLabel(1, true), QStringLiteral("1 bytes"));
    }
};

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    const QStringList arguments = application.arguments();
    if (arguments.size() == 3 && arguments[1] == QLatin1String("--hold-instance")) {
        rufus::ApplicationInstance instance(arguments[2]); QString problem;
        if (!instance.acquire(0, problem)) return 1;
        QTextStream out(stdout); out << "locked\n"; out.flush();
        QThread::msleep(350); return 0;
    }
    TestApplication test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_application.moc"
