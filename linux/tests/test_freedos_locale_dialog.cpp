// SPDX-License-Identifier: GPL-3.0-or-later
#include "qt/freedos_locale_dialog.h"
#include <QComboBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QSet>
#include <QTest>
#include <cstdio>
#include <cstdlib>
#include <limits>

class TestFreeDosLocaleDialog : public QObject
{
    Q_OBJECT
    QLocale originalLocale_;

    QComboBox* field(rufus::FreeDosLocaleDialog& dialog, const char* name)
    {
        return dialog.findChild<QComboBox*>(QString::fromLatin1(name));
    }

    QPushButton* ok(rufus::FreeDosLocaleDialog& dialog)
    {
        return dialog.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok);
    }

private slots:
    void cleanup()
    {
        QLocale::setDefault(originalLocale_);
    }

    void defaultDoesNotUseHostLocale()
    {
        QLocale::setDefault(QLocale(QStringLiteral("ro_RO")));
        rufus::FreeDosLocaleDialog dialog;
        QCOMPARE(QByteArray(dialog.selection().keyboard), QByteArray("us"));
        QCOMPARE(dialog.selection().codepage, uint32_t(437));
        QVERIFY(!dialog.selection().use_menu);
        QVERIFY(ok(dialog)->isEnabled());
        QVERIFY(!field(dialog, "freeDosKeyboard")->isEditable());
        QVERIFY(!field(dialog, "freeDosCodepage")->isEditable());
        QCOMPARE(dialog.result(), int(QDialog::Rejected)); // Not implicitly accepted.
    }

    void sharedCatalogs()
    {
        rufus::FreeDosLocaleDialog dialog;
        auto* keyboard = field(dialog, "freeDosKeyboard");
        auto* codepage = field(dialog, "freeDosCodepage");
        QSet<QByteArray> keys;
        for (int row = 0; row < keyboard->count(); row++)
        {
            const QByteArray id = keyboard->itemData(row).toString().toLatin1();
            QVERIFY(!keys.contains(id));
            keys.insert(id);
            const DOS_LOCALE locale = SelectDOSLocale(id.constData(), 437, true);
            QVERIFY(!locale.keyboard_fallback);
            QVERIFY(keyboard->itemText(row).contains(QString::fromUtf8(locale.keyboard_name)));
        }
        QSet<uint32_t> pages;
        for (int row = 0; row < codepage->count(); row++)
        {
            const uint32_t cp = codepage->itemData(row).toUInt();
            QVERIFY(!pages.contains(cp));
            pages.insert(cp);
            const DOS_LOCALE locale = SelectDOSLocale("us", cp, true);
            QVERIFY(!locale.codepage_fallback);
            QCOMPARE(locale.codepage, cp);
        }
        for (size_t index = 0; const char* key = GetFreeDOSKeyboard(index); index++)
            QVERIFY(keys.remove(key));
        for (size_t index = 0; const uint32_t cp = GetFreeDOSCodepage(index); index++)
            QVERIFY(pages.remove(cp));
        QVERIFY(keys.isEmpty() && pages.isEmpty());
        QVERIFY(GetFreeDOSKeyboard(std::numeric_limits<size_t>::max()) == nullptr);
        QCOMPARE(GetFreeDOSCodepage(std::numeric_limits<size_t>::max()), uint32_t(0));
        QVERIFY(keyboard->findData(QStringLiteral("sk")) >= 0);
        QCOMPARE(keyboard->findData(QStringLiteral("sl")), -1); // MS-DOS ID, not the FreeDOS entry.
        QCOMPARE(keyboard->findData(QStringLiteral("ch")), -1); // No bundled keyboard driver.
        QCOMPARE(codepage->findData(65001u), -1);
        QCOMPARE(codepage->findData(932u), -1); // Named code page, but no bundled EGA payload.
        QCOMPARE(codepage->findData(850u), -1); // Show final 858, not a silently upgraded choice.
        QVERIFY(codepage->findData(858u) >= 0);
    }

    void explicitChoice_data()
    {
        QTest::addColumn<QString>("keyboard");
        QTest::addColumn<quint32>("requested");
        QTest::addColumn<quint32>("actual");
        QTest::newRow("US") << QStringLiteral("us") << quint32(437) << quint32(437);
        QTest::newRow("Romanian") << QStringLiteral("ro") << quint32(852) << quint32(852);
        QTest::newRow("Dvorak") << QStringLiteral("dv") << quint32(437) << quint32(437);
        QTest::newRow("missing-friendly-label") << QStringLiteral("co") << quint32(437) << quint32(437);
        QTest::newRow("original-upgrade") << QStringLiteral("fr") << quint32(850) << quint32(858);
        QTest::newRow("EGA12") << QStringLiteral("la") << quint32(30003) << quint32(30003);
    }

    void explicitChoice()
    {
        QFETCH(QString, keyboard);
        QFETCH(quint32, requested);
        QFETCH(quint32, actual);
        DOS_LOCALE retained;
        {
            rufus::FreeDosLocaleDialog dialog(keyboard, requested);
            retained = dialog.selection();
            QCOMPARE(QByteArray(retained.keyboard), keyboard.toLatin1());
            QCOMPARE(retained.codepage, actual);
            QVERIFY(!retained.keyboard_fallback && !retained.codepage_fallback);
            QTest::mouseClick(ok(dialog), Qt::LeftButton);
            QCOMPARE(dialog.result(), int(QDialog::Accepted));
        }
        // Returned strings belong to shared static tables, not temporary Qt data.
        QCOMPARE(QByteArray(retained.keyboard), keyboard.toLatin1());
        rufus::FreeDosLocaleDialog reopened(QString::fromLatin1(retained.keyboard), retained.codepage);
        QCOMPARE(reopened.selection().codepage, actual);
    }

    void invalidChoice_data()
    {
        QTest::addColumn<QString>("keyboard");
        QTest::addColumn<quint32>("codepage");
        QTest::newRow("unknown-keyboard") << QStringLiteral("unknown") << quint32(437);
        QTest::newRow("XKB-not-DOS") << QStringLiteral("us(dvorak)") << quint32(437);
        QTest::newRow("empty-keyboard") << QString() << quint32(437);
        QTest::newRow("UTF8-not-DOS") << QStringLiteral("us") << quint32(65001);
        QTest::newRow("unknown-page") << QStringLiteral("us") << quint32(0);
    }

    void invalidChoice()
    {
        QFETCH(QString, keyboard);
        QFETCH(quint32, codepage);
        rufus::FreeDosLocaleDialog dialog(keyboard, codepage);
        QVERIFY(!dialog.selection().freedos);
        QVERIFY(!ok(dialog)->isEnabled());
        dialog.accept();
        QCOMPARE(dialog.result(), int(QDialog::Rejected));
    }

    void changingKeyboardKeepsExplicitCodepage()
    {
        rufus::FreeDosLocaleDialog dialog(QStringLiteral("ro"), 852);
        auto* keyboard = field(dialog, "freeDosKeyboard");
        keyboard->setCurrentIndex(keyboard->findData(QStringLiteral("pl")));
        QCOMPARE(QByteArray(dialog.selection().keyboard), QByteArray("pl"));
        QCOMPARE(dialog.selection().codepage, uint32_t(852));
        auto* codepage = field(dialog, "freeDosCodepage");
        codepage->setCurrentIndex(codepage->findData(437u));
        keyboard->setCurrentIndex(keyboard->findData(QStringLiteral("us")));
        QVERIFY(!dialog.selection().use_menu);
        QVERIFY(dialog.findChild<QLabel*>(QStringLiteral("freeDosLocaleNote"))->text().contains("will start"));
        keyboard->setCurrentIndex(-1);
        QVERIFY(!ok(dialog)->isEnabled());
        dialog.accept();
        QCOMPARE(dialog.result(), int(QDialog::Rejected));
    }

    void selectedContent()
    {
        rufus::FreeDosLocaleDialog dialog(QStringLiteral("ro"), 852);
        const DOS_LOCALE locale = dialog.selection();
        char* output = nullptr;
        size_t length = 0;
        FILE* stream = open_memstream(&output, &length);
        QVERIFY(stream);
        const bool written = WriteDOSConfig(stream, &locale) && WriteDOSAutoexec(stream, &locale);
        const int closed = fclose(stream);
        const QByteArray data(output, qsizetype(length));
        free(output);
        QVERIFY(written);
        QCOMPARE(closed, 0);
        QVERIFY(data.contains("Romanian keyboard"));
        QVERIFY(data.contains("Central-European codepage [852]"));
        QVERIFY(data.contains("MENUDEFAULT=1,5\r\n"));
        QVERIFY(data.contains("Use US-English keyboard with US-English codepage [437]"));
        QVERIFY(data.contains("keyb ro,,\\locale\\keybrd2.sys\r\n"));
        QVERIFY(data.contains(QByteArray(31, char(0xcd))));
        QVERIFY(!data.contains('\0'));
    }

    void malformedItemCannotBecomeAnImplicitFallback()
    {
        rufus::FreeDosLocaleDialog dialog;
        auto* keyboard = field(dialog, "freeDosKeyboard");
        keyboard->addItem(QStringLiteral("invalid"), QString::fromLatin1("us\0other", 8));
        keyboard->setCurrentIndex(keyboard->count() - 1);
        QVERIFY(!ok(dialog)->isEnabled());
        dialog.accept();
        QCOMPARE(dialog.result(), int(QDialog::Rejected));
    }

    void cancelAndLargeFont()
    {
        rufus::FreeDosLocaleDialog dialog(QStringLiteral("ro"), 852);
        QFont font = dialog.font();
        font.setPointSize(18);
        dialog.setFont(font);
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        for (QComboBox* combo : dialog.findChildren<QComboBox*>())
        {
            QVERIFY(dialog.rect().contains(combo->geometry()));
            QVERIFY(combo->width() >= combo->minimumSizeHint().width());
        }
        const QString screenshot = qEnvironmentVariable("RUFUS_TEST_FREEDOS_SCREENSHOT");
        if (!screenshot.isEmpty())
            QVERIFY(dialog.grab().save(screenshot));
        QTest::keyClick(&dialog, Qt::Key_Escape);
        QCOMPARE(dialog.result(), int(QDialog::Rejected));
    }
};

QTEST_MAIN(TestFreeDosLocaleDialog)
#include "test_freedos_locale_dialog.moc"
