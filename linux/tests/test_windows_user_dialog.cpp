// SPDX-License-Identifier: GPL-3.0-or-later
#include "qt/windows_user_dialog.h"
#include "qt/windows_image_dialog.h"
#include "qt/windows_region_dialog.h"
#include "qt/windows_silent_dialog.h"
#include "platform/iso_write_check.h"
#include "platform/windows_install_options.h"
#include "unattend.h"
#include <wimlib.h>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

class TestWindowsUserDialog : public QObject
{
    Q_OBJECT
    RUFUS_IMG_REPORT report_ = {};

    QCheckBox* choice(rufus::WindowsUserDialog& dialog, int flag)
    {
        return dialog.findChild<QCheckBox*>(QStringLiteral("windowsOption%1").arg(flag));
    }

    rufus::IsoWriteCheck metadata() const
    {
        rufus::IsoWriteCheck checked;
        checked.analysis.report = report_;
        checked.windowsInstallPath = QStringLiteral("/sources/install.esd");
        checked.windowsInstall.editions = {{1, QStringLiteral("Same label")}, {4, QStringLiteral("Same label")}};
        return checked;
    }

    void fillRegion(rufus::WindowsRegionDialog& editor)
    {
        const char* const names[] = {"windowsDisplayLanguage", "windowsUserLocale", "windowsSystemLocale", "windowsInputLocale"};
        const QStringList values = {QStringLiteral("en-US"), QStringLiteral("en-GB"),
            QStringLiteral("ro-RO"), QStringLiteral("0409:00020409;ro-RO")};
        for (int i = 0; i < values.size(); ++i)
            editor.findChild<QComboBox*>(QString::fromLatin1(names[i]))->setEditText(values[i]);
        auto* timezone = editor.findChild<QComboBox*>(QStringLiteral("windowsTimezone"));
        timezone->setCurrentIndex(timezone->findData(QStringLiteral("GTB Standard Time")));
    }

    void prepareSilent(rufus::WindowsUserDialog& dialog)
    {
        dialog.setInstallOptions(metadata());
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        choice(dialog, UNATTEND_SET_USER)->setChecked(true);
        choice(dialog, UNATTEND_NO_DATA_COLLECTION)->setChecked(true);
        dialog.findChild<QLineEdit*>()->setText(QStringLiteral("TestUser"));
        QTest::mouseClick(dialog.findChild<QPushButton*>(QStringLiteral("windowsRegionButton")), Qt::LeftButton);
        auto* region = dialog.findChild<rufus::WindowsRegionDialog*>();
        QVERIFY(region && region->isVisible());
        fillRegion(*region);
        region->accept();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        auto* silent = choice(dialog, UNATTEND_SILENT_INSTALL);
        QVERIFY(silent->isVisible() && silent->isEnabled());
        QVERIFY(!silent->isChecked());
        silent->setChecked(true);
        auto* editions = dialog.findChild<QComboBox*>(QStringLiteral("windowsEdition"));
        QCOMPARE(editions->currentIndex(), -1);
        editions->setCurrentIndex(1);
    }

private slots:
    void init()
    {
        report_ = {};
        report_.has_bootmgr_efi = true;
        report_.win_version.major = 11;
        report_.win_version.build = 26200;
        report_.has_efi = 4;
        report_.wininst_index = 1;
    }

    void windowsToGoChoices()
    {
        rufus::WindowsUserDialog dialog(report_, UNATTEND_DEFAULT_SELECTION_MASK, true, nullptr, true);
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        QVERIFY(choice(dialog, UNATTEND_OFFLINE_INTERNAL_DRIVES)->isEnabled());
        QCOMPARE(dialog.selectedFlags(), UNATTEND_OFFLINE_INTERNAL_DRIVES | UNATTEND_NO_ONLINE_ACCOUNT);
        QVERIFY(!choice(dialog, UNATTEND_SECUREBOOT_TPM_MINRAM));
        QVERIFY(!choice(dialog, UNATTEND_SILENT_INSTALL));
        QVERIFY(!choice(dialog, UNATTEND_DISABLE_BITLOCKER));
        QVERIFY(!choice(dialog, UNATTEND_DUPLICATE_LOCALE)->isEnabled());
        QVERIFY(choice(dialog, UNATTEND_DUPLICATE_LOCALE)->toolTip().contains(QStringLiteral("Windows host")));
        QVERIFY(choice(dialog, UNATTEND_FORCE_S_MODE)->isEnabled());
        dialog.accept();
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
        QCOMPARE(dialog.options().windowsFlags, UNATTEND_OFFLINE_INTERNAL_DRIVES | UNATTEND_NO_ONLINE_ACCOUNT);
        rufus::WindowsUserDialog sMode(report_, UNATTEND_FORCE_S_MODE, true, nullptr, true);
        QCOMPARE(sMode.selectedFlags(), UNATTEND_FORCE_S_MODE);
        sMode.accept();
        QCOMPARE(sMode.options().windowsFlags, UNATTEND_FORCE_S_MODE);
    }

    void windowsImageChoices()
    {
        rufus::WindowsImageDialog editions(QList<rufus::WindowsEdition>{{1, QStringLiteral("Same label")}, {4, QStringLiteral("Same label")}});
        QCOMPARE(editions.selectedEdition(), 1);
        editions.findChild<QComboBox*>()->setCurrentIndex(1);
        QCOMPARE(editions.selectedEdition(), 4);
        rufus::WindowsImageDialog archives(QStringList{QStringLiteral("/sources/install.wim"), QStringLiteral("/x64/sources/install.esd")});
        archives.findChild<QComboBox*>()->setCurrentIndex(1);
        QCOMPARE(archives.selectedArchive(), QStringLiteral("/x64/sources/install.esd"));
    }

    void originalOrderAndAvailableDefaults()
    {
        rufus::WindowsUserDialog dialog(report_, UNATTEND_DEFAULT_SELECTION_MASK);
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        const int expected[] = {UNATTEND_SECUREBOOT_TPM_MINRAM, UNATTEND_NO_ONLINE_ACCOUNT,
            UNATTEND_SET_USER, UNATTEND_DUPLICATE_LOCALE, UNATTEND_NO_DATA_COLLECTION,
            UNATTEND_SILENT_INSTALL, UNATTEND_DISABLE_BITLOCKER, UNATTEND_QOL_ENHANCEMENTS,
            UNATTEND_USE_MS2023_BOOTLOADERS, UNATTEND_APPLY_SKUSIPOLICY};
        int previous = -1;
        for (int flag : expected)
        {
            QCheckBox* box = choice(dialog, flag);
            QVERIFY(box);
            if (flag == UNATTEND_SILENT_INSTALL)
            {
                QVERIFY(box->isHidden()); // Win32 hides this until all prerequisites are supplied.
                continue;
            }
            QVERIFY(box->isVisible());
            QVERIFY(box->geometry().top() > previous);
            previous = box->geometry().top();
        }
        QCOMPARE(dialog.windowTitle(), QStringLiteral("Windows User Experience"));
        QCOMPARE(dialog.selectedFlags(), UNATTEND_SECUREBOOT_TPM_MINRAM | UNATTEND_NO_ONLINE_ACCOUNT);
        QVERIFY(!choice(dialog, UNATTEND_DUPLICATE_LOCALE)->isEnabled());
        QVERIFY(choice(dialog, UNATTEND_DUPLICATE_LOCALE)->toolTip().contains(QStringLiteral("Windows host")));
        QVERIFY(choice(dialog, UNATTEND_SECUREBOOT_TPM_MINRAM)->isEnabled());
        QVERIFY(choice(dialog, UNATTEND_SECUREBOOT_TPM_MINRAM)->isChecked());
        QVERIFY(choice(dialog, UNATTEND_USE_MS2023_BOOTLOADERS)->isEnabled());
        QVERIFY(!choice(dialog, UNATTEND_USE_MS2023_BOOTLOADERS)->isChecked());
        QVERIFY(choice(dialog, UNATTEND_USE_MS2023_BOOTLOADERS)->text().contains(QStringLiteral("Requires a compatible target PC")));
        QCheckBox* policy = choice(dialog, UNATTEND_APPLY_SKUSIPOLICY);
        QVERIFY(policy->isEnabled());
        QVERIFY(!policy->isChecked());
        QCOMPARE(policy->text(), QStringLiteral("Apply SkuSiPolicy.p7b on installation (See KB5042562)"));
        QVERIFY(policy->toolTip().contains(QStringLiteral("preventing standard Windows media from booting")));
        QVERIFY(!choice(dialog, UNATTEND_SILENT_INSTALL)->isEnabled());
        QVERIFY(!choice(dialog, UNATTEND_FORCE_S_MODE)); // Expert mode is separate upstream too.
        auto* edit = dialog.findChild<QLineEdit*>(QStringLiteral("windowsUsername"));
        QVERIFY(edit && edit->isEnabled() && !edit->text().isEmpty());
    }

    void windows10AndOlderWindows11()
    {
        report_.win_version.major = 10;
        report_.win_version.build = 19045;
        rufus::WindowsUserDialog ten(report_, UNATTEND_FULL_MASK);
        QCOMPARE(ten.findChildren<QCheckBox*>().size(), 3);
        QCOMPARE(ten.selectedFlags(), UNATTEND_SET_USER | UNATTEND_NO_DATA_COLLECTION);
        QVERIFY(!ten.findChild<QComboBox*>(QStringLiteral("windowsEdition")));
        report_.win_version.major = 11;
        report_.win_version.build = 22000;
        rufus::WindowsUserDialog eleven(report_, 0);
        QVERIFY(!choice(eleven, UNATTEND_NO_ONLINE_ACCOUNT));
        QVERIFY(!choice(eleven, UNATTEND_USE_MS2023_BOOTLOADERS));
        QVERIFY(!choice(eleven, UNATTEND_APPLY_SKUSIPOLICY));
    }

    void expertModeChoiceMatchesRufus()
    {
        rufus::WindowsUserDialog normal(report_, UNATTEND_FORCE_S_MODE);
        QVERIFY(!choice(normal, UNATTEND_FORCE_S_MODE));
        QCOMPARE(normal.selectedFlags(), 0);
        rufus::WindowsUserDialog expert(report_, UNATTEND_DEFAULT_SELECTION_MASK, true);
        expert.show();
        QVERIFY(QTest::qWaitForWindowExposed(&expert));
        auto* sMode = choice(expert, UNATTEND_FORCE_S_MODE);
        QVERIFY(sMode && sMode->isVisible() && sMode->isEnabled() && !sMode->isChecked());
        QVERIFY(sMode->geometry().top() > choice(expert, UNATTEND_APPLY_SKUSIPOLICY)->geometry().top());
        QCOMPARE(sMode->text(), QStringLiteral("Restrict Windows to S-Mode (INCOMPATIBLE with online account bypass)"));
        QVERIFY(sMode->toolTip().contains(QStringLiteral("even after you completely erase and reinstall Windows")));
        sMode->setChecked(true);
        // Win32 warns about this combination but does not change either choice.
        QVERIFY(choice(expert, UNATTEND_NO_ONLINE_ACCOUNT)->isChecked());
        expert.accept();
        QCOMPARE(expert.result(), int(QDialog::Accepted));
        QCOMPARE(expert.options().windowsFlags, UNATTEND_SECUREBOOT_TPM_MINRAM |
            UNATTEND_NO_ONLINE_ACCOUNT | UNATTEND_FORCE_S_MODE);
        report_.win_version.major = 10;
        report_.win_version.build = 19045;
        rufus::WindowsUserDialog ten(report_, UNATTEND_FORCE_S_MODE, true);
        QCOMPARE(ten.selectedFlags(), UNATTEND_FORCE_S_MODE);
        report_.has_panther_unattend = true;
        rufus::WindowsUserDialog existingAnswer(report_, UNATTEND_FORCE_S_MODE, true);
        QVERIFY(!choice(existingAnswer, UNATTEND_FORCE_S_MODE));
        QCOMPARE(existingAnswer.selectedFlags(), 0);
    }

    void usernameNormalizationAndValidation()
    {
        rufus::WindowsUserDialog dialog(report_, UNATTEND_SET_USER);
        auto* edit = dialog.findChild<QLineEdit*>();
        auto* actions = dialog.findChild<QDialogButtonBox*>();
        QVERIFY(edit && actions);
        edit->setText(QStringLiteral("  Rufus.Test&User  "));
        QCOMPARE(dialog.username(), QStringLiteral("Rufus_Test_User"));
        QVERIFY(actions->button(QDialogButtonBox::Ok)->isEnabled());
        edit->setText(QString(70, QChar(0x0219)));
        QVERIFY(!actions->button(QDialogButtonBox::Ok)->isEnabled());
        choice(dialog, UNATTEND_SET_USER)->setChecked(false);
        QVERIFY(actions->button(QDialogButtonBox::Ok)->isEnabled());
        QVERIFY(dialog.username().isEmpty());
        choice(dialog, UNATTEND_SET_USER)->setChecked(true);
        edit->setText(QStringLiteral("Măriuș"));
        QCOMPARE(dialog.username(), QStringLiteral("Măriuș"));
        QVERIFY(actions->button(QDialogButtonBox::Ok)->isEnabled());
    }

    void editionIdentityAndReadFailure()
    {
        rufus::WindowsUserDialog dialog(report_, UNATTEND_FULL_MASK);
        auto checked = metadata();
        checked.windowsInstall.editions = {{1, QStringLiteral("Same label")}, {4, QStringLiteral("Same label")},
            {7, QStringLiteral("<b>Custom Windows</b>")}};
        dialog.setInstallOptions(checked);
        auto* editions = dialog.findChild<QComboBox*>(QStringLiteral("windowsEdition"));
        QVERIFY(editions);
        QCOMPARE(editions->count(), 3);
        QCOMPARE(editions->itemData(1).toInt(), 4);
        QCOMPARE(editions->itemData(2).toInt(), 7);
        QCOMPARE(editions->itemText(2), QStringLiteral("<b>Custom Windows</b>"));
        QCOMPARE(editions->currentIndex(), -1);
        QVERIFY(!editions->isEnabled());
        QVERIFY(!(dialog.selectedFlags() & UNATTEND_SILENT_INSTALL));
        checked.windowsInstall.editions.clear();
        QCOMPARE(editions->count(), 3); // The dialog owns its labels/indexes.
        const int flags = dialog.selectedFlags();
        checked.windowsInstallError = WIMLIB_ERR_READ;
        dialog.setInstallOptions(checked);
        QCOMPARE(editions->count(), 0);
        QVERIFY(editions->toolTip().contains(QStringLiteral("could not be read")));
        QCOMPARE(dialog.selectedFlags(), flags); // Ordinary options still work.
    }

    void silentPrerequisitesAndAcknowledgements()
    {
        rufus::WindowsUserDialog dialog(report_, 0);
        QSignalSpy accepted(&dialog, &QDialog::accepted);
        prepareSilent(dialog);
        auto* actions = dialog.findChild<QDialogButtonBox*>();
        auto* name = dialog.findChild<QLineEdit*>();
        name->setText(QStringLiteral("Administrator"));
        QVERIFY(!actions->button(QDialogButtonBox::Ok)->isEnabled());
        dialog.accept();
        QVERIFY(!dialog.findChild<rufus::WindowsSilentDialog*>());
        name->setText(QChar(0x00a0) + QStringLiteral("TestUser") + QChar(0x00a0));
        QVERIFY(!actions->button(QDialogButtonBox::Ok)->isEnabled());
        name->setText(QStringLiteral("  Rufus.Test  "));
        auto* editions = dialog.findChild<QComboBox*>(QStringLiteral("windowsEdition"));
        editions->setCurrentIndex(-1);
        QVERIFY(!actions->button(QDialogButtonBox::Ok)->isEnabled());
        editions->setCurrentIndex(1);
        const QString optionsScreenshot = qEnvironmentVariable("RUFUS_TEST_WINDOWS_SILENT_OPTIONS_SCREENSHOT");
        if (!optionsScreenshot.isEmpty())
            QVERIFY(dialog.grab().save(optionsScreenshot));
        QTest::mouseClick(actions->button(QDialogButtonBox::Ok), Qt::LeftButton);
        auto* warning = dialog.findChild<rufus::WindowsSilentDialog*>();
        QVERIFY(warning && warning->isVisible());
        const QString warningScreenshot = qEnvironmentVariable("RUFUS_TEST_WINDOWS_SILENT_CONFIRM_SCREENSHOT");
        if (!warningScreenshot.isEmpty())
            QVERIFY(warning->grab().save(warningScreenshot));
        const auto boxes = warning->findChildren<QCheckBox*>();
        QCOMPARE(boxes.size(), 3);
        QVERIFY(!warning->allConfirmed());
        warning->accept();
        QCOMPARE(accepted.size(), 0);
        boxes[0]->setChecked(true);
        boxes[1]->setChecked(true);
        warning->accept();
        QCOMPARE(accepted.size(), 0);
        boxes[2]->setChecked(true);
        QVERIFY(warning->allConfirmed());
        QTest::mouseClick(warning->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok), Qt::LeftButton);
        QCOMPARE(accepted.size(), 1);
        const auto& options = dialog.options();
        QCOMPARE(options.windowsFlags, UNATTEND_SILENT_INSTALL | UNATTEND_SET_USER |
            UNATTEND_NO_DATA_COLLECTION | UNATTEND_DUPLICATE_LOCALE);
        QCOMPARE(options.windowsUsername, QStringLiteral("Rufus_Test"));
        QCOMPARE(options.windowsEdition, 4); // Native index, not dropdown position or label.
        QCOMPARE(options.windowsInstallPath, QStringLiteral("/sources/install.esd"));
        QCOMPARE(options.windowsRegion.inputLocale, QStringLiteral("0409:00020409;ro-RO"));
        QCOMPARE(options.windowsRegion.timezone, QStringLiteral("GTB Standard Time"));
        QVERIFY(options.windowsSilentConfirmed);
        QVERIFY(!choice(dialog, UNATTEND_DUPLICATE_LOCALE)->isChecked());
        QVERIFY(!choice(dialog, UNATTEND_DUPLICATE_LOCALE)->isEnabled());
    }

    void warningCancellationAndChangedSelection()
    {
        rufus::WindowsUserDialog dialog(report_, 0);
        QSignalSpy accepted(&dialog, &QDialog::accepted);
        prepareSilent(dialog);
        dialog.accept();
        auto* warning = dialog.findChild<rufus::WindowsSilentDialog*>();
        QVERIFY(warning);
        for (auto* box : warning->findChildren<QCheckBox*>())
            box->setChecked(true);
        QTest::keyClick(warning, Qt::Key_Escape);
        QVERIFY(dialog.isVisible());
        QCOMPARE(accepted.size(), 0);
        QVERIFY(!dialog.options().windowsSilentConfirmed);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        dialog.accept();
        warning = dialog.findChild<rufus::WindowsSilentDialog*>();
        QVERIFY(warning && !warning->allConfirmed());
        for (auto* box : warning->findChildren<QCheckBox*>())
            QVERIFY(!box->isChecked());
        // Even a programmatic change while a warning is open invalidates it.
        dialog.findChild<QComboBox*>(QStringLiteral("windowsEdition"))->setCurrentIndex(0);
        QVERIFY(!warning->isVisible());
        QCOMPARE(accepted.size(), 0);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        dialog.accept();
        warning = dialog.findChild<rufus::WindowsSilentDialog*>();
        QVERIFY(warning && !warning->allConfirmed());
        dialog.reject();
        QVERIFY(!warning->isVisible());
        QCOMPARE(dialog.options().windowsFlags, 0);
    }

    void regionalCancelAndPrerequisiteChanges()
    {
        rufus::WindowsUserDialog dialog(report_, 0);
        prepareSilent(dialog);
        auto* button = dialog.findChild<QPushButton*>(QStringLiteral("windowsRegionButton"));
        QTest::mouseClick(button, Qt::LeftButton);
        auto* editor = dialog.findChild<rufus::WindowsRegionDialog*>();
        QVERIFY(editor);
        QCOMPARE(editor->options().userLocale, QStringLiteral("en-GB"));
        editor->findChild<QComboBox*>(QStringLiteral("windowsUserLocale"))->setEditText(QStringLiteral("de-DE"));
        editor->reject();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QTest::mouseClick(button, Qt::LeftButton);
        editor = dialog.findChild<rufus::WindowsRegionDialog*>();
        QVERIFY(editor);
        QCOMPARE(editor->options().userLocale, QStringLiteral("en-GB"));
        dialog.reject();
        QVERIFY(!editor->isVisible());
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        dialog.show();
        choice(dialog, UNATTEND_NO_DATA_COLLECTION)->setChecked(false);
        QVERIFY(choice(dialog, UNATTEND_SILENT_INSTALL)->isHidden());
        QVERIFY(!(dialog.selectedFlags() & UNATTEND_SILENT_INSTALL));
        choice(dialog, UNATTEND_NO_DATA_COLLECTION)->setChecked(true);
        QVERIFY(choice(dialog, UNATTEND_SILENT_INSTALL)->isVisible());
        QVERIFY(!choice(dialog, UNATTEND_SILENT_INSTALL)->isChecked());
        dialog.accept();
        QVERIFY(!dialog.options().hasWindowsInstallSettings());
    }

    void unavailableSilentImages_data()
    {
        QTest::addColumn<QString>("change");
        for (const char* name : {"multiple", "nonstandard", "read-error", "no-editions", "bad-language"})
            QTest::newRow(name) << QString::fromLatin1(name);
    }

    void unavailableSilentImages()
    {
        QFETCH(QString, change);
        auto checked = metadata();
        if (change == QStringLiteral("multiple")) checked.analysis.report.wininst_index = 2;
        if (change == QStringLiteral("nonstandard")) checked.windowsInstallPath = QStringLiteral("/x64/sources/install.wim");
        if (change == QStringLiteral("read-error")) checked.windowsInstallError = WIMLIB_ERR_READ;
        if (change == QStringLiteral("no-editions")) checked.windowsInstall.editions.clear();
        if (change == QStringLiteral("bad-language")) checked.windowsInstall.uiLanguage = QStringLiteral("bad</xml>");
        rufus::WindowsUserDialog dialog(report_, UNATTEND_NO_DATA_COLLECTION);
        dialog.setInstallOptions(checked);
        QVERIFY(!dialog.findChild<QPushButton*>(QStringLiteral("windowsRegionButton"))->isEnabled());
        QVERIFY(!choice(dialog, UNATTEND_SILENT_INSTALL)->isEnabled());
        dialog.accept();
        QCOMPARE(dialog.options().windowsFlags, UNATTEND_NO_DATA_COLLECTION);
        QVERIFY(!dialog.options().hasWindowsInstallSettings());
    }

    void everyWarningCombination()
    {
        rufus::WindowsSilentDialog warning;
        QSignalSpy accepted(&warning, &QDialog::accepted);
        const auto boxes = warning.findChildren<QCheckBox*>();
        const int all = (1 << boxes.size()) - 1;
        QCOMPARE(all, 7);
        for (int mask = 0; mask <= all; ++mask)
        {
            for (int i = 0; i < boxes.size(); ++i)
                boxes[i]->setChecked(mask & (1 << i));
            const bool complete = mask == all;
            QCOMPARE(warning.allConfirmed(), complete);
            QCOMPARE(warning.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->isEnabled(), complete);
            warning.accept();
            QCOMPARE(accepted.size(), complete ? 1 : 0);
        }
    }

    void keyboardAndLargeFontLayout()
    {
        rufus::WindowsUserDialog dialog(report_, 0, true);
        QFont large = dialog.font();
        large.setPointSize(18);
        dialog.setFont(large);
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        for (QCheckBox* box : dialog.findChildren<QCheckBox*>())
        {
            if (box->isHidden())
                continue;
            QVERIFY(dialog.rect().contains(box->geometry()));
            QVERIFY(box->width() >= box->sizeHint().width());
        }
        QCheckBox* privacy = choice(dialog, UNATTEND_NO_DATA_COLLECTION);
        privacy->setFocus();
        QTest::keyClick(privacy, Qt::Key_Space);
        QCOMPARE(dialog.selectedFlags(), UNATTEND_NO_DATA_COLLECTION);
        QTemporaryDir output;
        QVERIFY(dialog.grab().save(output.filePath(QStringLiteral("windows-options.png"))));
        const QString screenshot = qEnvironmentVariable("RUFUS_TEST_WINDOWS_OPTIONS_SCREENSHOT");
        if (!screenshot.isEmpty())
            QVERIFY(dialog.grab().save(screenshot));
        QTest::keyClick(&dialog, Qt::Key_Escape);
        QCOMPARE(dialog.result(), int(QDialog::Rejected));
        rufus::WindowsSilentDialog warning;
        warning.setFont(large);
        warning.show();
        QVERIFY(QTest::qWaitForWindowExposed(&warning));
        for (auto* box : warning.findChildren<QCheckBox*>())
        {
            QVERIFY(warning.rect().contains(box->geometry()));
            QVERIFY(box->width() >= box->sizeHint().width());
        }
        const QString warningScreenshot = qEnvironmentVariable("RUFUS_TEST_WINDOWS_SILENT_SCREENSHOT");
        if (!warningScreenshot.isEmpty())
            QVERIFY(warning.grab().save(warningScreenshot));
    }
};

QTEST_MAIN(TestWindowsUserDialog)
#include "test_windows_user_dialog.moc"
