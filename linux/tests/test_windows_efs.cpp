// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/windows_apply.h"
#include "platform/wim_file.h"
extern "C" {
#include "native_ntfs_efs.h"
#include <volume.h>
#include <dir.h>
#include <attrib.h>
#include <security.h>
int MarkWimEfsFixture(WIMStruct*, int);
}
#undef min
#undef max
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest>
#include <memory>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

static bool cancelled;
enum class EfsWriteFault { None, ShortWrite, Failure, Cancel };
static EfsWriteFault writeFault;
static bool writeFaultHit;
extern "C" int IsIsoScanCancelled(void) { return cancelled; }
extern "C" int IsRufusJobCancelled(void) { return cancelled; }
extern "C" void uprintf(const char*, ...) {}
extern "C" void uprintfs(const char*) {}
extern "C" void _UpdateProgressWithInfo(int, int, uint64_t, uint64_t, int) {}
extern "C" s64 __real_ntfs_attr_pwrite(ntfs_attr*, s64, s64, const void*);
extern "C" s64 __wrap_ntfs_attr_pwrite(ntfs_attr* attribute, s64 offset, s64 count, const void* data)
{
    const bool fixturePayload = count >= 16 && attribute->type == AT_DATA
        && memcmp(data, "AAAAAAAAAAAAAAAA", 16) == 0;
    if (!writeFaultHit && writeFault != EfsWriteFault::None && fixturePayload) {
        writeFaultHit = true;
        if (writeFault == EfsWriteFault::Failure) { errno = ENOSPC; return -1; }
        const s64 written = __real_ntfs_attr_pwrite(attribute, offset, qMin(count, s64(73)), data);
        if (writeFault == EfsWriteFault::Cancel) cancelled = true;
        return written;
    }
    return __real_ntfs_attr_pwrite(attribute, offset, count, data);
}

namespace
{
constexpr qint64 volumeBytes = 64 * 1024 * 1024;
QByteArray unicode(const QString& text)
{
    QByteArray bytes(2 * text.size() + 2, '\0');
    for (qsizetype i = 0; i < text.size(); ++i) qToLittleEndian<quint16>(text[i].unicode(), bytes.data() + 2 * i);
    return bytes;
}
QByteArray metadata()
{
    QByteArray bytes(96, '\0');
    qToLittleEndian<quint32>(bytes.size(), bytes.data());
    qToLittleEndian<quint32>(2, bytes.data() + 8);
    bytes.replace(16, 16, QByteArray::fromHex("00112233445566778899aabbccddeeff"));
    return bytes;
}
QByteArray stream(const QByteArray& name, bool plain)
{
    QByteArray bytes(28, '\0');
    qToLittleEndian<quint32>(28 + name.size(), bytes.data());
    bytes.replace(4, 8, QByteArray("N\0T\0F\0S\0", 8));
    qToLittleEndian<quint32>(plain, bytes.data() + 12);
    qToLittleEndian<quint32>(name.size(), bytes.data() + 24);
    return bytes + name;
}
QByteArray segment(const QByteArray& payload)
{
    QByteArray bytes(16, '\0');
    qToLittleEndian<quint32>(16 + payload.size(), bytes.data());
    bytes.replace(4, 8, QByteArray("G\0U\0R\0E\0", 8));
    return bytes + payload;
}
QByteArray encryptedSegment(quint64 start, quint32 logical, quint32 valid,
    const QList<quint32>& sizes, const QByteArray& payload, unsigned shift = 12)
{
    QByteArray header(28 + sizes.size() * 4, '\0');
    qToLittleEndian<quint64>(start, header.data());
    qToLittleEndian<quint32>(header.size(), header.data() + 8);
    qToLittleEndian<quint32>(logical, header.data() + 12);
    qToLittleEndian<quint32>(valid, header.data() + 16);
    header[22] = header[23] = shift;
    header[24] = 9;
    header[25] = 1;
    qToLittleEndian<quint16>(sizes.size(), header.data() + 26);
    for (qsizetype i = 0; i < sizes.size(); ++i) qToLittleEndian<quint32>(sizes[i], header.data() + 28 + 4 * i);
    return segment(header + payload);
}
QByteArray raw(bool directory = false)
{
    QByteArray bytes(20, '\0');
    qToLittleEndian<quint32>(0x100, bytes.data());
    bytes.replace(4, 8, QByteArray("R\0O\0B\0S\0", 8));
    bytes += stream(QByteArray::fromHex("1019"), false) + segment(metadata());
    if (directory) return bytes;
    bytes += stream(unicode(QStringLiteral("::$DATA")), false);
    bytes += encryptedSegment(0, 4096, 4096, {8192}, QByteArray(8192, 'A'));
    bytes += encryptedSegment(4096, 4884, 4704, {0, 4096}, QByteArray(4096, 'B'));
    bytes += stream(unicode(QStringLiteral(":secret:$DATA")), false);
    // The wire format can include more than one unit across VDL. Bytes past
    // the logical stream's sector padding must not change its saved size.
    bytes += encryptedSegment(0, 7, 7, {1024}, QByteArray(1024, 'C'), 9);
    bytes += stream(unicode(QStringLiteral(":visible:$DATA")), true) + segment("visible");
    bytes += stream(unicode(QStringLiteral(":empty:$DATA")), false);
    return bytes;
}
bool put(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
QByteArray hash(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash digest(QCryptographicHash::Sha256);
    return digest.addData(&file) ? digest.result() : QByteArray();
}
}

class WindowsEfsTest : public QObject
{
    Q_OBJECT
    QTemporaryDir temporary_;

    int parse(const QByteArray& data, int chunk)
    {
        WimNtfsEfs* state = nullptr;
        int error = BeginWimNtfsEfs(nullptr, data.size(), &state);
        for (qsizetype offset = 0; error == 0 && offset < data.size(); offset += chunk)
            error = ConsumeWimNtfsEfs(state, offset, data.constData() + offset, qMin(qsizetype(chunk), data.size() - offset));
        if (error == 0) error = FinishWimNtfsEfs(state);
        const int closed = CloseWimNtfsEfs(state);
        return error != 0 ? error : closed;
    }
    void readStream(ntfs_inode* file, const QString& name, const QByteArray& expected,
        quint64 logical, quint64 valid, bool encrypted)
    {
        QByteArray encoded = unicode(name);
        std::unique_ptr<ntfs_attr, decltype(&ntfs_attr_close)> retained(ntfs_attr_open(file, AT_DATA,
            name.isEmpty() ? AT_UNNAMED : reinterpret_cast<ntfschar*>(encoded.data()), name.size()), ntfs_attr_close);
        ntfs_attr* attribute = retained.get();
        QVERIFY(attribute != nullptr);
        QCOMPARE(quint64(attribute->data_size), logical);
        QCOMPARE(quint64(attribute->initialized_size), valid);
        QCOMPARE(bool(attribute->data_flags & ATTR_IS_ENCRYPTED), encrypted);
        const quint64 readable = encrypted && logical != 0 ? ((logical + 511) & ~quint64(511)) : logical;
        QByteArray actual(readable, '\0');
        if (encrypted && readable != 0) {
            // Read the stored ciphertext without the provider's raw API VDL
            // restriction or its synthetic padding trailer. Keep both saved
            // length fields unchanged. Holes are expanded by the runlist API.
            QCOMPARE(ntfs_attr_map_whole_runlist(attribute), 0);
            if (name.isEmpty() && logical != 0) {
                QVERIFY(attribute->data_flags & ATTR_IS_SPARSE);
                QCOMPARE(ntfs_rl_vcn_to_lcn(attribute->rl, 1), LCN(LCN_HOLE));
            }
            QCOMPARE(ntfs_rl_pread(file->vol, attribute->rl, 0, readable, actual.data()), s64(readable));
        } else QCOMPARE(ntfs_attr_pread(attribute, 0, readable, actual.data()), s64(readable));
        QCOMPARE(actual, expected);
    }
private slots:
    void initTestCase()
    {
        QVERIFY(temporary_.isValid());
        QCOMPARE(wimlib_global_init(WIMLIB_INIT_FLAG_ASSUME_UTF8 | WIMLIB_INIT_FLAG_DEFAULT_CASE_INSENSITIVE), 0);
    }
    void cleanupTestCase() { wimlib_global_cleanup(); }
    void init()
    {
        cancelled = false;
        writeFault = EfsWriteFault::None;
        writeFaultHit = false;
    }
    void framing_data()
    {
        QTest::addColumn<int>("chunk");
        QTest::newRow("single-byte") << 1;
        QTest::newRow("split-headers") << 17;
        QTest::newRow("whole-buffer") << 65536;
    }
    void framing()
    {
        QFETCH(int, chunk);
        QCOMPARE(parse(raw(), chunk), 0);
        QCOMPARE(parse(raw(true), chunk), 0);
    }
    void malformed_data()
    {
        QTest::addColumn<QString>("change");
        for (const char* change : {"signature", "truncated", "stream-size", "missing-metadata", "bad-metadata-size", "duplicate", "bad-block-size", "trailing"})
            QTest::newRow(change) << QString::fromLatin1(change);
    }
    void malformed()
    {
        QFETCH(QString, change);
        QByteArray bytes = raw();
        if (change == "signature") bytes[4] = 'X';
        if (change == "truncated") bytes.chop(1);
        if (change == "stream-size") qToLittleEndian<quint32>(UINT32_MAX, bytes.data() + 20);
        if (change == "missing-metadata") bytes.remove(20, 30 + 16 + metadata().size());
        if (change == "bad-metadata-size") bytes[20 + 30 + 16] = 0;
        if (change == "duplicate") bytes += stream(unicode(QStringLiteral(":empty:$DATA")), false);
        if (change == "bad-block-size") {
            const int position = bytes.indexOf(QByteArray("G\0U\0R\0E\0", 8), 20 + 30 + 16 + metadata().size());
            QVERIFY(position > 0);
            qToLittleEndian<quint32>(511, bytes.data() + position - 4 + 16 + 28);
        }
        if (change == "trailing") bytes += 'X';
        QVERIFY(parse(bytes, 23) != 0);
    }
    void apply_data()
    {
        QTest::addColumn<int>("compression");
        QTest::addColumn<bool>("solid");
        QTest::addColumn<QString>("change");
        QTest::newRow("WIM-XPRESS") << int(WIMLIB_COMPRESSION_TYPE_XPRESS) << false << QString();
        QTest::newRow("WIM-LZX") << int(WIMLIB_COMPRESSION_TYPE_LZX) << false << QString();
        QTest::newRow("ESD-LZMS") << int(WIMLIB_COMPRESSION_TYPE_LZMS) << true << QString();
        QTest::newRow("prewrite-refusal") << int(WIMLIB_COMPRESSION_TYPE_LZX) << false << QStringLiteral("bad");
        QTest::newRow("sparse-cluster-refusal") << int(WIMLIB_COMPRESSION_TYPE_LZX) << false << QStringLiteral("cluster");
        QTest::newRow("short-write") << int(WIMLIB_COMPRESSION_TYPE_LZX) << false << QStringLiteral("short");
        QTest::newRow("partial-write-failure") << int(WIMLIB_COMPRESSION_TYPE_LZX) << false << QStringLiteral("failure");
        QTest::newRow("partial-write-cancel") << int(WIMLIB_COMPRESSION_TYPE_LZX) << false << QStringLiteral("cancel");
    }
    void apply()
    {
        QFETCH(int, compression); QFETCH(bool, solid); QFETCH(QString, change);
        const bool bad = change == "bad" || change == "cluster";
        const QString tree = temporary_.filePath(QStringLiteral("tree"));
        QVERIFY(QDir(tree).removeRecursively());
        QVERIFY(QDir().mkpath(tree));
        QByteArray input = raw();
        if (change == "bad") input[4] = 'X';
        QVERIFY(put(tree + "/encrypted.bin", input));
        QVERIFY(put(tree + "/encrypted-directory", raw(true)));
        QVERIFY(put(tree + "/empty-encrypted.bin", raw(true)));
        QCOMPARE(link(QFile::encodeName(tree + "/encrypted.bin").constData(), QFile::encodeName(tree + "/alias.bin").constData()), 0);
        WIMStruct* captured = nullptr;
        QCOMPARE(wimlib_create_new_wim(wimlib_compression_type(compression), &captured), 0);
        QCOMPARE(wimlib_add_image(captured, QFile::encodeName(tree).constData(), "EFS raw fixture", nullptr, 0), 0);
        QCOMPARE(MarkWimEfsFixture(captured, 1), 0);
        const QByteArray image = QFile::encodeName(temporary_.filePath("input.wim"));
        QFile::remove(QString::fromUtf8(image));
        QCOMPARE(wimlib_write(captured, image.constData(), WIMLIB_ALL_IMAGES, solid ? WIMLIB_WRITE_FLAG_SOLID : 0, 1), 0);
        wimlib_free(captured);
        const QString target = temporary_.filePath("target.ntfs");
        QFile targetFile(target);
        QVERIFY(targetFile.open(QIODevice::ReadWrite | QIODevice::Truncate));
        QVERIFY(targetFile.resize(volumeBytes));
        targetFile.close();
        QProcess formatter;
        formatter.start(QStringLiteral(RUFUS_MKNTFS_PATH), {"-F", "-Q", "-c", change == "cluster" ? "8192" : "4096", target});
        QVERIFY(formatter.waitForFinished(30000));
        QCOMPARE(formatter.exitCode(), 0);
        const QByteArray before = bad ? hash(target) : QByteArray();
        IMAGE_FILE_SOURCE source = {-1, {}, false, false, nullptr, 0};
        QCOMPARE(OpenImageFileSource(image.constData(), &source), 0);
        WIMStruct* wim = nullptr;
        QCOMPARE(OpenWimFileDescriptor(source.fd, &wim), 0);
        const int descriptor = open(QFile::encodeName(target).constData(), O_RDWR | O_CLOEXEC);
        QVERIFY(descriptor >= 0);
        rufus::WindowsApplyResult result;
        if (change == "short") writeFault = EfsWriteFault::ShortWrite;
        if (change == "failure") writeFault = EfsWriteFault::Failure;
        if (change == "cancel") writeFault = EfsWriteFault::Cancel;
        const int error = rufus::ApplyWindowsImage(wim, 1, source, image.constData(), descriptor, 0, volumeBytes, result);
        cancelled = false;
        writeFault = EfsWriteFault::None;
        QCOMPARE(close(descriptor), 0);
        wimlib_free(wim);
        CloseImageFileSource(&source);
        if (bad) {
            QVERIFY(error != 0 && !result.started);
            if (change == "cluster") QCOMPARE(error, ENOTSUP);
            QCOMPARE(hash(target), before);
            return;
        }
        if (!change.isEmpty()) QVERIFY(writeFaultHit);
        if (change == "failure" || change == "cancel") {
            QVERIFY(error != 0 && result.started);
            QCOMPARE(result.syncError, 0);
            if (change == "cancel") QCOMPARE(error, ECANCELED);
            return;
        }
        QCOMPARE(error, 0);
        QCOMPARE(result.plan.efsClusterLimit, quint32(4096));
        ntfs_volume* volume = ntfs_mount(QFile::encodeName(target).constData(), NTFS_MNT_RDONLY | NTFS_MNT_FORENSIC);
        QVERIFY(volume != nullptr);
        volume->efs_raw = TRUE;
        ntfs_inode* alias = ntfs_pathname_to_inode(volume, nullptr, "alias.bin");
        QVERIFY(alias != nullptr);
        const quint64 aliasInode = alias->mft_no;
        // Do not keep two handles for one cached inode open through hardlink names.
        QCOMPARE(ntfs_inode_close(alias), 0);
        ntfs_inode* file = ntfs_pathname_to_inode(volume, nullptr, "encrypted.bin");
        ntfs_inode* directory = ntfs_pathname_to_inode(volume, nullptr, "encrypted-directory");
        ntfs_inode* empty = ntfs_pathname_to_inode(volume, nullptr, "empty-encrypted.bin");
        QVERIFY(file && directory && empty);
        QCOMPARE(file->mft_no, aliasInode);
        QVERIFY(file->flags & FILE_ATTR_ENCRYPTED);
        QVERIFY(directory->flags & FILE_ATTR_ENCRYPTED);
        QVERIFY(directory->mrec->flags & MFT_RECORD_IS_DIRECTORY);
        readStream(file, {}, QByteArray(4096, 'A') + QByteArray(4096, '\0') + QByteArray(1024, 'B'), 8980, 8800, true);
        readStream(file, QStringLiteral("secret"), QByteArray(512, 'C'), 7, 7, true);
        readStream(file, QStringLiteral("visible"), "visible", 7, 7, false);
        readStream(file, QStringLiteral("empty"), {}, 0, 0, true);
        readStream(empty, {}, {}, 0, 0, true);
        ntfschar efs[] = {qToLittleEndian<quint16>('$'), qToLittleEndian<quint16>('E'), qToLittleEndian<quint16>('F'), qToLittleEndian<quint16>('S')};
        for (ntfs_inode* inode : {file, directory}) {
            s64 size = 0;
            void* value = ntfs_attr_readall(inode, AT_LOGGED_UTILITY_STREAM, efs, 4, &size);
            QVERIFY(value != nullptr);
            QCOMPARE(QByteArray(static_cast<char*>(value), size), metadata());
            free(value);
        }
        QCOMPARE(ntfs_inode_close(file), 0);
        QCOMPARE(ntfs_inode_close(directory), 0);
        QCOMPARE(ntfs_inode_close(empty), 0);
        QCOMPARE(ntfs_umount(volume, FALSE), 0);
    }
};
QTEST_GUILESS_MAIN(WindowsEfsTest)
#include "test_windows_efs.moc"
