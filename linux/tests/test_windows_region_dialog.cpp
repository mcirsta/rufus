// SPDX-License-Identifier: GPL-3.0-or-later
#include "qt/windows_region_dialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>
#include <cerrno>

namespace
{
rufus::WindowsRegionOptions validOptions()
{
    return {QStringLiteral("en-US"), QStringLiteral("ro-RO"), QStringLiteral("ro-RO"),
        QStringLiteral("en-US"), QString(), QStringLiteral("GTB Standard Time")};
}
}

class TestWindowsRegionDialog : public QObject
{
    Q_OBJECT
    QLocale originalLocale_;

    QComboBox* field(rufus::WindowsRegionDialog& dialog, const char* name)
    {
        return dialog.findChild<QComboBox*>(QString::fromLatin1(name));
    }

    QPushButton* ok(rufus::WindowsRegionDialog& dialog)
    {
        return dialog.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok);
    }

private slots:
    void cleanup()
    {
        QLocale::setDefault(originalLocale_);
    }

    void noHostDefaultsOrImplicitAcceptance()
    {
        QLocale::setDefault(QLocale(QStringLiteral("ro_RO")));
        rufus::WindowsRegionDialog dialog;
        const auto options = dialog.options();
        QVERIFY(options.userLocale.isEmpty());
        QVERIFY(options.systemLocale.isEmpty());
        QVERIFY(options.inputLocale.isEmpty());
        QVERIFY(options.uiLanguage.isEmpty());
        QVERIFY(options.uiLanguageFallback.isEmpty());
        QVERIFY(options.timezone.isEmpty());
        QVERIFY(!ok(dialog)->isEnabled());
        dialog.accept();
        QCOMPARE(dialog.result(), int(QDialog::Rejected));
    }

    void windowsTimeZoneAndExplicitValues()
    {
        const auto initial = validOptions();
        rufus::WindowsRegionDialog dialog(initial);
        QVERIFY(ok(dialog)->isEnabled());
        auto* zone = field(dialog, "windowsTimezone");
        const int bucharest = zone->findText(QStringLiteral("GTB Standard Time (Europe/Bucharest)"));
        QCOMPARE(zone->currentIndex(), bucharest);
        QVERIFY(bucharest >= 0);
        QCOMPARE(dialog.options().timezone, QStringLiteral("GTB Standard Time"));
        QCOMPARE(zone->toolTip(), zone->currentText()); // Full label remains available in a narrow field.
        QStringList windowsZones;
        for (int row = 0; row < zone->count(); ++row)
            windowsZones.append(zone->itemData(row).toString());
        QCOMPARE(windowsZones.removeDuplicates(), 0);
        const int pacific = zone->findText(QStringLiteral("Pacific Standard Time (America/Los_Angeles)"));
        QVERIFY(pacific >= 0);
        zone->setCurrentIndex(pacific);
        QCOMPARE(dialog.options().timezone, QStringLiteral("Pacific Standard Time"));
        QCOMPARE(zone->toolTip(), zone->currentText());
        rufus::WindowsRegionDialog reopened(dialog.options());
        QCOMPARE(field(reopened, "windowsTimezone")->currentText(), zone->currentText());
        auto* user = field(dialog, "windowsUserLocale");
        const int english = user->findData(QStringLiteral("en-GB"));
        QVERIFY(english >= 0);
        user->setCurrentIndex(english);
        QCOMPARE(dialog.options().userLocale, QStringLiteral("en-GB"));
        QCOMPARE(dialog.options().systemLocale, initial.systemLocale);
        QCOMPARE(dialog.options().inputLocale, initial.inputLocale);
        QCOMPARE(dialog.options().uiLanguage, initial.uiLanguage);
        QCOMPARE(initial.userLocale, QStringLiteral("ro-RO")); // Caller-owned state was not changed.
        QTest::mouseClick(ok(dialog), Qt::LeftButton);
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
    }

    void tagsAndKeyboardLayouts_data()
    {
        QTest::addColumn<QString>("input");
        QTest::addColumn<QString>("language");
        QTest::addColumn<bool>("valid");
        QTest::newRow("default-keyboard") << QStringLiteral("ro-RO") << QStringLiteral("en-US") << true;
        QTest::newRow("US-international") << QStringLiteral("0409:00020409") << QStringLiteral("en-US") << true;
        QTest::newRow("multiple-inputs") << QStringLiteral("en-US;0409:00010409;fr-FR") << QStringLiteral("en-US") << true;
        QTest::newRow("full-script-tag") << QStringLiteral("zh-CN") << QStringLiteral("zh-Hans-CN") << true;
        QTest::newRow("numeric-region") << QStringLiteral("es-419") << QStringLiteral("es-MX") << true;
        QTest::newRow("empty-input") << QString() << QStringLiteral("en-US") << false;
        QTest::newRow("missing-layout") << QStringLiteral("0409:") << QStringLiteral("en-US") << false;
        QTest::newRow("empty-list-entry") << QStringLiteral("en-US;;ro-RO") << QStringLiteral("en-US") << false;
        QTest::newRow("trailing-entry") << QStringLiteral("en-US;") << QStringLiteral("en-US") << false;
        QTest::newRow("linux-layout") << QStringLiteral("us(intl)") << QStringLiteral("en-US") << false;
        QTest::newRow("xml-language") << QStringLiteral("en-US") << QStringLiteral("en-US</UILanguage>") << false;
        QTest::newRow("xml-input") << QStringLiteral("en-US&ro-RO") << QStringLiteral("en-US") << false;
        QTest::newRow("unicode-hyphen") << QStringLiteral("en-US") << QStringLiteral("en–US") << false;
        QTest::newRow("embedded-nul") << QString::fromLatin1("en\0-US", 6) << QStringLiteral("en-US") << false;
        QTest::newRow("long-input") << QString(257, QLatin1Char('a')) << QStringLiteral("en-US") << false;
    }

    void tagsAndKeyboardLayouts()
    {
        QFETCH(QString, input);
        QFETCH(QString, language);
        QFETCH(bool, valid);
        auto options = validOptions();
        options.inputLocale = input;
        options.uiLanguage = language;
        QCOMPARE(rufus::CheckWindowsRegionOptions(options), valid ? 0 : EINVAL);
    }

    void invalidFieldsCannotBeAccepted()
    {
        const char* fields[] = {"windowsInputLocale", "windowsSystemLocale", "windowsUserLocale",
            "windowsDisplayLanguage", "windowsFallbackLanguage"};
        for (const char* name : fields)
        {
            rufus::WindowsRegionDialog dialog(validOptions());
            field(dialog, name)->setEditText(QStringLiteral("<bad>"));
            QVERIFY(!ok(dialog)->isEnabled());
            dialog.accept();
            QCOMPARE(dialog.result(), int(QDialog::Rejected));
        }
        auto options = validOptions();
        options.timezone = QStringLiteral("Europe/Bucharest"); // Answer files need the Windows ID.
        QCOMPARE(rufus::CheckWindowsRegionOptions(options), EINVAL);
        options.timezone = QStringLiteral("GTB Standard Time</TimeZone>");
        QCOMPARE(rufus::CheckWindowsRegionOptions(options), EINVAL);
        rufus::WindowsRegionDialog unknown(options);
        QVERIFY(!ok(unknown)->isEnabled());
    }

    void preserveTagsAndCancel()
    {
        rufus::WindowsRegionDialog dialog(validOptions());
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        field(dialog, "windowsDisplayLanguage")->setEditText(QStringLiteral("zh-Hans-CN"));
        field(dialog, "windowsFallbackLanguage")->setEditText(QStringLiteral("en-US"));
        field(dialog, "windowsInputLocale")->setEditText(QStringLiteral(" 0409:00020409;ro-RO "));
        QCOMPARE(dialog.options().uiLanguage, QStringLiteral("zh-Hans-CN"));
        QCOMPARE(dialog.options().uiLanguageFallback, QStringLiteral("en-US"));
        QCOMPARE(dialog.options().inputLocale, QStringLiteral("0409:00020409;ro-RO"));
        QVERIFY(ok(dialog)->isEnabled());
        QTest::keyClick(&dialog, Qt::Key_Escape);
        QCOMPARE(dialog.result(), int(QDialog::Rejected));
    }

    void keyboardAndLargeFontLayout()
    {
        rufus::WindowsRegionDialog dialog(validOptions());
        QFont large = dialog.font();
        large.setPointSize(18);
        dialog.setFont(large);
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        for (QComboBox* combo : dialog.findChildren<QComboBox*>())
        {
            QVERIFY(dialog.rect().contains(combo->geometry()));
            QVERIFY(combo->width() >= combo->minimumSizeHint().width());
        }
        QTemporaryDir output;
        QVERIFY(dialog.grab().save(output.filePath(QStringLiteral("windows-region.png"))));
        const QString screenshot = qEnvironmentVariable("RUFUS_TEST_REGION_SCREENSHOT");
        if (!screenshot.isEmpty())
            QVERIFY(dialog.grab().save(screenshot));
        auto* input = field(dialog, "windowsInputLocale")->lineEdit();
        input->setFocus();
        input->selectAll();
        QTest::keyClicks(input, "en-GB");
        QCOMPARE(dialog.options().inputLocale, QStringLiteral("en-GB"));
        QTest::keyClick(input, Qt::Key_Return);
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
    }
};

QTEST_MAIN(TestWindowsRegionDialog)
#include "test_windows_region_dialog.moc"
