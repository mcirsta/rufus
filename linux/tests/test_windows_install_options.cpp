// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/windows_install_options.h"
#include "iso_file.h"
#include <wimlib.h>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>
#include <cstring>

enum Fault { NoFault, FailInstallRead, FailBootRead, CancelEditionRead, CancelLanguageRead };
enum LanguageCase { MissingBoot, InvalidBoot, OneBootImage, MissingLanguage, UnreadableBoot, WrongCaseBoot };
enum InstallCase { MissingInstall, InvalidInstall, EmptyInstall, UnreadableInstall,
    CancelBeforeRead, CancelDuringEditions, CancelDuringLanguage };
static Fault fault;
static bool cancelled, active;
static ISO_FILE_READER* installReader;
static quint64 bytesRead;
static constexpr int fixtureTimeoutMs = 10000;

extern "C" int IsIsoScanCancelled(void) { return cancelled; }
extern "C" int IsRufusJobCancelled(void) { return cancelled; }
extern "C" void uprintf(const char*, ...) {}
extern "C" void uprintfs(const char*) {}
extern "C" int __real_ReadIsoFileRange(ISO_FILE_READER*, uint64_t, void*, size_t);
extern "C" const char* __real_wimlib_get_image_property(const WIMStruct*, int, const char*);

extern "C" int __wrap_ReadIsoFileRange(ISO_FILE_READER* reader, uint64_t offset, void* buffer, size_t size)
{
    if (active)
    {
        if (installReader == nullptr)
            installReader = reader;
        bytesRead += size;
        const bool failedInstall = fault == FailInstallRead && reader == installReader;
        const bool failedBoot = fault == FailBootRead && reader != installReader;
        if (failedInstall || failedBoot)
            return ISO_SCAN_ERROR;
    }
    return __real_ReadIsoFileRange(reader, offset, buffer, size);
}

extern "C" const char* __wrap_wimlib_get_image_property(const WIMStruct* wim, int image, const char* name)
{
    const char* value = __real_wimlib_get_image_property(wim, image, name);
    if (active)
    {
        const bool duringEditions = fault == CancelEditionRead && image == 2 && strcmp(name, "DISPLAYNAME") == 0;
        const bool duringLanguage = fault == CancelLanguageRead && strcmp(name, "WINDOWS/LANGUAGES/LANGUAGE[1]") == 0;
        if (duringEditions || duringLanguage)
            cancelled = true;
    }
    return value;
}

class TestWindowsInstallOptions : public QObject
{
    Q_OBJECT
    QTemporaryDir directory_;
    QString tree_, image_;
    ISO_ANALYSIS_SOURCE source_ = {};

    void put(const QString& path, const QByteArray& bytes)
    {
        QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(bytes), bytes.size());
    }

    QByteArray imageHash()
    {
        QFile file(image_);
        if (!file.open(QIODevice::ReadOnly))
            qFatal("Cannot read fixture ISO");
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (!hash.addData(&file))
            qFatal("Cannot hash fixture ISO");
        return hash.result();
    }

    void writeArchive(WIMStruct* wim, const QString& path, bool solid = false)
    {
        QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
        const int error = wimlib_write(wim, path.toUtf8().constData(), WIMLIB_ALL_IMAGES,
            solid ? WIMLIB_WRITE_FLAG_SOLID : 0, 1);
        wimlib_free(wim);
        QCOMPARE(error, 0);
    }

    void makeInstall(const QString& name = QStringLiteral("Install.wim"), bool solid = false, int images = 5)
    {
        WIMStruct* wim = nullptr;
        QCOMPARE(wimlib_create_new_wim(solid ? WIMLIB_COMPRESSION_TYPE_LZMS : WIMLIB_COMPRESSION_TYPE_NONE, &wim), 0);
        for (int image = 1; image <= images; ++image)
        {
            int added = 0;
            const QByteArray internalName = "internal-" + QByteArray::number(image);
            QCOMPARE(wimlib_add_empty_image(wim, internalName.constData(), &added), 0);
            QCOMPARE(added, image);
            if (image == 3)
                QCOMPARE(wimlib_set_image_property(wim, image, "DESCRIPTION", "Windows Éducation"), 0);
            else if (image != 4)
                QCOMPARE(wimlib_set_image_property(wim, image, "DISPLAYNAME",
                    image == 1 ? "Windows Home" : "Windows Pro"), 0);
        }
        writeArchive(wim, tree_ + "/sources/" + name, solid);
    }

    void makeBoot(int images = 2, const char* language = "ro-RO")
    {
        WIMStruct* wim = nullptr;
        QCOMPARE(wimlib_create_new_wim(WIMLIB_COMPRESSION_TYPE_NONE, &wim), 0);
        for (int image = 1; image <= images; ++image)
        {
            int added = 0;
            QCOMPARE(wimlib_add_empty_image(wim, nullptr, &added), 0);
            if (image == 1)
                QCOMPARE(wimlib_set_image_property(wim, image, "WINDOWS/LANGUAGES/LANGUAGE", "en-GB"), 0);
            else if (language != nullptr)
            {
                QCOMPARE(wimlib_set_image_property(wim, image, "WINDOWS/LANGUAGES/LANGUAGE[1]", language), 0);
                QCOMPARE(wimlib_set_image_property(wim, image, "WINDOWS/LANGUAGES/LANGUAGE[2]", "de-DE"), 0);
                QCOMPARE(wimlib_set_image_property(wim, image, "WINDOWS/LANGUAGES/DEFAULT", "de-DE"), 0);
            }
        }
        struct wimlib_wim_info info = {};
        info.boot_index = 1; // Intentionally differs from Rufus's UI-language image.
        if (images != 0)
            QCOMPARE(wimlib_set_wim_info(wim, &info, WIMLIB_CHANGE_BOOT_INDEX), 0);
        writeArchive(wim, tree_ + "/sources/boot.wim");
    }

    void makeImage(bool udf = false)
    {
        QStringList arguments = {QStringLiteral("-quiet"), QStringLiteral("-R"), QStringLiteral("-J"),
            QStringLiteral("-o"), image_};
        if (udf)
            arguments.append(QStringLiteral("-udf"));
        arguments.append(tree_);
        QProcess process;
        process.start(QStringLiteral(RUFUS_MKISOFS), arguments);
        QVERIFY(process.waitForFinished(fixtureTimeoutMs));
        QCOMPARE(process.exitCode(), 0);
        const QByteArray path = image_.toUtf8();
        if (udf)
        {
            source_.udf = udf_open(path.constData());
            QVERIFY(source_.udf != nullptr);
        }
        else
        {
            source_.iso = iso9660_open_ext(path.constData(), ISO_EXTENSION_ALL);
            QVERIFY(source_.iso != nullptr);
        }
    }

    int read(rufus::WindowsInstallOptions& options, const char* path = "/sources/Install.wim")
    {
        active = true;
        const int error = rufus::ReadWindowsInstallOptions(source_, path, options);
        active = false;
        return error;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(directory_.isValid());
        QCOMPARE(wimlib_global_init(WIMLIB_INIT_FLAG_ASSUME_UTF8), 0);
    }

    void cleanupTestCase() { wimlib_global_cleanup(); }

    void init()
    {
        active = cancelled = false;
        fault = NoFault;
        installReader = nullptr;
        bytesRead = 0;
        tree_ = directory_.filePath("tree");
        image_ = directory_.filePath("metadata.iso");
        QVERIFY(QDir(tree_).removeRecursively());
    }

    void cleanup() { CloseIsoAnalysisSource(&source_); }

    void nativeMetadata_data()
    {
        QTest::addColumn<bool>("udf");
        QTest::addColumn<bool>("esd");
        QTest::newRow("ISO9660-WIM") << false << false;
        QTest::newRow("UDF-WIM") << true << false;
        QTest::newRow("ISO9660-ESD") << false << true;
        QTest::newRow("UDF-ESD") << true << true;
    }

    void nativeMetadata()
    {
        QFETCH(bool, udf);
        QFETCH(bool, esd);
        makeInstall(esd ? QStringLiteral("Install.esd") : QStringLiteral("Install.wim"), esd);
        makeBoot();
        makeImage(udf);
        const QByteArray before = imageHash();
        rufus::WindowsInstallOptions options;
        QCOMPARE(read(options, esd ? "/sources/Install.esd" : "/sources/Install.wim"), 0);
        // Results remain usable after both native WIM and borrowed ISO handles close.
        CloseIsoAnalysisSource(&source_);
        QCOMPARE(options.editions.size(), 5);
        const QStringList names = {QStringLiteral("Windows Home"), QStringLiteral("Windows Pro"),
            QStringLiteral("Windows Éducation"), QStringLiteral("Unknown Windows Version"), QStringLiteral("Windows Pro")};
        for (int i = 0; i < names.size(); ++i)
        {
            QCOMPARE(options.editions[i].image, i + 1);
            QCOMPARE(options.editions[i].name, names[i]);
        }
        QVERIFY(options.nonstandardNames);
        QCOMPARE(options.uiLanguage, QStringLiteral("ro-RO"));
        QVERIFY(options.languageFromImage);
        QCOMPARE(options.languageError, 0);
        QVERIFY(bytesRead > 0 && bytesRead < quint64(QFileInfo(image_).size()));
        QCOMPARE(imageHash(), before);
    }

    void originalLanguageFallback_data()
    {
        QTest::addColumn<int>("mode");
        QTest::addColumn<bool>("udf");
        for (bool udf : {false, true})
        {
            const QByteArray prefix = udf ? "UDF-" : "ISO9660-";
            QTest::newRow((prefix + "missing-boot-wim").constData()) << int(MissingBoot) << udf;
            QTest::newRow((prefix + "invalid-boot-wim").constData()) << int(InvalidBoot) << udf;
            QTest::newRow((prefix + "only-one-image").constData()) << int(OneBootImage) << udf;
            QTest::newRow((prefix + "missing-language").constData()) << int(MissingLanguage) << udf;
            QTest::newRow((prefix + "read-error").constData()) << int(UnreadableBoot) << udf;
            QTest::newRow((prefix + "wrong-case-boot-path").constData()) << int(WrongCaseBoot) << udf;
        }
    }

    void originalLanguageFallback()
    {
        QFETCH(int, mode);
        QFETCH(bool, udf);
        makeInstall();
        if (mode == InvalidBoot)
            put(tree_ + "/sources/boot.wim", "invalid WIM");
        else if (mode != MissingBoot)
            makeBoot(mode == OneBootImage ? 1 : 2, mode == MissingLanguage ? nullptr : "ro-RO");
        // The original Win32 ISO path lookup also requires sources/boot.wim.
        if (mode == WrongCaseBoot)
            QVERIFY(QFile::rename(tree_ + "/sources/boot.wim", tree_ + "/sources/BOOT.WIM"));
        makeImage(udf);
        if (mode == UnreadableBoot)
            fault = FailBootRead;
        rufus::WindowsInstallOptions options;
        QCOMPARE(read(options), 0);
        QCOMPARE(options.editions.size(), 5);
        QCOMPARE(options.uiLanguage, QStringLiteral("en-US"));
        QVERIFY(!options.languageFromImage);
        QCOMPARE(options.languageError == 0, mode == MissingLanguage);
    }

    void longLanguageAndExplicitEnglish_data()
    {
        QTest::addColumn<QByteArray>("language");
        QTest::newRow("script-tag") << QByteArray("zh-Hans-CN");
        QTest::newRow("explicit-english") << QByteArray("en-US");
    }

    void longLanguageAndExplicitEnglish()
    {
        QFETCH(QByteArray, language);
        makeInstall(QStringLiteral("Install.wim"), false, 1);
        makeBoot(2, language.constData());
        makeImage();
        rufus::WindowsInstallOptions options;
        QCOMPARE(read(options), 0);
        QCOMPARE(options.uiLanguage, QString::fromUtf8(language));
        QVERIFY(options.languageFromImage);
        QCOMPARE(options.editions.size(), 1);
        QVERIFY(!options.nonstandardNames);
    }

    void installErrorsAndCancellation_data()
    {
        QTest::addColumn<int>("mode");
        QTest::newRow("missing-install") << int(MissingInstall);
        QTest::newRow("invalid-install") << int(InvalidInstall);
        QTest::newRow("empty-archive") << int(EmptyInstall);
        QTest::newRow("install-read-error") << int(UnreadableInstall);
        QTest::newRow("cancel-before-read") << int(CancelBeforeRead);
        QTest::newRow("cancel-during-editions") << int(CancelDuringEditions);
        QTest::newRow("cancel-during-language") << int(CancelDuringLanguage);
    }

    void installErrorsAndCancellation()
    {
        QFETCH(int, mode);
        if (mode == InvalidInstall)
            put(tree_ + "/sources/Install.wim", "invalid WIM");
        else if (mode != MissingInstall)
            makeInstall(QStringLiteral("Install.wim"), false, mode == EmptyInstall ? 0 : 5);
        makeInstall(QStringLiteral("Other.wim")); // A later candidate must never mask a selected-file failure.
        makeBoot();
        makeImage();
        if (mode == UnreadableInstall)
            fault = FailInstallRead;
        else if (mode == CancelBeforeRead)
            cancelled = true;
        else if (mode == CancelDuringEditions)
            fault = CancelEditionRead;
        else if (mode == CancelDuringLanguage)
            fault = CancelLanguageRead;
        rufus::WindowsInstallOptions options;
        options.editions.append({99, QStringLiteral("old selection")});
        options.uiLanguage = QStringLiteral("old language");
        options.nonstandardNames = options.languageFromImage = true;
        options.languageError = 99;
        const int error = read(options);
        const bool expectsCancellation = mode == CancelBeforeRead || mode == CancelDuringEditions || mode == CancelDuringLanguage;
        if (expectsCancellation)
            QCOMPARE(error, WIMLIB_ERR_ABORTED_BY_PROGRESS);
        else
            QVERIFY(error != 0);
        QVERIFY(options.editions.isEmpty());
        QCOMPARE(options.uiLanguage, QStringLiteral("en-US"));
        QVERIFY(!options.languageFromImage && !options.nonstandardNames);
        QCOMPARE(options.languageError, 0);
    }

    void invalidSourceAndPath()
    {
        rufus::WindowsInstallOptions options;
        QCOMPARE(read(options), WIMLIB_ERR_INVALID_PARAM);
        makeInstall();
        makeBoot();
        makeImage(true);
        QCOMPARE(read(options, nullptr), WIMLIB_ERR_INVALID_PARAM);
        QCOMPARE(read(options, "sources/Install.wim"), WIMLIB_ERR_INVALID_PARAM);
        QCOMPARE(read(options, ""), WIMLIB_ERR_INVALID_PARAM);
        source_.iso = iso9660_open_ext(image_.toUtf8().constData(), ISO_EXTENSION_ALL);
        QVERIFY(source_.iso != nullptr);
        QCOMPARE(read(options), WIMLIB_ERR_INVALID_PARAM); // Both views are ambiguous.
        QVERIFY(options.editions.isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestWindowsInstallOptions)
#include "test_windows_install_options.moc"
