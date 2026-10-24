// SPDX-License-Identifier: GPL-3.0-or-later
#include "qt/application_language.h"
#include "qt/application_storage.h"
#include "qt/startup_options.h"
#include "qt/main_window.h"
#include "qt/selection_controller.h"
#include "qt/image_select_button.h"
#include "locale_text.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>

namespace
{
QByteArray catalogBytes()
{
    QFile file(QStringLiteral(":/rufus/rufus.loc"));
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
QByteArray synthetic(const QByteArray& commands)
{
    return "#" + QByteArray(1020, ' ') + "\r\n" + commands;
}
}

class TestLocalization : public QObject
{
    Q_OBJECT
    QTemporaryDir profile_;
private slots:
    void initTestCase()
    {
        QVERIFY(profile_.isValid()); QString problem;
        QVERIFY(rufus::ConfigureApplicationStorage(profile_.filePath(QStringLiteral("rufus-linux")), true, problem) == 0);
        qputenv("XDG_CACHE_HOME", profile_.filePath(QStringLiteral("cache")).toUtf8());
        QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
        QCOMPARE(rufus::ApplicationLanguages().locales().size(), 38);
        QCOMPARE(rufus::SelectedApplicationLanguage(), QStringLiteral("en-US"));
    }
    void cleanup()
    {
        QString problem; QVERIFY2(rufus::SetApplicationLanguage(QStringLiteral("en-US"), problem), qPrintable(problem));
        QCoreApplication::processEvents();
    }
    void originalCatalog_data()
    {
        QTest::addColumn<QString>("tag");
        for (const auto& locale : rufus::ApplicationLanguages().locales())
            QTest::newRow(qPrintable(locale.tag)) << locale.tag;
    }
    void originalCatalog()
    {
        QFETCH(QString, tag);
        const auto& catalog = rufus::ApplicationLanguages();
        const int index = catalog.selection(tag);
        const auto& locale = catalog.locales()[index];
        QCOMPARE(locale.tag, tag); QVERIFY(!locale.name.isEmpty()); QVERIFY(locale.major > 0);
        for (unsigned lcid : locale.lcids) {
            int firstMatch = 0;
            while (!catalog.locales()[firstMatch].lcids.contains(lcid)) ++firstMatch;
            QCOMPARE(catalog.selection(QString::number(lcid)), firstMatch); // Catalog order resolves the shared Croatian/Serbian LCID.
        }
        const bool rtl = tag == QLatin1String("ar-SA") || tag == QLatin1String("he-IL") || tag == QLatin1String("fa-IR");
        QCOMPARE(locale.rightToLeft, rtl);
        QVERIFY(!catalog.text(index, QStringLiteral("IDD_DIALOG/IDS_DEVICE_TXT")).isEmpty());
        QVERIFY(!catalog.translate(index, QStringLiteral("START")).isEmpty());
        QVERIFY(!catalog.translate(index, QStringLiteral("READY")).isEmpty());
        QVERIFY(catalog.translate(index, QStringLiteral("Using image: %1")).contains(QLatin1String("%1")));
        QVERIFY(catalog.translate(index, QStringLiteral("Show advanced drive properties")).contains(
            catalog.text(index, QStringLiteral("MSG_119"))));
        QVERIFY(!catalog.translate(index, QStringLiteral("Disk or ISO image (Please select)")).contains(QLatin1String("%1")));
        QVERIFY(!catalog.translate(index, QStringLiteral("4 passes (TLC)")).contains(QLatin1String("%2")));
    }
    void nativeBindings_data() { originalCatalog_data(); }
    void nativeBindings()
    {
        QFETCH(QString, tag);
        const auto& catalog = rufus::ApplicationLanguages();
        const int index = catalog.selection(tag);
        struct Binding { const char* source; const char* key; };
        const Binding aliases[] = {
            {"Check now", "IDD_UPDATE_POLICY/IDC_CHECK_NOW"},
            {"Writing master boot record...", "MSG_228"},
            {"Writing partition boot record...", "MSG_229"},
            {"Bad blocks detected", "MSG_010"},
            {"VMware disk detection", "MSG_265"},
            {"File indexing", "MSG_290"},
            {"Checking for UEFI DBX updates…", "MSG_352"},
        };
        for (const auto& binding : aliases)
            QCOMPARE(catalog.translate(index, QString::fromUtf8(binding.source)), catalog.text(index, QString::fromLatin1(binding.key)));
        const QLocale locale(tag);
        QCOMPARE(catalog.translate(index, QStringLiteral("CANCELLED")), locale.toUpper(catalog.text(index, QStringLiteral("MSG_211"))));
        QCOMPARE(catalog.translate(index, QStringLiteral("FAILED")), locale.toUpper(catalog.text(index, QStringLiteral("MSG_212"))));
        for (const QString& noun : {QStringLiteral("Expert Mode"), QStringLiteral("Hard disk detection"),
                QStringLiteral("Dual UEFI/BIOS mode"), QStringLiteral("Use Rufus MBR"),
                QStringLiteral("Extra hashes (SHA512)"), QStringLiteral("Persistent log")}) {
            const QString translated = catalog.translate(index, noun);
            QVERIFY(!translated.isEmpty());
            const QString enabled = catalog.translate(index, QStringLiteral("%1 enabled"));
            const QString disabled = catalog.translate(index, QStringLiteral("%1 disabled"));
            QCOMPARE(catalog.translate(index, noun + QStringLiteral(" enabled")), enabled.isEmpty() ? QString() : enabled.arg(translated));
            QCOMPARE(catalog.translate(index, noun + QStringLiteral(" disabled")), disabled.isEmpty() ? QString() : disabled.arg(translated));
        }
        const Binding progress[] = {
            {"Computing image hashes: %p%", "Computing image checksums: %1"},
            {"Writing image: %p%", "Writing image: %1"},
            {"Zeroing device: %p%", "Zeroing drive: %1"},
            {"Downloading image: %p%", "Downloading: %1"},
        };
        for (const auto& binding : progress) {
            const QString expected = catalog.translate(index, QString::fromLatin1(binding.key));
            QCOMPARE(catalog.translate(index, QString::fromLatin1(binding.source)), expected.isEmpty() ? QString() : expected.arg(QStringLiteral("%p%")));
        }
        const QString enable = catalog.translate(index, QStringLiteral("Use %1 (in the main application window) to enable."));
        QCOMPARE(catalog.translate(index, QStringLiteral("Use Alt+H (in the main application window) to enable.")),
            enable.isEmpty() ? QString() : enable.arg(QStringLiteral("Alt+H")));
        const QString format = catalog.translate(index, QStringLiteral("Formatting (%1)..."));
        for (const QString& filesystem : {QStringLiteral("UDF"), QStringLiteral("exFAT"), QStringLiteral("NTFS")})
            QCOMPARE(catalog.translate(index, QStringLiteral("Formatting ") + filesystem + QStringLiteral("...")),
                format.isEmpty() ? QString() : format.arg(filesystem));
        const QString fat = catalog.translate(index, QStringLiteral("Formatting FAT%1..."));
        const QString ext = catalog.translate(index, QStringLiteral("Formatting ext%1..."));
        QCOMPARE(fat.isEmpty() ? QString() : fat.arg(32), format.isEmpty() ? QString() : format.arg(QStringLiteral("FAT32")));
        QCOMPARE(ext.isEmpty() ? QString() : ext.arg(3), format.isEmpty() ? QString() : format.arg(QStringLiteral("ext3")));
        const QString detected = catalog.translate(index, QStringLiteral("%1 image detected"));
        QCOMPARE(catalog.translate(index, QStringLiteral("ISOHybrid image detected")), detected.isEmpty() ? QString() : detected.arg(QStringLiteral("ISOHybrid")));
        const QString hybrid = QStringLiteral(
            "The image you have selected is an 'ISOHybrid' image. This means it can be written either in "
            "%1 (file copy) mode or %2 (disk image) mode.\n"
            "Rufus recommends using %1 mode, so that you always have full access to the drive after writing it.\n"
            "However, if you encounter issues during boot, you can try writing this image again in %2 mode.\n\n"
            "Please select the mode that you want to use to write this image:");
        const QString original = QString(hybrid).replace(QStringLiteral("using %1 mode"), QStringLiteral("using %3 mode"))
            .replace(QStringLiteral("again in %2 mode"), QStringLiteral("again in %4 mode"));
        const QString expected = catalog.translate(index, original);
        const QString translatedHybrid = catalog.translate(index, hybrid);
        QCOMPARE(translatedHybrid.isEmpty() ? QString() : translatedHybrid.arg(QStringLiteral("ISO"), QStringLiteral("DD")),
            expected.isEmpty() ? QString() : expected.arg(QStringLiteral("ISO"), QStringLiteral("DD"), QStringLiteral("ISO"), QStringLiteral("DD")));
        const QString daily = catalog.translate(index, QStringLiteral("%1 (Default)"));
        QCOMPARE(catalog.translate(index, QStringLiteral("Daily (Default)")), daily.isEmpty() ? QString() : daily.arg(catalog.text(index, QStringLiteral("MSG_014"))));
        QVERIFY(catalog.translate(index, QStringLiteral("A native-only diagnostic that is not in Rufus.")).isEmpty());
    }
    void nativeBindingPlaceholderRefusal()
    {
        const QByteArray data = synthetic(
            "l \"en-US\" \"English\" 0x409\r\nv 4.14\r\n"
            "t MSG_222 \"Formatting (%s)...\"\r\nt MSG_271 \"Computing image checksums: %s\"\r\n"
            "t MSG_274 \"%s image detected\"\r\nt MSG_311 \"Use %s (in the main application window) to enable.\"\r\n"
            "l \"fr-FR\" \"French\" 0x40c\r\nv 4.14\r\nb \"en-US\"\r\n"
            "t MSG_222 \"Format %d\"\r\nt MSG_271 \"Checksum %s %s\"\r\n"
            "t MSG_274 \"Detected\"\r\nt MSG_311 \"Use %n\"\r\n");
        rufus::LocaleCatalog catalog; QString problem; QVERIFY2(catalog.load(data, problem), qPrintable(problem));
        for (const QString& source : {QStringLiteral("Formatting UDF..."), QStringLiteral("Computing image hashes: %p%"),
                QStringLiteral("ISOHybrid image detected"), QStringLiteral("Use Alt+H (in the main application window) to enable.")})
            QVERIFY2(catalog.translate(1, source).isEmpty(), qPrintable(source));
    }
    void inventoryCoverage()
    {
        const QString inputPath = qEnvironmentVariable("RUFUS_LOCALIZATION_INVENTORY_INPUT");
        if (inputPath.isEmpty()) return;
        QFile input(inputPath); QVERIFY(input.open(QIODevice::ReadOnly));
        QJsonParseError problem;
        const QJsonDocument inventory = QJsonDocument::fromJson(input.readAll(), &problem);
        QCOMPARE(problem.error, QJsonParseError::NoError);
        const QJsonArray messages = inventory.object().value(QStringLiteral("messages")).toArray();
        QVERIFY(!messages.isEmpty());
        const auto& catalog = rufus::ApplicationLanguages();
        QJsonArray locales, unbound;
        for (const QJsonValue& value : messages) {
            const QString source = value.toObject().value(QStringLiteral("source")).toString();
            QVERIFY(!source.isEmpty());
            if (catalog.translate(0, source).isEmpty()) unbound.append(source);
        }
        for (int index = 0; index < catalog.locales().size(); ++index) {
            const auto& locale = catalog.locales()[index];
            int bound = 0, sameAsEnglish = 0;
            QJsonArray placeholderFallbacks, inherited;
            for (const QJsonValue& value : messages) {
                const QString source = value.toObject().value(QStringLiteral("source")).toString();
                const QString translated = catalog.translate(index, source);
                const QString english = catalog.translate(0, source);
                if (translated.isEmpty()) {
                    if (!english.isEmpty()) placeholderFallbacks.append(source);
                    continue;
                }
                ++bound;
                if (translated == english) ++sameAsEnglish;
            }
            for (const auto& entry : catalog.locales()[0].entries) {
                if (entry.key.isEmpty()) continue;
                bool direct = false;
                for (const auto& candidate : locale.entries)
                    if (candidate.key == entry.key) direct = true;
                if (!direct) inherited.append(entry.key);
            }
            locales.append(QJsonObject{{QStringLiteral("tag"), locale.tag},
                {QStringLiteral("catalog_version"), QStringLiteral("%1.%2").arg(locale.major).arg(locale.minor)},
                {QStringLiteral("right_to_left"), locale.rightToLeft},
                {QStringLiteral("catalog_bound_sources"), bound},
                {QStringLiteral("bound_sources_equal_to_english"), sameAsEnglish},
                {QStringLiteral("placeholder_refusal_sources"), placeholderFallbacks},
                {QStringLiteral("original_catalog_inherited_keys"), inherited}});
        }
        const QJsonObject result{{QStringLiteral("inventory"), inputPath},
            {QStringLiteral("literal_sources"), int(messages.size())},
            {QStringLiteral("unbound_english_sources"), unbound},
            {QStringLiteral("locales"), locales},
            {QStringLiteral("meaning"), QStringLiteral("Catalog binding is not full translation coverage. Native-only unbound text stays English. Original catalog inheritance and invalid-placeholder refusal are reported separately. Technical trace logs and dynamic provider diagnostics require separate reachability review.")}};
        QFile output(qEnvironmentVariable("RUFUS_LOCALIZATION_INVENTORY_OUTPUT"));
        QVERIFY(output.open(QIODevice::WriteOnly));
        const QByteArray bytes = QJsonDocument(result).toJson(QJsonDocument::Indented);
        QCOMPARE(output.write(bytes), bytes.size());
    }
    void malformed_data()
    {
        QTest::addColumn<QByteArray>("data");
        const QByteArray valid = "l \"en-US\" \"English\" 0x409\r\nv 4.14\r\ng IDD_DIALOG\r\nt IDC_START \"Start\"\r\n";
        QTest::newRow("empty") << QByteArray();
        QTest::newRow("no-crlf") << QByteArray(1200, 'a');
        QTest::newRow("bom") << (QByteArray::fromHex("efbbbf") + synthetic(valid));
        QTest::newRow("nul") << (synthetic(valid) + QByteArray(1, 0));
        QTest::newRow("bad-utf8") << (synthetic(valid) + QByteArray::fromHex("ff"));
        QTest::newRow("quoted-bad-utf8") << synthetic(valid + "t MSG_001 \"" + QByteArray::fromHex("ff") + "\"\r\n");
        QTest::newRow("trailing-partial-utf8") << (synthetic(valid) + "#" + QByteArray::fromHex("e2"));
        QTest::newRow("oversize") << QByteArray(8 * 1024 * 1024 + 1, 'a');
        QTest::newRow("duplicate") << synthetic(valid + valid);
        QTest::newRow("no-version") << synthetic("l \"en-US\" \"English\" 0x409\r\nt MSG_001 \"x\"\r\n");
        QTest::newRow("bad-version") << synthetic("l \"en-US\" \"English\" 0x409\r\nv 4.14.2\r\n");
        QTest::newRow("unknown-base") << synthetic(valid + "b \"missing\"\r\n");
        QTest::newRow("cycle") << synthetic(valid + "b \"en-US\"\r\n");
        QTest::newRow("missing-quotes") << synthetic(valid + "t MSG_001 no\r\n");
        QTest::newRow("unknown-command") << synthetic(valid + "z 1\r\n");
        QTest::newRow("trailing-argument") << synthetic(valid + "t MSG_001 \"x\" extra\r\n");
        QTest::newRow("bad-lcid") << synthetic("l \"en-US\" \"English\" 0x10000\r\nv 4.14\r\n");
        QTest::newRow("long-line") << synthetic(valid + "#" + QByteArray(33000, 'a') + "\r\n");
    }
    void malformed()
    {
        QFETCH(QByteArray, data); rufus::LocaleCatalog catalog = rufus::ApplicationLanguages(); QString problem;
        QVERIFY(!catalog.load(data, problem)); QVERIFY(!problem.isEmpty());
        QCOMPARE(catalog.locales().size(), 38); // A failed load cannot replace a working catalog.
    }
    void lineAndQuotedRules()
    {
        const QByteArray source = "t MSG_001 \"A\\nB \\\"quote\\\"\"\r\n  \" and C\\\\D\"\r\nt MSG_002 \"next\"";
        size_t offset = 0; char line[256];
        QCOMPARE(ReadLocaleLine(source.constData(), size_t(source.size()), &offset, line, sizeof(line)), 1);
        QByteArray logical(line); size_t cursor = size_t(logical.indexOf('"')), start = 0;
        QVERIFY(ReadLocaleQuoted(logical.data(), size_t(logical.size()) + 1, &cursor, &start));
        QCOMPARE(QByteArray(logical.constData() + start), QByteArray("A\r\nB \"quote\" and C\\D"));
        QCOMPARE(ReadLocaleLine(source.constData(), size_t(source.size()), &offset, line, sizeof(line)), 1);
        QCOMPARE(QByteArray(line), QByteArray("t MSG_002 \"next\""));
        QCOMPARE(ReadLocaleLine(source.constData(), size_t(source.size()), &offset, line, sizeof(line)), 0);
        char arguments[] = "\"one\" \"two\""; cursor = 0;
        QVERIFY(ReadLocaleQuoted(arguments, sizeof(arguments), &cursor, &start));
        ++cursor; QVERIFY(ReadLocaleQuoted(arguments, sizeof(arguments), &cursor, &start));
        QCOMPARE(QByteArray(arguments + start), QByteArray("two"));
        cursor = sizeof(arguments); QVERIFY(!ReadLocaleQuoted(arguments, sizeof(arguments), &cursor, &start));
        cursor = 0; QVERIFY(!ReadLocaleQuoted(nullptr, 0, &cursor, &start));
        char unterminated[2] = {'"','x'};
        QVERIFY(!ReadLocaleQuoted(unterminated, sizeof(unterminated), &cursor, &start));
        offset = 0; QCOMPARE(ReadLocaleLine(source.constData(), size_t(source.size()), &offset, line, 8), -1);
    }
    void fallbackAndFormats()
    {
        const QByteArray data = synthetic(
            "l \"en-US\" \"English\" 0x409\r\nv 4.14\r\ng IDD_DIALOG\r\n"
            "t IDC_START \"Start\"\r\nt MSG_001 \"Value %s: %d%%\"\r\nt MSG_002 \"Base\"\r\n"
            "l \"fr-FR\" \"French\" 0x40c\r\nv 4.14\r\nb \"en-US\"\r\n"
            "t MSG_001 \"Valeur %s : %d%%\"\r\n"
            "l \"tr-TR\" \"Turkish\" 0x41f\r\nv 4.14\r\nb \"en-US\"\r\n"
            "t MSG_001 \"wrong %d %s\"\r\ng IDD_DIALOG\r\nt IDC_START \"işlem\"\r\n");
        rufus::LocaleCatalog catalog; QString problem; QVERIFY2(catalog.load(data, problem), qPrintable(problem));
        QCOMPARE(catalog.selection(QStringLiteral("0x40c-trailing")), 1);
        QCOMPARE(catalog.selection(QStringLiteral("FR-fr")), 0); QCOMPARE(catalog.selection(QStringLiteral("missing")), 0);
        QCOMPARE(catalog.translate(1, QStringLiteral("Value %1: %2%")), QStringLiteral("Valeur %1 : %2%"));
        QVERIFY(catalog.translate(2, QStringLiteral("Value %1: %2%")).isEmpty());
        QCOMPARE(catalog.text(1, QStringLiteral("MSG_002")), QStringLiteral("Base"));
        QCOMPARE(catalog.translate(2, QStringLiteral("START")), QString::fromUtf8("İŞLEM"));
    }
    void startupAndPersistence()
    {
        rufus::StartupOptions options; QString problem;
        QVERIFY(rufus::ParseStartupOptions({"rufus", "--locale=fr-FR"}, options, problem));
        QCOMPARE(options.locale, QStringLiteral("fr-FR"));
        QVERIFY(rufus::ParseStartupOptions({"rufus", "-l", "0x401"}, options, problem));
        QCOMPARE(options.locale, QStringLiteral("0x401"));
        QVERIFY(!rufus::StartupRequestsText({"rufus", "-l", "--help"}));
        QVERIFY(rufus::StartupRequestsText({"rufus", "--locale=fr-FR", "--help"}));
        rufus::ApplicationSettings settings; settings.remove(QStringLiteral("Locale")); settings.sync();
        const QString executable = profile_.filePath(QStringLiteral("rufus-linux"));
        QVERIFY(rufus::InitializeApplicationLanguage(executable, QStringLiteral("fr-FR"), problem));
        QCOMPARE(rufus::SelectedApplicationLanguage(), QStringLiteral("fr-FR"));
        QVERIFY(rufus::InitializeApplicationLanguage(executable, QStringLiteral("ar-SA"), problem));
        QCOMPARE(rufus::SelectedApplicationLanguage(), QStringLiteral("fr-FR")); // Saved value wins, as on Windows.
        QCOMPARE(rufus::ApplicationSettings().value(QStringLiteral("Locale")).toString(), QStringLiteral("fr-FR"));
    }
    void liveWindow_data()
    {
        QTest::addColumn<QString>("tag");
        QTest::newRow("French") << QStringLiteral("fr-FR");
        QTest::newRow("Arabic") << QStringLiteral("ar-SA");
        QTest::newRow("Romanian") << QStringLiteral("ro-RO");
    }
    void liveWindow()
    {
        QFETCH(QString, tag);
        rufus::SelectionController controller;
        rufus::Device device{QStringLiteral("fixture"), QStringLiteral("/test/no-device"), QStringLiteral("USB"), 16ULL << 30, true, false};
        device.mediaRemovable = true; device.defaultVisible = true;
        controller.setDiscovery({device}, {}); controller.selectDevice(device.id); controller.setBootType(BT_NON_BOOTABLE);
        controller.setFileSystem(FS_NTFS); controller.setVolumeLabel(QStringLiteral("KEEP LABEL"));
        rufus::MainWindow window(controller); window.show();
        auto* language = window.findChild<QToolButton*>(QStringLiteral("languageButton")); QVERIFY(language && language->isEnabled());
        auto* split = window.findChild<rufus::ImageSelectButton*>(); QVERIFY(split);
        QTest::keyClick(split, Qt::Key_Down);
        split->findChild<QMenu*>()->actions()[1]->trigger(); split->findChild<QMenu*>()->hide();
        auto* log = window.findChild<QPlainTextEdit*>(QStringLiteral("logText")); QVERIFY(log);
        const QString before = log->toPlainText();
        QTest::mouseClick(language, Qt::LeftButton);
        auto* menu = window.findChild<QMenu*>(QStringLiteral("applicationLanguageMenu")); QVERIFY(menu);
        QCOMPARE(menu->actions().size(), 38);
        QAction* selected = nullptr;
        for (QAction* action : menu->actions()) if (action->data().toString() == tag) selected = action;
        QVERIFY(selected); selected->trigger(); menu->hide(); QCoreApplication::processEvents();
        const auto& catalog = rufus::ApplicationLanguages(); const int index = catalog.selection(tag);
        QCOMPARE(window.findChild<QLabel*>(QStringLiteral("deviceLabel"))->text(), catalog.text(index, QStringLiteral("IDD_DIALOG/IDS_DEVICE_TXT")));
        QCOMPARE(window.findChild<QPushButton*>(QStringLiteral("startButton"))->text(), catalog.translate(index, QStringLiteral("START")));
        QCOMPARE(split->text(), catalog.translate(index, QStringLiteral("DOWNLOAD"))); QVERIFY(split->downloadMode());
        QCOMPARE(controller.selection().deviceId, device.id); QCOMPARE(controller.selection().fileSystem, FS_NTFS);
        QCOMPARE(controller.selection().volumeLabel, QStringLiteral("KEEP LABEL"));
        QCOMPARE(log->toPlainText(), before); QCOMPARE(QApplication::layoutDirection(), catalog.locales()[index].rightToLeft ? Qt::RightToLeft : Qt::LeftToRight);
        QVERIFY(!controller.isBusy());
        for (const auto& name : {QStringLiteral("driveHeader"), QStringLiteral("formatHeader"), QStringLiteral("statusHeader")}) {
            auto* heading = window.findChild<QLabel*>(name); QVERIFY(heading);
            QTRY_VERIFY(heading->width() >= heading->sizeHint().width());
        }
        QTest::mouseClick(language, Qt::LeftButton); QVERIFY(selected->isChecked());
        if (catalog.locales()[index].rightToLeft) QVERIFY(menu->actions()[0]->text().startsWith(QChar(0x202a)));
        menu->hide();
        const QString screenshots = qEnvironmentVariable("RUFUS_LOCALIZATION_SCREENSHOTS");
        if (!screenshots.isEmpty()) QVERIFY(window.grab().save(screenshots + QLatin1Char('/') + tag + QStringLiteral(".png")));
    }
    void externalCatalog()
    {
        QString problem; const QString executable = profile_.filePath(QStringLiteral("rufus-linux"));
        QFile file(profile_.filePath(QStringLiteral("rufus.loc"))); QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write("invalid") == 7); file.close();
        QVERIFY(!rufus::InitializeApplicationLanguage(executable, {}, problem));
        QCOMPARE(rufus::SelectedApplicationLanguage(), QStringLiteral("en-US"));
        QByteArray bytes = catalogBytes(); bytes.replace("t IDS_DEVICE_TXT \"Device\"", "t IDS_DEVICE_TXT \"Custom device\"");
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate)); QCOMPARE(file.write(bytes), bytes.size()); file.close();
        QVERIFY2(rufus::InitializeApplicationLanguage(executable, {}, problem), qPrintable(problem));
        QCOMPARE(QCoreApplication::translate("RufusWindow", "Device"), QStringLiteral("Custom device"));
        QVERIFY(file.remove());
    }
};
QTEST_MAIN(TestLocalization)
#include "test_localization.moc"
