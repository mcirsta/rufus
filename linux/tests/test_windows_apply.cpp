// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/windows_apply.h"
#include "platform/wim_file.h"
#include "platform/windows_install_options.h"
#include "iso_analyze.h"
#include "iso_file.h"

extern "C" {
#include "native_input.h"
#include <volume.h>
#include <dir.h>
#include <attrib.h>
#include <security.h>
#include <reparse.h>
#include <ea.h>
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
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/xattr.h>

namespace
{
enum Fault { NoFault, CancelBefore, CancelStreams, MutateSource, SyncFailure };
Fault fault;
bool cancelled;
int targetFd = -1, syncCalls;
QByteArray sourcePath;
wimlib_progress_func_t progressFunction;
void* progressContext;
constexpr qint64 volumeBytes = 64 * 1024 * 1024;
constexpr int commandTimeoutMs = 30000;

bool writeFile(const QByteArray& path, const QByteArray& bytes)
{
    QFile file(QString::fromUtf8(path));
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.flush();
}

QByteArray fileHash(const QByteArray& path)
{
    QFile file(QString::fromUtf8(path));
    if (!file.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) return {};
    return hash.result();
}

QByteArray payload()
{
    QByteArray bytes(4 * 1024 * 1024, Qt::Uninitialized);
    for (qsizetype i = 0; i < bytes.size(); ++i)
        bytes[i] = char((i * 31 + i / 251) % 251);
    return bytes;
}

QByteArray unicodeName(const QString& name)
{
    QByteArray bytes(name.size() * 2, Qt::Uninitialized);
    for (qsizetype i = 0; i < name.size(); ++i)
        qToLittleEndian<quint16>(name[i].unicode(), bytes.data() + i * 2);
    return bytes;
}

QByteArray reparseData()
{
    const QByteArray substitute = unicodeName(QStringLiteral("\\??\\C:\\Windows"));
    const QByteArray display = unicodeName(QStringLiteral("C:\\Windows"));
    QByteArray bytes(16, '\0');
    bytes += substitute + QByteArray(2, '\0') + display + QByteArray(2, '\0');
    qToLittleEndian<quint32>(0xa0000003, bytes.data());
    qToLittleEndian<quint16>(quint16(bytes.size() - 8), bytes.data() + 4);
    qToLittleEndian<quint16>(quint16(substitute.size()), bytes.data() + 10);
    qToLittleEndian<quint16>(quint16(substitute.size() + 2), bytes.data() + 12);
    qToLittleEndian<quint16>(quint16(display.size()), bytes.data() + 14);
    return bytes;
}

QByteArray readAttribute(ntfs_inode* file, const QString& stream = {})
{
    QByteArray name = unicodeName(stream);
    ntfs_attr* attribute = ntfs_attr_open(file, AT_DATA,
        name.isEmpty() ? AT_UNNAMED : reinterpret_cast<ntfschar*>(name.data()), stream.size());
    if (attribute == nullptr) return {};
    QByteArray bytes;
    if (attribute->data_size >= 0 && attribute->data_size <= volumeBytes)
    {
        bytes.resize(attribute->data_size);
        if (ntfs_attr_pread(attribute, 0, bytes.size(), bytes.data()) != bytes.size())
            bytes.clear();
    }
    ntfs_attr_close(attribute);
    return bytes;
}

bool writeAttribute(ntfs_inode* file, const QString& stream, const QByteArray& bytes)
{
    QByteArray name = unicodeName(stream);
    if (!name.isEmpty() && ntfs_attr_add(file, AT_DATA,
        reinterpret_cast<ntfschar*>(name.data()), stream.size(), nullptr, 0) != 0)
        return false;
    ntfs_attr* attribute = ntfs_attr_open(file, AT_DATA,
        name.isEmpty() ? AT_UNNAMED : reinterpret_cast<ntfschar*>(name.data()), stream.size());
    if (attribute == nullptr) return false;
    const bool written = ntfs_attr_pwrite(attribute, 0, bytes.size(), bytes.constData()) == bytes.size();
    ntfs_attr_close(attribute);
    return written;
}

void closeSecure(ntfs_volume* volume)
{
    if (volume->secure_ni == nullptr) return;
    ntfs_index_ctx_put(volume->secure_xsii);
    ntfs_index_ctx_put(volume->secure_xsdh);
    if (ntfs_inode_close(volume->secure_ni) != 0) abort();
    volume->secure_ni = nullptr;
}

struct NtfsSnapshot
{
    QByteArray data, stream, security, reparse, dosName;
    quint32 attributes = 0;
    quint64 created = 0, written = 0, accessed = 0;
    bool alias = false;
};

bool inspectNtfs(const QByteArray& path, NtfsSnapshot& snapshot)
{
    ntfs_volume* volume = ntfs_mount(path.constData(), NTFS_MNT_RDONLY | NTFS_MNT_FORENSIC);
    if (volume == nullptr) return false;
    bool ok = ntfs_open_secure(volume) == 0;
    ntfs_inode* root = ntfs_inode_open(volume, FILE_root);
    ntfs_inode* file = ntfs_pathname_to_inode(volume, nullptr, "payload.bin");
    ntfs_inode* alias = ntfs_pathname_to_inode(volume, nullptr, "alias.bin");
    ntfs_inode* junction = ntfs_pathname_to_inode(volume, nullptr, "junction");
    ok = ok && root != nullptr && file != nullptr && alias != nullptr && junction != nullptr;
    if (ok)
    {
        snapshot.alias = file->mft_no == alias->mft_no;
        snapshot.data = readAttribute(file);
        snapshot.stream = readAttribute(file, QStringLiteral("Zone.Identifier"));
        snapshot.attributes = le32_to_cpu(file->flags);
        snapshot.created = le64_to_cpu(file->creation_time);
        snapshot.written = le64_to_cpu(file->last_data_change_time);
        snapshot.accessed = le64_to_cpu(file->last_access_time);
        SECURITY_CONTEXT context = {};
        context.vol = volume;
        const int securityBytes = ntfs_get_ntfs_acl(&context, file, nullptr, 0);
        ok = securityBytes > 0 && securityBytes < 65536;
        if (ok)
        {
            snapshot.security.resize(securityBytes);
            ok = ntfs_get_ntfs_acl(&context, file, snapshot.security.data(), securityBytes) == securityBytes;
        }
        // The convenience DOS-name getter rejects hardlinked files. Inspect
        // their on-disk FILE_NAME attributes instead.
        ntfs_attr_search_ctx* names = ntfs_attr_get_search_ctx(file, nullptr);
        if (names == nullptr) ok = false;
        while (names != nullptr && ntfs_attr_lookup(AT_FILE_NAME, nullptr, 0,
            CASE_SENSITIVE, 0, nullptr, 0, names) == 0)
        {
            const quint32 offset = le16_to_cpu(names->attr->value_offset);
            const quint32 length = le32_to_cpu(names->attr->length);
            if (names->attr->non_resident || offset > length || sizeof(FILE_NAME_ATTR) > length - offset)
            { ok = false; break; }
            const auto* name = reinterpret_cast<const FILE_NAME_ATTR*>(reinterpret_cast<const char*>(names->attr) + offset);
            const quint32 nameBytes = quint32(name->file_name_length) * 2;
            if (nameBytes > length - offset - sizeof(FILE_NAME_ATTR)) { ok = false; break; }
            if (name->file_name_type == FILE_NAME_DOS)
                snapshot.dosName = QByteArray(reinterpret_cast<const char*>(name->file_name), nameBytes);
        }
        if (names != nullptr) ntfs_attr_put_search_ctx(names);
        const int reparseBytes = ntfs_get_ntfs_reparse_data(junction, nullptr, 0);
        ok = ok && reparseBytes > 0 && reparseBytes < 65536;
        if (ok)
        {
            snapshot.reparse.resize(reparseBytes);
            ok = ntfs_get_ntfs_reparse_data(junction, snapshot.reparse.data(), reparseBytes) == reparseBytes;
        }
    }
    if (file != nullptr && ntfs_inode_close(file) != 0) ok = false;
    if (alias != nullptr && ntfs_inode_close(alias) != 0) ok = false;
    if (junction != nullptr && ntfs_inode_close(junction) != 0) ok = false;
    if (root != nullptr && ntfs_inode_close(root) != 0) ok = false;
    closeSecure(volume);
    return ntfs_umount(volume, FALSE) == 0 && ok;
}
}

extern "C" int IsIsoScanCancelled(void) { return cancelled; }
extern "C" int IsRufusJobCancelled(void) { return cancelled; }
extern "C" void uprintf(const char*, ...) {}
extern "C" void uprintfs(const char*) {}
extern "C" void _UpdateProgressWithInfo(int, int, uint64_t, uint64_t, int) {}
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd)
{
    if (fd == targetFd)
    {
        ++syncCalls;
        if (fault == SyncFailure) { errno = EIO; return -1; }
    }
    return __real_fsync(fd);
}
extern "C" void __real_wimlib_register_progress_function(WIMStruct*, wimlib_progress_func_t, void*);
static wimlib_progress_status testProgress(wimlib_progress_msg message, wimlib_progress_info* info, void*)
{
    if (fault == CancelStreams && message == WIMLIB_PROGRESS_MSG_EXTRACT_STREAMS && info->extract.completed_bytes > 0)
        cancelled = true;
    if (fault == MutateSource && message == WIMLIB_PROGRESS_MSG_EXTRACT_IMAGE_BEGIN)
    {
        QFile file(QString::fromUtf8(sourcePath));
        if (!file.open(QIODevice::Append) || file.write("changed", 7) != 7 || !file.flush()) abort();
    }
    return progressFunction(message, info, progressContext);
}
extern "C" void __wrap_wimlib_register_progress_function(WIMStruct* wim, wimlib_progress_func_t function, void* context)
{
    progressFunction = function;
    progressContext = context;
    __real_wimlib_register_progress_function(wim, function != nullptr ? testProgress : nullptr, nullptr);
}

class WindowsApplyTest : public QObject
{
    Q_OBJECT
    QTemporaryDir temporary_;
    QByteArray sourceVolume_, imagePath_, targetPath_;
    NtfsSnapshot original_;

    bool format(const QByteArray& path)
    {
        QFile image(QString::fromUtf8(path));
        if (!image.open(QIODevice::WriteOnly) || !image.resize(volumeBytes)) return false;
        image.close();
        QProcess command;
        command.start(QStringLiteral(RUFUS_MKNTFS_PATH), {QStringLiteral("--force"), QStringLiteral("--quick"),
            QStringLiteral("--sector-size"), QStringLiteral("512"), QString::fromUtf8(path)});
        return command.waitForFinished(commandTimeoutMs) && command.exitStatus() == QProcess::NormalExit
            && command.exitCode() == 0;
    }

    bool prepareSource()
    {
        ntfs_volume* volume = ntfs_mount(sourceVolume_.constData(), NTFS_MNT_EXCLUSIVE);
        if (volume == nullptr) return false;
        bool ok = ntfs_open_secure(volume) == 0;
        ntfs_inode* root = ntfs_inode_open(volume, FILE_root);
        const QByteArray name = unicodeName(QStringLiteral("payload.bin"));
        ntfs_inode* file = root == nullptr ? nullptr : ntfs_create(root, 0,
            reinterpret_cast<const ntfschar*>(name.constData()), name.size() / 2, S_IFREG);
        ok = ok && file != nullptr;
        if (ok)
        {
            ok = writeAttribute(file, {}, payload()) && writeAttribute(file,
                QStringLiteral("Zone.Identifier"), QByteArray("[ZoneTransfer]\r\nZoneId=3\r\n"));
            if (!ok) qWarning("NTFS fixture data: %s", strerror(errno));
            SECURITY_CONTEXT context = {};
            context.vol = volume;
            const int bytes = ntfs_get_ntfs_acl(&context, root, nullptr, 0);
            const bool validSecurity = bytes > 0 && bytes < 65536;
            QByteArray security(validSecurity ? bytes : 0, Qt::Uninitialized);
            ok = ok && validSecurity && ntfs_get_ntfs_acl(&context, root, security.data(), security.size()) == bytes;
            ok = ok && ntfs_set_ntfs_acl(&context, file, security.constData(), security.size(), 0) == 0;
            if (!ok) qWarning("NTFS fixture ACL (%d bytes): %s", bytes, strerror(errno));
            const le32 flags = cpu_to_le32(0x00000027); // readonly, hidden, system, archive
            ok = ok && ntfs_set_ntfs_attrib(file, reinterpret_cast<const char*>(&flags), sizeof(flags), 0) == 0;
            const le64 times[] = {cpu_to_le64(132000000000000001ULL), cpu_to_le64(132000000000000002ULL),
                cpu_to_le64(132000000000000003ULL)};
            ok = ok && ntfs_inode_set_times(file, reinterpret_cast<const char*>(times), sizeof(times), 0) == 0;
            if (!ok) qWarning("NTFS fixture attributes/times: %s", strerror(errno));
            if (ok)
            {
                const quint64 inode = file->mft_no;
                ok = ntfs_inode_close_in_dir(file, root) == 0;
                ok = ntfs_inode_close(root) == 0 && ok;
                file = ntfs_inode_open(volume, inode);
                root = ntfs_inode_open(volume, FILE_root);
                ok = ok && file != nullptr && root != nullptr;
                const QByteArray dos("PAYLOA~1.BIN");
                // This setter takes UTF-8 and consumes both inode handles.
                if (ok)
                {
                    ok = ntfs_set_ntfs_dos_name(file, root, dos.constData(), dos.size(), 0) == 0;
                    file = nullptr;
                    root = nullptr;
                }
                if (ok)
                {
                    root = ntfs_inode_open(volume, FILE_root);
                    file = ntfs_inode_open(volume, inode);
                    ok = root != nullptr && file != nullptr;
                    const QByteArray alias = unicodeName(QStringLiteral("alias.bin"));
                    // Install the DOS alias before adding the second hard link,
                    // as wimlib's NTFS restore does.
                    ok = ok && ntfs_link(file, root, reinterpret_cast<const ntfschar*>(alias.constData()), alias.size() / 2) == 0;
                }
                if (!ok) qWarning("NTFS fixture DOS name: %s", strerror(errno));
            }
        }
        if (file != nullptr && ntfs_inode_close_in_dir(file, root) != 0) ok = false;
        const QByteArray junctionName = unicodeName(QStringLiteral("junction"));
        ntfs_inode* junction = root == nullptr ? nullptr : ntfs_create(root, 0,
            reinterpret_cast<const ntfschar*>(junctionName.constData()), junctionName.size() / 2, S_IFDIR);
        if (junction != nullptr)
        {
            const QByteArray data = reparseData();
            ok = ok && ntfs_set_ntfs_reparse_data(junction, data.constData(), data.size(), 0) == 0;
            if (!ok) qWarning("NTFS fixture reparse: %s", strerror(errno));
            if (ntfs_inode_close_in_dir(junction, root) != 0) ok = false;
        }
        else ok = false;
        if (root != nullptr && ntfs_inode_close(root) != 0) ok = false;
        closeSecure(volume);
        return ntfs_umount(volume, FALSE) == 0 && ok;
    }

    int capture(int compression, bool solid, bool extendedAttributes = false)
    {
        QFile::remove(QString::fromUtf8(imagePath_));
        WIMStruct* wim = nullptr;
        int error = wimlib_create_new_wim(static_cast<wimlib_compression_type>(compression), &wim);
        if (error == 0)
            error = wimlib_add_image(wim, sourceVolume_.constData(), "NTFS metadata", nullptr, WIMLIB_ADD_FLAG_NTFS);
        const QByteArray second = temporary_.filePath("second").toUtf8();
        if (!QDir().mkpath(QString::fromUtf8(second)) || !writeFile(second + "/selected.txt", "edition two\n"))
            error = WIMLIB_ERR_WRITE;
        if (extendedAttributes)
        {
            const QByteArray path = second + "/selected.txt";
            const QByteArray binary = QByteArray::fromHex("00010200ff7f0042");
            const QByteArray large(1027, 'X');
            const bool added = setxattr(path.constData(), "user.binary", binary.constData(), binary.size(), 0) == 0
                && setxattr(path.constData(), "user.empty", "", 0, 0) == 0
                && setxattr(path.constData(), "user.large", large.constData(), large.size(), 0) == 0;
            if (!added) error = WIMLIB_ERR_SET_XATTR;
        }
        if (error == 0) error = wimlib_add_image(wim, second.constData(), "Second edition", nullptr,
            extendedAttributes ? WIMLIB_ADD_FLAG_UNIX_DATA : 0);
        if (error == 0) error = wimlib_set_image_property(wim, 1, "DISPLAYNAME", "Windows Ω edition");
        if (error == 0) error = wimlib_set_image_property(wim, 2, "DESCRIPTION", "Second edition");
        if (error == 0) error = wimlib_set_image_property(wim, 1, "WINDOWS/VERSION/MAJOR", "10");
        if (error == 0) error = wimlib_set_image_property(wim, 1, "WINDOWS/VERSION/MINOR", "0");
        if (error == 0) error = wimlib_set_image_property(wim, 1, "WINDOWS/VERSION/BUILD", "26100");
        if (error == 0) error = wimlib_set_image_property(wim, 1, "WINDOWS/VERSION/SPBUILD", "1");
        if (error == 0) error = wimlib_write(wim, imagePath_.constData(), WIMLIB_ALL_IMAGES,
            solid ? WIMLIB_WRITE_FLAG_SOLID : 0, 1);
        wimlib_free(wim);
        return error;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(getuid() != 0);
        QVERIFY(temporary_.isValid());
        QCOMPARE(wimlib_global_init(WIMLIB_INIT_FLAG_ASSUME_UTF8 | WIMLIB_INIT_FLAG_DEFAULT_CASE_INSENSITIVE), 0);
        sourceVolume_ = temporary_.filePath("source.ntfs").toUtf8();
        imagePath_ = temporary_.filePath("selected.wim").toUtf8();
        targetPath_ = temporary_.filePath("target.ntfs").toUtf8();
        QVERIFY(format(sourceVolume_));
        QVERIFY(prepareSource());
        QVERIFY(inspectNtfs(sourceVolume_, original_));
        QCOMPARE(original_.data, payload());
        QCOMPARE(original_.reparse, reparseData());
        QVERIFY(!original_.security.isEmpty());
        QCOMPARE(original_.dosName, unicodeName(QStringLiteral("PAYLOA~1.BIN")));
        QVERIFY(original_.alias);
    }

    void init()
    {
        cancelled = false;
        fault = NoFault;
        targetFd = -1;
        syncCalls = 0;
    }

    void nativeApply_data()
    {
        QTest::addColumn<int>("compression");
        QTest::addColumn<bool>("solid");
        QTest::addColumn<bool>("isoSource");
        QTest::newRow("plain-WIM") << int(WIMLIB_COMPRESSION_TYPE_NONE) << false << false;
        QTest::newRow("XPRESS-WIM") << int(WIMLIB_COMPRESSION_TYPE_XPRESS) << false << false;
        QTest::newRow("LZX-WIM") << int(WIMLIB_COMPRESSION_TYPE_LZX) << false << false;
        QTest::newRow("solid-ESD") << int(WIMLIB_COMPRESSION_TYPE_LZMS) << true << false;
        QTest::newRow("solid-ESD-in-ISO") << int(WIMLIB_COMPRESSION_TYPE_LZMS) << true << true;
    }

    void nativeApply()
    {
        QFETCH(int, compression);
        QFETCH(bool, solid);
        QFETCH(bool, isoSource);
        QCOMPARE(capture(compression, solid), 0);
        sourcePath = imagePath_;
        ISO_ANALYSIS_SOURCE view = {};
        ISO_FILE_READER reader = {};
        if (isoSource)
        {
            const QString tree = temporary_.filePath("iso-tree");
            QVERIFY(QDir().mkpath(tree + "/sources"));
            QFile::remove(tree + "/sources/install.esd");
            QVERIFY(QFile::copy(QString::fromUtf8(imagePath_), tree + "/sources/install.esd"));
            sourcePath = temporary_.filePath("source.iso").toUtf8();
            QProcess command;
            command.start(QStringLiteral(RUFUS_MKISOFS_PATH), {"-quiet", "-R", "-o", QString::fromUtf8(sourcePath), tree});
            QVERIFY(command.waitForFinished(commandTimeoutMs));
            QCOMPARE(command.exitCode(), 0);
        }
        IMAGE_FILE_SOURCE source = {};
        source.fd = -1;
        QCOMPARE(OpenImageFileSource(sourcePath.constData(), &source), 0);
        QVERIFY(lseek(source.fd, 17, SEEK_SET) == 17);
        const QByteArray sourceHash = fileHash(sourcePath);
        WIMStruct* wim = nullptr;
        if (isoSource)
        {
            char retained[64];
            snprintf(retained, sizeof(retained), "/proc/self/fd/%d", source.fd);
            ISO_ANALYSIS analysis = {};
            ISO_ANALYSIS_OPTIONS options = {};
            options.enable_rockridge = options.enable_joliet = true;
            QVERIFY(AnalyzeIsoImageWithSource(retained, source.info.st_size, &options, &analysis, &view) != ISO_SCAN_ERROR);
            QCOMPARE(OpenIsoFileReader(&reader, view.iso, view.udf, "/sources/install.esd"), ISO_SCAN_OK);
            QCOMPARE(OpenWimFromIsoReader(&reader, 0, &wim), 0);
        }
        else QCOMPARE(OpenWimFileDescriptor(source.fd, &wim), 0);
        rufus::WindowsInstallOptions editions;
        QCOMPARE(rufus::ReadWindowsImageEditions(wim, editions), 0);
        QCOMPARE(editions.editions.size(), 2);
        QCOMPARE(editions.editions[0].image, 1);
        QCOMPARE(editions.editions[0].name, QString::fromUtf8("Windows Ω edition"));
        QCOMPARE(editions.editions[1].image, 2);
        QCOMPARE(editions.editions[1].name, QStringLiteral("Second edition"));
        QVERIFY(editions.nonstandardNames);
        rufus::WindowsApplyPlan plan;
        QCOMPARE(rufus::ReadWindowsApplyPlan(wim, 1, plan), 0);
        QCOMPARE(plan.metadata.win_version.major, 11);
        QCOMPARE(plan.metadata.archive.image_count, 2U);
        QCOMPARE(plan.hardLinks, 1U);
        // WIM stores the reparse payload without NTFS's eight-byte tag/length
        // header. Read its independent on-disk length rather than count it twice.
        const quint16 reparsePayload = qFromLittleEndian<quint16>(original_.reparse.constData() + 4);
        QCOMPARE(plan.bytes, quint64(original_.data.size() + original_.stream.size() + reparsePayload));
        QVERIFY(format(targetPath_));
        targetFd = open(targetPath_.constData(), O_RDWR | O_CLOEXEC);
        QVERIFY(targetFd >= 0);
        QVERIFY(lseek(targetFd, 31, SEEK_SET) == 31);
        rufus::WindowsApplyResult result;
        QCOMPARE(rufus::ApplyWindowsImage(wim, 1, source, sourcePath.constData(), targetFd, 0, volumeBytes, result), 0);
        QVERIFY(result.started);
        QCOMPARE(result.wimError, 0);
        QCOMPARE(result.syncError, 0);
        QCOMPARE(syncCalls, 1);
        QCOMPARE(result.completedBytes, result.totalBytes);
        QCOMPARE(lseek(targetFd, 0, SEEK_CUR), off_t(31));
        QCOMPARE(lseek(source.fd, 0, SEEK_CUR), off_t(17));
        QCOMPARE(close(targetFd), 0);
        targetFd = -1;
        NtfsSnapshot output;
        QVERIFY(inspectNtfs(targetPath_, output));
        QCOMPARE(output.data, original_.data);
        QCOMPARE(output.stream, original_.stream);
        QCOMPARE(output.security, original_.security);
        QCOMPARE(output.reparse, original_.reparse);
        QCOMPARE(output.dosName, original_.dosName);
        QCOMPARE(output.attributes, original_.attributes);
        QCOMPARE(output.created, original_.created);
        QCOMPARE(output.written, original_.written);
        QCOMPARE(output.accessed, original_.accessed);
        QVERIFY(output.alias);
        wimlib_free(wim);
        CloseIsoFileReader(&reader);
        CloseIsoAnalysisSource(&view);
        CloseImageFileSource(&source);
        QCOMPARE(fileHash(sourcePath), sourceHash);
    }

    void extendedAttributes()
    {
        QCOMPARE(capture(WIMLIB_COMPRESSION_TYPE_LZX, false, true), 0);
        IMAGE_FILE_SOURCE source = {};
        source.fd = -1;
        sourcePath = imagePath_;
        QCOMPARE(OpenImageFileSource(sourcePath.constData(), &source), 0);
        WIMStruct* wim = nullptr;
        QCOMPARE(OpenWimFileDescriptor(source.fd, &wim), 0);
        QVERIFY(format(targetPath_));
        targetFd = open(targetPath_.constData(), O_RDWR | O_CLOEXEC);
        QVERIFY(targetFd >= 0);
        rufus::WindowsApplyResult result;
        QCOMPARE(rufus::ApplyWindowsImage(wim, 2, source, sourcePath.constData(), targetFd, 0, volumeBytes, result), 0);
        QCOMPARE(close(targetFd), 0);
        targetFd = -1;
        wimlib_free(wim);
        CloseImageFileSource(&source);
        ntfs_volume* volume = ntfs_mount(targetPath_.constData(), NTFS_MNT_RDONLY | NTFS_MNT_FORENSIC);
        QVERIFY(volume != nullptr);
        ntfs_inode* file = ntfs_pathname_to_inode(volume, nullptr, "selected.txt");
        QVERIFY(file != nullptr);
        const int size = ntfs_get_ntfs_ea(file, nullptr, 0);
        QVERIFY(size > 0 && size < 65536);
        QByteArray raw(size, Qt::Uninitialized);
        QCOMPARE(ntfs_get_ntfs_ea(file, raw.data(), raw.size()), size);
        QMap<QByteArray, QByteArray> attributes;
        quint32 offset = 0;
        while (offset < quint32(size))
        {
            QVERIFY(quint32(size) - offset >= 8);
            const char* entry = raw.constData() + offset;
            const quint32 next = qFromLittleEndian<quint32>(entry);
            const quint8 flags = quint8(entry[4]), nameBytes = quint8(entry[5]);
            const quint16 valueBytes = qFromLittleEndian<quint16>(entry + 6);
            QVERIFY(next != 0 && next % 4 == 0 && next <= quint32(size) - offset);
            QVERIFY(quint32(8 + nameBytes + 1 + valueBytes) <= next);
            QCOMPARE(flags, quint8(0));
            QCOMPARE(entry[8 + nameBytes], '\0');
            const QByteArray name(entry + 8, nameBytes);
            QVERIFY(!attributes.contains(name));
            attributes.insert(name, QByteArray(entry + 8 + nameBytes + 1, valueBytes));
            offset += next;
        }
        QCOMPARE(attributes.size(), 3);
        QCOMPARE(attributes.value("user.binary"), QByteArray::fromHex("00010200ff7f0042"));
        QVERIFY(attributes.contains("user.empty") && attributes.value("user.empty").isEmpty());
        QCOMPARE(attributes.value("user.large"), QByteArray(1027, 'X'));
        QCOMPARE(ntfs_inode_close(file), 0);
        QCOMPARE(ntfs_umount(volume, FALSE), 0);
    }

    void refusalsAndFailures_data()
    {
        QTest::addColumn<QString>("mode");
        for (const char* mode : {"cancel-before", "cancel-streams", "mutate-source", "sync-fail", "bad-index",
            "readonly", "geometry", "capacity", "source-target", "changed-source", "invalid-ntfs", "corrupt-data", "second-edition"})
            QTest::newRow(mode) << QString::fromLatin1(mode);
    }

    void refusalsAndFailures()
    {
        QFETCH(QString, mode);
        QCOMPARE(capture(WIMLIB_COMPRESSION_TYPE_NONE, false), 0);
        if (mode == "corrupt-data")
        {
            QFile image(QString::fromUtf8(imagePath_));
            QVERIFY(image.open(QIODevice::ReadWrite));
            const QByteArray bytes = image.readAll();
            const qsizetype offset = bytes.indexOf(payload().left(1024));
            QVERIFY(offset >= 0);
            QVERIFY(image.seek(offset + 1024));
            QCOMPARE(image.write("BAD", 3), 3);
        }
        IMAGE_FILE_SOURCE source = {};
        source.fd = -1;
        sourcePath = imagePath_;
        QCOMPARE(OpenImageFileSource(sourcePath.constData(), &source), 0);
        WIMStruct* wim = nullptr;
        QCOMPARE(OpenWimFileDescriptor(source.fd, &wim), 0);
        QVERIFY(format(targetPath_));
        if (mode == "invalid-ntfs") QVERIFY(writeFile(targetPath_, QByteArray(volumeBytes, '\0')));
        if (mode == "capacity")
        {
            QFile file(QString::fromUtf8(targetPath_));
            QVERIFY(file.open(QIODevice::ReadWrite) && file.resize(1024 * 1024));
        }
        targetFd = open(targetPath_.constData(), mode == "readonly" ? O_RDONLY | O_CLOEXEC : O_RDWR | O_CLOEXEC);
        QVERIFY(targetFd >= 0);
        const QByteArray before = fileHash(targetPath_);
        if (mode == "changed-source")
        {
            QFile file(QString::fromUtf8(sourcePath));
            QVERIFY(file.open(QIODevice::Append) && file.write("changed", 7) == 7 && file.flush());
        }
        if (mode == "cancel-before") { fault = CancelBefore; cancelled = true; }
        if (mode == "cancel-streams") fault = CancelStreams;
        if (mode == "mutate-source") fault = MutateSource;
        if (mode == "sync-fail") fault = SyncFailure;
        const int image = mode == "bad-index" ? 3 : mode == "second-edition" ? 2 : 1;
        const quint64 size = mode == "geometry" ? volumeBytes + 512
            : mode == "capacity" ? 1024 * 1024 : mode == "source-target" ? source.info.st_size : volumeBytes;
        rufus::WindowsApplyResult result;
        const int sourceTarget = mode == "source-target" ? open(sourcePath.constData(), O_RDWR | O_CLOEXEC) : -1;
        if (mode == "source-target") QVERIFY(sourceTarget >= 0);
        const int output = sourceTarget >= 0 ? sourceTarget : targetFd;
        const int error = rufus::ApplyWindowsImage(wim, image, source, sourcePath.constData(), output, 0, size, result);
        if (mode == "second-edition")
        {
            QCOMPARE(error, 0);
            QCOMPARE(result.plan.files, 1U);
            QCOMPARE(result.plan.bytes, quint64(strlen("edition two\n")));
            ntfs_volume* volume = ntfs_mount(targetPath_.constData(), NTFS_MNT_RDONLY | NTFS_MNT_FORENSIC);
            QVERIFY(volume != nullptr);
            ntfs_inode* selected = ntfs_pathname_to_inode(volume, nullptr, "selected.txt");
            QVERIFY(selected != nullptr);
            QCOMPARE(readAttribute(selected), QByteArray("edition two\n"));
            QCOMPARE(ntfs_inode_close(selected), 0);
            QVERIFY(ntfs_pathname_to_inode(volume, nullptr, "payload.bin") == nullptr);
            QCOMPARE(ntfs_umount(volume, FALSE), 0);
        }
        else
        {
            const int expected = mode.startsWith("cancel-") ? ECANCELED
                : mode == "mutate-source" || mode == "changed-source" || mode == "geometry" ? ESTALE
                : mode == "readonly" ? EBADF : mode == "bad-index" ? EINVAL
                : mode == "capacity" ? ENOSPC : mode == "source-target" ? EBUSY : EIO;
            QCOMPARE(error, expected);
            const bool started = mode == "cancel-streams" || mode == "mutate-source" || mode == "sync-fail"
                || mode == "invalid-ntfs" || mode == "corrupt-data";
            QCOMPARE(result.started, started);
            QCOMPARE(syncCalls, started ? 1 : 0);
            if (!started || mode == "mutate-source" || mode == "invalid-ntfs")
                QCOMPARE(fileHash(targetPath_), before);
            if (mode == "sync-fail") QCOMPARE(result.syncError, EIO);
            if (mode == "corrupt-data") QCOMPARE(result.wimError, int(WIMLIB_ERR_INVALID_RESOURCE_HASH));
        }
        cancelled = false;
        if (sourceTarget >= 0) QCOMPARE(close(sourceTarget), 0);
        QCOMPARE(close(targetFd), 0);
        targetFd = -1;
        wimlib_free(wim);
        CloseImageFileSource(&source);
    }

    void cleanupTestCase() { wimlib_global_cleanup(); }
};

QTEST_GUILESS_MAIN(WindowsApplyTest)
#include "test_windows_apply.moc"
