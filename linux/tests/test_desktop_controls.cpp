// SPDX-License-Identifier: GPL-3.0-or-later
#include "qt/main_window.h"
#include "qt/selection_controller.h"
#include "qt/application_storage.h"
#include "qt/application_language.h"
#include "qt/desktop_controls.h"
#include "qt/job_priority.h"
#include "qt/device_policy.h"
#include "qt/prepare_image_process.h"
#include "http_fixture.h"
#include <QDir>
#include <QNetworkProxy>
#include <QFile>
#include <QLineEdit>
#include <QPalette>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QProgressBar>
#include <QTemporaryFile>
#include <QSemaphore>
#include <QSignalSpy>
#include <QShortcut>
#include <QJsonArray>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <linux/ioprio.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace
{
bool putFile(const QString& path, const QByteArray& data)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}

class PriorityProbe final : public rufus::RufusJob
{
    Q_OBJECT
public:
    QSemaphore next;
signals:
    void sampled(int level, int io);
private:
    void execute() override
    {
        for (int count = 0; count < 5; ++count) {
            next.acquire();
            emit sampled(rufus::JobPriority(),int(syscall(SYS_ioprio_get,IOPRIO_WHO_PROCESS,0)));
        }
    }
};

struct JoinedProbe
{
    QThread thread;
    PriorityProbe* job = nullptr;
    ~JoinedProbe()
    {
        if (thread.isRunning()) { job->next.release(5); thread.quit(); thread.wait(); }
    }
};
}

class TestDesktopControls : public QObject
{
    Q_OBJECT
    QTemporaryDir profile_;
    QPalette originalPalette_;
    rufus::PrepareImageProcess* runningHelper_ = nullptr;
    void updateHelperPriority()
    {
        if (runningHelper_ && runningHelper_->state() == rufus::PrepareImageProcess::State::Running) {
            runningHelper_->setJobPriority(0); runningHelper_->setJobPriority(2); runningHelper_->setJobPriority(-2);
            runningHelper_ = nullptr;
        }
    }
private slots:
    void devicePolicySettingsAndProtocol()
    {
        rufus::ApplicationSettings settings;
        settings.setValue("IgnoreUsb01", "0x095116ab");
        settings.setValue("IgnoreUsb08", "4294967295");
        settings.setValue("IgnoreUsb02", "4294967296");
        settings.setValue("IgnoreDisk01", "{01234567-89AB-CDEF-0123-456789ABCDEF}");
        settings.setValue("IgnoreDisk02", "01234567-89ab-cdef-0123-456789abcdef"); settings.sync();
        const auto exclusions = rufus::ConfiguredDeviceExclusions();
        QCOMPARE(exclusions.usb, QList<quint32>({0x095116ab, UINT32_MAX}));
        QCOMPARE(exclusions.disks, QStringList({"{01234567-89ab-cdef-0123-456789abcdef}"}));
        for (const char* key : {"IgnoreUsb01", "IgnoreUsb08", "IgnoreUsb02", "IgnoreDisk01", "IgnoreDisk02"}) settings.remove(key);
        settings.sync();
        QVERIFY(rufus::ConfiguredDeviceExclusions().empty());
        rufus::SetDeviceLocking(false); QVERIFY(!rufus::DeviceLocking());
        rufus::PrepareImageRequest request; rufus::FreezeDevicePolicy(request); QVERIFY(!request.lockDevice);
        rufus::SetDeviceLocking(true);
        request.isDevice = request.nonBootable = true; request.deviceOperation = 2;
        request.path = "/dev/sdz"; request.device = 2048; request.size = 64 * 1024 * 1024;
        request.diskSequence = 42; request.objectPath = "/org/freedesktop/UDisks2/block_devices/sdz";
        request.serviceOwner = ":1.2"; request.identity = {"fixture", "42"};
        request.exclusions = exclusions; request.espRecords = exclusions.disks;
        const QJsonObject packet = rufus::EncodePrepareRequest(request); QVERIFY(!packet.isEmpty());
        rufus::PrepareImageRequest decoded; QVERIFY(rufus::DecodePrepareRequest(packet, decoded));
        QCOMPARE(decoded.deviceOperation, 2); QCOMPARE(decoded.espRecords, request.espRecords);
        QCOMPARE(decoded.exclusions.usb, exclusions.usb); QVERIFY(!decoded.lockDevice);
        QJsonObject invalid = packet; invalid.insert("deviceOperation", QJsonObject{{"command", 2.5}, {"esp", QJsonArray()}});
        QVERIFY(!rufus::DecodePrepareRequest(invalid, decoded));
        invalid = packet; invalid.insert("devicePolicy", QJsonObject{{"lock", false}, {"usb", QJsonArray({"4294967296"})}, {"disks", QJsonArray()}});
        QVERIFY(!rufus::DecodePrepareRequest(invalid, decoded));
        request.rawWrite = true; QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        rufus::SelectionController controller;
        rufus::MainWindow window(controller); window.show();
        auto* shortcut = window.findChild<QShortcut*>(QStringLiteral("deviceLockingShortcut"));
        QVERIFY(shortcut); QVERIFY(QMetaObject::invokeMethod(shortcut, "activated", Qt::DirectConnection));
        QVERIFY(!rufus::DeviceLocking());
        QVERIFY(QMetaObject::invokeMethod(shortcut, "activated", Qt::DirectConnection)); QVERIFY(rufus::DeviceLocking());
        QVERIFY(window.findChild<QShortcut*>(QStringLiteral("opticalCaptureShortcut")));
        QVERIFY(window.findChild<QShortcut*>(QStringLiteral("resetUsbPortShortcut")));
        QVERIFY(window.findChild<QShortcut*>(QStringLiteral("toggleEspShortcut")));
    }
    void initTestCase()
    {
        QVERIFY(profile_.isValid()); originalPalette_ = qApp->palette();
        QString problem;
        QCOMPARE(rufus::ConfigureApplicationStorage(profile_.filePath("rufus-linux"),true,problem),0);
        QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
        QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);
    }
    void init()
    {
        rufus::ApplicationSettings settings; settings.clear(); settings.sync();
        rufus::SetJobPriority(rufus::DefaultJobPriority);
        QDir(rufus::ApplicationFilesDirectory()).removeRecursively();
    }
    void cleanup() { qApp->setPalette(originalPalette_); }
    void priorityChangesActualWorkerOnly()
    {
        const int originalIo = int(syscall(SYS_ioprio_get,IOPRIO_WHO_PROCESS,0));
        const int originalNice = getpriority(PRIO_PROCESS,0);
        JoinedProbe probe; auto& thread=probe.thread; auto* job = new PriorityProbe; probe.job=job;
        job->moveToThread(&thread);
        QSignalSpy sampled(job,&PriorityProbe::sampled);
        connect(&thread,&QThread::started,job,&rufus::RufusJob::run);
        connect(job,&rufus::RufusJob::finished,&thread,&QThread::quit,Qt::DirectConnection);
        connect(&thread,&QThread::finished,job,&QObject::deleteLater);
        thread.start();
        constexpr int expected[] = {7,6,4,2,0};
        for (int index = 0; index < 5; ++index) {
            const int level = index-2;
            QCOMPARE(rufus::SetJobPriority(level),0);
            job->next.release();
            QTRY_COMPARE_WITH_TIMEOUT(sampled.size(),index+1,1000);
            QCOMPARE(sampled[index][0].toInt(),level);
            QCOMPARE(IOPRIO_PRIO_CLASS(sampled[index][1].toInt()),IOPRIO_CLASS_BE);
            QCOMPARE(IOPRIO_PRIO_DATA(sampled[index][1].toInt()),expected[index]);
            QCOMPARE(int(syscall(SYS_ioprio_get,IOPRIO_WHO_PROCESS,0)),originalIo);
            QCOMPARE(getpriority(PRIO_PROCESS,0),originalNice);
        }
        QVERIFY(thread.wait(1000));
        QCOMPARE(rufus::SetJobPriority(3),EINVAL);
    }
    void cacheClearingPreservesUserFilesAndSelectedPayload()
    {
        const QDir cache(rufus::ApplicationFilesDirectory());
        const QString core = cache.filePath("grub-2.12/core.img");
        QVERIFY(putFile(core,"selected source"));
        QVERIFY(putFile(core+".sig","signature"));
        QVERIFY(putFile(cache.filePath("diskcopy.dll"),"resource"));
        QVERIFY(putFile(cache.filePath("rufus.log"),"session"));
        QVERIFY(putFile(cache.filePath("rufus-2026.badblocks.log"),"report"));
        QVERIFY(putFile(cache.filePath("personal.iso"),"user image"));
        QVERIFY(putFile(cache.filePath("grub-2.12/personal.txt"),"user file"));
        const QString link = profile_.filePath("selected-hardlink.img");
        QVERIFY(::link(QFile::encodeName(core).constData(),QFile::encodeName(link).constData())==0);
        QString problem;
        QCOMPARE(rufus::ClearApplicationResources({link},problem),0);
        QVERIFY(QFile::exists(core)); QVERIFY(!QFile::exists(core+".sig"));
        QVERIFY(!QFile::exists(cache.filePath("diskcopy.dll")));
        for (const char* name : {"rufus.log","rufus-2026.badblocks.log","personal.iso","grub-2.12/personal.txt"})
            QVERIFY(QFile::exists(cache.filePath(QString::fromLatin1(name))));
    }
    void cacheDoesNotFollowLinks()
    {
        const QString outside = profile_.filePath("outside/core.img");
        QVERIFY(putFile(outside,"untouched"));
        const QString cache = rufus::ApplicationFilesDirectory();
        QVERIFY(QDir().mkpath(cache));
        QVERIFY(QFile::link(QFileInfo(outside).absolutePath(),QDir(cache).filePath("grub-2.12")));
        QString problem;
        QCOMPARE(rufus::ClearApplicationResources({},problem),0);
        QVERIFY(QFile::exists(outside));
        QVERIFY(QDir(cache).removeRecursively());
        QVERIFY(QFile::link(QFileInfo(outside).absolutePath(),cache));
        QVERIFY(rufus::ClearApplicationResources({},problem)!=0);
        QVERIFY(QFile::exists(outside));
        QVERIFY(QFile::remove(cache));
    }
    void desktopShortcutsAndVisibleLogSelection()
    {
        rufus::SelectionController controller; rufus::MainWindow window(controller);
        window.show(); QVERIFY(QTest::qWaitForWindowExposed(&window)); window.activateWindow();
        auto* select = window.findChild<QPushButton*>("selectImageButton"); QVERIFY(select); select->setFocus();
        QTRY_VERIFY(window.isActiveWindow());
        QTest::keyClick(select,Qt::Key_Minus,Qt::AltModifier);
        QCOMPARE(rufus::JobPriority(),0);
        QCOMPARE(rufus::ApplicationSettings().value("DefaultThreadPriority").toInt(),-1);
        QTest::keyClick(select,Qt::Key_Equal,Qt::AltModifier); QCOMPARE(rufus::JobPriority(),1);
        QTest::keyClick(select,Qt::Key_Plus,Qt::AltModifier); QCOMPARE(rufus::JobPriority(),2);
        QTest::keyClick(select,Qt::Key_Plus,Qt::AltModifier); QCOMPARE(rufus::JobPriority(),2);
        QTest::keyClick(select,Qt::Key_Minus,Qt::AltModifier|Qt::KeypadModifier); QCOMPARE(rufus::JobPriority(),1);
        QTest::keyClick(select,Qt::Key_Plus,Qt::AltModifier|Qt::KeypadModifier); QCOMPARE(rufus::JobPriority(),2);
        QTest::keyClick(select,Qt::Key_Period,Qt::AltModifier);
        QVERIFY(controller.usbDebug()); QVERIFY(rufus::ApplicationSettings().value("EnableUsbDebug",false).toBool());
        QTest::keyClick(select,Qt::Key_D,Qt::ControlModifier|Qt::AltModifier);
        QTRY_COMPARE(window.palette().color(QPalette::Window),qApp->palette().color(QPalette::Window));
        QVERIFY(rufus::ApplicationIsDark());
        if (!qEnvironmentVariableIsEmpty("RUFUS_DESKTOP_SCREENSHOTS"))
            QVERIFY(window.grab().save(qEnvironmentVariable("RUFUS_DESKTOP_SCREENSHOTS")+"/desktop-dark.png"));
        QCOMPARE(rufus::ApplicationSettings().value("DarkMode").toInt(),1);
        QTest::keyClick(select,Qt::Key_D,Qt::ControlModifier|Qt::AltModifier);
        QTRY_COMPARE(window.palette().color(QPalette::Window),qApp->palette().color(QPalette::Window));
        QVERIFY(!rufus::ApplicationIsDark());
        if (!qEnvironmentVariableIsEmpty("RUFUS_DESKTOP_SCREENSHOTS"))
            QVERIFY(window.grab().save(qEnvironmentVariable("RUFUS_DESKTOP_SCREENSHOTS")+"/desktop-light.png"));
        QCOMPARE(rufus::ApplicationSettings().value("DarkMode").toInt(),2);
        controller.logMessage("line one\nline two");
        QTest::keyClick(select,Qt::Key_L,Qt::ControlModifier);
        auto* log = window.findChild<QPlainTextEdit*>("logText"); QVERIFY(log && log->isVisible());
        window.activateWindow(); select->setFocus(); QTRY_VERIFY(window.isActiveWindow());
        QTest::keyClick(select,Qt::Key_A,Qt::ControlModifier);
        QCOMPARE(log->textCursor().selectedText().replace(QChar::ParagraphSeparator,QLatin1Char('\n')),log->toPlainText());
        log->window()->activateWindow(); log->setFocus(); QTRY_VERIFY(log->window()->isActiveWindow());
        QTest::keyClick(log,Qt::Key_Minus,Qt::AltModifier); QCOMPARE(rufus::JobPriority(),1);
        QTest::keyClick(log,Qt::Key_Period,Qt::AltModifier); QVERIFY(!controller.usbDebug());
    }
    void resourceClearAndAppearanceWaitForUpdates()
    {
        rufus_test::HttpFixture server; QVERIFY(server.listen(QHostAddress::LocalHost,0));
        server.holdHeaders.insert("/repos/mcirsta/rufus/releases?per_page=100&page=1");
        rufus::ApplicationUpdateSources sources; sources.api=server.url("/");
        rufus::SelectionController controller; rufus::MainWindow window(controller,nullptr,sources);
        window.show(); QVERIFY(QTest::qWaitForWindowExposed(&window)); window.activateWindow();
        auto* select=window.findChild<QPushButton*>("selectImageButton"); QVERIFY(select); select->setFocus();
        QTRY_VERIFY(window.isActiveWindow());
        const QString cached=QDir(rufus::ApplicationFilesDirectory()).filePath("diskcopy.dll"); QVERIFY(putFile(cached,"resource"));
        auto* updates=window.findChild<rufus::ApplicationUpdates*>(); QVERIFY(updates);
        QCOMPARE(updates->start("4.15.2404","x86_64",false),0);
        const QPalette before=qApp->palette();
        QTest::keyClick(select,Qt::Key_D,Qt::AltModifier); QVERIFY(QFile::exists(cached));
        QTest::keyClick(select,Qt::Key_D,Qt::ControlModifier|Qt::AltModifier); QCOMPARE(qApp->palette(),before);
        updates->cancel(); QTRY_VERIFY(!updates->isActive());
        QTest::keyClick(select,Qt::Key_D,Qt::AltModifier); QVERIFY(!QFile::exists(cached));
    }
    void progressModesAndPhaseReset()
    {
        rufus::ProgressDisplay display;
        const QString percentage=QStringLiteral("Writing: %p%");
        display.observe("copy",0,1000000);
        display.cycle(); QCOMPARE(display.format(percentage),percentage);
        QTest::qWait(220);
        display.observe("copy",500000,1000000);
        QVERIFY(display.format(percentage).contains("/s"));
        display.cycle(); QVERIFY(display.format(percentage).contains("remaining"));
        display.cycle(); QCOMPARE(display.format(percentage),percentage);
        display.cycle(); display.observe("verify",0,1000000);
        QCOMPARE(display.format(percentage),percentage);
        display.clear(); QCOMPARE(display.format("READY"),QStringLiteral("READY"));
    }
    void bareAltCyclesActualHashProgress()
    {
        QTemporaryFile image; QVERIFY(image.open());
        QVERIFY(image.resize(512LL*1024*1024)); QVERIFY(image.seek(510)); QCOMPARE(image.write("\x55\xaa",2),2LL); QVERIFY(image.flush());
        rufus::SelectionController controller; rufus::MainWindow window(controller);
        window.show(); QVERIFY(QTest::qWaitForWindowExposed(&window)); window.activateWindow();
        controller.selectImage(image.fileName());
        QTRY_VERIFY_WITH_TIMEOUT(!controller.isAnalyzing(),3000); QVERIFY(controller.canHashImage());
        controller.startHashes(); QVERIFY(controller.hasher().isActive());
        auto* progress=window.findChild<QProgressBar*>("progressBar"); QVERIFY(progress);
        QTest::keyClick(&window,Qt::Key_Alt);
        QTRY_VERIFY_WITH_TIMEOUT(progress->format().contains("/s"),3000);
        QTest::keyClick(&window,Qt::Key_Alt);
        QTRY_VERIFY_WITH_TIMEOUT(progress->format().contains("remaining"),1000);
        QTest::keyClick(&window,Qt::Key_H,Qt::AltModifier);
        QVERIFY(progress->format().contains("remaining")); // An Alt shortcut does not count as bare Alt.
        QTest::keyClick(&window,Qt::Key_Alt);
        QVERIFY(progress->format().contains("%p%"));
        controller.hasher().cancel();
        QTRY_VERIFY_WITH_TIMEOUT(!controller.hasher().isActive(),3000);
    }
    void priorityProtocolAndRealFileJob()
    {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        rufus::PrepareImageRequest request;
        request.path=directory.filePath("target.img"); request.size=64*1024*1024;
        QFile file(request.path); QVERIFY(file.open(QIODevice::ReadWrite)); QVERIFY(file.resize(qint64(request.size)));
        struct stat info={}; QCOMPARE(fstat(file.handle(),&info),0);
        request.device=quint64(info.st_dev); request.inode=quint64(info.st_ino); file.close();
        for (int level=-2;level<=2;++level) {
            request.jobPriority=level; rufus::PrepareImageRequest decoded;
            QVERIFY(rufus::DecodePrepareRequest(rufus::EncodePrepareRequest(request),decoded));
            QCOMPARE(decoded.jobPriority,level);
        }
        auto encoded=rufus::EncodePrepareRequest(request);
        for (auto invalid : {QJsonValue(true),QJsonValue("1"),QJsonValue(1.5),QJsonValue(3)}) {
            encoded["jobPriority"]=invalid; rufus::PrepareImageRequest decoded;
            QVERIFY(!rufus::DecodePrepareRequest(encoded,decoded));
        }
        rufus::SetJobPriority(-2);
        rufus::PrepareImageProcess process(QString::fromLocal8Bit(RUFUS_PREPARE_HELPER));
        QSignalSpy finished(&process,&rufus::PrepareImageProcess::finished);
        runningHelper_ = &process;
        connect(&process,&rufus::PrepareImageProcess::changed,this,&TestDesktopControls::updateHelperPriority);
        QVERIFY(process.start(request));
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(),1,10000);
        QVERIFY(runningHelper_ == nullptr);
        QCOMPARE(process.state(),rufus::PrepareImageProcess::State::Complete);
        QVERIFY(file.open(QIODevice::ReadOnly)); QVERIFY(file.seek(qint64(process.outcome().offset)+82));
        QCOMPARE(file.read(8),QByteArray("FAT32   "));
    }
    void catalogFilterBindings()
    {
        const auto& catalog=rufus::ApplicationLanguages();
        for (int locale=0;locale<catalog.locales().size();++locale) {
            const QString filter=catalog.translate(locale,QStringLiteral("ISO images (*.iso);;All files (*)"));
            QVERIFY2(filter.contains("*.iso") && filter.contains(";;") && filter.endsWith("(*)"),qPrintable(catalog.locales()[locale].tag));
            QCOMPARE(catalog.translate(locale,"USB debug"),catalog.text(locale,"MSG_270"));
            struct Alias { const char* source; const char* key; };
            const Alias aliases[] = {{"ISO images","MSG_036"},{"All files","MSG_107"},{"Disk images","MSG_095"},
                {"Disk and ISO images","MSG_280"},{"VHDX image","MSG_342"},{"Uncompressed VHD image","MSG_343"},
                {"Full Flash Update image","MSG_344"},{"Log files","IDD_LOG/IDD_LOG"}};
            for (const auto& alias : aliases) {
                const QString expected=catalog.text(locale,QString::fromLatin1(alias.key));
                QVERIFY(!expected.isEmpty());
                QCOMPARE(catalog.translate(locale,QString::fromLatin1(alias.source)),expected);
            }
        }
    }
};
QTEST_MAIN(TestDesktopControls)
#include "test_desktop_controls.moc"
