// SPDX-License-Identifier: GPL-3.0-or-later
#include "qt/main_window.h"
#include "qt/selection_controller.h"
#include "qt/application_storage.h"
#include "qt/policy_update_dialog.h"
#include "http_fixture.h"
#include "secure_boot_fixture.h"
#include <QNetworkProxy>
#include <QDateTime>

#include <QComboBox>
#include <QCheckBox>
#include <QDialog>
#include <QDate>
#include <QDir>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalSpy>
#include <QSettings>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTest>
#include <QToolButton>

#include <memory>

static constexpr int uiTimeoutMs = 1000;

namespace
{

QList<rufus::Device> testDevices()
{
    // Descriptive paths only: these records must never open host devices.
    QList<rufus::Device> devices = {
        {QStringLiteral("test-usb-a"), QStringLiteral("/test/usb-a"),
         QStringLiteral("Test USB"), 16ULL * 1024 * 1024 * 1024, true, false},
        {QStringLiteral("test-usb-b"), QStringLiteral("/test/usb-b"),
         QStringLiteral("Test USB"), 16ULL * 1024 * 1024 * 1024, true, false},
        {QStringLiteral("test-system"), QStringLiteral("/test/system"),
         QStringLiteral("Internal disk"), 128ULL * 1024 * 1024 * 1024, false, false}
    };
    for (rufus::Device& device : devices)
    {
        device.mediaRemovable = device.removable;
        device.defaultVisible = device.removable;
    }
    return devices;
}

} // namespace

class TestRufusWindow : public QObject
{
    Q_OBJECT

private:
    void chooseFirstDevice()
    {
        deviceCombo_->setFocus();
        QTest::keyClick(deviceCombo_, Qt::Key_Home);
    }

    std::unique_ptr<rufus::SelectionController> controller_;
    std::unique_ptr<rufus::MainWindow> window_;
    QComboBox* deviceCombo_ = nullptr;
    QComboBox* bootCombo_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QPushButton* selectButton_ = nullptr;
    QPushButton* startButton_ = nullptr;
    QToolButton* logButton_ = nullptr;
    QTemporaryDir profile_;

private slots:
    void initTestCase()
    {
        QVERIFY(profile_.isValid());
        QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, profile_.filePath(QStringLiteral("settings")));
        qputenv("XDG_DATA_HOME", profile_.filePath(QStringLiteral("data")).toUtf8());
        // Only tests force a Widgets file picker. The application uses the
        // native dialog where the desktop provides one.
        QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    }

    void policyUpdateControls()
    {
        auto* update = window_->findChild<rufus::PolicyUpdates*>(); QVERIFY(update && !update->isActive());
        window_->initializePolicyUpdates();
        auto* question = window_->findChild<QMessageBox*>(QStringLiteral("initialPolicyUpdateChoice"));
        QVERIFY(question && question->isVisible()); QCOMPARE(question->defaultButton(), question->button(QMessageBox::No));
        QTest::mouseClick(question->button(QMessageBox::No),Qt::LeftButton);
        rufus::ApplicationSettings settings;
        QCOMPARE(settings.value(QStringLiteral("UpdateCheckInterval"),0).toInt(),-1); QVERIFY(!update->isActive());
        settings.setValue(QStringLiteral("LastPolicyUpdateCheck"),QString::number(QDateTime::currentSecsSinceEpoch())); settings.sync();
        auto* button = window_->findChild<QToolButton*>(QStringLiteral("settingsButton")); QVERIFY(button && button->isEnabled());
        QTest::mouseClick(button,Qt::LeftButton);
        auto* dialog = window_->findChild<rufus::PolicyUpdateDialog*>(QStringLiteral("policyUpdateDialog")); QVERIFY(dialog && dialog->isVisible());
        auto* frequency = dialog->findChild<QComboBox*>(QStringLiteral("policyUpdateFrequency")); QVERIFY(frequency);
        QCOMPARE(frequency->count(),4); QCOMPARE(frequency->currentData().toInt(),-1);
        frequency->setCurrentIndex(frequency->findData(604800));
        QCOMPARE(rufus::ApplicationSettings().value(QStringLiteral("UpdateCheckInterval"),0).toInt(),604800);
        QVERIFY(!update->isActive()); dialog->reject();
    }
    void policyDbxConsent_data()
    {
        QTest::addColumn<bool>("accept"); QTest::newRow("decline") << false; QTest::newRow("accept") << true;
    }
    void policyDbxConsent()
    {
        QFETCH(bool,accept);QTemporaryDir cache;rufus_test::HttpFixture server;QVERIFY(server.listen(QHostAddress::LocalHost));
        server.responses["/sbat_level.txt"]=rufus_test::response("shim,999");
        server.responses["/sb_active.txt"]=rufus_test::response(QByteArray(40,'a'));
        server.responses["/sb_revoked.txt"]=rufus_test::response(QByteArray(40,'b'));
        server.responses["/repos/policy/commits?path=dbx&page=1&per_page=1"]=rufus_test::response("[{\"commit\":{\"author\":{\"date\":\"2026-09-01T22:05:39Z\"}}}]");
        server.responses["/repos/policy/contents/dbx"]=rufus_test::response(secureboot_fixture::dbxList(QByteArray(32,'z')));
        rufus::PolicyUpdateSources sources;sources.textRoot=server.url("/");sources.dbx={{2,server.url("/repos/policy/contents/dbx"),1}};
        auto* update = window_->findChild<rufus::PolicyUpdates*>();QVERIFY(update);
        QSignalSpy done(update,&rufus::PolicyUpdates::completed),ask(update,&rufus::PolicyUpdates::dbxAvailable);
        QCOMPARE(update->start(cache.path(),controller_->secureBootPolicy(),sources),0);
        QTRY_COMPARE_WITH_TIMEOUT(ask.size(),1,3000);
        auto* question=window_->findChild<QMessageBox*>(QStringLiteral("dbxUpdateAvailable"));
        QVERIFY(question && question->isVisible());QCOMPARE(question->defaultButton(),question->button(QMessageBox::No));
        QVERIFY(!server.paths().contains("/repos/policy/contents/dbx"));
        QTest::mouseClick(question->button(accept?QMessageBox::Yes:QMessageBox::No),Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,3000);
        QCOMPARE(server.paths().contains("/repos/policy/contents/dbx"),accept);
        QCOMPARE(controller_->secureBootPolicy().sbat,QByteArray("shim,999"));
        QVERIFY(!controller_->isWritingActive());
    }

    void init()
    {
        QSettings(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("Rufus"), QStringLiteral("Linux")).clear();
        controller_ = std::make_unique<rufus::SelectionController>();
        window_ = std::make_unique<rufus::MainWindow>(*controller_);
        deviceCombo_ = window_->findChild<QComboBox*>(QStringLiteral("deviceCombo"));
        bootCombo_ = window_->findChild<QComboBox*>(QStringLiteral("bootCombo"));
        statusLabel_ = window_->findChild<QLabel*>(QStringLiteral("statusLabel"));
        selectButton_ = window_->findChild<QPushButton*>(QStringLiteral("selectImageButton"));
        startButton_ = window_->findChild<QPushButton*>(QStringLiteral("startButton"));
        logButton_ = window_->findChild<QToolButton*>(QStringLiteral("logButton"));
        QVERIFY(deviceCombo_ && bootCombo_ && statusLabel_);
        QVERIFY(selectButton_ && startButton_ && logButton_);
        window_->show();
        QVERIFY(QTest::qWaitForWindowExposed(window_.get(), 1000));
    }

    void cleanup()
    {
        window_.reset();
        controller_.reset();
        QString problem;
        QCOMPARE(rufus::ConfigureApplicationStorage(profile_.filePath(QStringLiteral("rufus-linux")), false, problem), 0);
    }

    void oldBiosChoice()
    {
        auto* option = window_->findChild<QCheckBox*>(QStringLiteral("oldBiosFixes"));
        QVERIFY(option && !option->isChecked() && !option->isEnabled());
        controller_->setDevices(testDevices()); controller_->setBootType(BT_GRUB2);
        QVERIFY(option->isEnabled() && !option->isChecked());
        controller_->setOldBiosFixes(true); QVERIFY(option->isChecked());
        controller_->setBootType(BT_NON_BOOTABLE);
        QVERIFY(!option->isEnabled() && !option->isChecked());
        controller_->setBootType(BT_GRUB2);
        QVERIFY(option->isEnabled() && option->isChecked());
        controller_->setOldBiosFixes(false); QVERIFY(!option->isChecked());
    }

    void extendedLabelChoice()
    {
        auto* option = window_->findChild<QCheckBox*>(QStringLiteral("extendedLabel"));
        QVERIFY(option && option->isChecked() && !option->isEnabled());
        controller_->setDevices(testDevices()); controller_->setBootType(BT_NON_BOOTABLE);
        QVERIFY(option->isEnabled() && option->isChecked());
        controller_->setExtendedLabel(false); QVERIFY(!option->isChecked());
        controller_->setExtendedLabel(true); QVERIFY(option->isChecked());
        controller_->setBootType(BT_GRUB2); controller_->setAdvancedFormat(true); controller_->setFileSystem(FS_EXT3);
        QCOMPARE(controller_->selection().fileSystem, int(FS_EXT3));
        QVERIFY(!option->isChecked() && !option->isEnabled());
        controller_->setFileSystem(FS_FAT32);
        QVERIFY(option->isChecked() && option->isEnabled());
    }

    void badBlockChoicesAndPreference()
    {
        controller_->setDevices(testDevices());
        auto* test = window_->findChild<QCheckBox*>(QStringLiteral("badBlocks"));
        auto* patterns = window_->findChild<QComboBox*>(QStringLiteral("passesCombo"));
        QVERIFY(test && patterns && test->isEnabled());
        QCOMPARE(patterns->count(), 5); QVERIFY(!patterns->isEnabled());
        controller_->setBadBlocksEnabled(true); controller_->setBadBlocksPattern(4);
        QVERIFY(test->isChecked() && patterns->isEnabled()); QCOMPARE(patterns->currentIndex(), 4);
        controller_->setBadBlocksPattern(5); QCOMPARE(patterns->currentIndex(), 4);
        const bool fake = controller_->detectFakes();
        controller_->toggleFakeDetection();
        rufus::SelectionController fresh;
        QCOMPARE(fresh.detectFakes(), !fake);
        controller_->toggleFakeDetection();
        auto devices = testDevices(); devices[0].readOnly = true;
        controller_->setDevices(devices);
        QVERIFY(!test->isEnabled() && !patterns->isEnabled());
    }

    void emptyStateExplainsUnavailableOperations()
    {
        QCOMPARE(deviceCombo_->count(), 0);
        QCOMPARE(deviceCombo_->currentIndex(), -1);
        QVERIFY(deviceCombo_->isEnabled()); // Win32 EnableControls leaves the empty list enabled.
        QVERIFY(bootCombo_->currentData().toString().isEmpty());
        QCOMPARE(bootCombo_->currentText(), QStringLiteral("Disk or ISO image (Please select)"));
        QVERIFY(!bootCombo_->isEditable());
        QVERIFY(selectButton_->isEnabled());
        QVERIFY(!startButton_->isEnabled());
        QLabel* status = window_->findChild<QLabel*>(QStringLiteral("statusLabel"));
        QProgressBar* progress = window_->findChild<QProgressBar*>(QStringLiteral("progressBar"));
        QVERIFY(status && progress);
        QVERIFY(status->text().contains(QStringLiteral("unavailable")));
        QCOMPARE(progress->value(), 0);
        QCOMPARE(progress->format(), QStringLiteral("READY"));
        QVERIFY(!window_->windowIcon().isNull());
        QVERIFY(!logButton_->icon().isNull());
        const QString screenshot = qEnvironmentVariable("RUFUS_TEST_SCREENSHOT");
        if (!screenshot.isEmpty())
            QVERIFY(window_->grab().save(screenshot));
    }

    void eligibleDevicesCanBeSelectedWithKeyboard()
    {
        controller_->setDevices(testDevices());
        QCOMPARE(deviceCombo_->count(), 2);
        QCOMPARE(deviceCombo_->currentIndex(), 0);
        QVERIFY(deviceCombo_->itemText(0).contains(QStringLiteral("/test/usb-a")));
        QVERIFY(deviceCombo_->itemText(1).contains(QStringLiteral("/test/usb-b")));
        QCOMPARE(controller_->selection().deviceId, QStringLiteral("test-usb-a"));
        deviceCombo_->setFocus();
        QTest::keyClick(deviceCombo_, Qt::Key_End);
        QCOMPARE(controller_->selection().deviceId, QStringLiteral("test-usb-b"));
        QVERIFY(!startButton_->isEnabled());
    }

    void standaloneSyslinuxSelection_data()
    {
        QTest::addColumn<int>("bootType");
        QTest::addColumn<QString>("name");
        QTest::addColumn<int>("index");
        QTest::newRow("v4") << int(BT_SYSLINUX_V4) << QStringLiteral("Syslinux 4.07") << 4;
        QTest::newRow("v6") << int(BT_SYSLINUX_V6) << QStringLiteral("Syslinux 6.04") << 5;
    }

    void standaloneSyslinuxSelection()
    {
        QFETCH(int, bootType);
        QFETCH(QString, name);
        QFETCH(int, index);
        auto* advanced = window_->findChild<QToolButton*>(QStringLiteral("advancedDriveButton"));
        auto* partitions = window_->findChild<QComboBox*>(QStringLiteral("partitionCombo"));
        auto* filesystems = window_->findChild<QComboBox*>(QStringLiteral("fileSystemCombo"));
        auto* target = window_->findChild<QComboBox*>(QStringLiteral("targetCombo"));
        QCOMPARE(bootCombo_->findText(QStringLiteral("Syslinux 4.07")), -1);
        QCOMPARE(bootCombo_->findText(QStringLiteral("Syslinux 6.04")), -1);
        QTest::mouseClick(advanced, Qt::LeftButton);
        QCOMPARE(bootCombo_->findText(name), index);
        QCOMPARE(bootCombo_->itemText(2), QStringLiteral("MS-DOS"));
        QCOMPARE(bootCombo_->itemText(3), QStringLiteral("FreeDOS"));
        QCOMPARE(bootCombo_->itemText(4), QStringLiteral("Syslinux 4.07"));
        QCOMPARE(bootCombo_->itemText(5), QStringLiteral("Syslinux 6.04"));
        QCOMPARE(bootCombo_->itemText(6), QStringLiteral("ReactOS"));
        QCOMPARE(bootCombo_->itemText(7), QStringLiteral("Grub 2.14"));
        QCOMPARE(bootCombo_->itemText(8), QStringLiteral("Grub4DOS 0.4.6a"));
        QCOMPARE(bootCombo_->itemText(9), QStringLiteral("UEFI:NTFS"));
        bootCombo_->setCurrentIndex(index);
        QVERIFY(QMetaObject::invokeMethod(bootCombo_, "activated", Q_ARG(int, index)));
        QCOMPARE(controller_->selection().bootType, bootType);
        QCOMPARE(partitions->count(), 0);
        QCOMPARE(filesystems->count(), 0);
        QVERIFY(!startButton_->isEnabled());
        QVERIFY(selectButton_->isEnabled());
        auto device = testDevices().first();
        device.fileSystemType = QStringLiteral("ntfs");
        device.volumeLabel = QStringLiteral("EXISTING");
        controller_->setDevices({device});
        QCOMPARE(partitions->count(), 1);
        QCOMPARE(partitions->currentText(), QStringLiteral("MBR"));
        QCOMPARE(target->count(), 1);
        QCOMPARE(target->currentText(), QStringLiteral("BIOS (or UEFI-CSM)"));
        const bool ntfs = bootType == BT_SYSLINUX_V6;
        QCOMPARE(filesystems->count(), ntfs ? 2 : 1);
        QCOMPARE(filesystems->itemText(0), QStringLiteral("FAT32 (Default)"));
        QCOMPARE(filesystems->currentText(), ntfs ? QStringLiteral("NTFS") : QStringLiteral("FAT32 (Default)"));
        QCOMPARE(controller_->selection().volumeLabel, QStringLiteral("EXISTING"));
        QCOMPARE(controller_->selection().clusterSize, quint32(ntfs ? 4096 : 8192));
        QCOMPARE(window_->findChild<QProgressBar*>(QStringLiteral("progressBar"))->format(), QStringLiteral("READY"));
        QTest::mouseClick(advanced, Qt::LeftButton);
        QCOMPARE(controller_->selection().bootType, int(BT_IMAGE));
        QVERIFY(!startButton_->isEnabled());
        QTest::mouseClick(advanced, Qt::LeftButton);
        QCOMPARE(bootCombo_->currentIndex(), 1);
        controller_->setBootType(-1);
        QCOMPARE(controller_->selection().bootType, int(BT_IMAGE));
    }

    void standaloneGrub4dosSelection()
    {
        auto* advanced = window_->findChild<QToolButton*>(QStringLiteral("advancedDriveButton"));
        QCOMPARE(bootCombo_->findText(QStringLiteral("Grub4DOS 0.4.6a")), -1);
        QTest::mouseClick(advanced, Qt::LeftButton);
        const int index = bootCombo_->findText(QStringLiteral("Grub4DOS 0.4.6a"));
        QCOMPARE(index, 8);
        bootCombo_->setCurrentIndex(index);
        QVERIFY(QMetaObject::invokeMethod(bootCombo_, "activated", Q_ARG(int, index)));
        auto device = testDevices().first();
        device.size = 64 * 1024 * 1024;
        device.fileSystemType = QStringLiteral("ntfs");
        controller_->setDevices({device});
        QCOMPARE(controller_->selection().bootType, int(BT_GRUB4DOS));
        QCOMPARE(controller_->selection().fileSystem, int(FS_NTFS));
        QCOMPARE(controller_->selection().defaultFileSystem, int(FS_FAT16));
        auto* filesystems = window_->findChild<QComboBox*>(QStringLiteral("fileSystemCombo"));
        QCOMPARE(filesystems->count(), 3);
        QCOMPARE(filesystems->itemText(0), QStringLiteral("FAT16 (Default)"));
        QCOMPARE(filesystems->currentText(), QStringLiteral("NTFS"));
        QCOMPARE(controller_->selection().clusterSize, quint32(4096));
        controller_->setFileSystem(FS_FAT16);
        QCOMPARE(controller_->selection().clusterSize, quint32(2048));
        controller_->setFileSystem(FS_FAT32);
        QCOMPARE(controller_->selection().fileSystem, int(FS_FAT32));
        QCOMPARE(controller_->selection().clusterSize, quint32(512));
        controller_->setFileSystem(FS_NTFS);
        QCOMPARE(controller_->selection().fileSystem, int(FS_NTFS));
        QCOMPARE(controller_->selection().clusterSize, quint32(4096));
        auto* clusters = window_->findChild<QComboBox*>(QStringLiteral("clusterCombo"));
        QVERIFY(clusters);
        const QList<quint32> ntfsClusters = {4096, 8192, 16384, 32768, 65536};
        QCOMPARE(clusters->count(), ntfsClusters.size());
        for (int i = 0; i < ntfsClusters.size(); ++i)
            QCOMPARE(clusters->itemData(i).toUInt(), ntfsClusters.at(i));
        controller_->setClusterSize(8192);
        QCOMPARE(controller_->selection().clusterSize, quint32(8192));
        controller_->setClusterSize(512);
        QCOMPARE(controller_->selection().clusterSize, quint32(8192));
        controller_->setFileSystem(FS_FAT16);
        QCOMPARE(window_->findChild<QComboBox*>(QStringLiteral("partitionCombo"))->currentText(), QStringLiteral("MBR"));
        QCOMPARE(window_->findChild<QComboBox*>(QStringLiteral("targetCombo"))->currentText(), QStringLiteral("BIOS (or UEFI-CSM)"));
        for (int fs : {FS_EXFAT, FS_EXT2}) controller_->setFileSystem(fs);
        QCOMPARE(controller_->selection().fileSystem, int(FS_FAT16));
        controller_->setPartitionScheme(rufus::PartitionScheme::Gpt);
        QCOMPARE(controller_->selection().partition, rufus::PartitionScheme::Mbr);
        const auto smallDevice = device;
        device.size = 4ULL * 1024 * 1024 * 1024;
        controller_->setDevices({device});
        QCOMPARE(controller_->selection().fileSystem, int(FS_UNKNOWN));
        controller_->selectDevice(device.id);
        QCOMPARE(filesystems->findData(FS_FAT16), -1);
        QVERIFY(filesystems->findData(FS_NTFS) >= 0);
        // The explicit FAT16 preference falls back at 4 GiB, then returns
        // when supported again; it takes precedence over existing NTFS.
        QCOMPARE(controller_->selection().fileSystem, int(FS_FAT32));
        QCOMPARE(controller_->selection().defaultFileSystem, int(FS_FAT32));
        QCOMPARE(filesystems->itemText(0), QStringLiteral("FAT32 (Default)"));
        controller_->setDevices({smallDevice});
        controller_->selectDevice(smallDevice.id);
        QCOMPARE(controller_->selection().fileSystem, int(FS_FAT16));
        QCOMPARE(controller_->selection().defaultFileSystem, int(FS_FAT16));
        QTest::mouseClick(advanced, Qt::LeftButton);
        QCOMPARE(controller_->selection().bootType, int(BT_IMAGE));
        QCOMPARE(bootCombo_->findText(QStringLiteral("Grub4DOS 0.4.6a")), -1);
    }

    void standaloneGrub2Selection()
    {
        auto* advanced = window_->findChild<QToolButton*>(QStringLiteral("advancedDriveButton"));
        auto* partitions = window_->findChild<QComboBox*>(QStringLiteral("partitionCombo"));
        auto* filesystems = window_->findChild<QComboBox*>(QStringLiteral("fileSystemCombo"));
        auto* target = window_->findChild<QComboBox*>(QStringLiteral("targetCombo"));
        QCOMPARE(bootCombo_->findText(QStringLiteral("Grub 2.14")), -1);
        QTest::mouseClick(advanced, Qt::LeftButton);
        const int index = bootCombo_->findText(QStringLiteral("Grub 2.14"));
        QCOMPARE(index, 7);
        bootCombo_->setCurrentIndex(index);
        QVERIFY(QMetaObject::invokeMethod(bootCombo_, "activated", Q_ARG(int, index)));
        QCOMPARE(controller_->selection().bootType, int(BT_GRUB2));
        auto device = testDevices().first();
        device.size = 64 * 1024 * 1024;
        device.fileSystemType = QStringLiteral("ntfs");
        device.volumeLabel = QStringLiteral("EXISTING");
        controller_->setDevices({device});
        QCOMPARE(partitions->count(), 1);
        QCOMPARE(partitions->currentText(), QStringLiteral("MBR"));
        QCOMPARE(target->currentText(), QStringLiteral("BIOS (or UEFI-CSM)"));
        QCOMPARE(filesystems->count(), 3);
        QCOMPARE(filesystems->itemText(0), QStringLiteral("FAT16 (Default)"));
        QCOMPARE(filesystems->itemText(1), QStringLiteral("FAT32"));
        QCOMPARE(filesystems->itemText(2), QStringLiteral("NTFS"));
        QCOMPARE(controller_->selection().fileSystem, int(FS_NTFS));
        QCOMPARE(controller_->selection().volumeLabel, QStringLiteral("EXISTING"));
        controller_->setFileSystem(FS_FAT32);
        QCOMPARE(controller_->selection().fileSystem, int(FS_FAT32));
        controller_->setFileSystem(FS_NTFS);
        QCOMPARE(controller_->selection().fileSystem, int(FS_NTFS));
        controller_->setPartitionScheme(rufus::PartitionScheme::Gpt);
        QCOMPARE(partitions->currentText(), QStringLiteral("MBR"));
        controller_->setPartitionScheme(rufus::PartitionScheme::Sfd);
        QCOMPARE(partitions->currentText(), QStringLiteral("MBR"));
        QTest::mouseClick(advanced, Qt::LeftButton);
        QCOMPARE(controller_->selection().bootType, int(BT_IMAGE));
        QCOMPARE(bootCombo_->findText(QStringLiteral("Grub 2.14")), -1);
    }

    void standaloneBootSelection()
    {
        auto* advanced = window_->findChild<QToolButton*>(QStringLiteral("advancedDriveButton"));
        auto* partitions = window_->findChild<QComboBox*>(QStringLiteral("partitionCombo"));
        auto* filesystems = window_->findChild<QComboBox*>(QStringLiteral("fileSystemCombo"));
        auto* target = window_->findChild<QComboBox*>(QStringLiteral("targetCombo"));
        QCOMPARE(bootCombo_->findText(QStringLiteral("UEFI:NTFS")), -1);
        QTest::mouseClick(advanced, Qt::LeftButton);
        const int standalone = bootCombo_->findText(QStringLiteral("UEFI:NTFS"));
        QVERIFY(standalone >= 0);
        bootCombo_->setCurrentIndex(standalone);
        QVERIFY(QMetaObject::invokeMethod(bootCombo_, "activated", Q_ARG(int, standalone)));
        QCOMPARE(controller_->selection().bootType, int(BT_UEFI_NTFS));
        QCOMPARE(filesystems->count(), 0);
        QVERIFY(!startButton_->isEnabled());
        QVERIFY(selectButton_->isEnabled());
        auto device = testDevices().first();
        device.partitionTableType = QStringLiteral("dos");
        device.volumeLabel = QStringLiteral("EXISTING");
        controller_->setDevices({device});
        QCOMPARE(partitions->currentText(), QStringLiteral("GPT"));
        QCOMPARE(partitions->count(), 2);
        QCOMPARE(target->count(), 1);
        QCOMPARE(target->currentText(), QStringLiteral("UEFI (non CSM)"));
        QCOMPARE(filesystems->count(), 2);
        QCOMPARE(filesystems->itemText(0), QStringLiteral("NTFS")); // FAT32 owns Win32's default marker, but is filtered out.
        QCOMPARE(filesystems->itemText(1), QStringLiteral("exFAT"));
        QCOMPARE(controller_->selection().fileSystem, int(FS_NTFS));
        QCOMPARE(controller_->selection().clusterSize, quint32(4096));
        QCOMPARE(controller_->selection().volumeLabel, QStringLiteral("EXISTING"));
        QVERIFY(partitions->isEnabled() && filesystems->isEnabled());
        const QString screenshot = qEnvironmentVariable("RUFUS_TEST_UEFI_NTFS_SCREENSHOT");
        if (!screenshot.isEmpty())
            QVERIFY(window_->grab().save(screenshot));
        QVERIFY(!controller_->canSelectSfd());
        controller_->setPartitionScheme(rufus::PartitionScheme::Sfd);
        QCOMPARE(partitions->currentText(), QStringLiteral("GPT"));
        controller_->setPartitionScheme(rufus::PartitionScheme::Mbr);
        controller_->setFileSystem(FS_EXFAT);
        controller_->setVolumeLabel(QStringLiteral("MY USB"));
        QCOMPARE(partitions->currentText(), QStringLiteral("MBR"));
        QCOMPARE(controller_->selection().clusterSize, quint32(32 * 1024));
        QTest::mouseClick(advanced, Qt::LeftButton);
        QCOMPARE(controller_->selection().bootType, int(BT_IMAGE));
        QCOMPARE(bootCombo_->findText(QStringLiteral("UEFI:NTFS")), -1);
        QCOMPARE(filesystems->findData(FS_EXFAT), -1);
        QTest::mouseClick(advanced, Qt::LeftButton);
        QCOMPARE(bootCombo_->currentIndex(), 1); // Expanding does not select the advanced choice again.
        controller_->setBootType(BT_UEFI_NTFS);
        QCOMPARE(partitions->currentText(), QStringLiteral("MBR"));
        QCOMPARE(controller_->selection().fileSystem, int(FS_EXFAT));
        QCOMPARE(controller_->selection().volumeLabel, QStringLiteral("MY USB"));
        controller_->setBootType(-1); // Unknown boot selections cannot create jobs.
        QCOMPARE(controller_->selection().bootType, int(BT_UEFI_NTFS));
        controller_->setBootType(BT_NON_BOOTABLE);
        controller_->setPartitionScheme(rufus::PartitionScheme::Sfd);
        controller_->setBootType(BT_UEFI_NTFS);
        QCOMPARE(partitions->currentText(), QStringLiteral("MBR")); // SetComboEntry falls back to the first allowed entry.
        device.size = (quint64(1) << 41) + 512;
        controller_->setAllowFixedUsb(true);
        controller_->setDevices({device});
        controller_->selectDevice(device.id);
        QCOMPARE(partitions->count(), 1);
        QCOMPARE(partitions->currentText(), QStringLiteral("GPT"));
        controller_->setPartitionScheme(rufus::PartitionScheme::Mbr);
        QCOMPARE(partitions->currentText(), QStringLiteral("GPT"));
        device.size = 16ULL * 1024 * 1024 * 1024;
        controller_->setDevices({device});
        controller_->selectDevice(device.id);
        QCOMPARE(partitions->currentText(), QStringLiteral("GPT"));
    }

    void standaloneExistingFormat_data()
    {
        QTest::addColumn<QString>("existing");
        QTest::addColumn<int>("expected");
        QTest::newRow("empty") << QString() << int(FS_NTFS);
        QTest::newRow("ntfs") << QStringLiteral("ntfs") << int(FS_NTFS);
        QTest::newRow("exfat") << QStringLiteral("exfat") << int(FS_EXFAT);
        QTest::newRow("vfat") << QStringLiteral("vfat") << int(FS_NTFS);
    }

    void standaloneExistingFormat()
    {
        QFETCH(QString, existing);
        QFETCH(int, expected);
        auto device = testDevices().first();
        device.fileSystemType = existing;
        device.fileSystemVersion = QStringLiteral("FAT32");
        controller_->setDevices({device});
        QTest::mouseClick(window_->findChild<QToolButton*>(QStringLiteral("advancedDriveButton")), Qt::LeftButton);
        controller_->setBootType(BT_UEFI_NTFS);
        QCOMPARE(controller_->selection().fileSystem, expected);
        controller_->setFileSystem(FS_FAT32);
        QCOMPARE(controller_->selection().fileSystem, expected);
        device.size = 16 * 1024 * 1024; // Neither native data volume fits beside the boot partition.
        controller_->setDevices({device});
        controller_->selectDevice(device.id);
        QCOMPARE(controller_->selection().fileSystems, quint32(0));
        QCOMPARE(controller_->selection().clusterSize, quint32(0));
        QVERIFY(!startButton_->isEnabled());
    }

    void nonBootableFileSystems_data()
    {
        QTest::addColumn<quint64>("capacity");
        QTest::addColumn<QString>("type");
        QTest::addColumn<QString>("version");
        QTest::addColumn<int>("expected");
        QTest::addColumn<int>("defaultFormat");
        QTest::addColumn<int>("count");
        constexpr quint64 MiB = 1024 * 1024;
        QTest::newRow("small-fat16") << 16 * MiB << QString() << QString() << int(FS_FAT16) << int(FS_FAT16) << 2;
        QTest::newRow("ntfs-without-boot-partition") << 18 * MiB << QStringLiteral("ntfs") << QString() << int(FS_NTFS) << int(FS_FAT16) << 4;
        QTest::newRow("all-four") << 64 * MiB << QString() << QString() << int(FS_FAT16) << int(FS_FAT16) << 5;
        QTest::newRow("existing-fat32") << 64 * MiB << QStringLiteral("vfat") << QStringLiteral("FAT32") << int(FS_FAT32) << int(FS_FAT16) << 5;
        QTest::newRow("existing-exfat") << 64 * MiB << QStringLiteral("exfat") << QString() << int(FS_EXFAT) << int(FS_FAT16) << 5;
        QTest::newRow("existing-ntfs") << 64 * MiB << QStringLiteral("ntfs") << QString() << int(FS_NTFS) << int(FS_FAT16) << 5;
        QTest::newRow("fat16-limit") << 4096 * MiB << QStringLiteral("vfat") << QStringLiteral("FAT16") << int(FS_FAT32) << int(FS_FAT32) << 4;
        QTest::newRow("large-fat32") << 65536 * MiB << QString() << QString() << int(FS_FAT32) << int(FS_FAT32) << 4;
        QTest::newRow("fat32-limit") << 2 * 1024 * 1024 * MiB << QString() << QString() << int(FS_NTFS) << int(FS_NTFS) << 3;
        QTest::newRow("too-large") << 256 * 1024 * 1024 * MiB << QString() << QString() << int(FS_UNKNOWN) << int(FS_UNKNOWN) << 0;
    }

    void nonBootableFileSystems()
    {
        QFETCH(quint64, capacity);
        QFETCH(QString, type);
        QFETCH(QString, version);
        QFETCH(int, expected);
        QFETCH(int, defaultFormat);
        QFETCH(int, count);
        rufus::Device device = testDevices().first();
        device.size = capacity;
        device.fileSystemType = type;
        device.fileSystemVersion = version;
        controller_->setDiscovery({device}, {});
        controller_->setBootType(BT_NON_BOOTABLE);
        auto* filesystems = window_->findChild<QComboBox*>(QStringLiteral("fileSystemCombo"));
        QCOMPARE(filesystems->count(), count);
        QCOMPARE(controller_->selection().fileSystem, expected);
        QCOMPARE(controller_->selection().defaultFileSystem, defaultFormat);
        for (int i = 0; i < count; ++i)
        {
            const int filesystem = filesystems->itemData(i).toInt();
            if (i > 0)
                QVERIFY(filesystems->itemData(i - 1).toInt() < filesystem);
            controller_->setFileSystem(filesystem);
            QCOMPARE(controller_->selection().fileSystem, filesystem);
            const QString tip = window_->findChild<QLineEdit*>(QStringLiteral("volumeEdit"))->toolTip();
            QVERIFY(tip.startsWith(filesystems->itemText(i).section(QLatin1Char(' '), 0, 0)));
            const bool longLabel = filesystem == FS_NTFS || filesystem == FS_UDF;
            QVERIFY(tip.contains(longLabel ? QStringLiteral("32 characters") : QStringLiteral("11 characters")));
            QVERIFY(controller_->selection().clusterSize != 0);
            const QString name = filesystems->itemText(i);
            QCOMPARE(name.endsWith(QStringLiteral(" (Default)")), filesystem == defaultFormat);
            QVERIFY(!name.contains(QStringLiteral("Large")));
            controller_->setDiscovery({device}, {});
            QCOMPARE(controller_->selection().fileSystem, filesystem); // Manual choice survives refresh.
        }
    }

    void nonBootableDeviceLimits_data()
    {
        QTest::addColumn<quint64>("capacity");
        QTest::addColumn<quint32>("cluster");
        constexpr quint64 MiB = 1024 * 1024;
        QTest::newRow("too-small") << 16 * MiB << quint32(0);
        QTest::newRow("small") << 64 * MiB << quint32(4096);
        QTest::newRow("256-MiB") << 256 * MiB << quint32(32768);
        QTest::newRow("32-GiB") << 32768 * MiB << quint32(131072);
        QTest::newRow("too-large") << 256 * 1024 * 1024 * MiB << quint32(0);
    }

    void nonBootableDeviceLimits()
    {
        QFETCH(quint64, capacity);
        QFETCH(quint32, cluster);
        QList<rufus::Device> devices = testDevices();
        devices = {devices.first()};
        devices[0].size = capacity;
        controller_->setDiscovery(devices, {});
        bootCombo_->setCurrentIndex(0);
        QVERIFY(QMetaObject::invokeMethod(bootCombo_, "activated", Q_ARG(int, 0)));
        auto* filesystems = window_->findChild<QComboBox*>(QStringLiteral("fileSystemCombo"));
        QCOMPARE(filesystems->findData(FS_EXFAT) >= 0, cluster != 0);
        if (cluster != 0)
        {
            controller_->setFileSystem(FS_EXFAT);
            QCOMPARE(controller_->selection().clusterSize, cluster);
        }
        QVERIFY(selectButton_->isEnabled());
        QVERIFY(!startButton_->isEnabled()); // No configured device helper in this fixture.
        controller_->setBootType(BT_IMAGE);
        QCOMPARE(filesystems->findData(FS_EXFAT), -1);
        QVERIFY(selectButton_->isEnabled());
    }

    void deviceFormatsAreVisibleBeforeImageSelection_data()
    {
        QTest::addColumn<QString>("tableType");
        QTest::newRow("mbr") << QStringLiteral("dos");
        QTest::newRow("gpt") << QStringLiteral("gpt");
        QTest::newRow("raw") << QString();
        QTest::newRow("unknown") << QStringLiteral("unknown");
    }

    void deviceFormatsAreVisibleBeforeImageSelection()
    {
        QFETCH(QString, tableType);
        auto* filesystem = window_->findChild<QComboBox*>(QStringLiteral("fileSystemCombo"));
        auto* cluster = window_->findChild<QComboBox*>(QStringLiteral("clusterCombo"));
        auto* partition = window_->findChild<QComboBox*>(QStringLiteral("partitionCombo"));
        auto* target = window_->findChild<QComboBox*>(QStringLiteral("targetCombo"));
        QVERIFY(filesystem && cluster && partition && target);
        QList<rufus::Device> devices = testDevices();
        devices[0].partitionTableType = tableType;
        controller_->setDiscovery(devices, {});
        const bool gpt = tableType == QStringLiteral("gpt");
        const QString expectedPartition = gpt ? QStringLiteral("GPT") : QStringLiteral("MBR");
        const QString expectedTarget = gpt ? QStringLiteral("UEFI (non CSM)") : QStringLiteral("BIOS (or UEFI-CSM)");
        QCOMPARE(partition->count(), 2);
        QCOMPARE(partition->currentText(), expectedPartition);
        QCOMPARE(target->count(), gpt ? 1 : 2);
        QCOMPARE(target->currentText(), expectedTarget);
        QVERIFY(!partition->isEnabled());
        QVERIFY(!target->isEnabled());
        QVERIFY(!controller_->selection().syslinux);
        QCOMPARE(filesystem->count(), 2);
        QCOMPARE(filesystem->currentText(), QStringLiteral("FAT32 (Default)"));
        QCOMPARE(cluster->currentText(), QStringLiteral("8192 bytes (Default)"));
        QVERIFY(!filesystem->isEnabled());
        QVERIFY(!cluster->isEnabled());
        QVERIFY(!startButton_->isEnabled());
        QVERIFY(selectButton_->isEnabled());

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        controller_->selectImage(directory.filePath(QStringLiteral("missing.iso")));
        QVERIFY(controller_->selection().imagePath.isEmpty());
        QCOMPARE(partition->currentText(), expectedPartition);
        QCOMPARE(target->currentText(), expectedTarget);
        QVERIFY(!partition->isEnabled());
        QVERIFY(!target->isEnabled());
        QCOMPARE(filesystem->currentText(), QStringLiteral("FAT32 (Default)"));
        QCOMPARE(cluster->currentText(), QStringLiteral("8192 bytes (Default)"));
        QVERIFY(!filesystem->isEnabled());
        QVERIFY(!cluster->isEnabled());
        QVERIFY(!startButton_->isEnabled());
        controller_->selectDevice(devices[1].id);
        QCOMPARE(partition->currentText(), QStringLiteral("MBR"));
        QCOMPARE(target->currentText(), QStringLiteral("BIOS (or UEFI-CSM)"));
        controller_->selectDevice(devices[0].id);
        QCOMPARE(partition->currentText(), expectedPartition);
        QCOMPARE(target->currentText(), expectedTarget);
        controller_->setDiscovery({}, {});
        QCOMPARE(filesystem->count(), 0);
        QCOMPARE(cluster->count(), 0);
        QCOMPARE(partition->count(), 0);
        QCOMPARE(target->count(), 0);
    }

    void idleVolumeLabelUsesRufusCapacityProposal_data()
    {
        QTest::addColumn<quint64>("capacity");
        QTest::addColumn<QString>("label");
        QTest::newRow("bytes") << quint64(512) << QStringLiteral("512 bytes");
        QTest::newRow("small-decimal") << quint64(1500000) << QStringLiteral("1.5MB");
        QTest::newRow("below-eight") << quint64(7900000000) << QStringLiteral("7.9GB");
        QTest::newRow("eight") << quint64(8000000000) << QStringLiteral("8 GB");
        QTest::newRow("near-sixteen") << quint64(15800000000) << QStringLiteral("16 GB");
        QTest::newRow("rounding-boundary") << quint64(15200000000) << QStringLiteral("16 GB");
        QTest::newRow("outside-rounding") << quint64(15000000000) << QStringLiteral("15 GB");
        QTest::newRow("binary-capacity") << quint64(16ULL * 1024 * 1024 * 1024) << QStringLiteral("17 GB");
        QTest::newRow("terabytes") << quint64(2000000000000) << QStringLiteral("2TB");
    }

    void idleVolumeLabelUsesRufusCapacityProposal()
    {
        QFETCH(quint64, capacity);
        QFETCH(QString, label);
        rufus::Device device = testDevices().first();
        device.size = capacity;
        controller_->setDiscovery({device}, {});
        auto* volume = window_->findChild<QLineEdit*>(QStringLiteral("volumeEdit"));
        QVERIFY(volume);
        QCOMPARE(volume->text(), label);
        QVERIFY(!volume->isEnabled());
        QVERIFY(!startButton_->isEnabled());
        device.volumeLabel = QStringLiteral("MY USB");
        controller_->setDiscovery({device}, {});
        QCOMPARE(volume->text(), device.volumeLabel);
        controller_->selectImage(QStringLiteral("/missing-label-test.iso"));
        QCOMPARE(volume->text(), device.volumeLabel);
        controller_->setDiscovery({}, {});
        QVERIFY(volume->text().isEmpty());
    }

    void idleFileSystemUsesExistingSupportedFormat_data()
    {
        QTest::addColumn<QString>("type");
        QTest::addColumn<QString>("version");
        QTest::addColumn<quint64>("capacity");
        QTest::addColumn<int>("expected");
        constexpr quint64 normalSize = 16ULL * 1024 * 1024 * 1024;
        constexpr quint64 smallSize = 64ULL * 1024 * 1024;
        QTest::newRow("ntfs") << QStringLiteral("ntfs") << QString() << normalSize << int(FS_NTFS);
        QTest::newRow("fat32") << QStringLiteral("vfat") << QStringLiteral("FAT32") << normalSize << int(FS_FAT32);
        QTest::newRow("fat16") << QStringLiteral("vfat") << QStringLiteral("FAT16") << normalSize << int(FS_FAT32);
        QTest::newRow("unknown-fat") << QStringLiteral("vfat") << QString() << normalSize << int(FS_FAT32);
        QTest::newRow("exfat") << QStringLiteral("exfat") << QString() << normalSize << int(FS_FAT32);
        QTest::newRow("unknown") << QString() << QString() << normalSize << int(FS_FAT32);
        QTest::newRow("small-fat16") << QStringLiteral("vfat") << QStringLiteral("FAT16") << smallSize << int(FS_FAT16);
        QTest::newRow("small-fat32") << QStringLiteral("vfat") << QStringLiteral("FAT32") << smallSize << int(FS_FAT32);
        QTest::newRow("small-unknown") << QString() << QString() << smallSize << int(FS_FAT16);
        QTest::newRow("fat32-ineligible-capacity") << QStringLiteral("vfat") << QStringLiteral("FAT32")
            << quint64(2ULL * 1024 * 1024 * 1024 * 1024) << int(FS_NTFS);
    }

    void idleFileSystemUsesExistingSupportedFormat()
    {
        QFETCH(QString, type);
        QFETCH(QString, version);
        QFETCH(quint64, capacity);
        QFETCH(int, expected);
        rufus::Device device = testDevices().first();
        device.size = capacity;
        device.fileSystemType = type;
        device.fileSystemVersion = version;
        controller_->setDiscovery({device}, {});
        auto* filesystem = window_->findChild<QComboBox*>(QStringLiteral("fileSystemCombo"));
        auto* cluster = window_->findChild<QComboBox*>(QStringLiteral("clusterCombo"));
        QVERIFY(filesystem && cluster);
        QCOMPARE(controller_->selection().fileSystem, expected);
        QCOMPARE(filesystem->currentData().toInt(), expected);
        QCOMPARE(cluster->currentData().toUInt(), controller_->selection().clusterSizes.Default);
        QVERIFY(!filesystem->isEnabled());
        QVERIFY(!startButton_->isEnabled());
        QCOMPARE(filesystem->findData(FS_FAT16) >= 0, capacity < 4ULL * 1024 * 1024 * 1024);
        QCOMPARE(filesystem->findData(FS_EXFAT), -1);
        controller_->selectImage(QStringLiteral("/missing-format-test.iso"));
        QCOMPARE(filesystem->currentData().toInt(), expected);
        rufus::Device second = testDevices().at(1);
        second.fileSystemType = QStringLiteral("ntfs");
        controller_->setDiscovery({device, second}, {});
        controller_->selectDevice(second.id);
        QCOMPARE(filesystem->currentText(), QStringLiteral("NTFS"));
        QCOMPARE(filesystem->itemText(filesystem->findData(FS_FAT32)), QStringLiteral("FAT32 (Default)"));
        controller_->selectDevice(device.id);
        QCOMPARE(filesystem->currentData().toInt(), expected);
    }

    void initialDiscoverySelectsSmallestEligibleDevice_data()
    {
        QTest::addColumn<QString>("initialState");
        QTest::newRow("startup") << QStringLiteral("startup");
        QTest::newRow("empty-then-arrival") << QStringLiteral("empty");
        QTest::newRow("excluded-then-arrival") << QStringLiteral("excluded");
        QTest::newRow("initial-discovery-error") << QStringLiteral("error");
    }

    void initialDiscoverySelectsSmallestEligibleDevice()
    {
        QFETCH(QString, initialState);
        QList<rufus::Device> devices = testDevices();
        devices[0].size = 32ULL * 1024 * 1024 * 1024;
        devices[1].size = 8ULL * 1024 * 1024 * 1024;
        devices[2].size = 4ULL * 1024 * 1024 * 1024;
        if (initialState == QStringLiteral("empty"))
            controller_->setDiscovery({}, {});
        else if (initialState == QStringLiteral("excluded"))
            controller_->setDiscovery({devices[2]}, {});
        else if (initialState == QStringLiteral("error"))
            controller_->setDiscovery({}, QStringLiteral("Discovery unavailable"));
        QCOMPARE(deviceCombo_->currentIndex(), -1);

        controller_->setDiscovery(devices, {});
        QCOMPARE(deviceCombo_->count(), 2);
        QCOMPARE(deviceCombo_->currentIndex(), 0);
        QCOMPARE(deviceCombo_->itemData(0).toString(), devices[1].id);
        QCOMPARE(deviceCombo_->itemData(1).toString(), devices[0].id);
        QCOMPARE(controller_->selection().deviceId, devices[1].id);
        QVERIFY(!controller_->isWritingActive());
        QVERIFY(!startButton_->isEnabled());
        for (QMessageBox* dialog : window_->findChildren<QMessageBox*>())
            QVERIFY(!dialog->isVisible());
    }

    void smallerArrivalPreservesUserSelection()
    {
        QList<rufus::Device> devices = testDevices();
        devices[1].size *= 2;
        controller_->setDiscovery(devices, {});
        deviceCombo_->setFocus();
        QTest::keyClick(deviceCombo_, Qt::Key_End);
        QCOMPARE(controller_->selection().deviceId, devices[1].id);

        rufus::Device smaller = devices[0];
        smaller.id = QStringLiteral("test-small-usb");
        smaller.path = QStringLiteral("/test/small-usb");
        smaller.size /= 2;
        devices.append(smaller);
        controller_->setDiscovery(devices, {});
        QCOMPARE(deviceCombo_->itemData(0).toString(), smaller.id);
        QCOMPARE(deviceCombo_->currentIndex(), 2);
        QCOMPARE(controller_->selection().deviceId, devices[1].id);
        QVERIFY(!startButton_->isEnabled());
    }

    void refreshPreservesIdentityAcrossReordering()
    {
        QList<rufus::Device> devices = testDevices();
        controller_->setDevices(devices);
        chooseFirstDevice();
        QSignalSpy logs(controller_.get(), &rufus::SelectionController::logMessage);
        devices.swapItemsAt(0, 1);
        devices[1].name = QStringLiteral("Renamed USB");
        controller_->setDevices(devices);
        QCOMPARE(deviceCombo_->currentIndex(), 1);
        QCOMPARE(controller_->selection().deviceId, QStringLiteral("test-usb-a"));
        QVERIFY(deviceCombo_->currentText().contains(QStringLiteral("Renamed USB")));
        QCOMPARE(logs.size(), 0);
    }

    void extendedDeviceCaptionKeepsSelection()
    {
        auto devices=testDevices(); devices[0].volumeLabel=QStringLiteral("Long Windows installer label");
        controller_->setDevices(devices); chooseFirstDevice();
        QCOMPARE(controller_->selection().deviceId,QStringLiteral("test-usb-a"));
        QVERIFY(deviceCombo_->currentText().startsWith(QStringLiteral("Long Windows installer label —")));
        devices[0].mountedVolumes=2; controller_->setDevices(devices);
        QVERIFY(deviceCombo_->currentText().startsWith(QStringLiteral("Multiple Volumes —")));
        QCOMPARE(controller_->selection().deviceId,QStringLiteral("test-usb-a"));
        devices[0].mountedVolumes=0; devices[0].volumeLabel.clear(); controller_->setDevices(devices);
        QVERIFY(deviceCombo_->currentText().contains(devices[0].name));
    }

    void readOnlyDeviceCanSaveButCannotCreate()
    {
        QList<rufus::Device> devices = testDevices();
        devices[0].identity = {QStringLiteral("stable-physical-identity")};
        devices[0].readOnly = true;
        controller_->isoWriter().configure(QDBusConnection::sessionBus(), QStringLiteral("/usr/bin/true"));
        controller_->setDiscovery(devices, {});
        controller_->setBootType(BT_NON_BOOTABLE);
        auto* save = window_->findChild<QToolButton*>(QStringLiteral("saveDeviceButton"));
        QVERIFY(save && save->isEnabled() && controller_->canSaveDevice());
        QVERIFY(!controller_->canStartWrite() && !startButton_->isEnabled());
        QVERIFY(deviceCombo_->currentText().contains(QStringLiteral("[Read-only]")));
        for (const char* name : {"fileSystemCombo", "clusterCombo", "partitionCombo", "targetCombo"})
            QVERIFY(!window_->findChild<QComboBox*>(QLatin1String(name))->isEnabled());
        const QString selected = controller_->selection().deviceId;
        devices[0].readOnly = false;
        controller_->setDiscovery(devices, {});
        QCOMPARE(controller_->selection().deviceId, selected);
        QVERIFY(controller_->canStartWrite() && startButton_->isEnabled());
        devices[0].readOnly = true;
        controller_->setDiscovery(devices, {});
        QCOMPARE(controller_->selection().deviceId, selected);
        QVERIFY(!controller_->canStartWrite() && controller_->canSaveDevice());
        controller_->startWrite();
        QVERIFY(!controller_->isBusy());
    }

    void changedDeviceRequiresReselection_data()
    {
        QTest::addColumn<QString>("change");
        QTest::newRow("removed") << QStringLiteral("removed");
        QTest::newRow("replacement-at-same-path") << QStringLiteral("replacement");
        QTest::newRow("capacity-changed") << QStringLiteral("capacity");
        QTest::newRow("path-changed") << QStringLiteral("path");
        QTest::newRow("ambiguous-identity") << QStringLiteral("ambiguous");
    }

    void changedDeviceRequiresReselection()
    {
        QFETCH(QString, change);
        QList<rufus::Device> devices = testDevices();
        controller_->setDevices(devices);
        chooseFirstDevice();
        if (change == QStringLiteral("removed"))
            devices.removeFirst();
        else if (change == QStringLiteral("replacement"))
            devices[0].id = QStringLiteral("test-replacement");
        else if (change == QStringLiteral("capacity"))
            devices[0].size /= 2;
        else if (change == QStringLiteral("path"))
            devices[0].path = QStringLiteral("/test/changed-path");
        else if (change == QStringLiteral("readonly"))
            devices[0].readOnly = true;
        else if (change == QStringLiteral("ambiguous"))
            devices[1].id = devices[0].id;
        controller_->setDevices(devices);
        QVERIFY(controller_->selection().deviceId.isEmpty());
        QCOMPARE(deviceCombo_->currentIndex(), -1);
        QVERIFY(!startButton_->isEnabled());
        controller_->setDevices(testDevices());
        QCOMPARE(deviceCombo_->currentIndex(), -1);
    }

    void ctrlSelectArchivePreservesImageAndCancelClearsArchive()
    {
        QTemporaryDir directory;
        const QString image = directory.filePath(QStringLiteral("disk.img"));
        QFile raw(image); QVERIFY(raw.open(QIODevice::WriteOnly));
        QByteArray data(512, '\0'); data[510] = '\x55'; data[511] = '\xaa';
        QCOMPARE(raw.write(data), qint64(data.size())); raw.close();
        controller_->selectImage(image);
        QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), 3000);
        const QString archive = directory.filePath(QString::fromUtf8("Fișiere.zip"));
        QFile zip(archive); QVERIFY(zip.open(QIODevice::WriteOnly));
        QCOMPARE(zip.write(QByteArray::fromHex("504b0506000000000000000000000000000000000000")), qint64(22)); zip.close();
        QTest::mouseClick(selectButton_, Qt::LeftButton, Qt::ControlModifier);
        auto* dialog = window_->findChild<QFileDialog*>(QStringLiteral("archiveDialog"));
        QVERIFY(dialog && dialog->isVisible());
        dialog->setDirectory(directory.path()); dialog->selectFile(archive);
        auto* buttons = dialog->findChild<QDialogButtonBox*>();
        QVERIFY(buttons && buttons->button(QDialogButtonBox::Open));
        QTRY_VERIFY_WITH_TIMEOUT(buttons->button(QDialogButtonBox::Open)->isEnabled(), uiTimeoutMs);
        QTest::mouseClick(buttons->button(QDialogButtonBox::Open), Qt::LeftButton);
        QCOMPARE(controller_->selection().archivePath, archive);
        QCOMPARE(controller_->selection().imagePath, image);
        QVERIFY(selectButton_->toolTip().contains(archive));
        QTest::mouseClick(selectButton_, Qt::LeftButton, Qt::ControlModifier);
        QVERIFY(dialog->isVisible()); QTest::keyClick(dialog, Qt::Key_Escape);
        QVERIFY(controller_->selection().archivePath.isEmpty());
        QCOMPARE(controller_->selection().imagePath, image);
        controller_->selectArchive(archive); QCOMPARE(controller_->selection().archivePath, archive);
        controller_->selectImage(image); QVERIFY(controller_->selection().archivePath.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), 3000);
    }

    void filePickerSelectsImageAndCancellationPreservesIt()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QString::fromUtf8("Mediu și fișiere.img"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QByteArray image(512, '\0');
        image[510] = '\x55';
        image[511] = '\xaa';
        QCOMPARE(file.write(image), qint64(image.size()));
        file.close();

        QTest::mouseClick(selectButton_, Qt::LeftButton);
        QFileDialog* dialog = window_->findChild<QFileDialog*>(QStringLiteral("imageDialog"));
        QVERIFY(dialog && dialog->isVisible());
        dialog->setDirectory(directory.path());
        dialog->selectFile(path);
        QDialogButtonBox* buttons = dialog->findChild<QDialogButtonBox*>();
        QVERIFY(buttons && buttons->button(QDialogButtonBox::Open));
        QTRY_VERIFY_WITH_TIMEOUT(buttons->button(QDialogButtonBox::Open)->isEnabled(), uiTimeoutMs);
        QTest::mouseClick(buttons->button(QDialogButtonBox::Open), Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(bootCombo_->currentData().toString(), path, uiTimeoutMs);
        QCOMPARE(bootCombo_->currentText(), QString::fromUtf8("Mediu și fișiere.img"));
        QCOMPARE(controller_->selection().imageSize, quint64(image.size()));
        QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), 3000);
        QCOMPARE(controller_->selection().imageState, rufus::ImageState::Complete);
        QVERIFY(bootCombo_->toolTip().contains(path));
        controller_->setDevices(testDevices());
        chooseFirstDevice();
        QVERIFY(!startButton_->isEnabled());

        QTest::mouseClick(selectButton_, Qt::LeftButton);
        QVERIFY(dialog->isVisible());
        QTest::keyClick(dialog, Qt::Key_Escape);
        QTRY_VERIFY_WITH_TIMEOUT(!dialog->isVisible(), uiTimeoutMs);
        QCOMPARE(bootCombo_->currentData().toString(), path);
        QCOMPARE(controller_->selection().deviceId, QStringLiteral("test-usb-a"));
    }

    void invalidImageClearsPreviousSelection_data()
    {
        QTest::addColumn<QString>("kind");
        QTest::newRow("missing") << QStringLiteral("missing");
        QTest::newRow("directory") << QStringLiteral("directory");
        QTest::newRow("empty") << QStringLiteral("empty");
        QTest::newRow("unaligned-raw") << QStringLiteral("unaligned-raw");
        QTest::newRow("invalid-iso") << QStringLiteral("invalid-iso");
    }

    void invalidImageClearsPreviousSelection()
    {
        QFETCH(QString, kind);
        QTemporaryFile previous;
        QVERIFY(previous.open());
        QCOMPARE(previous.write("image"), qint64(5));
        QVERIFY(previous.flush());
        controller_->selectImage(previous.fileName());
        QVERIFY(!bootCombo_->currentData().toString().isEmpty());
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QString invalidPath = directory.filePath(QStringLiteral("missing.img"));
        if (kind == QStringLiteral("directory"))
            invalidPath = directory.path();
        else if (kind == QStringLiteral("empty"))
        {
            QFile empty(invalidPath);
            QVERIFY(empty.open(QIODevice::WriteOnly));
        }
        else if (kind == QStringLiteral("unaligned-raw") || kind == QStringLiteral("invalid-iso"))
        {
            invalidPath = directory.filePath(kind == QStringLiteral("invalid-iso")
                ? QStringLiteral("invalid.iso") : QStringLiteral("invalid.img"));
            QFile invalid(invalidPath);
            QVERIFY(invalid.open(QIODevice::WriteOnly));
            QCOMPARE(invalid.write("test image"), qint64(10));
        }
        controller_->selectImage(invalidPath);
        QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), 3000);
        QVERIFY(bootCombo_->currentData().toString().isEmpty());
        QCOMPARE(controller_->selection().imageSize, quint64(0));
        QVERIFY(!controller_->selection().imageProblem.isEmpty());
        QCOMPARE(statusLabel_->text(), QStringLiteral("Failed to scan image"));
        QCOMPARE(statusLabel_->toolTip(), controller_->selection().imageProblem);
        QCOMPARE(window_->findChild<QProgressBar*>(QStringLiteral("progressBar"))->format(), QStringLiteral("READY"));
        auto* notice = window_->findChild<QMessageBox*>(QStringLiteral("imageNotice"));
        QVERIFY(notice && notice->isVisible());
        QCOMPARE(notice->windowTitle(), QStringLiteral("Unsupported image"));
        QCOMPARE(notice->detailedText(), controller_->selection().imageProblem);
        QTest::mouseClick(notice->button(QMessageBox::Ok), Qt::LeftButton);
        QVERIFY(!startButton_->isEnabled());
    }

    void logCanBeClosedAndReopenedWithoutLosingMessages()
    {
        QTest::mouseClick(logButton_, Qt::LeftButton);
        QDialog* logWindow = window_->findChild<QDialog*>(QStringLiteral("logWindow"));
        QPlainTextEdit* text = window_->findChild<QPlainTextEdit*>(QStringLiteral("logText"));
        QVERIFY(logWindow && text && logWindow->isVisible());
        QVERIFY(text->isReadOnly());
        QTest::keyClick(logWindow, Qt::Key_Escape);
        QVERIFY(!logWindow->isVisible());
        controller_->setDevices(testDevices());
        chooseFirstDevice();
        QTest::mouseClick(logButton_, Qt::LeftButton);
        QVERIFY(logWindow->isVisible());
        QVERIFY(text->toPlainText().contains(QStringLiteral("/test/usb-a")));
        QCOMPARE(window_->findChildren<QDialog*>(QStringLiteral("logWindow")).size(), 1);
    }

    void keyboardNavigationAndCloseLeaveNoOrphanWindow()
    {
        QTRY_VERIFY(window_->isActiveWindow());
        selectButton_->setFocus();
        QTest::keyClick(selectButton_, Qt::Key_Return);
        QFileDialog* picker = window_->findChild<QFileDialog*>(QStringLiteral("imageDialog"));
        QVERIFY(picker && picker->isVisible());
        QTest::keyClick(picker, Qt::Key_Escape);
        QTRY_VERIFY(!picker->isVisible());
        // Native compositors restore activation after the modal window closes.
        QTRY_VERIFY(window_->isActiveWindow());
        selectButton_->setFocus();
        QTest::keyClick(selectButton_, Qt::Key_Tab);
        QWidget* advancedDrive = window_->findChild<QWidget*>(QStringLiteral("advancedDriveButton"));
        QWidget* advancedFormat = window_->findChild<QWidget*>(QStringLiteral("advancedFormatButton"));
        QVERIFY(advancedDrive && advancedFormat);
        QTRY_VERIFY(advancedDrive->hasFocus());
        QTest::keyClick(advancedDrive, Qt::Key_Tab);
        QTRY_VERIFY(advancedFormat->hasFocus());
        QTest::keyClick(advancedFormat, Qt::Key_Tab);
        QWidget* language = window_->findChild<QWidget*>(QStringLiteral("languageButton"));
        QTRY_VERIFY(language && language->hasFocus());
        QTest::keyClick(language, Qt::Key_Tab);
        QWidget* about = window_->findChild<QWidget*>(QStringLiteral("aboutButton"));
        QTRY_VERIFY(about && about->hasFocus());
        QTest::keyClick(about, Qt::Key_Tab);
        QWidget* settings = window_->findChild<QWidget*>(QStringLiteral("settingsButton"));
        QTRY_VERIFY(settings && settings->hasFocus());
        QTest::keyClick(settings, Qt::Key_Tab);
        QTRY_VERIFY(logButton_->hasFocus());
        QTest::keyClick(logButton_, Qt::Key_Space);
        QDialog* logWindow = window_->findChild<QDialog*>(QStringLiteral("logWindow"));
        QVERIFY(logWindow && logWindow->isVisible());
        QPushButton* close = window_->findChild<QPushButton*>(QStringLiteral("closeButton"));
        QVERIFY(close);
        QTest::mouseClick(close, Qt::LeftButton);
        QVERIFY(!window_->isVisible());
        QVERIFY(!logWindow->isVisible());
    }

    void escapeClosesTheMainWindow()
    {
        QTRY_VERIFY(window_->isActiveWindow());
        QTest::keyClick(window_.get(), Qt::Key_Escape);
        QTRY_VERIFY(!window_->isVisible());
    }

    void win32ControlOrderAndDefaults()
    {
        // These captions and pairings come from upstream IDD_DIALOG, rather
        // than from a redesigned Linux workflow.
        const QStringList labelNames = {QStringLiteral("driveHeader"), QStringLiteral("deviceLabel"),
            QStringLiteral("bootLabel"), QStringLiteral("partitionLabel"), QStringLiteral("formatHeader"),
            QStringLiteral("volumeLabel"), QStringLiteral("fileSystemLabel"), QStringLiteral("statusHeader")};
        const QStringList captions = {QStringLiteral("Drive Properties"), QStringLiteral("Device"),
            QStringLiteral("Boot selection"), QStringLiteral("Partition scheme"), QStringLiteral("Format Options"),
            QStringLiteral("Volume label"), QStringLiteral("File system"), QStringLiteral("Status")};
        int previousY = -1;
        for (qsizetype i = 0; i < labelNames.size(); ++i)
        {
            QLabel* label = window_->findChild<QLabel*>(labelNames[i]);
            QVERIFY(label);
            QCOMPARE(label->text(), captions[i]);
            const int y = label->mapTo(window_.get(), QPoint()).y();
            QVERIFY(y > previousY);
            previousY = y;
        }
        QComboBox* partition = window_->findChild<QComboBox*>(QStringLiteral("partitionCombo"));
        QComboBox* target = window_->findChild<QComboBox*>(QStringLiteral("targetCombo"));
        QComboBox* filesystem = window_->findChild<QComboBox*>(QStringLiteral("fileSystemCombo"));
        QComboBox* cluster = window_->findChild<QComboBox*>(QStringLiteral("clusterCombo"));
        QVERIFY(partition && target && filesystem && cluster);
        QCOMPARE(partition->y(), target->y());
        QCOMPARE(filesystem->y(), cluster->y());
        QCOMPARE(partition->x(), filesystem->x());
        QCOMPARE(target->x(), cluster->x());
        QVERIFY(target->x() > partition->x());
        QVERIFY(!partition->isEnabled() && !target->isEnabled());
        QVERIFY(!filesystem->isEnabled() && !cluster->isEnabled());
        QCOMPARE(selectButton_->text(), QStringLiteral("SELECT"));
        QCOMPARE(startButton_->text(), QStringLiteral("START"));
        QCheckBox* quick = window_->findChild<QCheckBox*>(QStringLiteral("quickFormat"));
        QCheckBox* extended = window_->findChild<QCheckBox*>(QStringLiteral("extendedLabel"));
        QVERIFY(quick && extended && quick->isChecked() && extended->isChecked());
        QVERIFY(!window_->findChild<QWidget*>(QStringLiteral("imageOptions"))->isVisible());
    }

    void fixedUsbCheckboxAndShortcut()
    {
        auto* option = window_->findChild<QCheckBox*>(QStringLiteral("listUsbHardDrives"));
        QVERIFY(option && !option->isChecked());
        QCOMPARE(option->toolTip(), QStringLiteral("Enable the listing of USB Hard Drive enclosures. USE AT YOUR OWN RISKS!!!"));
        QSignalSpy changed(controller_.get(), &rufus::SelectionController::fixedUsbChanged);
        window_->activateWindow();
        QTRY_VERIFY_WITH_TIMEOUT(window_->isActiveWindow(), uiTimeoutMs);
        QTest::keyClick(window_.get(), Qt::Key_F, Qt::AltModifier);
        QTRY_VERIFY_WITH_TIMEOUT(option->isChecked(), uiTimeoutMs);
        QVERIFY(controller_->selection().allowFixedUsb);
        QCOMPARE(changed.size(), 1);
        QCOMPARE(window_->statusBar()->currentMessage(), QStringLiteral("Hard disk detection enabled"));
        auto* expand = window_->findChild<QToolButton*>(QStringLiteral("advancedDriveButton"));
        QTest::mouseClick(expand, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(option->isVisible(), uiTimeoutMs);
        QTest::mouseClick(option, Qt::LeftButton, Qt::NoModifier, QPoint(8, option->height() / 2));
        QVERIFY(!controller_->selection().allowFixedUsb);
        QCOMPARE(changed.size(), 2);
        QCOMPARE(window_->statusBar()->currentMessage(), QStringLiteral("Hard disk detection disabled"));
        controller_->setAllowFixedUsb(false);
        QCOMPARE(changed.size(), 2);
        rufus::SelectionController fresh;
        QVERIFY(!fresh.selection().allowFixedUsb); // Session choice, not a saved preference.
    }

    void advancedSectionsExpandAndCollapseLikeWin32()
    {
        QToolButton* drive = window_->findChild<QToolButton*>(QStringLiteral("advancedDriveButton"));
        QToolButton* format = window_->findChild<QToolButton*>(QStringLiteral("advancedFormatButton"));
        QWidget* driveOptions = window_->findChild<QWidget*>(QStringLiteral("advancedDriveOptions"));
        QWidget* formatOptions = window_->findChild<QWidget*>(QStringLiteral("advancedFormatOptions"));
        QWidget* saveDevice = window_->findChild<QWidget*>(QStringLiteral("saveDeviceButton"));
        QVERIFY(drive && format && driveOptions && formatOptions && saveDevice);
        QVERIFY(!driveOptions->isVisible() && !formatOptions->isVisible());
        QVERIFY(saveDevice->isVisible());
        const int initialHeight = window_->height();
        QTest::mouseClick(drive, Qt::LeftButton);
        QTest::mouseClick(format, Qt::LeftButton);
        QVERIFY(driveOptions->isVisible() && formatOptions->isVisible());
        QVERIFY(saveDevice->isVisible());
        QVERIFY(driveOptions->isEnabled() && formatOptions->isEnabled());
        QVERIFY(window_->findChild<QCheckBox*>(QStringLiteral("listUsbHardDrives"))->isEnabled());
        QVERIFY(!window_->findChild<QCheckBox*>(QStringLiteral("oldBiosFixes"))->isEnabled());
        QVERIFY(!window_->findChild<QCheckBox*>(QStringLiteral("uefiValidation"))->isEnabled());
        QCOMPARE(drive->text(), QStringLiteral("Hide advanced drive properties"));
        QCOMPARE(format->arrowType(), Qt::UpArrow);
        QTRY_VERIFY_WITH_TIMEOUT(window_->height() > initialHeight, uiTimeoutMs);
        const QString screenshot = qEnvironmentVariable("RUFUS_TEST_EXPANDED_SCREENSHOT");
        if (!screenshot.isEmpty())
            QVERIFY(window_->grab().save(screenshot));
        QTest::mouseClick(drive, Qt::LeftButton);
        QTest::mouseClick(format, Qt::LeftButton);
        QVERIFY(!driveOptions->isVisible() && !formatOptions->isVisible());
        QTRY_COMPARE_WITH_TIMEOUT(window_->height(), initialHeight, uiTimeoutMs);
    }

    void saveDevicePickerOffersDiskFormatsWithoutAnInputImage()
    {
        QSettings settings(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("Rufus"), QStringLiteral("Linux"));
        const QVariant previous = settings.value(QStringLiteral("PreferredSaveImageType"));
        settings.setValue(QStringLiteral("PreferredSaveImageType"), QStringLiteral("vhdx"));
        auto* save = window_->findChild<QToolButton*>(QStringLiteral("saveDeviceButton"));
        QVERIFY(save && save->isVisible() && !save->isEnabled());
        QList<rufus::Device> devices = testDevices();
        devices[0].partitionTableType = QStringLiteral("gpt");
        devices[0].fileSystemType = QStringLiteral("vfat");
        controller_->setDiscovery(devices, {});
        QVERIFY(save->isEnabled() && controller_->canSaveDevice());
        QVERIFY(controller_->selection().imagePath.isEmpty());
        QTest::mouseClick(save, Qt::LeftButton);
        auto* dialog = window_->findChild<QFileDialog*>(QStringLiteral("saveImageDialog"));
        QTRY_VERIFY(dialog && dialog->isVisible());
        QCOMPARE(dialog->nameFilters().size(), 4);
        QCOMPARE(dialog->defaultSuffix(), QStringLiteral("vhdx"));
        const QString ffu = dialog->nameFilters().at(2);
        QVERIFY(ffu.contains(QStringLiteral("*.ffu")));
        dialog->selectNameFilter(ffu);
        QVERIFY(QMetaObject::invokeMethod(dialog, "filterSelected", Q_ARG(QString, ffu)));
        QCOMPARE(dialog->defaultSuffix(), QStringLiteral("ffu"));
        QVERIFY(dialog->selectedFiles().first().endsWith(QStringLiteral(".ffu")));
        const QString iso = dialog->nameFilters().last();
        QVERIFY(iso.contains(QStringLiteral("*.iso")));
        dialog->selectNameFilter(iso);
        QVERIFY(QMetaObject::invokeMethod(dialog, "filterSelected", Q_ARG(QString, iso)));
        QCOMPARE(dialog->defaultSuffix(), QStringLiteral("iso"));
        dialog->reject();
        QVERIFY(!controller_->isBusy());
        devices[0].partitionTableType = QStringLiteral("dos");
        controller_->setDiscovery(devices, {});
        QTest::mouseClick(save, Qt::LeftButton);
        QTRY_VERIFY(dialog->isVisible());
        QCOMPARE(dialog->nameFilters().size(), 3);
        dialog->reject();
        if (previous.isValid()) settings.setValue(QStringLiteral("PreferredSaveImageType"), previous);
        else settings.remove(QStringLiteral("PreferredSaveImageType"));
    }

    void logCanBeSavedAndCleared()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QTest::mouseClick(logButton_, Qt::LeftButton);
        QPlainTextEdit* text = window_->findChild<QPlainTextEdit*>(QStringLiteral("logText"));
        QPushButton* save = window_->findChild<QPushButton*>(QStringLiteral("saveLogButton"));
        QPushButton* clear = window_->findChild<QPushButton*>(QStringLiteral("clearLogButton"));
        QVERIFY(text && save && clear);
        const QByteArray initial = text->toPlainText().toUtf8();
        QString messages;
        for (int index = 0; index < 2300; ++index) messages += QStringLiteral("retained message %1\n").arg(index);
        messages.chop(1);
        controller_->logMessage(messages);
        QVERIFY(!text->toPlainText().contains(QStringLiteral("retained message 0\n")));
        const QByteArray expected = initial + '\n' + messages.toUtf8() + '\n';
        QTest::mouseClick(save, Qt::LeftButton);
        QFileDialog* dialog = window_->findChild<QFileDialog*>(QStringLiteral("saveLogDialog"));
        QVERIFY(dialog && dialog->isVisible());
        dialog->setDirectory(directory.path());
        dialog->selectFile(QStringLiteral("rufus.log"));
        QDialogButtonBox* buttons = dialog->findChild<QDialogButtonBox*>();
        QVERIFY(buttons && buttons->button(QDialogButtonBox::Save));
        QTest::mouseClick(buttons->button(QDialogButtonBox::Save), Qt::LeftButton);
        QFile output(directory.filePath(QStringLiteral("rufus.log")));
        QTRY_VERIFY_WITH_TIMEOUT(output.exists(), uiTimeoutMs);
        QVERIFY(output.open(QIODevice::ReadOnly));
        QCOMPARE(output.readAll(), expected);
        QTest::mouseClick(clear, Qt::LeftButton);
        QVERIFY(text->toPlainText().isEmpty());
    }

    void desktopPreferencesAndStartupSelection()
    {
        controller_->setStartupFileSystem(FS_EXFAT);
        controller_->setBootType(BT_NON_BOOTABLE);
        controller_->setDevices(testDevices());
        QCOMPARE(controller_->selection().fileSystem, int(FS_EXFAT));
        controller_->setFileSystem(FS_FAT32);
        controller_->selectDevice(QStringLiteral("test-usb-b"));
        QCOMPARE(controller_->selection().fileSystem, int(FS_FAT32));
        auto* drive = window_->findChild<QToolButton*>(QStringLiteral("advancedDriveButton"));
        auto* format = window_->findChild<QToolButton*>(QStringLiteral("advancedFormatButton"));
        QTest::mouseClick(drive, Qt::LeftButton); QTest::mouseClick(format, Qt::LeftButton);
        const QString nominal = deviceCombo_->itemText(0);
        QTest::keyClick(window_.get(), Qt::Key_U, Qt::AltModifier);
        QVERIFY(deviceCombo_->itemText(0) != nominal);
        rufus::ApplicationSettings settings;
        QVERIFY(settings.value(QStringLiteral("UseProperSizeUnits"), false).toBool());
        QVERIFY(settings.value(QStringLiteral("ShowAdvancedDriveProperties"), false).toBool());
        QVERIFY(settings.value(QStringLiteral("ShowAdvancedFormatOptions"), false).toBool());
        rufus::SelectionController fresh;
        rufus::MainWindow restored(fresh);
        QVERIFY(!restored.findChild<QWidget*>(QStringLiteral("advancedDriveOptions"))->isHidden());
        QVERIFY(!restored.findChild<QWidget*>(QStringLiteral("advancedFormatOptions"))->isHidden());
        QVERIFY(restored.findChild<QComboBox*>(QStringLiteral("bootCombo"))->findText(QStringLiteral("Syslinux 4.07")) >= 0);
        fresh.setDevices(testDevices());
        QCOMPARE(restored.findChild<QComboBox*>(QStringLiteral("deviceCombo"))->itemText(0), deviceCombo_->itemText(0));
        QTest::keyClick(window_.get(), Qt::Key_R, Qt::AltModifier);
        QVERIFY(!rufus::ApplicationSettings().contains(QStringLiteral("UseProperSizeUnits")));
        QVERIFY(window_->findChild<QWidget*>(QStringLiteral("advancedDriveOptions"))->isVisible());
    }

    void automaticPersistentLogAndShortcuts()
    {
        QTest::keyClick(window_.get(), Qt::Key_L, Qt::ControlModifier);
        auto* dialog = window_->findChild<QDialog*>(QStringLiteral("logWindow"));
        QVERIFY(dialog && dialog->isVisible());
        QTest::keyClick(dialog, Qt::Key_P, Qt::ControlModifier);
        QVERIFY(rufus::ApplicationSettings().value(QStringLiteral("PersistentLog"), false).toBool());
        QTest::keyClick(dialog, Qt::Key_L, Qt::ControlModifier);
        QVERIFY(!dialog->isVisible());
        controller_->logMessage(QStringLiteral("CP59 session one"));
        QVERIFY(window_->close());
        const QString path = QDir(rufus::ApplicationFilesDirectory()).filePath(QStringLiteral("rufus.log"));
        QFile output(path); QVERIFY(output.open(QIODevice::ReadOnly));
        const QByteArray first = output.readAll(); output.close();
        QVERIFY(first.contains("CP59 session one"));
        {
            rufus::SelectionController next;
            rufus::MainWindow second(next);
            const QString visible = second.findChild<QPlainTextEdit*>(QStringLiteral("logText"))->toPlainText();
            QVERIFY(!visible.contains(QStringLiteral("CP59 session one")));
            QVERIFY(visible.contains(QDate::currentDate().toString(QStringLiteral("yyyy.MM.dd"))));
            next.logMessage(QStringLiteral("CP59 session two"));
            QVERIFY(second.close());
        }
        QVERIFY(output.open(QIODevice::ReadOnly));
        const QByteArray appended = output.readAll(); output.close();
        QVERIFY(appended.startsWith(first)); QVERIFY(appended.contains("CP59 session two"));
        { rufus::ApplicationSettings settings; settings.setValue(QStringLiteral("PersistentLog"), false); }
        {
            rufus::SelectionController next;
            rufus::MainWindow third(next);
            next.logMessage(QStringLiteral("CP59 replacement session")); QVERIFY(third.close());
        }
        QVERIFY(output.open(QIODevice::ReadOnly));
        const QByteArray replaced = output.readAll();
        QVERIFY(!replaced.contains("CP59 session one")); QVERIFY(!replaced.contains("CP59 session two"));
        QVERIFY(replaced.contains("CP59 replacement session"));
    }

    void portableWindowUsesWindowsIni()
    {
        window_.reset(); controller_.reset();
        QTemporaryDir portable; QVERIFY(portable.isValid());
        QFile ini(portable.filePath(QStringLiteral("rufus.ini")));
        QVERIFY(ini.open(QIODevice::WriteOnly));
        const QByteArray original = "ExpertMode=1\nDisableFakeDrivesCheck=1\nShowAdvancedDriveProperties=1\nShowAdvancedFormatOptions=1\n";
        QCOMPARE(ini.write(original), original.size()); ini.close();
        QString problem;
        QCOMPARE(rufus::ConfigureApplicationStorage(portable.filePath(QStringLiteral("rufus-linux")), false, problem), 0);
        {
            rufus::SelectionController controller;
            rufus::MainWindow window(controller);
            QVERIFY(window.windowTitle().endsWith(QStringLiteral("(Portable)")));
            QVERIFY(controller.isoWriter().expertMode()); QVERIFY(!controller.detectFakes());
            QVERIFY(!window.findChild<QWidget*>(QStringLiteral("advancedDriveOptions"))->isHidden());
            QVERIFY(!window.findChild<QWidget*>(QStringLiteral("advancedFormatOptions"))->isHidden());
            controller.logMessage(QStringLiteral("portable session"));
            QVERIFY(window.close());
            QFile log(portable.filePath(QStringLiteral("rufus_files/rufus.log")));
            QVERIFY(log.open(QIODevice::ReadOnly)); QVERIFY(log.readAll().contains("portable session"));
        }
        // Restore storage before this temporary portable directory is removed.
        QCOMPARE(rufus::ConfigureApplicationStorage(profile_.filePath(QStringLiteral("rufus-linux")), false, problem), 0);
    }

    void nativeDataFormatControls()
    {
        controller_->setDevices(testDevices()); controller_->setBootType(BT_NON_BOOTABLE);
        auto* filesystems = window_->findChild<QComboBox*>(QStringLiteral("fileSystemCombo"));
        auto* clusters = window_->findChild<QComboBox*>(QStringLiteral("clusterCombo"));
        auto* appearance = window_->findChild<QCheckBox*>(QStringLiteral("extendedLabel"));
        QVERIFY(filesystems->findData(FS_UDF) >= 0); QCOMPARE(filesystems->findData(FS_EXT2), -1);
        QTest::mouseClick(window_->findChild<QToolButton*>(QStringLiteral("advancedFormatButton")), Qt::LeftButton);
        for (int filesystem : {FS_UDF, FS_EXT2, FS_EXT3})
        for (auto scheme : {rufus::PartitionScheme::Mbr, rufus::PartitionScheme::Gpt, rufus::PartitionScheme::Sfd}) {
            controller_->setPartitionScheme(scheme); controller_->setFileSystem(filesystem);
            QCOMPARE(controller_->selection().partition, scheme);
            QCOMPARE(controller_->selection().fileSystem, filesystem);
            QCOMPARE(clusters->count(), 1); QCOMPARE(clusters->currentText(), QStringLiteral("Default"));
            QCOMPARE(appearance->isEnabled(), filesystem == FS_UDF);
        }
    }

    void layoutFitsLargeFonts()
    {
        QFont font = window_->font();
        font.setPointSize(18);
        window_->setFont(font);
        controller_->setDevices(testDevices());
        chooseFirstDevice();
        window_->resize(window_->sizeHint());
        QCoreApplication::processEvents();
        QWidget* content = window_->centralWidget();
        const QList<QWidget*> controls = {
            deviceCombo_, bootCombo_, selectButton_, startButton_, logButton_
        };
        for (QWidget* control : controls)
        {
            const QRect bounds(control->mapTo(content, QPoint(0, 0)), control->size());
            QVERIFY2(content->rect().contains(bounds), qPrintable(control->objectName()));
            QTRY_VERIFY2_WITH_TIMEOUT(control->height() >= control->minimumSizeHint().height(),
                     qPrintable(QStringLiteral("%1: height %2, minimum %3, hint %4")
                         .arg(control->objectName()).arg(control->height())
                         .arg(control->minimumSizeHint().height()).arg(control->sizeHint().height())), 1000);
        }
    }
};

QTEST_MAIN(TestRufusWindow)
#include "test_rufus_window.moc"
