// SPDX-License-Identifier: GPL-3.0-or-later
#include "qt/main_window.h"
#include "compressed_fixture.h"
#include "qt/selection_controller.h"
#include "qt/image_mode_dialog.h"
#include "platform/iso_write_check.h"
#include "platform/rufus_reporting.h"
#include "platform/compressed_write.h"

#include <cdio/logging.h>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QSemaphore>
#include <QSettings>
#include <QSlider>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>

#include <atomic>
#include <memory>
#include <stdexcept>
#include <QCryptographicHash>
#include <zlib.h>

static constexpr int workerTimeoutMs = 5000;
static constexpr int analysisTimeoutMs = 3000;
static constexpr int uiTimeoutMs = 1000;

namespace
{

QString compressedSuffix(int compression)
{
    return compression == BLED_COMPRESSION_XZ ? QStringLiteral("xz")
        : compression == BLED_COMPRESSION_BZIP2 ? QStringLiteral("bz2")
        : compression == BLED_COMPRESSION_LZMA ? QStringLiteral("lzma")
        : compression == BLED_COMPRESSION_LZW ? QStringLiteral("Z")
        : compression == BLED_COMPRESSION_ZIP ? QStringLiteral("zip")
        : compression == BLED_COMPRESSION_ZSTD ? QStringLiteral("zst") : QStringLiteral("gz");
}

QByteArray compressedLabel(int compression)
{
    return compression == BLED_COMPRESSION_XZ ? QByteArray("xz")
        : compression == BLED_COMPRESSION_BZIP2 ? QByteArray("bzip2")
        : compression == BLED_COMPRESSION_LZMA ? QByteArray("lzma")
        : compression == BLED_COMPRESSION_LZW ? QByteArray("lzw")
        : compression == BLED_COMPRESSION_ZIP ? QByteArray("zip")
        : compression == BLED_COMPRESSION_ZSTD ? QByteArray("zstd")
        : compression == LzwGzipFixture ? QByteArray("lzw-gzip") : QByteArray("gzip");
}

enum Gate { None, BeforeScan, AfterScan, Throw, BeforeWim, AfterWim };
enum ImageChange { ChangeSize, RemoveFile, ChangeTimestamp, ChangeMarker };
std::atomic<int> nextGate{None};
std::atomic<int> wimGate{None};
std::atomic<int> scanCalls{0};
std::atomic<int> compressedCalls{0};
std::atomic<QThread*> scanThread{nullptr};
QSemaphore entered;
QSemaphore releaseScan;

void waitForTest()
{
    entered.release();
    if (!releaseScan.tryAcquire(1, workerTimeoutMs))
        throw std::runtime_error("test scan release timed out");
}

struct ImageFile
{
    QString path;
    QByteArray content;
};

void previousCdioHandler(cdio_log_level_t level, const char* message)
{
    (void)level;
    (void)message;
}

} // namespace

extern "C" int __real_AnalyzeIsoImageWithSource(const char*, uint64_t, const ISO_ANALYSIS_OPTIONS*, ISO_ANALYSIS*, ISO_ANALYSIS_SOURCE*);
extern "C" int __real_OpenWimFromIsoReader(struct iso_file_reader*, int, WIMStruct**);
extern "C" int __real_ReadWimMetadata(WIMStruct*, int, RUFUS_WIM_METADATA*);
extern "C" int __real_PrepareCompressedWrite(const IMAGE_FILE_SOURCE*, const char*, uint64_t, unsigned, bool, COMPRESSED_WRITE_PLAN*);
extern "C" int __wrap_PrepareCompressedWrite(const IMAGE_FILE_SOURCE* source, const char* path,
    uint64_t maximum, unsigned sector, bool ignore, COMPRESSED_WRITE_PLAN* plan)
{
    compressedCalls++;
    scanThread = QThread::currentThread();
    const int gate = nextGate.exchange(None);
    if (gate == BeforeScan) waitForTest();
    const int error = __real_PrepareCompressedWrite(source, path, maximum, sector, ignore, plan);
    if (gate == AfterScan) waitForTest();
    return error;
}

extern "C" int __wrap_OpenWimFromIsoReader(struct iso_file_reader* reader, int flags, WIMStruct** wim)
{
    if (wimGate.load() == BeforeWim)
    {
        wimGate = None;
        waitForTest();
    }
    return __real_OpenWimFromIsoReader(reader, flags, wim);
}

extern "C" int __wrap_ReadWimMetadata(WIMStruct* wim, int image, RUFUS_WIM_METADATA* result)
{
    const int status = __real_ReadWimMetadata(wim, image, result);
    if (wimGate.exchange(None) == AfterWim)
        waitForTest();
    return status;
}

// Link-time gates make races repeatable without a production test hook or a
// fake job factory. Ordinary cases always execute the real shared C scanner.
extern "C" int __wrap_AnalyzeIsoImageWithSource(const char* image, uint64_t size,
                                   const ISO_ANALYSIS_OPTIONS* options, ISO_ANALYSIS* result, ISO_ANALYSIS_SOURCE* source)
{
    scanCalls++;
    scanThread = QThread::currentThread();
    const int gate = nextGate.exchange(None);
    if (gate == Throw)
        throw std::runtime_error("test analysis exception");
    if (gate == BeforeScan)
        waitForTest();
    const int status = __real_AnalyzeIsoImageWithSource(image, size, options, result, source);
    if (gate == AfterScan)
    {
        waitForTest();
        uprintfs("late scan output");
    }
    return status;
}

class TestImageAnalysis : public QObject
{
    Q_OBJECT

private:
    QString makeGzip(const QByteArray& decoded, const QString& name = QStringLiteral("source.GZ"))
    {
        const QString path = directory_->filePath(name);
        gzFile file = gzopen(QFile::encodeName(path).constData(), "wb0");
        if (!file) return {};
        const int written = gzwrite(file, decoded.constData(), unsigned(decoded.size()));
        const int closed = gzclose(file);
        return written == decoded.size() && closed == Z_OK ? path : QString();
    }

    QString makeCompressed(const QByteArray& decoded, int compression, const QString& name)
    {
        if (compression == BLED_COMPRESSION_GZIP)
            return makeGzip(decoded, name);
        const QString path = directory_->filePath(name);
        if (compression == BLED_COMPRESSION_ZIP)
        {
            QFile output(path);
            const QByteArray archive = ZipDeflateFixture(decoded);
            return !archive.isEmpty() && output.open(QIODevice::WriteOnly)
                && output.write(archive) == archive.size() ? path : QString();
        }
        if (compression == BLED_COMPRESSION_LZW || compression == LzwGzipFixture)
        {
            QFile output(path);
            const QByteArray packed = LzwLiteralFixture(decoded);
            return output.open(QIODevice::WriteOnly) && output.write(packed) == packed.size() ? path : QString();
        }
        QProcess encoder;
        encoder.setStandardOutputFile(path);
        if (compression == BLED_COMPRESSION_BZIP2)
            encoder.start(QStringLiteral("bzip2"), {QStringLiteral("--stdout")});
        else if (compression == BLED_COMPRESSION_ZSTD)
            encoder.start(QStringLiteral("zstd"), {QStringLiteral("--stdout"), QStringLiteral("--quiet")});
        else if (compression == BLED_COMPRESSION_LZMA)
            encoder.start(QStringLiteral("xz"), {QStringLiteral("--stdout"), QStringLiteral("--format=lzma"),
                QStringLiteral("--lzma1=dict=64KiB")});
        else
            encoder.start(QStringLiteral("xz"), {QStringLiteral("--stdout"), QStringLiteral("--check=crc64"),
                QStringLiteral("--lzma2=dict=64KiB")});
        if (!encoder.waitForStarted(workerTimeoutMs) || encoder.write(decoded) != decoded.size())
            return {};
        encoder.closeWriteChannel();
        return encoder.waitForFinished(workerTimeoutMs) && encoder.exitStatus() == QProcess::NormalExit
            && encoder.exitCode() == 0 ? path : QString();
    }

    bool corruptCompressedCheck(const QString& path, int compression)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadWrite) || file.size() < 24)
            return false;
        qint64 offset = file.size() - 8;
        if (compression == BLED_COMPRESSION_XZ)
        {
            if (!file.seek(7) || file.read(1) != QByteArray(1, char(4)) || !file.seek(file.size() - 8))
                return false;
            const QByteArray backward = file.read(4);
            if (backward.size() != 4)
                return false;
            quint64 words = 0;
            for (unsigned i = 0; i < 4; i++) words |= quint64(quint8(backward[int(i)])) << (8 * i);
            const quint64 indexBytes = (words + 1) * 4;
            if (indexBytes + 20 >= quint64(file.size()))
                return false;
            offset = file.size() - 12 - qint64(indexBytes) - 8;
        }
        if (compression == BLED_COMPRESSION_BZIP2) offset = 10;
        if (compression == BLED_COMPRESSION_ZIP) offset = 14;
        if (compression == BLED_COMPRESSION_ZSTD) offset = file.size() - 1;
        if (!file.seek(offset))
            return false;
        const QByteArray check = file.read(1);
        return check.size() == 1 && file.seek(offset)
            && file.write(QByteArray(1, char(check[0] ^ 1))) == 1 && file.flush();
    }

    QByteArray compressedPayload()
    {
        QByteArray bytes(4096 + 37, 'g');
        bytes[510] = char(0x55);
        bytes[511] = char(0xaa);
        return bytes;
    }

    QByteArray makeWim(const QString& kind)
    {
        const QString path = directory_->filePath(kind + QStringLiteral(".wim"));
        if (!runImageTool(QStringLiteral(RUFUS_WIM_FIXTURE), {path, kind}))
            return {};
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            return {};
        return file.readAll();
    }

    QString makeImage(const QString& name, const QList<ImageFile>& files, bool udf = false)
    {
        const QString tree = directory_->filePath(name);
        if (!QDir().mkpath(tree))
            return {};
        // Worker-lifecycle fixtures still need a Rufus-recognized boot method.
        // Explicit file lists remain exact, including non-bootable fixtures.
        if (files.isEmpty())
        {
            QFile config(tree + QStringLiteral("/syslinux.cfg"));
            if (!config.open(QIODevice::WriteOnly) || config.write("default linux\n") < 0)
                return {};
        }
        for (const ImageFile& entry : files)
        {
            const QString path = tree + QLatin1Char('/') + entry.path;
            if (!QDir().mkpath(QFileInfo(path).absolutePath()))
                return {};
            QFile file(path);
            if (!file.open(QIODevice::WriteOnly) || file.write(entry.content) != entry.content.size())
                return {};
        }
        const QString image = directory_->filePath(name + QString::fromUtf8("-ș.iso"));
        QProcess process;
        if (udf)
        {
            process.start(QStringLiteral(RUFUS_MKISOFS), {
                QStringLiteral("-quiet"), QStringLiteral("-udf"), QStringLiteral("-R"),
                QStringLiteral("-J"), QStringLiteral("-input-charset"), QStringLiteral("utf-8"),
                QStringLiteral("-V"), name, QStringLiteral("-o"), image, tree});
        }
        else
        {
            process.start(QStringLiteral(RUFUS_BSDTAR), {
                QStringLiteral("--format"), QStringLiteral("iso9660"),
                QStringLiteral("--options"), QStringLiteral("iso9660:volume-id=") + name,
                QStringLiteral("-cf"), image, QStringLiteral("-C"), tree, QStringLiteral(".")});
        }
        if (!process.waitForFinished(workerTimeoutMs) || process.exitStatus() != QProcess::NormalExit
            || process.exitCode() != 0)
            return {};
        return image;
    }

    void recordLog(const QString& message)
    {
        onGuiThread_ &= QThread::currentThread() == thread();
        messages_.append(message);
    }

    bool runImageTool(const QString& program, const QStringList& arguments)
    {
        QProcess process;
        process.start(program, arguments);
        return process.waitForFinished(workerTimeoutMs) && process.exitStatus() == QProcess::NormalExit
            && process.exitCode() == 0;
    }

    QByteArray makeFatImage(const QString& name, bool loader)
    {
        const QString image = directory_->filePath(name + QStringLiteral(".fat"));
        if (!runImageTool(QStringLiteral(RUFUS_MKFS_FAT), {
                QStringLiteral("-C"), QStringLiteral("-F"), QStringLiteral("12"),
                image, QStringLiteral("1440")}))
            return {};
        if (loader)
        {
            if (!runImageTool(QStringLiteral(RUFUS_MMD), {
                    QStringLiteral("-i"), image, QStringLiteral("::/EFI"), QStringLiteral("::/EFI/BOOT")}))
                return {};
            const QString payload = directory_->filePath(name + QStringLiteral(".efi"));
            QFile file(payload);
            if (!file.open(QIODevice::WriteOnly) || file.write(QByteArray(512, '\0')) != 512)
                return {};
            file.close();
            if (!runImageTool(QStringLiteral(RUFUS_MCOPY), {
                    QStringLiteral("-i"), image, payload, QStringLiteral("::/EFI/BOOT/bootx64.efi")}))
                return {};
        }
        QFile file(image);
        if (!file.open(QIODevice::ReadOnly))
            return {};
        return file.readAll();
    }

    void recordSelection()
    {
        onGuiThread_ &= QThread::currentThread() == thread();
    }

    std::unique_ptr<QTemporaryDir> directory_;
    std::unique_ptr<rufus::SelectionController> controller_;
    std::unique_ptr<rufus::MainWindow> window_;
    QPushButton* select_ = nullptr;
    QPushButton* close_ = nullptr;
    QPushButton* start_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QLineEdit* volume_ = nullptr;
    QPlainTextEdit* log_ = nullptr;
    QStringList messages_;
    bool onGuiThread_ = true;

private slots:
    void initTestCase()
    {
        QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    }

    void init()
    {
        nextGate = None;
        wimGate = None;
        scanCalls = 0;
        compressedCalls = 0;
        scanThread = nullptr;
        entered.tryAcquire(entered.available());
        releaseScan.tryAcquire(releaseScan.available());
        messages_.clear();
        onGuiThread_ = true;
        directory_ = std::make_unique<QTemporaryDir>();
        QVERIFY(directory_->isValid());
        qputenv("XDG_DATA_HOME", directory_->filePath(QStringLiteral("application-data")).toUtf8());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory_->filePath(QStringLiteral("settings")));
        controller_ = std::make_unique<rufus::SelectionController>();
        window_ = std::make_unique<rufus::MainWindow>(*controller_);
        select_ = window_->findChild<QPushButton*>(QStringLiteral("selectImageButton"));
        close_ = window_->findChild<QPushButton*>(QStringLiteral("closeButton"));
        start_ = window_->findChild<QPushButton*>(QStringLiteral("startButton"));
        progress_ = window_->findChild<QProgressBar*>(QStringLiteral("progressBar"));
        volume_ = window_->findChild<QLineEdit*>(QStringLiteral("volumeEdit"));
        log_ = window_->findChild<QPlainTextEdit*>(QStringLiteral("logText"));
        QVERIFY(select_ && close_ && start_ && progress_ && volume_ && log_);
        connect(controller_.get(), &rufus::SelectionController::logMessage,
                this, &TestImageAnalysis::recordLog);
        connect(controller_.get(), &rufus::SelectionController::selectionChanged,
                this, &TestImageAnalysis::recordSelection);
        window_->show();
        QVERIFY(QTest::qWaitForWindowExposed(window_.get(), 1000));
    }

    void cleanup()
    {
        releaseScan.release();
        window_.reset();
        controller_.reset(); // Must join before the image files or test gates go away.
        directory_.reset();
        QVERIFY(onGuiThread_);
    }

    void compressedSelectionFreezesDecodedIdentityOnTheWorker_data()
    {
        QTest::addColumn<int>("compression");
        QTest::newRow("gzip") << int(BLED_COMPRESSION_GZIP);
        QTest::newRow("xz") << int(BLED_COMPRESSION_XZ);
        QTest::newRow("bzip2") << int(BLED_COMPRESSION_BZIP2);
        QTest::newRow("lzma") << int(BLED_COMPRESSION_LZMA);
        QTest::newRow("lzw") << int(BLED_COMPRESSION_LZW);
        QTest::newRow("zip") << int(BLED_COMPRESSION_ZIP);
        QTest::newRow("zstd") << int(BLED_COMPRESSION_ZSTD);
        QTest::newRow("lzw-gzip") << LzwGzipFixture;
    }

    void compressedSelectionFreezesDecodedIdentityOnTheWorker()
    {
        QFETCH(int, compression);
        const QByteArray bytes = compressedPayload();
        const QString name = QStringLiteral("source.") + compressedSuffix(compression).toUpper();
        const QString path = makeCompressed(bytes, compression, name);
        QVERIFY(!path.isEmpty());
        controller_->selectImage(path);
        QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), analysisTimeoutMs);
        const auto& selected = controller_->selection();
        QCOMPARE(selected.imageState, rufus::ImageState::Complete);
        QVERIFY(selected.rawImage);
        QCOMPARE(selected.imagePath, path);
        QCOMPARE(selected.imageSize, quint64(QFileInfo(path).size()));
        QCOMPARE(selected.imageSource.imageCompression, CompressedFixtureAssociation(compression));
        QCOMPARE(selected.imageSource.decodedImageSize, quint64(bytes.size()));
        QCOMPARE(selected.imageSource.decodedImageSha256, QCryptographicHash::hash(bytes, QCryptographicHash::Sha256));
        QCOMPARE(selected.imageSource.imageSectorSize, 512U);
        QVERIFY(!selected.imageSource.ignoreBootMarker);
        QCOMPARE(selected.imageAnalysis.report.projected_size, uint64_t(bytes.size()));
        QCOMPARE(scanCalls.load(), 0);
        QCOMPARE(compressedCalls.load(), 1);
        QVERIFY(scanThread.load() != QThread::currentThread());
        QCOMPARE(progress_->value(), 0);
        QVERIFY(!volume_->isEnabled());
        QTest::mouseClick(select_, Qt::LeftButton);
        const auto* picker = window_->findChild<QFileDialog*>(QStringLiteral("imageDialog"));
        QVERIFY(picker && picker->nameFilters().first().contains(QStringLiteral("*.gz")));
        QVERIFY(picker->nameFilters().first().contains(QStringLiteral("*.xz")));
        QVERIFY(picker->nameFilters().first().contains(QStringLiteral("*.bz2")));
        QVERIFY(picker->nameFilters().first().contains(QStringLiteral("*.lzma")));
        QVERIFY(picker->nameFilters().first().contains(QStringLiteral("*.Z")));
        QVERIFY(picker->nameFilters().first().contains(QStringLiteral("*.z")));
        QVERIFY(picker->nameFilters().first().contains(QStringLiteral("*.zip")));
        QVERIFY(picker->nameFilters().first().contains(QStringLiteral("*.ZIP")));
        QVERIFY(picker->nameFilters().first().contains(QStringLiteral("*.zst")));
        QVERIFY(picker->nameFilters().first().contains(QStringLiteral("*.ZST")));
        QVERIFY(controller_->imageStatusText().startsWith(QStringLiteral("Compressed image selected")));
    }

    void vtsiSelectionFreezesLogicalAndLayoutIdentity()
    {
        QByteArray logical(4096, '\0');
        logical.replace(0, 512, QByteArray(512, 'V'));
        logical[510] = char(0x55);
        logical[511] = char(0xaa);
        logical.replace(3072, 512, QByteArray(512, 'S'));
        QByteArray layoutHash;
        const QByteArray packed = VtsiSparseFixture(logical, 512, 3072, 512, layoutHash);
        QVERIFY(!packed.isEmpty() && layoutHash.size() == 32);
        const QString path = directory_->filePath(QStringLiteral("source.VTSI"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(packed), qint64(packed.size()));
        file.close();
        controller_->selectImage(path);
        QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), analysisTimeoutMs);
        const auto& selected = controller_->selection();
        QCOMPARE(selected.imageState, rufus::ImageState::Complete);
        QVERIFY(selected.rawImage);
        QCOMPARE(selected.imageSource.imageCompression, int(BLED_COMPRESSION_VTSI));
        QCOMPARE(selected.imageSource.decodedImageSize, quint64(logical.size()));
        QCOMPARE(selected.imageSource.decodedImageSha256, QCryptographicHash::hash(logical, QCryptographicHash::Sha256));
        QCOMPARE(selected.imageSource.imageLayoutSha256, layoutHash);
        QCOMPARE(selected.imageAnalysis.report.projected_size, uint64_t(logical.size()));
        QCOMPARE(compressedCalls.load(), 1);
        QVERIFY(scanThread.load() != QThread::currentThread());
        QTest::mouseClick(select_, Qt::LeftButton);
        const auto* picker = window_->findChild<QFileDialog*>(QStringLiteral("imageDialog"));
        QVERIFY(picker && picker->nameFilters().first().contains(QStringLiteral("*.vtsi")));
        QVERIFY(picker->nameFilters().first().contains(QStringLiteral("*.VTSI")));
    }

    void vhdSelectionFreezesLogicalIdentity()
    {
        QByteArray logical(4096, 'V');
        logical[510] = char(0x55);
        logical[511] = char(0xaa);
        const QByteArray packed = VhdFixedFixture(logical);
        QVERIFY(!packed.isEmpty());
        const QString path = directory_->filePath(QStringLiteral("source.VHD"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(packed), qint64(packed.size()));
        file.close();
        controller_->selectImage(path);
        QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), analysisTimeoutMs);
        const auto& selected = controller_->selection();
        QCOMPARE(selected.imageState, rufus::ImageState::Complete);
        QVERIFY(selected.rawImage);
        QCOMPARE(selected.imageSource.imageCompression, int(IMG_COMPRESSION_VHD));
        QCOMPARE(selected.imageSource.decodedImageSize, quint64(logical.size()));
        QCOMPARE(selected.imageSource.decodedImageSha256,
            QCryptographicHash::hash(logical, QCryptographicHash::Sha256));
        QCOMPARE(selected.imageAnalysis.report.projected_size, uint64_t(logical.size()));
        QCOMPARE(compressedCalls.load(), 1);
        QTest::mouseClick(select_, Qt::LeftButton);
        const auto* picker = window_->findChild<QFileDialog*>(QStringLiteral("imageDialog"));
        QVERIFY(picker && picker->nameFilters().first().contains(QStringLiteral("*.vhd")));
        QVERIFY(picker->nameFilters().first().contains(QStringLiteral("*.VHD")));
    }

    void bzip2CorruptEmptyMembersAreRejected_data()
    {
        QTest::addColumn<QString>("placement");
        for (const char* placement : {"only", "first", "last", "middle"})
            QTest::newRow(placement) << QString::fromLatin1(placement);
    }

    void bzip2CorruptEmptyMembersAreRejected()
    {
        QFETCH(QString, placement);
        const QString path = makeCompressed(compressedPayload(), BLED_COMPRESSION_BZIP2, QStringLiteral("source.bz2"));
        const QString emptyPath = makeCompressed({}, BLED_COMPRESSION_BZIP2, QStringLiteral("empty.bz2"));
        QVERIFY(!path.isEmpty() && !emptyPath.isEmpty());
        QFile source(path), emptyFile(emptyPath);
        QVERIFY(source.open(QIODevice::ReadWrite) && emptyFile.open(QIODevice::ReadOnly));
        const QByteArray valid = source.readAll();
        QByteArray empty = emptyFile.readAll();
        QCOMPARE(empty.size(), 14);
        empty[empty.size() - 1] ^= 1;
        const bool prefix = placement == "last" || placement == "middle";
        const bool suffix = placement == "first" || placement == "middle";
        const QByteArray damaged = (prefix ? valid : QByteArray()) + empty + (suffix ? valid : QByteArray());
        QVERIFY(source.resize(0) && source.seek(0));
        QCOMPARE(source.write(damaged), qint64(damaged.size()));
        source.close();
        controller_->selectImage(path);
        QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), analysisTimeoutMs);
        QCOMPARE(controller_->selection().imageState, rufus::ImageState::Failed);
        QVERIFY(!controller_->selection().imageSource.hasCompressedImage());
        QVERIFY(!controller_->canStartWrite());
        QCOMPARE(scanCalls.load(), 0);
        QCOMPARE(compressedCalls.load(), 1);
    }

    void compressedMalformedOrUnmarked_data()
    {
        QTest::addColumn<QString>("kind");
        QTest::addColumn<int>("compression");
        for (int compression : CompressedFixtureKinds)
            for (const char* kind : {"crc", "truncated", "marker", "short", "unsupported"})
            {
                const bool lzw = compression == BLED_COMPRESSION_LZW || compression == LzwGzipFixture;
                const QString failure = lzw && QByteArray(kind) == "crc" ? QStringLiteral("code")
                    : lzw && QByteArray(kind) == "truncated" ? QStringLiteral("header")
                    : compression == BLED_COMPRESSION_LZMA && QByteArray(kind) == "crc"
                    ? QStringLiteral("size") : QString::fromLatin1(kind);
                const QByteArray row = compressedLabel(compression) + "-" + failure.toLatin1();
                QTest::newRow(row.constData()) << failure << compression;
            }
        QTest::newRow("lzma-properties") << QStringLiteral("properties") << int(BLED_COMPRESSION_LZMA);
        QTest::newRow("lzma-history") << QStringLiteral("history") << int(BLED_COMPRESSION_LZMA);
    }

    void compressedMalformedOrUnmarked()
    {
        QFETCH(QString, kind);
        QFETCH(int, compression);
        QByteArray bytes = compressedPayload();
        if (kind == "marker") bytes[510] = 0;
        if (kind == "short") bytes.resize(511);
        const QString name = kind == "unsupported" && compression != BLED_COMPRESSION_ZIP
            ? QStringLiteral("source.zip")
            : QStringLiteral("source.") + compressedSuffix(compression);
        const QString path = makeCompressed(bytes, compression, name);
        QVERIFY(!path.isEmpty());
        if (kind == "crc")
            QVERIFY(corruptCompressedCheck(path, compression));
        else if (kind == "code" || kind == "header")
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::ReadWrite));
            if (kind == "header") QVERIFY(file.resize(2));
            else
            {
                QVERIFY(file.seek(3));
                QCOMPARE(file.write(QByteArray::fromHex("ff01")), qint64(2));
            }
        }
        else if (kind == "size" || kind == "properties" || kind == "history")
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::ReadWrite));
            if (kind == "size")
            {
                QByteArray size(8, '\0');
                for (unsigned index = 0; index < 8; index++) size[int(index)] = char((quint64(bytes.size()) + 1) >> (8 * index));
                QVERIFY(file.seek(5));
                QCOMPARE(file.write(size), qint64(size.size()));
            }
            else if (kind == "properties")
                QCOMPARE(file.write(QByteArray(1, char(225))), qint64(1));
            else
            {
                const QByteArray history = QByteArray::fromHex("5d00100000010000000000000000c0000000") + QByteArray(32, '\0');
                QVERIFY(file.resize(0));
                QCOMPARE(file.write(history), qint64(history.size()));
            }
        }
        else if (kind == "truncated")
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::ReadWrite));
            // The original LZMA decoder may leave a final range flush byte
            // unread. Remove consumed EOS data rather than assume that byte.
            QVERIFY(file.resize(file.size() - (compression == BLED_COMPRESSION_LZMA ? 8 : 1)));
        }
        else if (kind == "unsupported" && compression == BLED_COMPRESSION_ZIP)
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::ReadWrite) && file.seek(8));
            QCOMPARE(file.write(QByteArray::fromHex("6300")), qint64(2));
        }
        controller_->selectImage(path);
        QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), analysisTimeoutMs);
        QCOMPARE(controller_->selection().imageState, rufus::ImageState::Failed);
        QVERIFY(controller_->selection().imagePath.isEmpty());
        QVERIFY(!controller_->selection().imageProblem.isEmpty());
        QVERIFY(!controller_->selection().imageSource.hasCompressedImage());
        QVERIFY(!controller_->canStartWrite());
        QCOMPARE(scanCalls.load(), 0);
    }

    void compressedCancellationAndQueuedReplacement_data()
    {
        QTest::addColumn<bool>("replace");
        QTest::addColumn<int>("compression");
        QTest::addColumn<int>("secondCompression");
        for (int compression : CompressedFixtureKinds)
        {
            const QByteArray cancelled = compressedLabel(compression) + "-cancel";
            QTest::newRow(cancelled.constData()) << false << compression << compression;
            for (int second : CompressedFixtureKinds)
                if (second != compression)
                {
                    const QByteArray row = compressedLabel(compression) + "-to-" + compressedLabel(second);
                    QTest::newRow(row.constData()) << true << compression << second;
                }
        }
    }

    void compressedCancellationAndQueuedReplacement()
    {
        QFETCH(bool, replace);
        QFETCH(int, compression);
        QFETCH(int, secondCompression);
        const QString first = makeCompressed(compressedPayload(), compression,
            QStringLiteral("first.") + compressedSuffix(compression));
        QByteArray secondBytes = compressedPayload();
        secondBytes.append("second");
        const QString second = makeCompressed(secondBytes, secondCompression,
            QStringLiteral("second.") + compressedSuffix(secondCompression));
        nextGate = AfterScan;
        controller_->selectImage(first);
        QVERIFY(entered.tryAcquire(1, workerTimeoutMs));
        QVERIFY(controller_->isAnalyzing());
        QVERIFY(!controller_->canStartWrite());
        if (replace) controller_->selectImage(second);
        else controller_->cancelAnalysis();
        releaseScan.release();
        QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), analysisTimeoutMs);
        if (replace)
        {
            QCOMPARE(controller_->selection().imageState, rufus::ImageState::Complete);
            QCOMPARE(controller_->selection().imagePath, second);
            QCOMPARE(controller_->selection().imageSource.imageCompression, CompressedFixtureAssociation(secondCompression));
            QCOMPARE(controller_->selection().imageSource.decodedImageSha256, QCryptographicHash::hash(secondBytes, QCryptographicHash::Sha256));
        }
        else
        {
            QCOMPARE(controller_->selection().imageState, rufus::ImageState::Cancelled);
            QVERIFY(controller_->selection().imagePath.isEmpty());
            QVERIFY(!controller_->selection().imageSource.hasCompressedImage());
        }
    }

    void compressedSourceChangeAfterDecodeIsRejected_data()
    {
        QTest::addColumn<int>("compression");
        QTest::newRow("gzip") << int(BLED_COMPRESSION_GZIP);
        QTest::newRow("xz") << int(BLED_COMPRESSION_XZ);
        QTest::newRow("bzip2") << int(BLED_COMPRESSION_BZIP2);
        QTest::newRow("lzma") << int(BLED_COMPRESSION_LZMA);
        QTest::newRow("lzw") << int(BLED_COMPRESSION_LZW);
        QTest::newRow("zip") << int(BLED_COMPRESSION_ZIP);
        QTest::newRow("zstd") << int(BLED_COMPRESSION_ZSTD);
        QTest::newRow("lzw-gzip") << LzwGzipFixture;
    }

    void compressedSourceChangeAfterDecodeIsRejected()
    {
        QFETCH(int, compression);
        const QString path = makeCompressed(compressedPayload(), compression,
            QStringLiteral("source.") + compressedSuffix(compression));
        nextGate = AfterScan;
        controller_->selectImage(path);
        QVERIFY(entered.tryAcquire(1, workerTimeoutMs));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Append));
        QCOMPARE(file.write("x", 1), qint64(1));
        file.close();
        releaseScan.release();
        QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), analysisTimeoutMs);
        QCOMPARE(controller_->selection().imageState, rufus::ImageState::Failed);
        QVERIFY(controller_->selection().imagePath.isEmpty());
    }

    void persistenceControlsAndReset()
    {
        QProcess bus;
        bus.start(QStringLiteral("/usr/bin/dbus-daemon"), {QStringLiteral("--session"),
            QStringLiteral("--nofork"), QStringLiteral("--print-address=1")});
        QVERIFY(bus.waitForReadyRead());
        const QString busName = QStringLiteral("persistence-controls");
        const auto connection = QDBusConnection::connectToBus(QString::fromUtf8(bus.readLine()).trimmed(), busName);
        QVERIFY(connection.isConnected());
        controller_->isoWriter().configure(connection, QStringLiteral("/usr/bin/true"));
        rufus::Device device;
        device.id = QStringLiteral("persistence-target");
        device.path = QStringLiteral("/dev/unused-persistence-fixture");
        device.size = 1024ULL * 1024 * 1024;
        device.defaultVisible = true;
        device.identity = {QStringLiteral("persistence-fixture")};
        controller_->setDiscovery({device}, {});
        const QString image = makeImage(QStringLiteral("LIVE"), {
            {QStringLiteral("isolinux/isolinux.cfg"), QByteArray("append boot=live\n")},
            {QStringLiteral("isolinux/isolinux.bin"), QByteArray(512, '\0')}});
        QVERIFY(!image.isEmpty());
        QFile hybrid(image);
        QVERIFY(hybrid.open(QIODevice::ReadWrite) && hybrid.seek(510));
        QCOMPARE(hybrid.write(QByteArray::fromHex("55aa")), 2);
        hybrid.close();
        controller_->selectImage(image);
        QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), analysisTimeoutMs);
        QVERIFY(controller_->canSetPersistence());
        QCOMPARE(controller_->isoWriteChoices(0, false, true).count, 2);
        const quint64 maximum = controller_->maximumPersistenceSize();
        QVERIFY(maximum >= 256ULL * 1024 * 1024);
        QVERIFY(maximum < device.size);
        auto slider = window_->findChild<QSlider*>(QStringLiteral("persistenceSlider"));
        auto size = window_->findChild<QLineEdit*>(QStringLiteral("persistenceSize"));
        auto units = window_->findChild<QComboBox*>(QStringLiteral("persistenceUnits"));
        QVERIFY(slider && size && units);
        QVERIFY(slider->isVisible() && slider->isEnabled());
        QCOMPARE(controller_->selection().persistenceSize, quint64(0));
        size->setText(QStringLiteral("256"));
        QVERIFY(QMetaObject::invokeMethod(size, "editingFinished"));
        QCOMPARE(controller_->selection().persistenceSize, 256ULL * 1024 * 1024);
        const QString screenshot = qEnvironmentVariable("RUFUS_TEST_PERSISTENCE_SCREENSHOT");
        if (!screenshot.isEmpty())
            QVERIFY(window_->grab().save(screenshot));
        units->setCurrentIndex(1);
        QVERIFY(QMetaObject::invokeMethod(units, "activated", Q_ARG(int, 1)));
        QCOMPARE(QLocale().toDouble(size->text()), 0.25);
        controller_->setPersistenceSize(278ULL * 1024 * 1024);
        QVERIFY(QMetaObject::invokeMethod(size, "editingFinished"));
        QCOMPARE(controller_->selection().persistenceSize, 278ULL * 1024 * 1024);
        slider->setValue(1000);
        QCOMPARE(controller_->selection().persistenceSize, maximum);
        const auto choices = controller_->isoWriteChoices(maximum, false, true);
        QVERIFY(choices.count != 0);
        for (int i = 0; i < choices.count; ++i)
            QVERIFY(choices.mode[i] != IMAGE_WRITE_DD);
        device.id = QStringLiteral("smaller-persistence-target");
        device.size /= 2;
        controller_->setDiscovery({device}, {});
        controller_->selectDevice(device.id);
        QVERIFY(controller_->selection().persistenceSize <= controller_->maximumPersistenceSize());
        controller_->setPersistenceSize(256ULL * 1024 * 1024);
        controller_->selectImage(image);
        QCOMPARE(controller_->selection().persistenceSize, quint64(0));
        QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), analysisTimeoutMs);
        controller_->setPersistenceSize(256ULL * 1024 * 1024);
        controller_->setBootType(BT_NON_BOOTABLE);
        QCOMPARE(controller_->selection().persistenceSize, quint64(0));
        QVERIFY(!slider->isVisible());
        QDBusConnection::disconnectFromBus(busName);
        bus.terminate();
        QVERIFY(bus.waitForFinished());
    }

    void selectRunsTheRealScannerAndShowsItsReport()
    {
        const QString image = makeImage(QStringLiteral("TEST_LINUX"), {
            {QStringLiteral("isolinux/isolinux.cfg"), QByteArray("default linux\n")},
            {QStringLiteral("isolinux/isolinux.bin"), QByteArray(512, '\0')}});
        QVERIFY(!image.isEmpty());
        QTest::mouseClick(select_, Qt::LeftButton);
        QFileDialog* picker = window_->findChild<QFileDialog*>(QStringLiteral("imageDialog"));
        QVERIFY(picker && picker->isVisible());
        picker->setDirectory(directory_->path());
        picker->selectFile(image);
        QDialogButtonBox* buttons = picker->findChild<QDialogButtonBox*>();
        QVERIFY(buttons && buttons->button(QDialogButtonBox::Open));
        QTRY_VERIFY_WITH_TIMEOUT(buttons->button(QDialogButtonBox::Open)->isEnabled(), uiTimeoutMs);
        QTest::mouseClick(buttons->button(QDialogButtonBox::Open), Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(controller_->selection().imageState, rufus::ImageState::Complete, analysisTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), analysisTimeoutMs);
        const ISO_ANALYSIS& analysis = controller_->selection().imageAnalysis;
        QCOMPARE(analysis.files, uint64_t(2));
        QVERIFY(analysis.tree_complete);
        QCOMPARE(analysis.report.sl_detected_version, uint16_t(0));
        QCOMPARE(analysis.report.sl_version, uint16_t(0x0407));
        QVERIFY(analysis.report.sl_version_inferred);
        QCOMPARE(volume_->text(), QStringLiteral("TEST_LINUX"));
        QVERIFY(!volume_->isEnabled() && !start_->isEnabled());
        QVERIFY(select_->isEnabled());
        QCOMPARE(close_->text(), QStringLiteral("CLOSE"));
        QCOMPARE(progress_->maximum(), 100);
        QCOMPARE(progress_->value(), 0); // Analysis is not a completed media write.
        QCOMPARE(progress_->format(), QStringLiteral("READY"));
        QVERIFY(log_->toPlainText().contains(QStringLiteral("Syslinux image version: unknown")));
        QVERIFY(log_->toPlainText().contains(QStringLiteral("inferred; native installer")));
        QVERIFY(controller_->imageStatusText().contains(QStringLiteral("complete")));
        const QString screenshot = qEnvironmentVariable("RUFUS_TEST_ANALYSIS_SCREENSHOT");
        if (!screenshot.isEmpty())
            QVERIFY(window_->grab().save(screenshot));
    }

    void partialAnalysisKeepsChecksExplicit()
    {
        const QString image = makeImage(QStringLiteral("EFI_PREVIEW"), {
            {QStringLiteral("EFI/BOOT/bootx64.efi"), QByteArray(512, '\0')}});
        QVERIFY(!image.isEmpty());
        controller_->selectImage(image);
        QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), analysisTimeoutMs);
        QCOMPARE(controller_->selection().imageState, rufus::ImageState::Partial);
        QCOMPARE(controller_->selection().imageAnalysis.pending, uint32_t(ISO_PENDING_SECURE_BOOT));
        QCOMPARE(volume_->text(), QStringLiteral("EFI_PREVIEW"));
        QCOMPARE(progress_->format(), QStringLiteral("INCOMPLETE"));
        QVERIFY(controller_->imageStatusText().contains(QStringLiteral("Secure Boot checks")));
        QVERIFY(!start_->isEnabled());
    }

    void nonBootableIsoIsRejectedButDdMarkerRemainsUsable()
    {
        const QString image = makeImage(QStringLiteral("DATA_ONLY"), {
            {QStringLiteral("payload"), QByteArray("not a bootloader")}});
        QVERIFY(!image.isEmpty());
        controller_->selectImage(image);
        QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), analysisTimeoutMs);
        QCOMPARE(controller_->selection().imageState, rufus::ImageState::Unsupported);
        QVERIFY(controller_->selection().imagePath.isEmpty());
        QVERIFY(controller_->selection().imageSource.imagePath.isEmpty());
        QVERIFY(volume_->text().isEmpty());
        QCOMPARE(window_->findChild<QLabel*>(QStringLiteral("statusLabel"))->text(), QStringLiteral("No image selected"));
        QCOMPARE(progress_->format(), QStringLiteral("READY"));
        QCOMPARE(progress_->value(), 0);
        auto* notice = window_->findChild<QMessageBox*>(QStringLiteral("imageNotice"));
        QVERIFY(notice && notice->isVisible());
        QCOMPARE(notice->windowTitle(), QStringLiteral("Unsupported image"));
        QTest::mouseClick(notice->button(QMessageBox::Ok), Qt::LeftButton);
        QFile file(image);
        QVERIFY(file.open(QIODevice::ReadWrite) && file.seek(510));
        QCOMPARE(file.write(QByteArray::fromHex("55aa")), qint64(2));
        file.close();
        controller_->selectImage(image);
        QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), analysisTimeoutMs);
        QCOMPARE(controller_->selection().imageState, rufus::ImageState::Complete);
        const IMAGE_WRITE_CHOICES choices = controller_->isoWriteChoices(0, true, true);
        QCOMPARE(choices.count, 1);
        QCOMPARE(choices.mode[0], IMAGE_WRITE_DD);
        QCOMPARE(controller_->selection().imagePath, image);
        QVERIFY(notice->isVisible());
        QCOMPARE(notice->windowTitle(), QStringLiteral("ISOHybrid image detected"));
    }

    void hybridDetectionDoesNotRecommendExtraction_data()
    {
        QTest::addColumn<QString>("label");
        QTest::addColumn<bool>("marker");
        QTest::addColumn<bool>("requiresDd");
        QTest::newRow("arch-hybrid") << QStringLiteral("ARCH_202609") << true << false;
        QTest::newRow("cachyos-hybrid") << QStringLiteral("CACHYOS") << true << false;
        QTest::newRow("unrecognized-label") << QStringLiteral("TEST_HYBRID") << true << false;
        QTest::newRow("upstream-dd-required") << QStringLiteral("Install-SUSE") << true << true;
        QTest::newRow("same-label-without-marker") << QStringLiteral("CACHYOS") << false << false;
    }

    void hybridDetectionDoesNotRecommendExtraction()
    {
        QFETCH(QString, label);
        QFETCH(bool, marker);
        QFETCH(bool, requiresDd);
        const QString image = makeImage(label, {
            {QStringLiteral("isolinux/isolinux.cfg"), QByteArray("default linux\n")},
            {QStringLiteral("isolinux/isolinux.bin"), QByteArray(512, '\0')}});
        QVERIFY(!image.isEmpty());
        if (marker)
        {
            QFile file(image);
            QVERIFY(file.open(QIODevice::ReadWrite) && file.seek(510));
            QCOMPARE(file.write("\x55\xaa", 2), qint64(2));
            // libarchive normalizes PVD labels; reproduce the actual mixed-case
            // label expected by Rufus's existing SUSE exception.
            if (requiresDd)
            {
                const QByteArray volume = label.toLatin1().leftJustified(32, ' ');
                QVERIFY(file.seek(16 * 2048 + 40));
                QCOMPARE(file.write(volume), qint64(32));
            }
        }
        controller_->selectImage(image);
        QCOMPARE(controller_->isoWriteChoices(0, false, true).count, 0);
        QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), analysisTimeoutMs);
        const auto& selection = controller_->selection();
        QCOMPARE(selection.imageState, rufus::ImageState::Complete);
        QCOMPARE(selection.imageAnalysis.report.is_bootable_img, int8_t(marker ? 1 : 0));
        QVERIFY(selection.imageAnalysis.report.is_iso);
        QCOMPARE(bool(selection.imageAnalysis.report.disable_iso), requiresDd);
        QCOMPARE(controller_->canSetPersistence(), !requiresDd);
        if (requiresDd)
            QVERIFY(window_->findChild<QWidget*>(QStringLiteral("persistenceSlider"))->isHidden());
        QVERIFY(!selection.rawImage && !controller_->canStartWrite() && !start_->isEnabled());
        const IMAGE_WRITE_CHOICES choices = controller_->isoWriteChoices(0, false, true);
        QCOMPARE(choices.count, marker && !requiresDd ? 2 : 1);
        QCOMPARE(choices.mode[0], requiresDd ? IMAGE_WRITE_DD : IMAGE_WRITE_ISO);
        QCOMPARE(choices.mode[1], marker && !requiresDd ? IMAGE_WRITE_DD : IMAGE_WRITE_NONE);
        const IMAGE_WRITE_CHOICES persistent = controller_->isoWriteChoices(4096, true, true);
        QCOMPARE(persistent.count, 1);
        QCOMPARE(persistent.mode[0], requiresDd ? IMAGE_WRITE_DD : IMAGE_WRITE_ISO);
        if (choices.count > 1)
        {
            rufus::ImageModeDialog dialog(choices, window_.get());
            dialog.open();
            QVERIFY(QTest::qWaitForWindowExposed(&dialog));
            QTest::keyClick(&dialog, Qt::Key_Return);
            QCOMPARE(dialog.selectedMode(), IMAGE_WRITE_ISO);
            QVERIFY(!controller_->canStartWrite());
            QCOMPARE(controller_->writer().status().state, rufus::WriteState::Idle);
        }
        const QString log = log_->toPlainText();
        QVERIFY(log.contains(QStringLiteral("Native ISO-mode installer compatibility is unchecked")));
        if (marker)
        {
            QVERIFY(log.contains(QStringLiteral("ISOHybrid detected")));
            QVERIFY(controller_->imageStatusText().contains(QStringLiteral("ISOHybrid")));
            QVERIFY(log.contains(requiresDd ? QStringLiteral("requires DD mode for this image")
                : QStringLiteral("does not require DD mode; this is not an ISO-mode recommendation")));
        }
        else
            QVERIFY(log.contains(QStringLiteral("not a DD candidate")));
    }

    void markerOverrideRetainsIsoPolicy()
    {
        const QString image = makeImage(QStringLiteral("FORCED_ISO"), {
            {QStringLiteral("isolinux/isolinux.cfg"), QByteArray("default linux\n")},
            {QStringLiteral("isolinux/isolinux.bin"), QByteArray(512, '\0')}});
        QVERIFY(!image.isEmpty());
        controller_->toggleBootMarker();
        controller_->selectImage(image);
        QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), analysisTimeoutMs);
        const auto& selected = controller_->selection();
        QCOMPARE(selected.imageState, rufus::ImageState::Complete);
        QVERIFY(selected.imageAnalysis.tree_complete && !selected.rawImage);
        QCOMPARE(selected.imageAnalysis.report.is_bootable_img, int8_t(2));
        QVERIFY(selected.imageSource.ignoreBootMarker);
        const auto choices = controller_->isoWriteChoices(0, false, true);
        QCOMPARE(choices.count, 2);
        QCOMPARE(choices.mode[0], IMAGE_WRITE_ISO);
    }

    void bootMarkerAloneSelectsRawWithoutAnIsoTree()
    {
        const QString image = directory_->filePath(QStringLiteral("CachyOS.iso"));
        QFile file(image);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QByteArray data(65536, '\0');
        data[510] = '\x55';
        data[511] = '\xaa';
        QCOMPARE(file.write(data), qint64(data.size()));
        file.close();
        controller_->selectImage(image);
        QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), analysisTimeoutMs);
        QCOMPARE(controller_->selection().imageState, rufus::ImageState::Complete);
        QVERIFY(controller_->selection().rawImage);
        QCOMPARE(controller_->selection().imageAnalysis.report.is_bootable_img, int8_t(1));
        QVERIFY(!controller_->selection().imageAnalysis.tree_complete);
        QVERIFY(!log_->toPlainText().contains(QStringLiteral("ISOHybrid detected")));
        QVERIFY(!controller_->canStartWrite());
    }

    void windowsMetadataUsesTheExistingForm_data()
    {
        QTest::addColumn<bool>("udf");
        QTest::addColumn<QString>("kind");
        const char* kinds[] = {"win11", "multi", "unknown", "invalid", "no-revision", "corrupt"};
        for (const char* kind : kinds)
        {
            const QByteArray isoName = QByteArray("ISO-") + kind;
            const QByteArray udfName = QByteArray("UDF-") + kind;
            QTest::newRow(isoName.constData()) << false << QString::fromLatin1(kind);
            QTest::newRow(udfName.constData()) << true << QString::fromLatin1(kind);
        }
    }

    void windowsMetadataUsesTheExistingForm()
    {
        QFETCH(bool, udf);
        QFETCH(QString, kind);
        const bool corrupt = kind == QStringLiteral("corrupt");
        const bool known = kind == QStringLiteral("win11") || kind == QStringLiteral("multi");
        const QByteArray wim = corrupt ? QByteArray("not a WIM") : makeWim(kind);
        QVERIFY(!wim.isEmpty());
        const QString image = makeImage(QStringLiteral("WINDOWS_TEST"), {
            {QStringLiteral("BootMgr"), QByteArray("fixture")},
            {QStringLiteral("Sources/Install.wim"), wim}}, udf);
        QVERIFY(!image.isEmpty());
        controller_->selectImage(image);
        QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), workerTimeoutMs);
        const rufus::Selection& selection = controller_->selection();
        QCOMPARE(selection.imageState, known ? rufus::ImageState::Complete : rufus::ImageState::Partial);
        QCOMPARE(selection.windowsAnalysis.state,
            known ? WINDOWS_IMAGE_KNOWN : corrupt ? WINDOWS_IMAGE_FAILED : WINDOWS_IMAGE_UNKNOWN);
        QCOMPARE(selection.imageAnalysis.report.wininst_version, corrupt ? uint32_t(0) : uint32_t(0x10d00));
        QCOMPARE(selection.imageAnalysis.pending, known ? uint32_t(0) : uint32_t(ISO_PENDING_WINDOWS));
        QCOMPARE(volume_->text(), QStringLiteral("WINDOWS_TEST"));
        QCOMPARE(progress_->format(), known ? QStringLiteral("READY") : QStringLiteral("INCOMPLETE"));
        QVERIFY(!start_->isEnabled());
        if (known)
        {
            const uint16_t major = kind == QStringLiteral("multi") ? 7 : 11;
            QCOMPARE(selection.imageAnalysis.report.win_version.major, major);
            QVERIFY(log_->toPlainText().contains(QStringLiteral("Windows version: %1.0").arg(major)));
            if (kind == QStringLiteral("multi"))
            {
                QCOMPARE(selection.windowsAnalysis.metadata.archive.image_count, uint32_t(2));
                QCOMPARE(selection.windowsAnalysis.metadata.archive.boot_index, uint32_t(2));
            }
        }
        else if (corrupt)
        {
            QVERIFY(selection.windowsAnalysis.wim_error != 0);
            QVERIFY(controller_->imageStatusText().contains(QStringLiteral("unreadable installation image")));
        }
        else
        {
            QVERIFY(controller_->imageStatusText().contains(QStringLiteral("version unknown or incomplete")));
            QVERIFY(log_->toPlainText().contains(kind == QStringLiteral("no-revision")
                ? QStringLiteral("revision unknown") : QStringLiteral("Windows version: unknown")));
        }
        // A later non-Windows selection must not inherit archive/version state.
        const QString next = makeImage(QStringLiteral("AFTER_WINDOWS"), {}, udf);
        QVERIFY(!next.isEmpty());
        controller_->selectImage(next);
        QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), analysisTimeoutMs);
        QCOMPARE(controller_->selection().windowsAnalysis.state, WINDOWS_IMAGE_UNCHECKED);
        QCOMPARE(controller_->selection().imageAnalysis.report.wininst_version, uint32_t(0));
        QCOMPARE(controller_->selection().imageAnalysis.report.win_version.major, uint16_t(0));
    }

    void windowsMetadataCancellationAndReplacement_data()
    {
        QTest::addColumn<int>("gate");
        QTest::addColumn<bool>("replace");
        QTest::newRow("cancel-before-open") << int(BeforeWim) << false;
        QTest::newRow("cancel-after-metadata") << int(AfterWim) << false;
        QTest::newRow("replace-after-metadata") << int(AfterWim) << true;
    }

    void windowsMetadataCancellationAndReplacement()
    {
        QFETCH(int, gate);
        QFETCH(bool, replace);
        const QByteArray wim = makeWim(QStringLiteral("win11"));
        QVERIFY(!wim.isEmpty());
        const QString image = makeImage(QStringLiteral("WIM_CANCEL"), {
            {QStringLiteral("BootMgr"), QByteArray("fixture")},
            {QStringLiteral("Sources/Install.wim"), wim}}, true);
        const QString next = makeImage(QStringLiteral("REPLACEMENT"), {});
        QVERIFY(!image.isEmpty() && !next.isEmpty());
        wimGate = gate;
        controller_->selectImage(image);
        QTRY_COMPARE_WITH_TIMEOUT(entered.available(), 1, analysisTimeoutMs);
        QCOMPARE(close_->text(), QStringLiteral("CANCEL"));
        if (replace)
            controller_->selectImage(next);
        else
            QTest::mouseClick(close_, Qt::LeftButton);
        messages_.clear();
        releaseScan.release();
        QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), workerTimeoutMs);
        QCOMPARE(controller_->selection().imageState,
            replace ? rufus::ImageState::Complete : rufus::ImageState::Cancelled);
        QCOMPARE(controller_->selection().windowsAnalysis.state, WINDOWS_IMAGE_UNCHECKED);
        QCOMPARE(controller_->selection().imageAnalysis.report.win_version.major, uint16_t(0));
        QVERIFY(!messages_.join(QLatin1Char('\n')).contains(QStringLiteral("Windows version: 11")));
        QVERIFY(!start_->isEnabled());
        controller_->selectImage(image);
        QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), workerTimeoutMs);
        QCOMPARE(controller_->selection().windowsAnalysis.state, WINDOWS_IMAGE_KNOWN);
    }

    void winPeOptionsUseTheExistingStatusAndLog_data()
    {
        QTest::addColumn<bool>("udf");
        QTest::newRow("ISO9660") << false;
        QTest::newRow("UDF") << true;
    }

    void winPeOptionsUseTheExistingStatusAndLog()
    {
        QFETCH(bool, udf);
        const QString image = makeImage(QStringLiteral("WINPE"), {
            {QStringLiteral("I386/NTDETECT.COM"), QByteArray("fixture")},
            {QStringLiteral("I386/SETUPLDR.BIN"), QByteArray("fixture")},
            {QStringLiteral("I386/TXTSETUP.SIF"), QByteArray("OsLoadOptions=\"/MININT /fastdetect\"\r\n")}}, udf);
        QVERIFY(!image.isEmpty());
        controller_->selectImage(image);
        QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), analysisTimeoutMs);
        QCOMPARE(controller_->selection().imageState, rufus::ImageState::Complete);
        QVERIFY(controller_->selection().imageAnalysis.report.winpe_options_known);
        QVERIFY(controller_->selection().imageAnalysis.report.uses_minint);
        QVERIFY(log_->toPlainText().contains(QStringLiteral("OsLoadOptions = /minint /fastdetect")));
        QCOMPARE(volume_->text(), QStringLiteral("WINPE"));
        QCOMPARE(progress_->format(), QStringLiteral("READY"));
        QVERIFY(rufus::IsLegacyWinPeImage(controller_->selection().imageAnalysis));
        QVERIFY(!start_->isEnabled()); // This analysis fixture has no target.

        const QString unreadable = makeImage(QStringLiteral("WINPE_PARTIAL"), {
            {QStringLiteral("i386/ntdetect.com"), QByteArray("fixture")},
            {QStringLiteral("i386/setupldr.bin"), QByteArray("fixture")},
            {QStringLiteral("i386/txtsetup.sif"), QByteArray()}}, udf);
        QVERIFY(!unreadable.isEmpty());
        controller_->selectImage(unreadable);
        QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), analysisTimeoutMs);
        QCOMPARE(controller_->selection().imageState, rufus::ImageState::Partial);
        QCOMPARE(controller_->selection().imageAnalysis.pending, uint32_t(ISO_PENDING_WINDOWS));
        QVERIFY(!controller_->selection().imageAnalysis.report.winpe_options_known);
        QVERIFY(!controller_->selection().imageAnalysis.report.uses_minint);
        QVERIFY(controller_->imageStatusText().contains(QStringLiteral("Windows metadata")));

        nextGate = AfterScan;
        controller_->selectImage(image);
        QTRY_COMPARE_WITH_TIMEOUT(entered.available(), 1, analysisTimeoutMs);
        QTest::mouseClick(close_, Qt::LeftButton);
        releaseScan.release();
        QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), analysisTimeoutMs);
        QCOMPARE(controller_->selection().imageState, rufus::ImageState::Cancelled);
        QVERIFY(!controller_->selection().imageAnalysis.report.uses_minint);
        QVERIFY(volume_->text().isEmpty() && !start_->isEnabled());
    }

    void udfUsesTheSharedWorkerAndKeepsWindowsChecksPending()
    {
        const QString image = makeImage(QStringLiteral("UDF_TEST"), {
            {QStringLiteral("syslinux.cfg"), QByteArray("default linux\n")},
            {QString::fromUtf8("ș-é/data"), QByteArray("unicode path")}}, true);
        QVERIFY(!image.isEmpty());
        controller_->selectImage(image);
        QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), analysisTimeoutMs);
        QCOMPARE(controller_->selection().imageState, rufus::ImageState::Complete);
        const ISO_ANALYSIS& analysis = controller_->selection().imageAnalysis;
        QCOMPARE(analysis.filesystem, ISO_FILESYSTEM_UDF);
        QCOMPARE(analysis.files, uint64_t(2));
        QVERIFY(analysis.tree_complete && analysis.volume_size_known);
        QCOMPARE(analysis.report.sl_detected_version, uint16_t(0));
        QCOMPARE(analysis.report.sl_version, uint16_t(0x0407));
        QVERIFY(analysis.report.sl_version_inferred);
        QCOMPARE(volume_->text(), QStringLiteral("UDF_TEST"));
        QVERIFY(controller_->imageStatusText().startsWith(QStringLiteral("UDF analysis complete")));
        QVERIFY(!start_->isEnabled());

        const QString windows = makeImage(QStringLiteral("UDF_WINDOWS"), {
            {QStringLiteral("bootmgr"), QByteArray(512, '\0')},
            {QStringLiteral("sources/install.wim"), QByteArray(512, '\0')}}, true);
        QVERIFY(!windows.isEmpty());
        controller_->selectImage(windows);
        QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), analysisTimeoutMs);
        QCOMPARE(controller_->selection().imageState, rufus::ImageState::Partial);
        QCOMPARE(controller_->selection().imageAnalysis.pending, uint32_t(ISO_PENDING_WINDOWS));
        QCOMPARE(progress_->format(), QStringLiteral("INCOMPLETE"));
        QVERIFY(controller_->imageStatusText().contains(QStringLiteral("Windows metadata")));
        QVERIFY(!start_->isEnabled());
    }

    void embeddedEfiSeparatesPresenceFromPendingValidation_data()
    {
        QTest::addColumn<bool>("udf");
        QTest::newRow("ISO9660") << false;
        QTest::newRow("UDF") << true;
    }

    void embeddedEfiSeparatesPresenceFromPendingValidation()
    {
        QFETCH(bool, udf);
        const QByteArray fat = makeFatImage(QStringLiteral("loader"), true);
        QVERIFY(!fat.isEmpty());
        const QString image = makeImage(QStringLiteral("EMBEDDED_EFI"), {
            {QStringLiteral("boot/efi.img"), fat}}, udf);
        QVERIFY(!image.isEmpty());
        controller_->selectImage(image);
        QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), analysisTimeoutMs);
        const ISO_ANALYSIS& analysis = controller_->selection().imageAnalysis;
        QCOMPARE(analysis.filesystem, udf ? ISO_FILESYSTEM_UDF : ISO_FILESYSTEM_ISO9660);
        QCOMPARE(controller_->selection().imageState, rufus::ImageState::Partial);
        QCOMPARE(analysis.report.has_efi, uint16_t(0x8000));
        QCOMPARE(analysis.pending, uint32_t(ISO_PENDING_SECURE_BOOT));
        QVERIFY(controller_->imageStatusText().contains(QStringLiteral("Secure Boot checks")));
        QVERIFY(!controller_->imageStatusText().contains(QStringLiteral("embedded EFI image")));
        QVERIFY(log_->toPlainText().contains(QStringLiteral("'bootx64.efi'")));
        QCOMPARE(volume_->text(), QStringLiteral("EMBEDDED_EFI"));
        QVERIFY(!start_->isEnabled());

        const QByteArray emptyFat = makeFatImage(QStringLiteral("empty"), false);
        QVERIFY(!emptyFat.isEmpty());
        const QString noLoader = makeImage(QStringLiteral("NO_EFI"), {
            {QStringLiteral("boot/efi.img"), emptyFat}}, udf);
        QVERIFY(!noLoader.isEmpty());
        controller_->selectImage(noLoader);
        QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), analysisTimeoutMs);
        QCOMPARE(controller_->selection().imageState, rufus::ImageState::Unsupported);
        QCOMPARE(controller_->selection().imageAnalysis.report.has_efi, uint16_t(0));
        QCOMPARE(controller_->selection().imageAnalysis.pending, uint32_t(0));
        QCOMPARE(progress_->format(), QStringLiteral("READY"));
        QVERIFY(!start_->isEnabled());

        const QString unreadable = makeImage(QStringLiteral("UNKNOWN_EFI"), {
            {QStringLiteral("boot/efi.img"), QByteArray(512, '\0')}}, udf);
        QVERIFY(!unreadable.isEmpty());
        controller_->selectImage(unreadable);
        QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), analysisTimeoutMs);
        QCOMPARE(controller_->selection().imageAnalysis.pending, uint32_t(ISO_PENDING_EMBEDDED_EFI));
        QCOMPARE(controller_->selection().imageAnalysis.report.has_efi, uint16_t(0));
        QVERIFY(controller_->imageStatusText().contains(QStringLiteral("embedded EFI image")));

        nextGate = AfterScan;
        controller_->selectImage(image);
        QTRY_COMPARE_WITH_TIMEOUT(entered.available(), 1, analysisTimeoutMs);
        QTest::mouseClick(close_, Qt::LeftButton);
        releaseScan.release();
        QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), analysisTimeoutMs);
        QCOMPARE(controller_->selection().imageState, rufus::ImageState::Cancelled);
        QCOMPARE(controller_->selection().imageAnalysis.report.has_efi, uint16_t(0));
        QVERIFY(volume_->text().isEmpty() && !start_->isEnabled());
    }

    void cancelCompletedUdfDiscardsItsQueuedReportAndCanScanAgain()
    {
        const QString image = makeImage(QStringLiteral("UDF_CANCEL"), {
            {QStringLiteral("syslinux.cfg"), QByteArray("default linux\n")}}, true);
        QVERIFY(!image.isEmpty());
        nextGate = AfterScan;
        controller_->selectImage(image);
        QTRY_COMPARE_WITH_TIMEOUT(entered.available(), 1, analysisTimeoutMs);
        QTest::mouseClick(close_, Qt::LeftButton);
        releaseScan.release();
        QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), analysisTimeoutMs);
        QCOMPARE(controller_->selection().imageState, rufus::ImageState::Cancelled);
        QCOMPARE(controller_->selection().imageAnalysis.filesystem, ISO_FILESYSTEM_NONE);
        QVERIFY(volume_->text().isEmpty() && !start_->isEnabled());
        controller_->selectImage(image);
        QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), analysisTimeoutMs);
        QCOMPARE(controller_->selection().imageState, rufus::ImageState::Complete);
        QCOMPARE(controller_->selection().imageAnalysis.filesystem, ISO_FILESYSTEM_UDF);
        QCOMPARE(scanCalls.load(), 2);
    }

    void unknownModernSyslinuxUsesTheBundledVersionWithoutInventingDetection()
    {
        const QString image = makeImage(QStringLiteral("SYSLINUX_V6"), {
            {QStringLiteral("isolinux.cfg"), QByteArray("default linux\n")},
            {QStringLiteral("isolinux.bin"), QByteArray(512, '\0')},
            {QStringLiteral("ldlinux.c32"), QByteArray(512, '\0')}});
        QVERIFY(!image.isEmpty());
        controller_->selectImage(image);
        QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), analysisTimeoutMs);
        QCOMPARE(controller_->selection().imageState, rufus::ImageState::Complete);
        const RUFUS_IMG_REPORT& report = controller_->selection().imageAnalysis.report;
        QCOMPARE(report.sl_detected_version, uint16_t(0));
        QCOMPARE(report.sl_version, uint16_t(0x0604));
        QVERIFY(report.has_syslinux && report.sl_version_inferred);
        QVERIFY(log_->toPlainText().contains(QStringLiteral("Syslinux fallback: 6.04")));
    }

    void cancelUsesWin32ControlsAndKeepsTheWindowResponsive()
    {
        const QString image = makeImage(QStringLiteral("CANCEL_TEST"), {});
        QVERIFY(!image.isEmpty());
        nextGate = BeforeScan;
        controller_->selectImage(image);
        QTRY_COMPARE_WITH_TIMEOUT(entered.available(), 1, analysisTimeoutMs);
        QCOMPARE(close_->text(), QStringLiteral("CANCEL"));
        QVERIFY(!select_->isEnabled() && !start_->isEnabled());
        QCOMPARE(progress_->maximum(), 0);
        auto* busyText = window_->findChild<QLabel*>(QStringLiteral("busyProgressLabel"));
        QVERIFY(busyText && busyText->isVisible());
        QCOMPARE(busyText->text(), QStringLiteral("Scanning image..."));
        QVERIFY(controller_->imageStatusText().contains(QStringLiteral("Scanning")));
        QToolButton* logButton = window_->findChild<QToolButton*>(QStringLiteral("logButton"));
        QTest::mouseClick(logButton, Qt::LeftButton);
        QDialog* logWindow = window_->findChild<QDialog*>(QStringLiteral("logWindow"));
        QVERIFY(logWindow->isVisible());
        logWindow->close();
        QVERIFY(!logWindow->isVisible());
        QTest::mouseClick(close_, Qt::LeftButton);
        QCOMPARE(controller_->selection().imageState, rufus::ImageState::Cancelling);
        QVERIFY(window_->isVisible() && !close_->isEnabled());
        QVERIFY(volume_->text().isEmpty());
        releaseScan.release();
        QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), analysisTimeoutMs);
        QCOMPARE(controller_->selection().imageState, rufus::ImageState::Cancelled);
        QCOMPARE(progress_->format(), QStringLiteral("READY"));
        QVERIFY(select_->isEnabled() && close_->isEnabled());
        QVERIFY(!busyText->isVisible());
        QVERIFY(controller_->selection().imageAnalysis.report.label[0] == 0);
        QVERIFY(controller_->selection().imagePath.isEmpty());
        controller_->selectImage(image); // Restart gets a fresh interruption flag.
        QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), analysisTimeoutMs);
        QCOMPARE(controller_->selection().imageState, rufus::ImageState::Complete);
        QTest::mouseClick(logButton, Qt::LeftButton);
        QVERIFY(logWindow->isVisible());
        QVERIFY(log_->toPlainText().contains(QStringLiteral("ISO analysis:")));
        QTest::mouseClick(close_, Qt::LeftButton);
        QVERIFY(!window_->isVisible() && !logWindow->isVisible());
    }

    void onlyTheLatestSelectionRunsAfterThePreviousThreadStops()
    {
        const QString first = makeImage(QStringLiteral("OLD_IMAGE"), {});
        const QString skipped = makeImage(QStringLiteral("SKIPPED_IMAGE"), {});
        const QString latest = makeImage(QStringLiteral("NEW_IMAGE"), {});
        QVERIFY(!first.isEmpty() && !skipped.isEmpty() && !latest.isEmpty());
        nextGate = AfterScan; // Old C result is already complete but not delivered.
        controller_->selectImage(first);
        QTRY_COMPARE_WITH_TIMEOUT(entered.available(), 1, analysisTimeoutMs);
        controller_->selectImage(skipped);
        controller_->selectImage(latest);
        QVERIFY(volume_->text().isEmpty());
        messages_.clear();
        releaseScan.release();
        QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), analysisTimeoutMs);
        QCOMPARE(scanCalls.load(), 2);
        QCOMPARE(controller_->selection().imagePath, latest);
        QCOMPARE(controller_->selection().imageState, rufus::ImageState::Complete);
        QCOMPARE(volume_->text(), QStringLiteral("NEW_IMAGE"));
        QVERIFY(!messages_.contains(QStringLiteral("late scan output")));
        QVERIFY(!messages_.join(QLatin1Char('\n')).contains(QStringLiteral("Another Rufus operation")));
    }

    void invalidReplacementCannotReceiveThePreviousReport()
    {
        const QString first = makeImage(QStringLiteral("OLD_IMAGE"), {});
        QVERIFY(!first.isEmpty());
        nextGate = AfterScan;
        controller_->selectImage(first);
        QTRY_COMPARE_WITH_TIMEOUT(entered.available(), 1, analysisTimeoutMs);
        controller_->selectImage(directory_->filePath(QStringLiteral("missing.iso")));
        QCOMPARE(controller_->selection().imageState, rufus::ImageState::Failed);
        const QString problem = controller_->selection().imageProblem;
        messages_.clear();
        releaseScan.release();
        QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), analysisTimeoutMs);
        QCOMPARE(scanCalls.load(), 1);
        QCOMPARE(controller_->selection().imageProblem, problem);
        QVERIFY(controller_->selection().imagePath.isEmpty());
        QVERIFY(volume_->text().isEmpty());
        QVERIFY(!messages_.contains(QStringLiteral("late scan output")));
    }

    void queuedSuccessIsRejectedAfterTheWorkerHasAlreadyExited()
    {
        const QString first = makeImage(QStringLiteral("QUEUED_OLD"), {});
        const QString latest = makeImage(QStringLiteral("QUEUED_NEW"), {});
        QVERIFY(!first.isEmpty() && !latest.isEmpty());
        nextGate = AfterScan;
        controller_->selectImage(first);
        // Do not deliver GUI events: all reports, including a successful result,
        // must remain queued until after the next selection invalidates them.
        QVERIFY(entered.tryAcquire(1, analysisTimeoutMs));
        releaseScan.release();
        QVERIFY(scanThread.load()->wait(analysisTimeoutMs));
        QCOMPARE(controller_->selection().imageState, rufus::ImageState::Scanning);
        controller_->selectImage(latest);
        QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), analysisTimeoutMs);
        QCOMPARE(controller_->selection().imageState, rufus::ImageState::Complete);
        QCOMPARE(volume_->text(), QStringLiteral("QUEUED_NEW"));
        QVERIFY(!messages_.contains(QStringLiteral("late scan output")));
        QVERIFY(!messages_.contains(QStringLiteral("Volume label: QUEUED_OLD")));
    }

    void cancelDiscardsAnAlreadyCompletedResult()
    {
        const QString image = makeImage(QStringLiteral("LATE_CANCEL"), {});
        QVERIFY(!image.isEmpty());
        nextGate = AfterScan;
        controller_->selectImage(image);
        QTRY_COMPARE_WITH_TIMEOUT(entered.available(), 1, analysisTimeoutMs);
        QTest::keyClick(window_.get(), Qt::Key_Escape);
        releaseScan.release();
        QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), analysisTimeoutMs);
        QCOMPARE(controller_->selection().imageState, rufus::ImageState::Cancelled);
        QVERIFY(controller_->selection().imageAnalysis.report.label[0] == 0);
        QVERIFY(window_->isVisible());
        QVERIFY(!messages_.contains(QStringLiteral("late scan output")));
    }

    void replacingTheSamePathGetsANewRequest()
    {
        const QString image = makeImage(QStringLiteral("SAME_PATH"), {});
        QVERIFY(!image.isEmpty());
        nextGate = AfterScan;
        controller_->selectImage(image);
        QTRY_COMPARE_WITH_TIMEOUT(entered.available(), 1, analysisTimeoutMs);
        controller_->selectImage(image);
        messages_.clear();
        releaseScan.release();
        QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), analysisTimeoutMs);
        QCOMPARE(scanCalls.load(), 2);
        QCOMPARE(controller_->selection().imageState, rufus::ImageState::Complete);
        QVERIFY(!messages_.contains(QStringLiteral("late scan output")));
    }

    void changedFileDoesNotPublishACompletedReport_data()
    {
        QTest::addColumn<int>("change");
        QTest::newRow("size-changed") << int(ChangeSize);
        QTest::newRow("removed") << int(RemoveFile);
        QTest::newRow("same-size-timestamp-changed") << int(ChangeTimestamp);
        QTest::newRow("marker-changed-timestamp-restored") << int(ChangeMarker);
    }

    void changedFileDoesNotPublishACompletedReport()
    {
        QFETCH(int, change);
        const QString image = makeImage(QStringLiteral("CHANGED"), {});
        QVERIFY(!image.isEmpty());
        nextGate = AfterScan;
        controller_->selectImage(image);
        QTRY_COMPARE_WITH_TIMEOUT(entered.available(), 1, analysisTimeoutMs);
        QFile file(image);
        if (change == RemoveFile)
            QVERIFY(file.remove());
        else if (change == ChangeTimestamp)
        {
            QVERIFY(file.open(QIODevice::ReadOnly));
            QVERIFY(file.setFileTime(file.fileTime(QFileDevice::FileModificationTime).addSecs(2),
                                     QFileDevice::FileModificationTime));
            file.close();
        }
        else if (change == ChangeMarker)
        {
            const QDateTime modified = QFileInfo(image).lastModified();
            QVERIFY(file.open(QIODevice::ReadWrite) && file.seek(510));
            QCOMPARE(file.write("\x55\xaa", 2), qint64(2));
            QVERIFY(file.setFileTime(modified, QFileDevice::FileModificationTime));
            file.close();
        }
        else
        {
            QVERIFY(file.open(QIODevice::Append));
            QCOMPARE(file.write("changed"), qint64(7));
            file.close();
        }
        releaseScan.release();
        QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), analysisTimeoutMs);
        QCOMPARE(controller_->selection().imageState, rufus::ImageState::Failed);
        QVERIFY(controller_->selection().imageProblem.contains(QStringLiteral("changed during analysis")));
        QVERIFY(volume_->text().isEmpty());
        QVERIFY(controller_->selection().imageAnalysis.report.label[0] == 0);
    }

    void exceptionRestoresLoggingAndAllowsAnotherScan()
    {
        const QString image = makeImage(QStringLiteral("EXCEPTION"), {});
        QVERIFY(!image.isEmpty());
        const cdio_log_handler_t previous = cdio_log_set_handler(previousCdioHandler);
        nextGate = Throw;
        controller_->selectImage(image);
        QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), analysisTimeoutMs);
        const cdio_log_handler_t restored = cdio_log_set_handler(previous);
        QVERIFY(restored == previousCdioHandler);
        QCOMPARE(controller_->selection().imageState, rufus::ImageState::Failed);
        QCOMPARE(controller_->selection().imageProblem, QStringLiteral("test analysis exception"));
        QVERIFY(volume_->text().isEmpty());
        controller_->selectImage(image);
        QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), analysisTimeoutMs);
        QCOMPARE(controller_->selection().imageState, rufus::ImageState::Complete);
    }

    void overlappingControllersReportFailureAndRecover()
    {
        const QString image = makeImage(QStringLiteral("SERIALIZED"), {});
        QVERIFY(!image.isEmpty());
        nextGate = BeforeScan;
        controller_->selectImage(image);
        QTRY_COMPARE_WITH_TIMEOUT(entered.available(), 1, analysisTimeoutMs);
        rufus::SelectionController other;
        other.selectImage(image);
        QTRY_VERIFY_WITH_TIMEOUT(!other.isAnalyzing(), analysisTimeoutMs);
        QCOMPARE(other.selection().imageState, rufus::ImageState::Failed);
        QVERIFY(other.selection().imageProblem.contains(QStringLiteral("Another Rufus operation")));
        QVERIFY(!other.selection().imageAnalysis.tree_complete);
        releaseScan.release();
        QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), analysisTimeoutMs);
        other.selectImage(image);
        QTRY_VERIFY_WITH_TIMEOUT(!other.isAnalyzing(), analysisTimeoutMs);
        QCOMPARE(other.selection().imageState, rufus::ImageState::Complete);
    }

    void teardownJoinsTheRunningWorker()
    {
        const QString image = makeImage(QStringLiteral("TEARDOWN"), {});
        QVERIFY(!image.isEmpty());
        nextGate = BeforeScan;
        controller_->selectImage(image);
        QTRY_COMPARE_WITH_TIMEOUT(entered.available(), 1, analysisTimeoutMs);
        controller_->cancelAnalysis();
        window_.reset();
        releaseScan.release();
        controller_.reset();
        // Queued reports now target a deleted controller; delivery must be safe.
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    }
};

QTEST_MAIN(TestImageAnalysis)
#include "test_image_analysis.moc"
