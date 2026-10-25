// SPDX-License-Identifier: GPL-3.0-or-later
// Qualify upstream splitting through the existing borrowed ISO/UDF reader.
// Every output is inside a disposable directory. Linker wrappers make multipart
// adapter tests small without introducing any production test settings.
extern "C" {
#include "native_input.h"
}
#include "iso_file.h"
#include "iso_analyze.h"
#include "platform/wim_read_file.h"
#include "platform/iso_extract.h"
#include "platform/iso_wim_split.h"
#include "platform/rufus_reporting.h"
#include "progress_policy.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRandomGenerator>
#include <QTemporaryDir>
#include <QtTest>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace
{
constexpr int fixtureFileBytes = 128 * 1024;
constexpr int fixtureFileCount = 6;
constexpr uint64_t fixturePartBytes = 256 * 1024;
constexpr uint64_t rufusPartBytes = 4094ULL * 1024 * 1024;
constexpr int fixtureTimeoutMs = 10000;
enum Fault { NoFault, ReadFailure, CancelRead, CancelFirstPart, SyncFailure,
    OversizedPart, RenameFailure, CancelPublish, SourceChange, DirectorySyncFailure,
    SplitBudgetExceeded, NoFreeSpace, InvalidSpaceUnit, SpaceQueryFailure };
Fault fault;
bool active, cancelled, nativeMode;
int syncCalls, renameCalls, splitCalls, planCalls;
uint64_t bytesRead;
QString sourcePath;
uint64_t progressBlocks, progressTotal;
unsigned progressCalls;
bool validProgress;
uint64_t requestedPartBytes;

struct SplitProgress
{
    uint64_t completed = 0;
    uint64_t total = 0;
    uint64_t partStart = 0;
    uint64_t lastWrite = 0;
    unsigned started = 0;
    unsigned finished = 0;
    unsigned parts = 0;
    unsigned writes = 0;
    bool valid = true;
    QStringList paths;
};

enum wimlib_progress_status splitProgress(enum wimlib_progress_msg message,
    union wimlib_progress_info* info, void* context)
{
    auto& progress = *static_cast<SplitProgress*>(context);
    if (message == WIMLIB_PROGRESS_MSG_SPLIT_BEGIN_PART)
    {
        ++progress.started;
        progress.parts = info->split.total_parts;
        progress.total = info->split.total_bytes;
        progress.partStart = info->split.completed_bytes;
        progress.paths.append(QString::fromUtf8(info->split.part_name));
        progress.valid &= info->split.cur_part_number == progress.started;
        progress.valid &= progress.partStart == progress.completed;
    }
    if (message == WIMLIB_PROGRESS_MSG_WRITE_STREAMS)
    {
        // Same aggregate resource counter used by Rufus's WimProgressFunc.
        const uint64_t completed = progress.partStart + info->write_streams.completed_compressed_bytes;
        progress.valid &= completed >= progress.lastWrite && completed <= progress.total;
        progress.lastWrite = completed;
        ++progress.writes;
    }
    if (message == WIMLIB_PROGRESS_MSG_SPLIT_END_PART)
    {
        ++progress.finished;
        progress.completed = info->split.completed_bytes;
        progress.valid &= info->split.cur_part_number == progress.finished;
        progress.valid &= progress.completed <= progress.total;
        if (fault == CancelFirstPart)
            cancelled = true;
    }
    return cancelled ? WIMLIB_PROGRESS_STATUS_ABORT : WIMLIB_PROGRESS_STATUS_CONTINUE;
}
}

extern "C" int IsIsoScanCancelled(void) { return cancelled; }
extern "C" int IsRufusJobCancelled(void) { return cancelled; }
extern "C" void uprintf(const char*, ...) {}
extern "C" void uprintfs(const char*) {}
extern "C" void _UpdateProgressWithInfo(int operation, int message, uint64_t processed, uint64_t total, int)
{
    validProgress &= operation == OP_FILE_COPY && message == 0;
    validProgress &= processed >= progressBlocks && processed <= total;
    if (progressCalls != 0)
        validProgress &= total == progressTotal;
    progressBlocks = processed;
    progressTotal = total;
    ++progressCalls;
}
extern "C" int __real_ReadIsoFileRange(ISO_FILE_READER*, uint64_t, void*, size_t);
extern "C" int __real_fsync(int);
extern "C" int __real_wimlib_split(WIMStruct*, const char*, uint64_t, int);
extern "C" int __real_fstatat64(int, const char*, struct stat64*, int);
extern "C" int __real_renameat2(int, const char*, int, const char*, unsigned);
extern "C" int __real_fstatvfs64(int, struct statvfs64*);

extern "C" int __wrap_fstatvfs64(int fd, struct statvfs64* space)
{
    const int status = __real_fstatvfs64(fd, space);
    if (status == 0 && active && nativeMode)
    {
        if (fault == NoFreeSpace)
            space->f_bavail = 0;
        if (fault == InvalidSpaceUnit)
            space->f_frsize = 0;
        if (fault == SpaceQueryFailure)
        {
            errno = EIO;
            return -1;
        }
    }
    return status;
}

extern "C" int __wrap_wimlib_split(WIMStruct* wim, const char* name, uint64_t size, int flags)
{
    if (active && nativeMode)
    {
        if (qstrcmp(name, "/proc/self/fd/-1/install.swm") == 0)
            ++planCalls;
        else
            ++splitCalls;
        requestedPartBytes = size;
        if (size != rufusPartBytes || flags != WIMLIB_WRITE_FLAG_FSYNC)
            qFatal("Native splitter changed Rufus parameters");
        size = fixturePartBytes;
    }
    return __real_wimlib_split(wim, name, size, flags);
}

extern "C" int __wrap_fstatat64(int fd, const char* name, struct stat64* info, int flags)
{
    const int result = __real_fstatat64(fd, name, info, flags);
    if (result == 0 && active && nativeMode && fault == OversizedPart && qstrcmp(name, "install.swm") == 0)
        info->st_size = qint64(UINT32_MAX) + 1;
    if (result == 0 && active && nativeMode && fault == SplitBudgetExceeded && qstrcmp(name, "install.swm") == 0)
        info->st_size = UINT32_MAX;
    return result;
}

extern "C" int __wrap_renameat2(int from, const char* name, int to, const char* output, unsigned flags)
{
    if (active && nativeMode)
    {
        ++renameCalls;
        if (fault == RenameFailure && renameCalls == 2)
        {
            errno = EIO;
            return -1;
        }
        if (fault == CancelPublish && renameCalls == 1)
            cancelled = true;
    }
    return __real_renameat2(from, name, to, output, flags);
}

extern "C" int __wrap_ReadIsoFileRange(ISO_FILE_READER* reader, uint64_t offset, void* bytes, size_t count)
{
    if (active)
    {
        bytesRead += count;
        if (fault == ReadFailure)
            return ISO_SCAN_ERROR;
        if (fault == CancelRead)
        {
            cancelled = true;
            return ISO_SCAN_CANCELLED;
        }
    }
    return __real_ReadIsoFileRange(reader, offset, bytes, count);
}

extern "C" int __wrap_fsync(int fd)
{
    if (active)
    {
        ++syncCalls;
        if (fault == SyncFailure)
        {
            errno = EIO;
            return -1;
        }
        if (nativeMode)
        {
            struct stat info = {};
            if (fstat(fd, &info) != 0)
                qFatal("Cannot inspect sync descriptor");
            if (fault == DirectorySyncFailure && S_ISDIR(info.st_mode))
            {
                errno = EIO;
                return -1;
            }
            if (fault == CancelFirstPart && syncCalls == 1)
                cancelled = true;
            if (fault == SourceChange && syncCalls == 1)
            {
                QFile file(sourcePath);
                if (!file.open(QIODevice::Append) || file.write("changed") != 7)
                    qFatal("Cannot change test source");
            }
        }
    }
    return __real_fsync(fd);
}

class TestWimSplit : public QObject
{
    Q_OBJECT
    QTemporaryDir directory_;
    QString work_, archive_, image_, output_;
    ISO_ANALYSIS_SOURCE view_ = {};
    ISO_FILE_READER reader_ = {};
    WIMStruct* input_ = nullptr;
    WIMStruct* joined_ = nullptr;
    SplitProgress progress_;
    ISO_WIM_SPLIT_RESULT nativeResult_ = {};
    qsizetype descriptors_ = 0;

    QByteArray content(int index) const
    {
        QRandomGenerator random(quint32(index + 1));
        QByteArray bytes(fixtureFileBytes, '\0');
        for (char& byte : bytes)
            byte = char(random.generate());
        return bytes;
    }

    QByteArray imageHash() const
    {
        QFile file(image_);
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (!file.open(QIODevice::ReadOnly) || !hash.addData(&file))
            qFatal("Cannot hash source image");
        return hash.result();
    }

    void makeInput(bool udf, int compression, int flags, const QString& extra = {})
    {
        const QString files = work_ + QStringLiteral("/files");
        QVERIFY(QDir().mkpath(files));
        for (int index = 0; index < fixtureFileCount; ++index)
        {
            QFile file(files + '/' + QString::number(index));
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(content(index)), qint64(fixtureFileBytes));
        }
        WIMStruct* wim = nullptr;
        QCOMPARE(wimlib_create_new_wim(static_cast<wimlib_compression_type>(compression), &wim), 0);
        const QByteArray source = files.toUtf8();
        int error = wimlib_add_image(wim, source.constData(), "edition-one", nullptr, 0);
        if (error == 0)
            error = wimlib_add_image(wim, source.constData(), "edition-two", nullptr, 0);
        if (error == 0)
            error = wimlib_set_image_property(wim, 2, "DISPLAYNAME", "Windows Éducation");
        if (error == 0)
            error = wimlib_write(wim, archive_.toUtf8().constData(), WIMLIB_ALL_IMAGES, flags, 1);
        wimlib_free(wim);
        QCOMPARE(error, 0);
        if (!extra.isEmpty())
        {
            QFile file(work_ + QStringLiteral("/iso/sources/") + extra);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write("keep me"), qint64(7));
        }
        QStringList arguments = {QStringLiteral("-quiet"), QStringLiteral("-R"), QStringLiteral("-J"),
            QStringLiteral("-o"), image_};
        if (udf)
            arguments.append(QStringLiteral("-udf"));
        arguments.append(work_ + QStringLiteral("/iso"));
        QProcess process;
        process.start(QStringLiteral(RUFUS_MKISOFS), arguments);
        QVERIFY(process.waitForFinished(fixtureTimeoutMs));
        QCOMPARE(process.exitCode(), 0);
        const QByteArray path = image_.toUtf8();
        if (udf)
            view_.udf = udf_open(path.constData());
        else
            view_.iso = iso9660_open_ext(path.constData(), ISO_EXTENSION_ALL);
        QVERIFY(view_.udf != nullptr || view_.iso != nullptr);
        QCOMPARE(OpenIsoFileReader(&reader_, view_.iso, view_.udf, "/sources/install.wim"), int(ISO_SCAN_OK));
        QCOMPARE(OpenWimFromIsoReader(&reader_, 0, &input_), 0);
        wimlib_register_progress_function(input_, splitProgress, &progress_);
    }

    int split(uint64_t partBytes = fixturePartBytes)
    {
        const QByteArray destination = (output_ + QStringLiteral("/install.swm")).toUtf8();
        active = true;
        const int error = wimlib_split(input_, destination.constData(), partBytes, WIMLIB_WRITE_FLAG_FSYNC);
        active = false;
        // Verify the output after releasing the complete borrowed input chain.
        wimlib_free(input_);
        input_ = nullptr;
        CloseIsoFileReader(&reader_);
        CloseIsoAnalysisSource(&view_);
        return error;
    }

    int nativeSplit()
    {
        IMAGE_FILE_SOURCE source = {};
        const QByteArray path = image_.toUtf8();
        int error = OpenImageFileSource(path.constData(), &source);
        if (error != 0)
            return error;
        const int parent = open(output_.toUtf8().constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (parent < 0)
            qFatal("Cannot open native split output");
        active = nativeMode = true;
        error = SplitIsoWim(&reader_, &source, path.constData(), parent, 3,
            3 + (reader_.size + ISO_BLOCKSIZE - 1) / ISO_BLOCKSIZE, &nativeResult_, nullptr, nullptr);
        active = nativeMode = false;
        close(parent);
        CloseImageFileSource(&source);
        wimlib_free(input_);
        input_ = nullptr;
        CloseIsoFileReader(&reader_);
        CloseIsoAnalysisSource(&view_);
        return error;
    }

    void verifyNativeParts(const QString& directory, unsigned parts, uint64_t bytes)
    {
        QList<QByteArray> paths;
        uint64_t actualBytes = 0;
        for (unsigned part = 1; part <= parts; ++part)
        {
            const QString name = part == 1 ? QStringLiteral("install.swm") : QStringLiteral("install%1.swm").arg(part);
            const QString path = directory + '/' + name;
            paths.append(path.toUtf8());
            actualBytes += uint64_t(QFileInfo(path).size());
        }
        QCOMPARE(actualBytes, bytes);
        QCOMPARE(QDir(directory).entryList(QDir::Files).size(), paths.size());
        QVERIFY(syncCalls >= int(parts) + 1); // Adapter also syncs the containing directory.
        verifyContents(paths, parts);
        QVERIFY(validProgress);
        QVERIFY(progressCalls > 0);
        QVERIFY(parts > 1);
        QCOMPARE(requestedPartBytes, rufusPartBytes);
        QCOMPARE(splitCalls, 1);
        QVERIFY(QDir(directory).entryList(QDir::Hidden | QDir::NoDotAndDotDot | QDir::Dirs).isEmpty());
    }

    void verifyParts()
    {
        QVERIFY(progress_.valid);
        QCOMPARE(progress_.started, progress_.parts);
        QCOMPARE(progress_.finished, progress_.parts);
        QCOMPARE(progress_.completed, progress_.total);
        QVERIFY(progress_.writes > 0);
        QVERIFY(bytesRead > 0);
        QCOMPARE(syncCalls, int(progress_.parts));
        QList<QByteArray> paths;
        for (unsigned part = 1; part <= progress_.parts; ++part)
        {
            const QString name = part == 1 ? QStringLiteral("install.swm") : QStringLiteral("install%1.swm").arg(part);
            QCOMPARE(progress_.paths[int(part - 1)], output_ + '/' + name);
            paths.append((output_ + '/' + name).toUtf8());
        }
        QCOMPARE(QDir(output_).entryList(QDir::Files).size(), paths.size());
        verifyContents(paths, progress_.parts);
    }

    void verifyContents(const QList<QByteArray>& paths, unsigned parts)
    {
        QVERIFY(!paths.isEmpty());
        QCOMPARE(wimlib_open_wim(paths.first().constData(), 0, &joined_), 0);
        QList<const char*> references;
        for (int part = 1; part < paths.size(); ++part)
            references.append(paths[part].constData());
        QCOMPARE(wimlib_reference_resource_files(joined_, references.constData(), unsigned(references.size()), 0, 0), 0);
        struct wimlib_wim_info info = {};
        QCOMPARE(wimlib_get_wim_info(joined_, &info), 0);
        QCOMPARE(info.image_count, 2u);
        QCOMPARE(info.total_parts, parts);
        QCOMPARE(QString::fromUtf8(wimlib_get_image_property(joined_, 2, "DISPLAYNAME")), QStringLiteral("Windows Éducation"));
        for (int image = 1; image <= 2; ++image)
        {
            for (int index = 0; index < fixtureFileCount; ++index)
            {
                uint8_t* bytes = nullptr;
                size_t size = 0;
                const QByteArray name = QByteArray::number(index);
                const int error = ReadWimFile(joined_, image, name.constData(), &bytes, &size);
                const QByteArray actual(reinterpret_cast<const char*>(bytes), qsizetype(size));
                free(bytes);
                QCOMPARE(error, 0);
                QCOMPARE(actual, content(index));
            }
        }
        QCOMPARE(wimlib_verify_wim(joined_, 0), 0);
    }

private slots:
    void initTestCase()
    {
        QVERIFY(directory_.isValid());
        QCOMPARE(wimlib_global_init(WIMLIB_INIT_FLAG_ASSUME_UTF8 | WIMLIB_INIT_FLAG_DEFAULT_CASE_INSENSITIVE), 0);
    }

    void cleanupTestCase() { wimlib_global_cleanup(); }

    void init()
    {
        fault = NoFault;
        active = cancelled = nativeMode = false;
        syncCalls = renameCalls = splitCalls = planCalls = 0;
        progressBlocks = progressTotal = requestedPartBytes = 0;
        progressCalls = 0;
        validProgress = true;
        bytesRead = 0;
        progress_ = {};
        nativeResult_ = {};
        work_ = directory_.filePath(QStringLiteral("case"));
        archive_ = work_ + QStringLiteral("/iso/sources/install.wim");
        image_ = work_ + QStringLiteral("/source.iso");
        sourcePath = image_;
        output_ = work_ + QStringLiteral("/parts");
        QVERIFY(QDir().mkpath(work_ + QStringLiteral("/iso/sources")));
        QVERIFY(QDir().mkpath(output_));
        descriptors_ = QDir(QStringLiteral("/proc/self/fd")).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size();
    }

    void cleanup()
    {
        active = cancelled = nativeMode = false;
        wimlib_free(joined_);
        wimlib_free(input_);
        joined_ = input_ = nullptr;
        CloseIsoFileReader(&reader_);
        CloseIsoAnalysisSource(&view_);
        QVERIFY(QDir(work_).removeRecursively());
        QCOMPARE(QDir(QStringLiteral("/proc/self/fd")).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size(), descriptors_);
    }

    void splitAndReadback_data()
    {
        QTest::addColumn<bool>("udf");
        QTest::addColumn<int>("compression");
        QTest::addColumn<int>("flags");
        QTest::newRow("iso-uncompressed") << false << int(WIMLIB_COMPRESSION_TYPE_NONE) << 0;
        QTest::newRow("udf-uncompressed") << true << int(WIMLIB_COMPRESSION_TYPE_NONE) << 0;
        QTest::newRow("iso-lzx") << false << int(WIMLIB_COMPRESSION_TYPE_LZX) << 0;
        QTest::newRow("udf-lzx") << true << int(WIMLIB_COMPRESSION_TYPE_LZX) << 0;
        QTest::newRow("iso-pipable") << false << int(WIMLIB_COMPRESSION_TYPE_LZX) << int(WIMLIB_WRITE_FLAG_PIPABLE);
        QTest::newRow("udf-pipable") << true << int(WIMLIB_COMPRESSION_TYPE_LZX) << int(WIMLIB_WRITE_FLAG_PIPABLE);
    }

    void splitAndReadback()
    {
        QFETCH(bool, udf);
        QFETCH(int, compression);
        QFETCH(int, flags);
        makeInput(udf, compression, flags);
        const QByteArray before = imageHash();
        QCOMPARE(split(), 0);
        QVERIFY(progress_.parts > 1);
        verifyParts();
        QCOMPARE(imageHash(), before);
    }

    void originalRufusPartLimit()
    {
        makeInput(true, WIMLIB_COMPRESSION_TYPE_LZX, 0);
        QCOMPARE(split(rufusPartBytes), 0);
        QCOMPARE(progress_.parts, 1u);
        verifyParts();
    }

    void resourceCanExceedRequestedPartSize()
    {
        makeInput(false, WIMLIB_COMPRESSION_TYPE_NONE, 0);
        constexpr uint64_t smallerThanResource = fixtureFileBytes / 2;
        QCOMPARE(split(smallerThanResource), 0);
        bool oversized = false;
        for (const QString& path : progress_.paths)
            oversized |= uint64_t(QFileInfo(path).size()) > smallerThanResource;
        QVERIFY(oversized); // A requested part size alone cannot enforce FAT32's limit.
        verifyParts();
    }

    void solidArchiveIsUnsupported()
    {
        makeInput(true, WIMLIB_COMPRESSION_TYPE_LZMS, WIMLIB_WRITE_FLAG_SOLID);
        const QByteArray before = imageHash();
        QCOMPARE(split(), int(WIMLIB_ERR_UNSUPPORTED));
        QCOMPARE(progress_.started, 0u);
        QVERIFY(QDir(output_).entryList(QDir::Files).isEmpty());
        QCOMPARE(imageHash(), before);
    }

    void failures_data()
    {
        QTest::addColumn<int>("failure");
        QTest::addColumn<int>("expected");
        QTest::newRow("source-read") << int(ReadFailure) << int(WIMLIB_ERR_READ);
        QTest::newRow("cancel-read") << int(CancelRead) << int(WIMLIB_ERR_ABORTED_BY_PROGRESS);
        QTest::newRow("cancel-after-first-part") << int(CancelFirstPart) << int(WIMLIB_ERR_ABORTED_BY_PROGRESS);
        QTest::newRow("part-sync") << int(SyncFailure) << int(WIMLIB_ERR_WRITE);
    }

    void failures()
    {
        QFETCH(int, failure);
        QFETCH(int, expected);
        makeInput(true, WIMLIB_COMPRESSION_TYPE_LZX, 0);
        const QByteArray before = imageHash();
        fault = Fault(failure);
        QCOMPARE(split(), expected);
        QVERIFY(progress_.finished < progress_.parts);
        if (fault == CancelFirstPart)
        {
            QCOMPARE(progress_.finished, 1u);
            QVERIFY(QFile::exists(output_ + QStringLiteral("/install.swm")));
        }
        if (fault == SyncFailure)
            QCOMPARE(syncCalls, 1);
        cancelled = false;
        QCOMPARE(imageHash(), before);
        // Upstream does not promise rollback. cleanup() removes every partial part.
    }

    void nativeReadback_data() { splitAndReadback_data(); }

    void readOnlyPlan_data() { splitAndReadback_data(); }

    void readOnlyPlan()
    {
        QFETCH(bool, udf);
        QFETCH(int, compression);
        QFETCH(int, flags);
        makeInput(udf, compression, flags);
        const QByteArray before = imageHash();
        ISO_WIM_SPLIT_PLAN plan = {};
        active = nativeMode = true;
        QCOMPARE(PlanIsoWimSplit(&reader_, &plan), 0);
        active = nativeMode = false;
        QVERIFY(plan.parts > 1);
        QCOMPARE(plan.reserved_bytes, reader_.size + plan.parts * (2ULL * 1024 * 1024));
        QCOMPARE(planCalls, 1);
        QCOMPARE(splitCalls, 0);
        QCOMPARE(syncCalls, 0);
        QCOMPARE(progressCalls, 0u);
        QVERIFY(QDir(output_).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
        QCOMPARE(imageHash(), before);
    }

    void nativeReadback()
    {
        QFETCH(bool, udf);
        QFETCH(int, compression);
        QFETCH(int, flags);
        makeInput(udf, compression, flags);
        const QByteArray before = imageHash();
        QCOMPARE(nativeSplit(), 0);
        QCOMPARE(nativeResult_.copied_blocks, progressBlocks);
        verifyNativeParts(output_, nativeResult_.parts, nativeResult_.bytes);
        QCOMPARE(imageHash(), before);
    }

    void nativeFailures_data()
    {
        QTest::addColumn<int>("failure");
        QTest::addColumn<int>("expected");
        QTest::newRow("source-read") << int(ReadFailure) << EIO;
        QTest::newRow("cancel-read") << int(CancelRead) << ECANCELED;
        QTest::newRow("cancel-part") << int(CancelFirstPart) << ECANCELED;
        QTest::newRow("part-sync") << int(SyncFailure) << EIO;
        QTest::newRow("oversized-part") << int(OversizedPart) << EFBIG;
        QTest::newRow("split-reservation") << int(SplitBudgetExceeded) << ENOSPC;
        QTest::newRow("publish-second-part") << int(RenameFailure) << EIO;
        QTest::newRow("cancel-publish") << int(CancelPublish) << ECANCELED;
        QTest::newRow("source-changed") << int(SourceChange) << ESTALE;
        QTest::newRow("directory-sync") << int(DirectorySyncFailure) << EIO;
    }

    void nativeFailures()
    {
        QFETCH(int, failure);
        QFETCH(int, expected);
        makeInput(true, WIMLIB_COMPRESSION_TYPE_LZX, 0);
        fault = Fault(failure);
        QCOMPARE(nativeSplit(), expected);
        QCOMPARE(nativeResult_.parts, 0u);
        QCOMPARE(nativeResult_.bytes, uint64_t(0));
        QVERIFY(QDir(output_).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
        QVERIFY(validProgress);
    }

    void keepsExistingPart_data()
    {
        QTest::addColumn<QString>("kind");
        QTest::newRow("regular") << QStringLiteral("regular");
        QTest::newRow("directory") << QStringLiteral("directory");
        QTest::newRow("symlink") << QStringLiteral("symlink");
        QTest::newRow("hardlink") << QStringLiteral("hardlink");
    }

    void keepsExistingPart()
    {
        QFETCH(QString, kind);
        makeInput(true, WIMLIB_COMPRESSION_TYPE_LZX, 0);
        const QString existing = output_ + QStringLiteral("/install2.swm");
        const QString original = work_ + QStringLiteral("/original");
        QFile file(original);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("keep me"), qint64(7));
        file.close();
        if (kind == QStringLiteral("regular"))
            QVERIFY(QFile::copy(original, existing));
        else if (kind == QStringLiteral("directory"))
            QVERIFY(QDir().mkpath(existing));
        else if (kind == QStringLiteral("symlink"))
            QCOMPARE(symlink(original.toUtf8().constData(), existing.toUtf8().constData()), 0);
        else
            QCOMPARE(link(original.toUtf8().constData(), existing.toUtf8().constData()), 0);
        QCOMPARE(nativeSplit(), EEXIST);
        QCOMPARE(QDir(output_).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot),
            QStringList{QStringLiteral("install2.swm")});
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("keep me"));
        if (kind != QStringLiteral("directory"))
        {
            QFile retained(existing);
            QVERIFY(retained.open(QIODevice::ReadOnly));
            QCOMPARE(retained.readAll(), QByteArray("keep me"));
        }
    }

    void extraction_data()
    {
        QTest::addColumn<bool>("udf");
        QTest::addColumn<bool>("splitInput");
        QTest::addColumn<bool>("solid");
        QTest::addColumn<QString>("extra");
        QTest::addColumn<int>("expected");
        QTest::newRow("iso-split") << false << true << false << QString() << 0;
        QTest::newRow("udf-split") << true << true << false << QString() << 0;
        QTest::newRow("ordinary-copy") << true << false << false << QString() << 0;
        QTest::newRow("solid") << true << true << true << QString() << ENOTSUP;
        QTest::newRow("first-part-collision") << true << true << false << QStringLiteral("install.swm") << EEXIST;
        QTest::newRow("later-part-collision") << true << true << false << QStringLiteral("INSTALL2.SWM") << EEXIST;
    }

    void extraction()
    {
        QFETCH(bool, udf);
        QFETCH(bool, splitInput);
        QFETCH(bool, solid);
        QFETCH(QString, extra);
        QFETCH(int, expected);
        makeInput(udf, solid ? WIMLIB_COMPRESSION_TYPE_LZMS : WIMLIB_COMPRESSION_TYPE_LZX,
            solid ? WIMLIB_WRITE_FLAG_SOLID : 0, extra);
        const uint64_t inputSize = reader_.size;
        wimlib_free(input_);
        input_ = nullptr;
        CloseIsoFileReader(&reader_);
        CloseIsoAnalysisSource(&view_);
        const ISO_EXTRACT_OPTIONS options = {{true, true, {0, 0}}, nullptr, nullptr, splitInput, 0, nullptr, false, false, false, nullptr, nullptr};
        ISO_EXTRACT_RESULT result = {};
        const QByteArray path = image_.toUtf8();
        const QByteArray before = imageHash();
        active = nativeMode = true;
        QCOMPARE(CheckIsoTree(path.constData(), &options, UINT32_MAX, &result), expected);
        QCOMPARE(splitCalls, 0);
        QCOMPARE(syncCalls, 0);
        QCOMPARE(progressCalls, 0u);
        QCOMPARE(ExtractIsoTree(path.constData(), output_.toUtf8().constData(), &options, &result), expected);
        active = nativeMode = false;
        if (expected != 0)
        {
            QCOMPARE(splitCalls, 0);
            QVERIFY(QDir(output_).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
        }
        else if (splitInput)
        {
            QCOMPARE(result.files, uint64_t(1));
            QCOMPARE(result.bytes_written, inputSize);
            QVERIFY(result.required_bytes > result.bytes_written);
            verifyNativeParts(output_ + QStringLiteral("/sources"), result.split_parts, result.split_bytes);
        }
        else
        {
            QCOMPARE(splitCalls, 0);
            QCOMPARE(result.split_parts, 0u);
            QCOMPARE(result.required_bytes, result.bytes_written);
            QCOMPARE(QFileInfo(output_ + QStringLiteral("/sources/install.wim")).size(), qint64(inputSize));
        }
        QCOMPARE(imageHash(), before);
    }

    void extractionFailureProgress()
    {
        makeInput(true, WIMLIB_COMPRESSION_TYPE_LZX, 0);
        const ISO_EXTRACT_OPTIONS options = {{true, true, {0, 0}}, nullptr, nullptr, true, 0, nullptr, false, false, false, nullptr, nullptr};
        ISO_EXTRACT_RESULT result = {};
        fault = CancelFirstPart;
        active = nativeMode = true;
        QCOMPARE(ExtractIsoTree(image_.toUtf8().constData(), output_.toUtf8().constData(), &options, &result), ECANCELED);
        active = nativeMode = false;
        QVERIFY(validProgress);
        QVERIFY(progressBlocks > 0);
        QVERIFY(QDir(output_ + QStringLiteral("/sources")).entryList(
            QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
        QCOMPARE(result.split_parts, 0u);
    }

    void insufficientSpace_data()
    {
        QTest::addColumn<int>("failure");
        QTest::addColumn<int>("expected");
        QTest::newRow("no-space") << int(NoFreeSpace) << ENOSPC;
        QTest::newRow("invalid-unit") << int(InvalidSpaceUnit) << EIO;
        QTest::newRow("query-failed") << int(SpaceQueryFailure) << EIO;
    }

    void insufficientSpace()
    {
        QFETCH(int, failure);
        QFETCH(int, expected);
        makeInput(true, WIMLIB_COMPRESSION_TYPE_LZX, 0);
        const ISO_EXTRACT_OPTIONS options = {{true, true, {0, 0}}, nullptr, nullptr, true, 0, nullptr, false, false, false, nullptr, nullptr};
        ISO_EXTRACT_RESULT result = {};
        fault = Fault(failure);
        active = nativeMode = true;
        QCOMPARE(ExtractIsoTree(image_.toUtf8().constData(), output_.toUtf8().constData(), &options, &result), expected);
        active = nativeMode = false;
        QCOMPARE(splitCalls, 0);
        QCOMPARE(syncCalls, 0);
        QVERIFY(QDir(output_).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestWimSplit)
#include "test_wim_split.moc"
