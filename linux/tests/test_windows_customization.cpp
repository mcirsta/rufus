// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/windows_customization.h"
#include "qt/prepare_image_protocol.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>
#include <QXmlStreamReader>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <unistd.h>

namespace
{
enum Failure { None, WriteFailure, CloseFailure, CancelDuringWrite };
enum InvalidSilentInput { MissingAccount, MissingPrivacy, MissingRegion, BadEdition, EmptyUser,
    ReservedUser, InvalidUtf8, InvalidName, PaddedName, NullUser, NullFallback,
    InvalidSetupLanguage, NullSetupLanguage, InvalidKeyboard, InvalidTimezone, UnsupportedFlag, HostCopyOnly };
Failure failure = None;
bool cancelled = false;
const QString answerPath = QStringLiteral("sources/$OEM$/$$/Panther/unattend.xml");
}

extern "C" void uprintf(const char*, ...) {}
extern "C" int IsRufusJobCancelled(void) { return cancelled; }
extern "C" int IsIsoScanCancelled(void) { return cancelled; }
extern "C" bool __real_WriteUnattendXml(FILE*, const RUFUS_UNATTEND_OPTIONS*, uint32_t[2]);
extern "C" int __real_fclose(FILE*);

extern "C" bool __wrap_WriteUnattendXml(FILE* file, const RUFUS_UNATTEND_OPTIONS* options, uint32_t offsets[2])
{
    if (failure == WriteFailure)
    {
        fputs("incomplete", file);
        errno = ENOSPC;
        return false;
    }
    const bool result = __real_WriteUnattendXml(file, options, offsets);
    if (failure == CancelDuringWrite)
        cancelled = true;
    return result;
}

extern "C" int __wrap_fclose(FILE* file)
{
    const int result = __real_fclose(file);
    if (failure == CloseFailure)
    {
        errno = EIO;
        return EOF;
    }
    return result;
}

class TestWindowsCustomization : public QObject
{
    Q_OBJECT

    void put(const QString& path, const QByteArray& bytes)
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(bytes), bytes.size());
    }

    QByteArray read(const QString& path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            qFatal("Cannot read test output");
        return file.readAll();
    }

private slots:
    void init()
    {
        failure = None;
        cancelled = false;
    }

    void originalOemPlacementAndXml()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        QVERIFY(QDir(root.path()).mkpath(QStringLiteral("sources/$OEM$/$$/Panther")));
        const QString bootWim = root.filePath(QStringLiteral("sources/boot.wim"));
        const QString preserved = root.filePath(QStringLiteral("sources/$OEM$/$$/Panther/preserved"));
        put(bootWim, "original boot.wim");
        put(preserved, "existing OEM content");
        const QByteArray path = QFile::encodeName(root.path());
        QStringList modified;
        QCOMPARE(rufus::ApplyWindowsCustomization(path.constData(), ARCH_X86_64, 26200,
            rufus::WindowsOemFlags, QStringLiteral("Rufus.Test&User"), &modified), 0);
        QVERIFY(modified.isEmpty()); // Adding an OEM answer leaves source files unchanged.
        const QByteArray xml = read(root.filePath(answerPath));
        QVERIFY(xml.contains("processorArchitecture=\"amd64\""));
        QVERIFY(xml.contains("<Name>Rufus_Test_User</Name>"));
        QVERIFY(xml.contains("BypassNRO"));
        QVERIFY(xml.contains("<ProtectYourPC>3</ProtectYourPC>"));
        QVERIFY(xml.contains("<PreventDeviceEncryption>true</PreventDeviceEncryption>"));
        QVERIFY(xml.contains("DisableFileSyncNGSC"));
        QCOMPARE(xml.count("SkuSiPolicy.p7b"), 1);
        QVERIFY(!xml.contains("windowsPE"));
        QXmlStreamReader parser(xml);
        while (!parser.atEnd())
            parser.readNext();
        QVERIFY2(!parser.hasError(), qPrintable(parser.errorString()));
        QCOMPARE(read(bootWim), QByteArray("original boot.wim"));
        QCOMPARE(read(preserved), QByteArray("existing OEM content"));
        QVERIFY(!QFile::exists(root.filePath(QStringLiteral("Autounattend.xml"))));
        QCOMPARE(rufus::ApplyWindowsCustomization(path.constData(), ARCH_X86_64, 19045,
            UNATTEND_NO_DATA_COLLECTION, {}), EEXIST);
        QCOMPARE(read(root.filePath(answerPath)), xml);
        QTemporaryDir requestRoot;
        QVERIFY(QDir(requestRoot.path()).mkdir(QStringLiteral("sources")));
        rufus::PrepareImageRequest request;
        request.windowsFlags = rufus::WindowsOemFlags;
        request.windowsUsername = QStringLiteral("Rufus.Test&User");
        QCOMPARE(rufus::ApplyWindowsCustomization(requestRoot.path().toUtf8().constData(), ARCH_X86_64,
            26200, request, {}), 0);
        QCOMPARE(read(requestRoot.filePath(answerPath)), xml);
        QCOMPARE(request.windowsUsername, QStringLiteral("Rufus.Test&User"));
    }

    void noChoicesAndUnsupportedChoicesDoNotWrite()
    {
        QTemporaryDir root;
        const QByteArray path = QFile::encodeName(root.path());
        QCOMPARE(rufus::ApplyWindowsCustomization(path.constData(), ARCH_X86_64, 19045, 0, {}), 0);
        const int unsupported[] = {UNATTEND_SILENT_INSTALL,
            UNATTEND_DUPLICATE_LOCALE, UNATTEND_OFFLINE_INTERNAL_DRIVES, UNATTEND_WINDOWS_TO_GO};
        for (int flags : unsupported)
            QCOMPARE(rufus::ApplyWindowsCustomization(path.constData(), ARCH_X86_64, 19045, flags, {}), ENOTSUP);
        QCOMPARE(rufus::ApplyWindowsToGoCustomization(path.constData(), ARCH_X86_64, 0, {}), 0);
        const int unsupportedToGo[] = {UNATTEND_SECUREBOOT_TPM_MINRAM, UNATTEND_DUPLICATE_LOCALE,
            UNATTEND_SILENT_INSTALL, UNATTEND_WINDOWS_TO_GO};
        for (int flags : unsupportedToGo)
            QCOMPARE(rufus::ApplyWindowsToGoCustomization(path.constData(), ARCH_X86_64, flags, {}), ENOTSUP);
        QVERIFY(QDir(root.path()).entryList(QDir::NoDotAndDotDot | QDir::AllEntries).isEmpty());
    }

    void invalidNames()
    {
        QCOMPARE(rufus::CheckWindowsCustomizationSettings(0, QStringLiteral("unused")), EINVAL);
        QCOMPARE(rufus::CheckWindowsCustomizationSettings(UNATTEND_SET_USER, QString(128, QLatin1Char('x'))), EINVAL);
        QCOMPARE(rufus::CheckWindowsCustomizationSettings(UNATTEND_SET_USER, QStringLiteral("bad\nname")), EINVAL);
        QCOMPARE(rufus::CheckWindowsCustomizationSettings(UNATTEND_SET_USER, QString(QChar::Null)), EINVAL);
        QCOMPARE(rufus::CheckWindowsCustomizationSettings(UNATTEND_SET_USER, QString(QChar(0xd800))), EINVAL);
        QCOMPARE(rufus::CheckWindowsCustomizationSettings(UNATTEND_SET_USER, QStringLiteral("Măriuș")), 0);
    }

    void silentInputsFailBeforeWrites_data()
    {
        QTest::addColumn<int>("invalid");
        QTest::newRow("missing-account") << int(MissingAccount);
        QTest::newRow("missing-privacy") << int(MissingPrivacy);
        QTest::newRow("missing-region") << int(MissingRegion);
        QTest::newRow("edition-zero") << int(BadEdition);
        QTest::newRow("empty-user") << int(EmptyUser);
        QTest::newRow("reserved-user") << int(ReservedUser);
        QTest::newRow("invalid-utf8") << int(InvalidUtf8);
        QTest::newRow("unnormalized-name") << int(InvalidName);
        QTest::newRow("whitespace-name") << int(PaddedName);
        QTest::newRow("null-user") << int(NullUser);
        QTest::newRow("null-fallback") << int(NullFallback);
        QTest::newRow("xml-setup-language") << int(InvalidSetupLanguage);
        QTest::newRow("null-setup-language") << int(NullSetupLanguage);
        QTest::newRow("invalid-keyboard") << int(InvalidKeyboard);
        QTest::newRow("iana-timezone") << int(InvalidTimezone);
        QTest::newRow("unsupported-choice") << int(UnsupportedFlag);
        QTest::newRow("host-copy-only") << int(HostCopyOnly);
    }

    void silentInputsFailBeforeWrites()
    {
        QFETCH(int, invalid);
        char username[MAX_USERNAME_LENGTH] = "TestUser";
        RUFUS_UNATTEND_OPTIONS options = {};
        options.arch = ARCH_X86_64;
        options.flags = UNATTEND_SILENT_INSTALL | UNATTEND_SET_USER | UNATTEND_NO_DATA_COLLECTION |
            UNATTEND_DUPLICATE_LOCALE | UNATTEND_SECUREBOOT_TPM_MINRAM;
        options.edition_index = 1;
        options.username = username;
        options.timezone = "GTB Standard Time";
        options.input_locale = "en-US";
        options.system_locale = "ro-RO";
        options.user_locale = "ro-RO";
        options.ui_language = "en-US";
        options.ui_language_fallback = "";
        options.winpe_ui_language = "en-US";
        switch (invalid)
        {
        case MissingAccount: options.flags &= ~UNATTEND_SET_USER; break;
        case MissingPrivacy: options.flags &= ~UNATTEND_NO_DATA_COLLECTION; break;
        case MissingRegion: options.flags &= ~UNATTEND_DUPLICATE_LOCALE; break;
        case BadEdition: options.edition_index = 0; break;
        case EmptyUser: username[0] = 0; break;
        case ReservedUser: strcpy(username, "aDmInIsTrAtOr"); break;
        case InvalidUtf8: username[0] = char(0xff); break;
        case InvalidName: strcpy(username, "User&Name"); break;
        case PaddedName: strcpy(username, " User "); break;
        case NullUser: options.username = nullptr; break;
        case NullFallback: options.ui_language_fallback = nullptr; break;
        case InvalidSetupLanguage: options.winpe_ui_language = "en-US</UILanguage>"; break;
        case NullSetupLanguage: options.winpe_ui_language = nullptr; break;
        case InvalidKeyboard: options.input_locale = "us(intl)"; break;
        case InvalidTimezone: options.timezone = "Europe/Bucharest"; break;
        case UnsupportedFlag: options.flags |= UNATTEND_OFFLINE_INTERNAL_DRIVES; break;
        case HostCopyOnly: options.flags &= ~UNATTEND_SILENT_INSTALL; break;
        }
        QTemporaryDir root;
        const bool unsupported = invalid == UnsupportedFlag || invalid == HostCopyOnly;
        QCOMPARE(rufus::ApplyWindowsCustomization(root.path().toUtf8().constData(), 26200, options),
            unsupported ? ENOTSUP : EINVAL);
        QVERIFY(QDir(root.path()).entryList(QDir::NoDotAndDotDot | QDir::AllEntries).isEmpty());
    }

    void requestInputsFailBeforeWrites_data()
    {
        QTest::addColumn<QString>("change");
        const char* const cases[] = {"unconfirmed", "file-target", "no-source", "relative-archive", "unclean-archive",
            "null-archive", "reserved-name", "invalid-region", "zero-edition", "no-privacy",
            "stray-region", "stray-archive", "stray-edition", "stray-confirmation", "invalid-setup-language"};
        for (const char* name : cases)
            QTest::newRow(name) << QString::fromLatin1(name);
    }

    void requestInputsFailBeforeWrites()
    {
        QFETCH(QString, change);
        rufus::PrepareImageRequest request;
        request.isDevice = true;
        request.imagePath = QStringLiteral("/test/source.iso");
        request.windowsFlags = UNATTEND_SILENT_INSTALL | UNATTEND_SET_USER |
            UNATTEND_NO_DATA_COLLECTION | UNATTEND_DUPLICATE_LOCALE;
        request.windowsUsername = QStringLiteral("TestUser");
        request.windowsInstallPath = QStringLiteral("/sources/install.wim");
        request.windowsEdition = 1;
        request.windowsSilentConfirmed = true;
        request.windowsRegion = {QStringLiteral("ro-RO"), QStringLiteral("ro-RO"), QStringLiteral("en-GB"),
            QStringLiteral("en-US"), {}, QStringLiteral("GTB Standard Time")};
        QCOMPARE(rufus::CheckWindowsCustomizationSettings(request), 0);
        if (change == QStringLiteral("unconfirmed")) request.windowsSilentConfirmed = false;
        if (change == QStringLiteral("file-target")) request.isDevice = false;
        if (change == QStringLiteral("no-source")) request.imagePath.clear();
        if (change == QStringLiteral("relative-archive")) request.windowsInstallPath = QStringLiteral("sources/install.wim");
        if (change == QStringLiteral("unclean-archive")) request.windowsInstallPath = QStringLiteral("/sources/../install.wim");
        if (change == QStringLiteral("null-archive")) request.windowsInstallPath.append(QChar::Null);
        if (change == QStringLiteral("reserved-name")) request.windowsUsername = QStringLiteral("aDmInIsTrAtOr");
        if (change == QStringLiteral("invalid-region")) request.windowsRegion.timezone = QStringLiteral("Europe/Bucharest");
        if (change == QStringLiteral("zero-edition")) request.windowsEdition = 0;
        if (change == QStringLiteral("no-privacy")) request.windowsFlags &= ~UNATTEND_NO_DATA_COLLECTION;
        if (change.startsWith(QStringLiteral("stray-")))
        {
            request = {};
            request.windowsFlags = UNATTEND_NO_DATA_COLLECTION;
            if (change == QStringLiteral("stray-region")) request.windowsRegion.uiLanguageFallback = QStringLiteral("en-US");
            if (change == QStringLiteral("stray-archive")) request.windowsInstallPath = QStringLiteral("/sources/install.wim");
            if (change == QStringLiteral("stray-edition")) request.windowsEdition = 1;
            if (change == QStringLiteral("stray-confirmation")) request.windowsSilentConfirmed = true;
        }
        const bool invalidLanguage = change == QStringLiteral("invalid-setup-language");
        const QString language = invalidLanguage ? QStringLiteral("en-US</UILanguage>") : QStringLiteral("en-US");
        if (!invalidLanguage)
            QCOMPARE(rufus::CheckWindowsCustomizationSettings(request), EINVAL);
        QTemporaryDir root;
        QCOMPARE(rufus::ApplyWindowsCustomization(root.path().toUtf8().constData(), ARCH_X86_64,
            26200, request, language), EINVAL);
        QVERIFY(QDir(root.path()).entryList(QDir::NoDotAndDotDot | QDir::AllEntries).isEmpty());
    }

    void sharedReservedNames()
    {
        const char* const reserved[] = {"Administrator", "Järjestelmänvalvoja", "Administrateur", "Rendszergazda",
            "Administrador", "Администратор", "Administratör", "Guest", "DefaultAccount", "WDAGUtilityAccount",
            "HelpAssistant", "KRBTGT", "Local", "NONE", "SYSTEM", "aDmInIsTrAtOr", "guest"};
        for (const char* name : reserved)
            QVERIFY(IsUnattendUserNameReserved(name));
        QVERIFY(!IsUnattendUserNameReserved("TestUser"));
        QVERIFY(!IsUnattendUserNameReserved(""));
        QVERIFY(!IsUnattendUserNameReserved(nullptr));
    }

    void newerChoicesImageEligibility_data()
    {
        QTest::addColumn<int>("flag");
        QTest::newRow("ca-2023") << int(UNATTEND_USE_MS2023_BOOTLOADERS);
        QTest::newRow("skusipolicy") << int(UNATTEND_APPLY_SKUSIPOLICY);
    }

    void newerChoicesImageEligibility()
    {
        QFETCH(int, flag);
        RUFUS_IMG_REPORT report = {};
        report.has_bootmgr_efi = true;
        report.has_efi = 1 << (ARCH_X86_64 + 1);
        report.win_version.major = 11;
        report.win_version.build = 26100;
        QCOMPARE(rufus::CheckWindowsCustomizationSettings(flag, {}), 0);
        QCOMPARE(rufus::CheckWindowsCustomizationImage(report, flag), ENOTSUP);
        report.win_version.build = 26200;
        QCOMPARE(rufus::CheckWindowsCustomizationImage(report, flag), 0);
        report.has_panther_unattend = true;
        QCOMPARE(rufus::CheckWindowsCustomizationImage(report, flag), ENOTSUP);
    }

    void sModeImageEligibility()
    {
        RUFUS_IMG_REPORT report = {};
        report.has_bootmgr_efi = true;
        report.has_efi = 1 << (ARCH_X86_64 + 1);
        report.win_version.major = 10;
        report.win_version.build = 19045;
        QCOMPARE(rufus::CheckWindowsCustomizationSettings(UNATTEND_FORCE_S_MODE, {}), 0);
        QCOMPARE(rufus::CheckWindowsCustomizationImage(report, UNATTEND_FORCE_S_MODE), 0);
        report.win_version.major = 11;
        report.win_version.build = 26200;
        QCOMPARE(rufus::CheckWindowsCustomizationImage(report, UNATTEND_FORCE_S_MODE), 0);
        report.has_panther_unattend = true;
        QCOMPARE(rufus::CheckWindowsCustomizationImage(report, UNATTEND_FORCE_S_MODE), ENOTSUP);
        report.has_panther_unattend = false;
        report.has_efi = 0;
        QCOMPARE(rufus::CheckWindowsCustomizationImage(report, UNATTEND_FORCE_S_MODE), ENOTSUP);
        report.has_efi = 1 << (ARCH_X86_64 + 1);
        report.win_version = {};
        QCOMPARE(rufus::CheckWindowsCustomizationImage(report, UNATTEND_FORCE_S_MODE), ENOTSUP);
    }

    void sModeAloneUsesOriginalOfflineServicingAnswer()
    {
        QTemporaryDir root;
        QVERIFY(QDir(root.path()).mkdir(QStringLiteral("sources")));
        const QString bootWim = root.filePath(QStringLiteral("sources/boot.wim"));
        put(bootWim, "S Mode alone must not open or modify boot.wim");
        rufus::PrepareImageRequest request;
        request.windowsFlags = UNATTEND_FORCE_S_MODE;
        QCOMPARE(rufus::ApplyWindowsCustomization(root.path().toUtf8().constData(), ARCH_X86_64,
            26200, request, {}), 0);
        const QByteArray xml = read(root.filePath(answerPath));
        QXmlStreamReader parser(xml);
        QStringList passes, components;
        QString policy;
        while (!parser.atEnd())
        {
            parser.readNext();
            if (!parser.isStartElement())
                continue;
            if (parser.name() == QStringLiteral("settings"))
                passes.append(parser.attributes().value(QStringLiteral("pass")).toString());
            if (parser.name() == QStringLiteral("component"))
                components.append(parser.attributes().value(QStringLiteral("name")).toString());
            if (parser.name() == QStringLiteral("SkuPolicyRequired"))
                policy = parser.readElementText();
        }
        QVERIFY2(!parser.hasError(), qPrintable(parser.errorString()));
        QCOMPARE(passes, QStringList({QStringLiteral("offlineServicing")}));
        QCOMPARE(components, QStringList({QStringLiteral("Microsoft-Windows-CodeIntegrity")}));
        QCOMPARE(policy, QStringLiteral("1"));
        QCOMPARE(read(bootWim), QByteArray("S Mode alone must not open or modify boot.wim"));
        QVERIFY(!QFile::exists(root.filePath(QStringLiteral("Autounattend.xml"))));
    }

    void policyAloneUsesInstalledWindowsFile()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        QVERIFY(QDir(root.path()).mkdir(QStringLiteral("sources")));
        const QString bootWim = root.filePath(QStringLiteral("sources/boot.wim"));
        put(bootWim, "not a WIM: policy alone must not open it");
        const QByteArray path = QFile::encodeName(root.path());
        QCOMPARE(rufus::ApplyWindowsCustomization(path.constData(), ARCH_X86_64, 26200,
            UNATTEND_APPLY_SKUSIPOLICY, {}), 0);
        const QByteArray xml = read(root.filePath(answerPath));
        const QString expected = QStringLiteral("cmd /c mountvol S: /S && "
            "copy %WINDIR%\\system32\\SecureBootUpdates\\SkuSiPolicy.p7b S:\\EFI\\Microsoft\\Boot && "
            "mountvol S: /D");
        QXmlStreamReader parser(xml);
        QStringList commands;
        int sections = 0;
        while (!parser.atEnd())
        {
            parser.readNext();
            if (!parser.isStartElement())
                continue;
            if (parser.name() == QStringLiteral("FirstLogonCommands"))
                ++sections;
            if (parser.name() == QStringLiteral("CommandLine"))
                commands.append(parser.readElementText());
        }
        QVERIFY2(!parser.hasError(), qPrintable(parser.errorString()));
        QCOMPARE(sections, 1);
        QCOMPARE(commands, QStringList({expected}));
        QVERIFY(!xml.contains("ProtectYourPC"));
        QVERIFY(!xml.contains("UserAccounts"));
        QCOMPARE(read(bootWim), QByteArray("not a WIM: policy alone must not open it"));
        QVERIFY(!QFile::exists(root.filePath(QStringLiteral("efi"))));
        QVERIFY(!QFile::exists(root.filePath(QStringLiteral("sources/appraiserres.dll"))));
    }

    void linksAndMissingSourcesAreRefused()
    {
        QTemporaryDir root, outside;
        const QByteArray path = QFile::encodeName(root.path());
        QCOMPARE(rufus::ApplyWindowsCustomization(path.constData(), ARCH_X86_64, 19045,
            UNATTEND_NO_DATA_COLLECTION, {}), ENOENT);
        const QString sources = root.filePath(QStringLiteral("sources"));
        QVERIFY(QFile::link(outside.path(), sources));
        QVERIFY(rufus::ApplyWindowsCustomization(path.constData(), ARCH_X86_64, 19045,
            UNATTEND_NO_DATA_COLLECTION, {}) != 0);
        QVERIFY(QFile::remove(sources));
        QVERIFY(QDir().mkpath(sources));
        QVERIFY(QFile::link(outside.path(), sources + QStringLiteral("/$OEM$")));
        QVERIFY(rufus::ApplyWindowsCustomization(path.constData(), ARCH_X86_64, 19045,
            UNATTEND_NO_DATA_COLLECTION, {}) != 0);
        QVERIFY(QDir(outside.path()).entryList(QDir::NoDotAndDotDot | QDir::AllEntries).isEmpty());
    }

    void failuresRemovePartialFile_data()
    {
        QTest::addColumn<int>("fault");
        QTest::addColumn<int>("expected");
        QTest::newRow("write") << int(WriteFailure) << ENOSPC;
        QTest::newRow("close") << int(CloseFailure) << EIO;
        QTest::newRow("cancel") << int(CancelDuringWrite) << ECANCELED;
    }

    void failuresRemovePartialFile()
    {
        QFETCH(int, fault);
        QFETCH(int, expected);
        QTemporaryDir root;
        QVERIFY(QDir(root.path()).mkdir(QStringLiteral("sources")));
        const QByteArray path = QFile::encodeName(root.path());
        failure = Failure(fault);
        QCOMPARE(rufus::ApplyWindowsCustomization(path.constData(), ARCH_X86_64, 19045,
            UNATTEND_NO_DATA_COLLECTION, {}), expected);
        QVERIFY(!QFile::exists(root.filePath(answerPath)));
        failure = None;
        cancelled = false;
        QCOMPARE(rufus::ApplyWindowsCustomization(path.constData(), ARCH_X86_64, 19045,
            UNATTEND_NO_DATA_COLLECTION, {}), 0);
    }

    void cancelledBeforeCreation()
    {
        QTemporaryDir root;
        const QByteArray path = QFile::encodeName(root.path());
        cancelled = true;
        QCOMPARE(rufus::ApplyWindowsCustomization(path.constData(), ARCH_X86_64, 19045,
            UNATTEND_NO_DATA_COLLECTION, {}), ECANCELED);
        QVERIFY(QDir(root.path()).entryList(QDir::NoDotAndDotDot | QDir::AllEntries).isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestWindowsCustomization)
#include "test_windows_customization.moc"
