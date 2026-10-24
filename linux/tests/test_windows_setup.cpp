// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/windows_customization.h"
#include "qt/prepare_image_protocol.h"
#include "platform/wim_read_file.h"
#include "platform/windows_bootloaders.h"
#include <hivex.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QXmlStreamReader>
#include <QtTest>
#include <cerrno>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <endian.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

enum Fault { NoFault, HiveCommitFailure, ExtractFailure, CancelExtract, CancelHive,
    UpdateFailure, WrapperWriteFailure, CancelWrapperWrite, CaWriteFailure,
    CancelCaWrite, ShortCaWrite, CaSyncFailure, AnswerWriteFailure, CancelAnswerWrite, CancelUpdate,
    ToGoHiveWriteFailure, ToGoHiveSyncFailure };
enum Fallback { MissingHive, MalformedHive, MissingSetupKey, FailedHiveCommit, FailedExtraction };
static Fault fault;
static bool active, cancelled;
static QByteArray messages;
static int overwriteCalls, hiveTargetWrites;
static const QString AnswerPath = QStringLiteral("sources/$OEM$/$$/Panther/unattend.xml");
static const char* const SystemPath = "Windows/System32/config/SYSTEM";

extern "C" int ReadFixtureHiveNumber(const void*, size_t, const char*, const char*, int64_t*);
extern "C" int IsRufusJobCancelled(void) { return cancelled; }
extern "C" int IsIsoScanCancelled(void) { return cancelled; }
extern "C" void uprintf(const char* format, ...)
{
    char line[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    messages += line;
    messages += '\n';
}
extern "C" void uprintfs(const char* text) { messages += text; }
extern "C" int __real_hivex_commit(hive_h*, const char*, int);
extern "C" int __real_wimlib_extract_paths(WIMStruct*, int, const char*, const char* const*, size_t, int);
extern "C" int __real_wimlib_overwrite(WIMStruct*, int, unsigned);
extern "C" ssize_t __real_write(int, const void*, size_t);
extern "C" ssize_t __real_pwrite64(int, const void*, size_t, off64_t);
extern "C" int __real_fsync(int);
extern "C" bool __real_WriteUnattendXml(FILE*, const RUFUS_UNATTEND_OPTIONS*, uint32_t[2]);

extern "C" bool __wrap_WriteUnattendXml(FILE* file, const RUFUS_UNATTEND_OPTIONS* options, uint32_t offsets[2])
{
    if (active && fault == AnswerWriteFailure)
    {
        fputs("partial answer", file);
        errno = ENOSPC;
        return false;
    }
    const bool result = __real_WriteUnattendXml(file, options, offsets);
    if (active && fault == CancelAnswerWrite)
        cancelled = true;
    return result;
}

static RUFUS_UNATTEND_OPTIONS silentOptions(char* username, int extraFlags = 0)
{
    RUFUS_UNATTEND_OPTIONS options = {};
    options.arch = ARCH_X86_64;
    options.flags = UNATTEND_SILENT_INSTALL | UNATTEND_SET_USER | UNATTEND_DUPLICATE_LOCALE |
        UNATTEND_NO_DATA_COLLECTION | extraFlags;
    options.edition_index = 4;
    options.username = username;
    options.timezone = "GTB Standard Time";
    options.input_locale = "0409:00020409;ro-RO";
    options.system_locale = "ro-RO";
    options.user_locale = "en-GB";
    options.ui_language = "en-US";
    options.ui_language_fallback = "";
    options.winpe_ui_language = "zh-Hans-CN";
    return options;
}

static bool isBootOutput(int fd)
{
    char path[1024];
    const QByteArray link = "/proc/self/fd/" + QByteArray::number(fd);
    const ssize_t length = readlink(link.constData(), path, sizeof(path));
    if (length <= 0)
        return false;
    const QByteArray output(path, length);
    return output.contains("/target/efi/boot/") || output.contains("/target/EFI/BOOT/");
}

static bool isSystemOutput(int fd)
{
    char path[1024];
    const QByteArray link = "/proc/self/fd/" + QByteArray::number(fd);
    const ssize_t length = readlink(link.constData(), path, sizeof(path));
    return length > 0 && QByteArray(path, length).endsWith("/target/Windows/System32/config/SYSTEM");
}

extern "C" ssize_t __wrap_pwrite64(int fd, const void* data, size_t size, off64_t offset)
{
    if (active && isSystemOutput(fd))
    {
        ++hiveTargetWrites;
        if (fault == ToGoHiveWriteFailure)
        {
            const ssize_t written = __real_pwrite64(fd, data, qMin(size, size_t(4097)), offset);
            if (written <= 0) qFatal("Could not inject a partial hive write");
            errno = ENOSPC;
            return -1;
        }
    }
    return __real_pwrite64(fd, data, size, offset);
}

extern "C" ssize_t __wrap_pwrite(int fd, const void* data, size_t size, off_t offset)
{
    return __wrap_pwrite64(fd, data, size, off64_t(offset));
}

extern "C" int __wrap_fsync(int fd)
{
    if (active && fault == ToGoHiveSyncFailure && isSystemOutput(fd))
    {
        errno = EIO;
        return -1;
    }
    if (active && fault == CaSyncFailure && isBootOutput(fd))
    {
        errno = EIO;
        return -1;
    }
    return __real_fsync(fd);
}

extern "C" int __wrap_hivex_commit(hive_h* hive, const char* path, int flags)
{
    if (active && fault == HiveCommitFailure)
    {
        errno = ENOSPC;
        return -1;
    }
    const int result = __real_hivex_commit(hive, path, flags);
    if (active && fault == CancelHive)
        cancelled = true;
    return result;
}

extern "C" int __wrap_wimlib_extract_paths(WIMStruct* wim, int image, const char* target,
    const char* const* paths, size_t count, int flags)
{
    if (active && fault == ExtractFailure)
        return WIMLIB_ERR_READ;
    if (active && fault == CancelExtract)
    {
        cancelled = true;
        return WIMLIB_ERR_ABORTED_BY_PROGRESS;
    }
    return __real_wimlib_extract_paths(wim, image, target, paths, count, flags);
}

extern "C" int __wrap_wimlib_overwrite(WIMStruct* wim, int flags, unsigned threads)
{
    ++overwriteCalls;
    if (active && fault == UpdateFailure)
        return WIMLIB_ERR_WRITE;
    if (active && fault == CancelUpdate)
    {
        cancelled = true;
        return WIMLIB_ERR_ABORTED_BY_PROGRESS;
    }
    return __real_wimlib_overwrite(wim, flags, threads);
}

extern "C" ssize_t __wrap_write(int fd, const void* data, size_t size)
{
    const bool caWrite = active && (fault == CaWriteFailure || fault == CancelCaWrite || fault == ShortCaWrite);
    if (caWrite && isBootOutput(fd))
    {
        const ssize_t written = __real_write(fd, data, qMin(size, size_t(37)));
        if (fault == CaWriteFailure)
        {
            errno = ENOSPC;
            return -1;
        }
        if (fault == CancelCaWrite)
            cancelled = true;
        return written;
    }
    const bool failWrapper = active && (fault == WrapperWriteFailure || fault == CancelWrapperWrite);
    if (failWrapper)
    {
        char path[1024];
        const QByteArray link = "/proc/self/fd/" + QByteArray::number(fd);
        const ssize_t length = readlink(link.constData(), path, sizeof(path));
        const bool isWrapper = length > 0 && QByteArray(path, length).endsWith("/setup.exe");
        if (isWrapper)
        {
            // Leave actual partial bytes for the adapter to remove/restore.
            const ssize_t written = __real_write(fd, data, qMin(size, size_t(7)));
            if (fault == CancelWrapperWrite)
            {
                cancelled = true;
                return written;
            }
            errno = ENOSPC;
            return -1;
        }
    }
    return __real_write(fd, data, size);
}

class TestWindowsSetup : public QObject
{
    Q_OBJECT
    QTemporaryDir directory_;
    QString target_;
    QByteArray originalHive_;
    QByteArray originalSetup_;

    void put(const QString& path, const QByteArray& bytes)
    {
        QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(bytes), bytes.size());
        QVERIFY(file.flush());
    }

    QByteArray read(const QString& path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            qFatal("Cannot read %s", qPrintable(path));
        return file.readAll();
    }

    QByteArray peHeader(quint16 machine)
    {
        QByteArray bytes(128, '\0');
        bytes.replace(0, 2, "MZ");
        bytes[60] = 64;
        bytes.replace(64, 4, QByteArray("PE\0\0", 4));
        bytes[68] = machine & 0xff;
        bytes[69] = machine >> 8;
        return bytes;
    }

    void makeHive(bool setupKey = true)
    {
        const QByteArray path = directory_.filePath("system-source").toUtf8();
        QFile::remove(QString::fromUtf8(path));
        QVERIFY(QFile::copy(QStringLiteral(RUFUS_MINIMAL_HIVE), QString::fromUtf8(path)));
        hive_h* hive = hivex_open(path.constData(), HIVEX_OPEN_WRITE);
        QVERIFY(hive != nullptr);
        const hive_node_h root = hivex_root(hive);
        hive_node_h setup = root;
        if (setupKey)
            setup = hivex_node_add_child(hive, root, "sEtUp");
        uint32_t number = htole32(73);
        hive_set_value value = {const_cast<char*>("Preserved"), hive_t_REG_DWORD, sizeof(number),
            reinterpret_cast<char*>(&number)};
        const int set = setup != 0 ? hivex_node_set_value(hive, setup, &value, 0) : -1;
        const int committed = hivex_commit(hive, nullptr, 0);
        const int closed = hivex_close(hive);
        QCOMPARE(set, 0);
        QCOMPARE(committed, 0);
        QCOMPARE(closed, 0);
        originalHive_ = read(QString::fromUtf8(path));
    }

    void makeWim(int images = 2, bool haveHive = true, bool pipable = false, bool caFiles = false)
    {
        WIMStruct* wim = nullptr;
        QCOMPARE(wimlib_create_new_wim(WIMLIB_COMPRESSION_TYPE_LZX, &wim), 0);
        int error = 0;
        for (int image = 1; image <= images && error == 0; ++image)
        {
            const QString tree = directory_.filePath("tree-" + QString::number(image));
            QDir(tree).removeRecursively();
            put(tree + "/keep.txt", "keep-" + QByteArray::number(image));
            if (haveHive)
                put(tree + "/wInDoWs/SyStEm32/CoNfIg/sYsTeM", originalHive_);
            if (caFiles)
            {
                const QString boot = tree + "/wInDoWs/BoOt/";
                const QByteArray suffix = "-" + QByteArray::number(image);
                put(boot + "eFi_eX/bOoTmGfW_eX.EfI", QByteArray(96 * 1024, 'F') + suffix);
                put(boot + "eFi_eX/bOoTmGr_eX.EfI", "ca-manager" + suffix);
                put(boot + "fOnTs_eX/win_EXboot_EX.ttf", "ca-font" + suffix);
                put(boot + "fOnTs_eX/lower_ex.ttf", "lowercase font" + suffix);
                put(boot + "fOnTs_eX/subdirectory/ignored.ttf", "not a direct font");
            }
            error = wimlib_add_image(wim, tree.toUtf8().constData(), nullptr, nullptr, 0);
        }
        struct wimlib_wim_info info = {};
        info.boot_index = 1;
        if (error == 0)
            error = wimlib_set_wim_info(wim, &info, WIMLIB_CHANGE_BOOT_INDEX);
        const QByteArray path = (target_ + "/sources/boot.wim").toUtf8();
        if (error == 0)
            error = wimlib_write(wim, path.constData(), WIMLIB_ALL_IMAGES,
                pipable ? WIMLIB_WRITE_FLAG_PIPABLE : 0, 1);
        wimlib_free(wim);
        QCOMPARE(error, 0);
    }

    QByteArray readWimFile(int image, const char* name, int expected = 0)
    {
        const QByteArray path = (target_ + "/sources/boot.wim").toUtf8();
        WIMStruct* wim = nullptr;
        if (wimlib_open_wim(path.constData(), WIMLIB_OPEN_FLAG_CHECK_INTEGRITY, &wim) != 0)
            qFatal("Cannot open fixture WIM");
        uint8_t* bytes = nullptr;
        size_t length = 0;
        const int error = ReadWimFile(wim, image, name, &bytes, &length);
        const QByteArray result(reinterpret_cast<const char*>(bytes), length);
        free(bytes);
        struct wimlib_wim_info info = {};
        const int queried = wimlib_get_wim_info(wim, &info);
        const int verified = wimlib_verify_wim(wim, 0);
        wimlib_free(wim);
        if (error != expected || queried != 0 || verified != 0 || info.boot_index != 1)
            qFatal("WIM readback failed: read=%d query=%d verify=%d boot=%u", error, queried, verified, info.boot_index);
        return result;
    }

    QStringList modified_;
    int apply(uint32_t build = 26200, int flags = UNATTEND_SECUREBOOT_TPM_MINRAM | UNATTEND_NO_DATA_COLLECTION)
    {
        const QStringList before = QDir(QDir::tempPath()).entryList({"rufus-windows-*"}, QDir::Dirs);
        active = true;
        modified_.clear();
        const int error = rufus::ApplyWindowsCustomization(target_.toUtf8().constData(), ARCH_X86_64,
            build, flags, {}, &modified_);
        active = false;
        const QStringList after = QDir(QDir::tempPath()).entryList({"rufus-windows-*"}, QDir::Dirs);
        if (before != after)
            qFatal("Windows customization left temporary files behind");
        return error;
    }

    int applySilent(RUFUS_UNATTEND_OPTIONS& options, bool throughRequest = false)
    {
        const QStringList before = QDir(QDir::tempPath()).entryList({"rufus-windows-*"}, QDir::Dirs);
        active = true;
        int error;
        if (throughRequest)
        {
            rufus::PrepareImageRequest request;
            request.isDevice = true;
            request.imagePath = QStringLiteral("/test/source.iso");
            request.windowsFlags = options.flags;
            request.windowsUsername = QString::fromUtf8(options.username);
            request.windowsInstallPath = QStringLiteral("/sources/install.wim");
            request.windowsEdition = options.edition_index;
            request.windowsSilentConfirmed = true;
            request.windowsRegion = {QString::fromUtf8(options.input_locale), QString::fromUtf8(options.system_locale),
                QString::fromUtf8(options.user_locale), QString::fromUtf8(options.ui_language),
                QString::fromUtf8(options.ui_language_fallback), QString::fromUtf8(options.timezone)};
            error = rufus::ApplyWindowsCustomization(target_.toUtf8().constData(), options.arch, 26200,
                request, QString::fromUtf8(options.winpe_ui_language));
            if (request.windowsUsername != QString::fromUtf8(options.username))
                qFatal("Customization modified the request's username");
        }
        else
            error = rufus::ApplyWindowsCustomization(target_.toUtf8().constData(), 26200, options);
        active = false;
        const QStringList after = QDir(QDir::tempPath()).entryList({"rufus-windows-*"}, QDir::Dirs);
        if (before != after)
            qFatal("Silent customization left temporary files behind");
        return error;
    }

    QByteArray originalSilentAnswer(RUFUS_UNATTEND_OPTIONS& options, bool removeBypass)
    {
        // Independent Win32 post-processing: generate all commands, then remove
        // the byte range reported by Rufus. Production instead clears the flag.
        FILE* file = tmpfile();
        if (!file)
            qFatal("Cannot create reference answer");
        uint32_t offsets[2];
        if (!WriteUnattendXml(file, &options, offsets) || fflush(file) != 0)
            qFatal("Cannot generate reference answer");
        const long length = ftell(file);
        if (length <= 0)
            qFatal("Cannot size reference answer");
        QByteArray result(length, '\0');
        rewind(file);
        const size_t copied = fread(result.data(), 1, size_t(length), file);
        if (fclose(file) != 0 || copied != size_t(length))
            qFatal("Cannot read reference answer");
        if (removeBypass)
        {
            if (offsets[0] == 0 || offsets[1] <= offsets[0] || offsets[1] >= uint64_t(length))
                qFatal("Invalid original bypass section offsets");
            result.remove(offsets[0], offsets[1] - offsets[0]);
        }
        return result;
    }

    void prepareCaFiles()
    {
        put(target_ + "/efi/boot/bootx64.efi", "original firmware loader");
        put(target_ + "/efi/microsoft/boot/fonts/winboot.ttf", "original font");
        put(target_ + "/bootmgr.efi", "original manager");
    }

    void changeSetupFile(const char* name, const QString& source = {})
    {
        const QByteArray path = (target_ + "/sources/boot.wim").toUtf8();
        WIMStruct* wim = nullptr;
        QCOMPARE(wimlib_open_wim(path.constData(), WIMLIB_OPEN_FLAG_WRITE_ACCESS, &wim), 0);
        wimlib_update_command command = {};
        const QByteArray encoded = source.toUtf8();
        if (source.isEmpty())
        {
            command.op = WIMLIB_UPDATE_OP_DELETE;
            command.delete_.wim_path = const_cast<char*>(name);
            command.delete_.delete_flags = WIMLIB_DELETE_FLAG_RECURSIVE;
        }
        else
        {
            command.op = WIMLIB_UPDATE_OP_ADD;
            command.add.fs_source_path = const_cast<char*>(encoded.constData());
            command.add.wim_target_path = const_cast<char*>(name);
        }
        int result = wimlib_update_image(wim, 2, &command, 1, 0);
        if (result == 0)
            result = wimlib_overwrite(wim, 0, 1);
        wimlib_free(wim);
        QCOMPARE(result, 0);
        overwriteCalls = 0;
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
        active = cancelled = false;
        fault = NoFault;
        overwriteCalls = 0;
        hiveTargetWrites = 0;
        messages.clear();
        target_ = directory_.filePath("target");
        QVERIFY(QDir(target_).removeRecursively());
        put(target_ + "/sources/appraiserres.dll", "original appraiser");
        originalSetup_ = peHeader(0x8664);
        put(target_ + "/setup.exe", originalSetup_);
        makeHive();
        makeWim();
    }

    void windowsToGo_data()
    {
        QTest::addColumn<QString>("change");
        QTest::addColumn<int>("expected");
        QTest::newRow("native-policy-and-answer") << QString() << 0;
        QTest::newRow("answer-only") << QStringLiteral("answer-only") << 0;
        QTest::newRow("native-s-mode-only") << QStringLiteral("s-mode-only") << 0;
        QTest::newRow("native-s-mode-and-san") << QStringLiteral("s-mode-and-san") << 0;
        QTest::newRow("existing-answer") << QStringLiteral("existing-answer") << EEXIST;
        QTest::newRow("linked-system") << QStringLiteral("linked-system") << ELOOP;
        QTest::newRow("hive-commit-failure") << QStringLiteral("commit-failure") << ENOSPC;
        QTest::newRow("cancel-hive-commit") << QStringLiteral("cancel-commit") << ECANCELED;
        QTest::newRow("answer-failure") << QStringLiteral("answer-failure") << ENOSPC;
        QTest::newRow("partial-hive-write") << QStringLiteral("partial-write") << ENOSPC;
        QTest::newRow("hive-sync-failure") << QStringLiteral("sync-failure") << EIO;
    }

    void windowsToGo()
    {
        QFETCH(QString, change);
        QFETCH(int, expected);
        // The ordinary ISO fixtures are irrelevant to this installed-tree path.
        const QString system = target_ + "/Windows/System32/config/SYSTEM";
        const QString answer = target_ + "/Windows/Panther/unattend.xml";
        put(system, originalHive_);
        const QByteArray systemPath = QFile::encodeName(system);
        hive_h* hive = hivex_open(systemPath.constData(), HIVEX_OPEN_WRITE);
        QVERIFY(hive != nullptr);
        const hive_node_h root = hivex_root(hive);
        const hive_node_h select = hivex_node_add_child(hive, root, "Select");
        QVERIFY(select != 0);
        uint32_t value = htole32(2);
        hive_set_value entry = {const_cast<char*>("Default"), hive_t_REG_DWORD, sizeof(value), reinterpret_cast<char*>(&value)};
        QCOMPARE(hivex_node_set_value(hive, select, &entry, 0), 0);
        hive_node_h node = hivex_node_add_child(hive, root, "ControlSet002");
        QVERIFY(node != 0);
        hive_node_h policy = hivex_node_add_child(hive, node, "Control");
        QVERIFY(policy != 0);
        policy = hivex_node_add_child(hive, policy, "CI");
        QVERIFY(policy != 0);
        policy = hivex_node_add_child(hive, policy, "Policy");
        QVERIFY(policy != 0);
        value = 0;
        entry.key = const_cast<char*>("SkuPolicyRequired");
        QCOMPARE(hivex_node_set_value(hive, policy, &entry, 0), 0);
        for (const char* name : {"Services", "partmgr", "Parameters"})
        {
            node = hivex_node_add_child(hive, node, name);
            QVERIFY(node != 0);
        }
        value = htole32(1);
        entry.key = const_cast<char*>("SanPolicy");
        QCOMPARE(hivex_node_set_value(hive, node, &entry, 0), 0);
        QCOMPARE(hivex_commit(hive, systemPath.constData(), 0), 0);
        QCOMPARE(hivex_close(hive), 0);
        const QByteArray before = read(system);
        struct stat original = {}, after = {};
        QCOMPARE(stat(systemPath.constData(), &original), 0);
        QCOMPARE(setxattr(systemPath.constData(), "user.fixture", "metadata", 8, 0), 0);
        const QByteArray alias = QFile::encodeName(system + ".alias");
        QCOMPARE(link(systemPath.constData(), alias.constData()), 0);
        if (change == "existing-answer") put(answer, "original answer");
        if (change == "linked-system")
        {
            QCOMPARE(unlink(systemPath.constData()), 0);
            QCOMPARE(symlink(alias.constData(), systemPath.constData()), 0);
        }
        if (change == "commit-failure") fault = HiveCommitFailure;
        if (change == "cancel-commit") fault = CancelHive;
        if (change == "answer-failure") fault = AnswerWriteFailure;
        if (change == "partial-write") fault = ToGoHiveWriteFailure;
        if (change == "sync-failure") fault = ToGoHiveSyncFailure;
        int flags = UNATTEND_NO_DATA_COLLECTION | UNATTEND_SET_USER | UNATTEND_NO_ONLINE_ACCOUNT;
        const bool offlineSan = change != "answer-only" && change != "s-mode-only";
        const bool sMode = change == "s-mode-only" || change == "s-mode-and-san";
        if (offlineSan) flags |= UNATTEND_OFFLINE_INTERNAL_DRIVES;
        if (sMode) flags |= UNATTEND_FORCE_S_MODE;
        active = true;
        const int error = rufus::ApplyWindowsToGoCustomization(target_.toUtf8().constData(), ARCH_X86_64, flags,
            QStringLiteral("ToGo.User"));
        active = false;
        QCOMPARE(error, expected);
        if (change == "partial-write" || change == "sync-failure") QVERIFY(hiveTargetWrites > 0);
        QCOMPARE(stat(alias.constData(), &after), 0);
        QCOMPARE(after.st_ino, original.st_ino);
        QCOMPARE(after.st_mode, original.st_mode);
        char metadata[16] = {};
        QCOMPARE(getxattr(alias.constData(), "user.fixture", metadata, sizeof(metadata)), ssize_t(8));
        QCOMPARE(QByteArray(metadata, 8), QByteArray("metadata"));
        if (change == "existing-answer" || change == "linked-system" || change == "commit-failure" || change == "cancel-commit")
            QCOMPARE(read(QString::fromUtf8(alias)), before);
        else if (change != "partial-write")
        {
            const QByteArray updated = read(QString::fromUtf8(alias));
            int64_t policy = 0;
            QCOMPARE(ReadFixtureHiveNumber(updated.constData(), size_t(updated.size()),
                "ControlSet002\\Services\\partmgr\\Parameters", "SanPolicy", &policy), 0);
            QCOMPARE(policy, offlineSan ? int64_t(4) : int64_t(1));
            QCOMPARE(ReadFixtureHiveNumber(updated.constData(), size_t(updated.size()),
                "ControlSet002\\Control\\CI\\Policy", "SkuPolicyRequired", &policy), 0);
            QCOMPARE(policy, sMode ? int64_t(1) : int64_t(0));
        }
        if (error == 0)
        {
            const QByteArray xml = read(answer);
            QVERIFY(xml.contains("<Name>ToGo_User</Name>"));
            QVERIFY(xml.contains("<ProtectYourPC>3</ProtectYourPC>"));
            QVERIFY(xml.contains("BypassNRO"));
            QCOMPARE(xml.contains("<SanPolicy>4</SanPolicy>"), offlineSan);
            QCOMPARE(xml.contains("<SkuPolicyRequired>1</SkuPolicyRequired>"), sMode);
            QVERIFY(!xml.contains("windowsPE"));
        }
        else if (change == "existing-answer") QCOMPARE(read(answer), QByteArray("original answer"));
        else QVERIFY(!QFile::exists(answer));
    }

    void nativeHiveAndOriginalWrapper_data()
    {
        QTest::addColumn<int>("images");
        QTest::addColumn<int>("build");
        QTest::addColumn<int>("machine");
        QTest::addColumn<bool>("pipable");
        QTest::newRow("win11-original") << 2 << 22000 << 0x8664 << false;
        QTest::newRow("24h2-x64") << 2 << 26100 << 0x8664 << false;
        QTest::newRow("25h2-arm64") << 2 << 26200 << 0xaa64 << false;
        QTest::newRow("one-image") << 1 << 26200 << 0x8664 << false;
        QTest::newRow("wim-rebuild") << 2 << 26200 << 0x8664 << true;
    }

    void silentWinpeAndNativeRegistry_data()
    {
        QTest::addColumn<int>("flags");
        QTest::addColumn<bool>("haveHive");
        QTest::addColumn<int>("images");
        QTest::addColumn<bool>("pipable");
        QTest::addColumn<bool>("throughRequest");
        QTest::newRow("silent-only") << 0 << true << 2 << false << false;
        QTest::newRow("single-image") << 0 << true << 1 << false << false;
        QTest::newRow("native-registry") << int(UNATTEND_SECUREBOOT_TPM_MINRAM) << true << 2 << false << false;
        QTest::newRow("answer-fallback") << int(UNATTEND_SECUREBOOT_TPM_MINRAM) << false << 2 << false << false;
        QTest::newRow("wim-rebuild") << int(UNATTEND_SECUREBOOT_TPM_MINRAM) << true << 2 << true << false;
        QTest::newRow("ca-and-policy") << int(UNATTEND_SECUREBOOT_TPM_MINRAM | UNATTEND_USE_MS2023_BOOTLOADERS |
            UNATTEND_APPLY_SKUSIPOLICY | UNATTEND_DISABLE_BITLOCKER) << true << 2 << false << false;
        QTest::newRow("request-silent-only") << 0 << true << 2 << false << true;
        QTest::newRow("request-native-registry") << int(UNATTEND_SECUREBOOT_TPM_MINRAM) << true << 2 << false << true;
        QTest::newRow("s-mode-silent") << int(UNATTEND_FORCE_S_MODE) << true << 2 << false << true;
        QTest::newRow("s-mode-native-registry") << int(UNATTEND_FORCE_S_MODE |
            UNATTEND_SECUREBOOT_TPM_MINRAM) << true << 2 << false << true;
    }

    void silentWinpeAndNativeRegistry()
    {
        QFETCH(int, flags);
        QFETCH(bool, haveHive);
        QFETCH(int, images);
        QFETCH(bool, pipable);
        QFETCH(bool, throughRequest);
        const bool ca = flags & UNATTEND_USE_MS2023_BOOTLOADERS;
        if (ca)
            prepareCaFiles();
        makeWim(images, haveHive, pipable, ca);
        char username[] = "TestUser";
        auto options = silentOptions(username, flags);
        const bool hardware = flags & UNATTEND_SECUREBOOT_TPM_MINRAM;
        const bool registryApplied = hardware && haveHive;
        const QByteArray expected = originalSilentAnswer(options, registryApplied);
        const int originalFlags = options.flags;
        QCOMPARE(applySilent(options, throughRequest), 0);
        QCOMPARE(options.flags, originalFlags);
        QCOMPARE(overwriteCalls, 1); // Both updates are committed together.
        const QByteArray xml = readWimFile(images, "Autounattend.xml");
        QCOMPARE(xml, expected);
        QVERIFY(xml.contains("pass=\"windowsPE\""));
        QVERIFY(!xml.contains("pass=\"disabled\""));
        QVERIFY(xml.contains("<WillWipeDisk>true</WillWipeDisk>"));
        QVERIFY(xml.contains("<Value>4</Value>"));
        QVERIFY(xml.contains("<UILanguage>zh-Hans-CN</UILanguage>"));
        QVERIFY(xml.contains("<TimeZone>GTB Standard Time</TimeZone>"));
        QVERIFY(xml.contains("<InputLocale>0409:00020409;ro-RO</InputLocale>"));
        QCOMPARE(xml.contains("BypassTPMCheck"), hardware && !registryApplied);
        QCOMPARE(xml.contains("<SkuPolicyRequired>1</SkuPolicyRequired>"), bool(flags & UNATTEND_FORCE_S_MODE));
        QVERIFY(!QFile::exists(target_ + '/' + AnswerPath));
        QXmlStreamReader parser(xml);
        while (!parser.atEnd())
            parser.readNext();
        QVERIFY2(!parser.hasError(), qPrintable(parser.errorString()));
        if (haveHive)
        {
            const QByteArray hive = readWimFile(images, SystemPath);
            if (registryApplied)
            {
                int64_t number = 0;
                QCOMPARE(ReadFixtureHiveNumber(hive.constData(), hive.size(), "Setup\\LabConfig", "BypassTPMCheck", &number), 0);
                QCOMPARE(number, int64_t(1));
            }
            else
                QCOMPARE(hive, originalHive_);
        }
        if (images == 2)
            readWimFile(1, "Autounattend.xml", WIMLIB_ERR_PATH_DOES_NOT_EXIST);
        if (!hardware)
        {
            QCOMPARE(read(target_ + "/sources/appraiserres.dll"), QByteArray("original appraiser"));
            QCOMPARE(read(target_ + "/setup.exe"), originalSetup_);
        }
        if (ca)
            QCOMPARE(read(target_ + "/bootmgr.efi"), QByteArray("ca-manager-2"));
    }

    void silentFailures_data()
    {
        QTest::addColumn<int>("failure");
        QTest::addColumn<int>("expected");
        QTest::newRow("cancel-hive") << int(CancelHive) << ECANCELED;
        QTest::newRow("answer-write") << int(AnswerWriteFailure) << ENOSPC;
        QTest::newRow("cancel-answer") << int(CancelAnswerWrite) << ECANCELED;
        QTest::newRow("update-failure") << int(UpdateFailure) << EIO;
        QTest::newRow("cancel-update") << int(CancelUpdate) << ECANCELED;
    }

    void silentFailures()
    {
        QFETCH(int, failure);
        QFETCH(int, expected);
        const QByteArray before = read(target_ + "/sources/boot.wim");
        char username[] = "TestUser";
        auto options = silentOptions(username, UNATTEND_SECUREBOOT_TPM_MINRAM);
        fault = Fault(failure);
        QCOMPARE(applySilent(options), expected);
        QCOMPARE(read(target_ + "/sources/boot.wim"), before);
        QVERIFY(!QFile::exists(target_ + '/' + AnswerPath));
        const bool updating = fault == UpdateFailure || fault == CancelUpdate;
        QCOMPARE(overwriteCalls, updating ? 1 : 0);
    }

    void nativeHiveAndOriginalWrapper()
    {
        QFETCH(int, images);
        QFETCH(int, build);
        QFETCH(int, machine);
        QFETCH(bool, pipable);
        originalSetup_ = peHeader(machine);
        put(target_ + "/setup.exe", originalSetup_);
        makeWim(images, true, pipable);
        QCOMPARE(apply(build), 0);
        QCOMPARE(overwriteCalls, 1);
        const QByteArray hive = readWimFile(images, SystemPath);
        const char* const names[] = {"BypassTPMCheck", "BypassSecureBootCheck", "BypassRAMCheck"};
        for (const char* name : names)
        {
            int64_t number = 0;
            QCOMPARE(ReadFixtureHiveNumber(hive.constData(), hive.size(), "Setup\\LabConfig", name, &number), 0);
            QCOMPARE(number, int64_t(1));
        }
        int64_t number = 0;
        QCOMPARE(ReadFixtureHiveNumber(hive.constData(), hive.size(), "Setup", "Preserved", &number), 0);
        QCOMPARE(number, int64_t(73));
        QCOMPARE(readWimFile(images, "keep.txt"), "keep-" + QByteArray::number(images));
        if (images == 2)
            QCOMPARE(readWimFile(1, SystemPath), originalHive_);
        readWimFile(images, "Autounattend.xml", WIMLIB_ERR_PATH_DOES_NOT_EXIST);
        const QByteArray xml = read(target_ + '/' + AnswerPath);
        QVERIFY(xml.contains("<settings pass=\"disabled\">"));
        QVERIFY(xml.contains("BypassTPMCheck")); // Original Rufus keeps the disabled commands.
        QVERIFY(xml.contains("<ProtectYourPC>3</ProtectYourPC>"));
        QVERIFY(!xml.contains("pass=\"windowsPE\""));
        QCOMPARE(read(target_ + "/sources/appraiserres.bak"), QByteArray("original appraiser"));
        QCOMPARE(read(target_ + "/sources/appraiserres.dll"), QByteArray());
        if (build >= 26000)
        {
            const QString binary = machine == 0x8664 ? "setup_x64.exe" : "setup_arm64.exe";
            QCOMPARE(read(target_ + "/setup.exe"), read(QStringLiteral(RUFUS_SETUP_RESOURCES) + '/' + binary));
            QCOMPARE(read(target_ + "/setup.dll"), originalSetup_);
        }
        else
        {
            QCOMPARE(read(target_ + "/setup.exe"), originalSetup_);
            QVERIFY(!QFile::exists(target_ + "/setup.dll"));
        }
    }

    void answerFileFallback_data()
    {
        QTest::addColumn<int>("mode");
        QTest::newRow("missing-hive") << int(MissingHive);
        QTest::newRow("invalid-hive") << int(MalformedHive);
        QTest::newRow("missing-setup-key") << int(MissingSetupKey);
        QTest::newRow("hive-commit") << int(FailedHiveCommit);
        QTest::newRow("extraction") << int(FailedExtraction);
    }

    void answerFileFallback()
    {
        QFETCH(int, mode);
        if (mode == MalformedHive)
            originalHive_ = "invalid hive";
        if (mode == MissingSetupKey)
            makeHive(false);
        makeWim(2, mode != MissingHive);
        if (mode == FailedHiveCommit)
            fault = HiveCommitFailure;
        if (mode == FailedExtraction)
            fault = ExtractFailure;
        QCOMPARE(apply(), 0);
        QVERIFY(messages.contains("Falling back to creating the registry keys through unattend.xml"));
        QVERIFY(!QFile::exists(target_ + '/' + AnswerPath));
        const QByteArray xml = readWimFile(2, "Autounattend.xml");
        QVERIFY(xml.contains("<settings pass=\"windowsPE\">"));
        QVERIFY(xml.contains("BypassTPMCheck"));
        QVERIFY(xml.contains("BypassSecureBootCheck"));
        QVERIFY(xml.contains("BypassRAMCheck"));
        QVERIFY(xml.contains("<ProtectYourPC>3</ProtectYourPC>"));
        if (mode != MissingHive)
            QCOMPARE(readWimFile(2, SystemPath), originalHive_);
        readWimFile(1, "Autounattend.xml", WIMLIB_ERR_PATH_DOES_NOT_EXIST);
    }

    void failuresDoNotBecomeFallbackSuccess_data()
    {
        QTest::addColumn<int>("failure");
        QTest::addColumn<int>("expected");
        QTest::newRow("cancel-extract") << int(CancelExtract) << ECANCELED;
        QTest::newRow("cancel-hive") << int(CancelHive) << ECANCELED;
        QTest::newRow("update-error") << int(UpdateFailure) << EIO;
        QTest::newRow("wrapper-write") << int(WrapperWriteFailure) << ENOSPC;
        QTest::newRow("cancel-wrapper") << int(CancelWrapperWrite) << ECANCELED;
    }

    void failuresDoNotBecomeFallbackSuccess()
    {
        QFETCH(int, failure);
        QFETCH(int, expected);
        const QByteArray before = read(target_ + "/sources/boot.wim");
        fault = Fault(failure);
        QCOMPARE(apply(), expected);
        QVERIFY(!messages.contains("Falling back"));
        QCOMPARE(read(target_ + "/sources/boot.wim"), before);
        QVERIFY(!QFile::exists(target_ + '/' + AnswerPath));
        const bool wrapperFailed = fault == WrapperWriteFailure || fault == CancelWrapperWrite;
        if (wrapperFailed)
        {
            QCOMPARE(read(target_ + "/setup.exe"), originalSetup_);
            QVERIFY(!QFile::exists(target_ + "/setup.dll"));
        }
    }

    void missingAppraiserAndHardwareOnly()
    {
        QVERIFY(QFile::remove(target_ + "/sources/appraiserres.dll"));
        QCOMPARE(apply(26200, UNATTEND_SECUREBOOT_TPM_MINRAM), 0);
        QCOMPARE(read(target_ + "/sources/appraiserres.dll"), QByteArray());
        QVERIFY(!QFile::exists(target_ + "/sources/appraiserres.bak"));
        QVERIFY(read(target_ + '/' + AnswerPath).contains("pass=\"disabled\""));
    }

    void efiRepairCreatesDirectories_data()
    {
        QTest::addColumn<QString>("existing");
        QTest::newRow("root-only") << QString();
        QTest::newRow("EFI-exists") << QStringLiteral("EFI");
        QTest::newRow("BOOT-exists") << QStringLiteral("EFI/BOOT");
    }

    void efiRepairCreatesDirectories()
    {
        QFETCH(QString, existing);
        if (!existing.isEmpty())
            QVERIFY(QDir().mkpath(target_ + '/' + existing));
        put(target_ + "/bootmgr.efi", "original root manager");
        const QByteArray payload(96 * 1024, 'E');
        QCOMPARE(rufus::InstallWindowsEfiRepair(payload, target_.toUtf8().constData()), 0);
        QCOMPARE(read(target_ + "/EFI/BOOT/BOOTX64.EFI"), payload);
        QCOMPARE(read(target_ + "/bootmgr.efi"), QByteArray("original root manager"));
        QCOMPARE(read(target_ + "/setup.exe"), originalSetup_);
        QCOMPARE(QDir(target_ + "/EFI/BOOT").entryList(QDir::Files | QDir::Hidden),
            QStringList({QStringLiteral("BOOTX64.EFI")}));
    }

    void efiRepairAtomicWrites_data()
    {
        QTest::addColumn<int>("injected");
        QTest::addColumn<bool>("cancelBefore");
        QTest::addColumn<int>("expected");
        QTest::newRow("replace") << int(NoFault) << false << 0;
        QTest::newRow("short-writes") << int(ShortCaWrite) << false << 0;
        QTest::newRow("partial-write-failure") << int(CaWriteFailure) << false << ENOSPC;
        QTest::newRow("cancel-during-write") << int(CancelCaWrite) << false << ECANCELED;
        QTest::newRow("sync-failure") << int(CaSyncFailure) << false << EIO;
        QTest::newRow("cancel-before-write") << int(NoFault) << true << ECANCELED;
    }

    void efiRepairAtomicWrites()
    {
        QFETCH(int, injected); QFETCH(bool, cancelBefore); QFETCH(int, expected);
        const QByteArray original("existing fallback stays intact on failure");
        put(target_ + "/EFI/BOOT/BOOTX64.EFI", original);
        const QByteArray payload(96 * 1024, 'E');
        const int before = QDir("/proc/self/fd").entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size();
        fault = Fault(injected);
        cancelled = cancelBefore;
        active = true;
        const int error = rufus::InstallWindowsEfiRepair(payload, target_.toUtf8().constData());
        active = cancelled = false;
        QCOMPARE(error, expected);
        QCOMPARE(read(target_ + "/EFI/BOOT/BOOTX64.EFI"), expected == 0 ? payload : original);
        QCOMPARE(QDir(target_ + "/EFI/BOOT").entryList(QDir::Files | QDir::Hidden),
            QStringList({QStringLiteral("BOOTX64.EFI")}));
        QCOMPARE(QDir("/proc/self/fd").entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size(), before);
    }

    void efiRepairUnsafeTargets_data()
    {
        QTest::addColumn<QString>("node");
        QTest::addColumn<int>("expected");
        QTest::newRow("root-symlink") << QStringLiteral("root-link") << ENOTDIR;
        QTest::newRow("EFI-symlink") << QStringLiteral("EFI") << ENOTDIR;
        QTest::newRow("BOOT-symlink") << QStringLiteral("EFI/BOOT") << ENOTDIR;
        QTest::newRow("loader-symlink") << QStringLiteral("EFI/BOOT/BOOTX64.EFI") << ENOTSUP;
        QTest::newRow("loader-directory") << QStringLiteral("directory") << ENOTSUP;
        QTest::newRow("loader-hardlink") << QStringLiteral("hardlink") << EMLINK;
        QTest::newRow("missing-root") << QStringLiteral("missing") << ENOENT;
        QTest::newRow("empty-payload") << QStringLiteral("empty") << EINVAL;
        QTest::newRow("null-root") << QStringLiteral("null") << EINVAL;
    }

    void efiRepairUnsafeTargets()
    {
        QFETCH(QString, node); QFETCH(int, expected);
        const QString outside = directory_.filePath("outside-loader");
        put(outside, "untouched external loader");
        QString root = target_;
        const QString fallback = target_ + "/EFI/BOOT/BOOTX64.EFI";
        QByteArray payload(96 * 1024, 'E');
        if (node == QStringLiteral("root-link"))
        {
            root = directory_.filePath("root-link");
            QFile::remove(root);
            QCOMPARE(symlink(target_.toUtf8().constData(), root.toUtf8().constData()), 0);
        }
        else if (node == QStringLiteral("directory"))
            QVERIFY(QDir().mkpath(fallback));
        else if (node == QStringLiteral("hardlink"))
        {
            QVERIFY(QDir().mkpath(QFileInfo(fallback).absolutePath()));
            QCOMPARE(link(outside.toUtf8().constData(), fallback.toUtf8().constData()), 0);
        }
        else if (node == QStringLiteral("missing"))
            root = directory_.filePath("missing");
        else if (node == QStringLiteral("empty"))
            payload.clear();
        else if (node != QStringLiteral("null"))
        {
            const QString path = target_ + '/' + node;
            QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
            QCOMPARE(symlink(outside.toUtf8().constData(), path.toUtf8().constData()), 0);
        }
        const int before = QDir("/proc/self/fd").entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size();
        const int error = rufus::InstallWindowsEfiRepair(payload,
            node == QStringLiteral("null") ? nullptr : root.toUtf8().constData());
        QCOMPARE(error, expected);
        QCOMPARE(read(outside), QByteArray("untouched external loader"));
        QCOMPARE(QDir("/proc/self/fd").entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size(), before);
    }

    void caPayloadsAndUnchangedWim_data()
    {
        QTest::addColumn<int>("images");
        QTest::addColumn<int>("writeFault");
        QTest::addColumn<bool>("policy");
        QTest::newRow("setup-image-two") << 2 << int(NoFault) << false;
        QTest::newRow("setup-image-one") << 1 << int(NoFault) << false;
        QTest::newRow("short-writes") << 2 << int(ShortCaWrite) << false;
        QTest::newRow("ca-and-policy") << 2 << int(NoFault) << true;
    }

    void caPayloadsAndUnchangedWim()
    {
        QFETCH(int, images);
        QFETCH(int, writeFault);
        QFETCH(bool, policy);
        prepareCaFiles();
        // CA alone must not need a SYSTEM hive or change upgrade files.
        makeWim(images, false, false, true);
        const QByteArray before = read(target_ + "/sources/boot.wim");
        fault = Fault(writeFault);
        const int flags = UNATTEND_USE_MS2023_BOOTLOADERS | (policy ? UNATTEND_APPLY_SKUSIPOLICY : 0);
        QCOMPARE(apply(26200, flags), 0);
        const QByteArray suffix = "-" + QByteArray::number(images);
        QCOMPARE(read(target_ + "/efi/boot/bootx64.efi"), QByteArray(96 * 1024, 'F') + suffix);
        QCOMPARE(read(target_ + "/bootmgr.efi"), "ca-manager" + suffix);
        QCOMPARE(read(target_ + "/efi/microsoft/boot/fonts/winboot.ttf"), "ca-font" + suffix);
        QCOMPARE(read(target_ + "/efi/microsoft/boot/fonts/lower_ex.ttf"), "lowercase font" + suffix);
        QVERIFY(!QFile::exists(target_ + "/efi/microsoft/boot/fonts/subdirectory"));
        QCOMPARE(read(target_ + "/sources/boot.wim"), before);
        QCOMPARE(overwriteCalls, 0);
        QCOMPARE(read(target_ + "/sources/appraiserres.dll"), QByteArray("original appraiser"));
        QCOMPARE(read(target_ + "/setup.exe"), originalSetup_);
        QVERIFY(!QFile::exists(target_ + "/setup.dll"));
        const QByteArray answer = read(target_ + '/' + AnswerPath);
        QVERIFY(!answer.contains("BypassTPMCheck"));
        QCOMPARE(answer.count("SkuSiPolicy.p7b"), policy ? 1 : 0);
        QVERIFY(messages.contains("target PC must have"));
    }

    void caUsesFirstExistingArchitecture()
    {
        prepareCaFiles();
        makeWim(2, true, false, true);
        put(target_ + "/efi/boot/bootia32.efi", "first fallback loader");
        put(target_ + "/efi/boot/bootaa64.efi", "other architecture");
        QCOMPARE(apply(26200, UNATTEND_USE_MS2023_BOOTLOADERS), 0);
        QCOMPARE(read(target_ + "/efi/boot/bootia32.efi"), QByteArray(96 * 1024, 'F') + "-2");
        QCOMPARE(read(target_ + "/efi/boot/bootx64.efi"), QByteArray("original firmware loader"));
        QCOMPARE(read(target_ + "/efi/boot/bootaa64.efi"), QByteArray("other architecture"));
        QVERIFY(modified_.contains(QStringLiteral("efi/boot/bootia32.efi")));
        QVERIFY(!modified_.contains(QStringLiteral("efi/boot/bootx64.efi")));
        QVERIFY(!modified_.contains(QStringLiteral("sources/boot.wim")));
    }

    void caDoesNotCreateFallbackLoader()
    {
        prepareCaFiles();
        makeWim(2, true, false, true);
        changeSetupFile("Windows/Boot/EFI_EX/bootmgfw_EX.efi");
        QVERIFY(QDir(target_ + "/efi/boot").removeRecursively());
        QVERIFY(QFile::remove(target_ + "/bootmgr.efi"));
        QCOMPARE(apply(26200, UNATTEND_USE_MS2023_BOOTLOADERS), 0);
        QVERIFY(!QFile::exists(target_ + "/efi/boot"));
        QCOMPARE(read(target_ + "/bootmgr.efi"), QByteArray("ca-manager-2"));
    }

    void caRejectsUnsafeSourceFiles_data()
    {
        QTest::addColumn<QString>("name");
        QTest::newRow("font-source") << QStringLiteral("Windows/Boot/Fonts_EX/win_EXboot_EX.ttf");
        QTest::newRow("loader-source") << QStringLiteral("Windows/Boot/EFI_EX/bootmgfw_EX.efi");
    }

    void caRejectsUnsafeSourceFiles()
    {
        QFETCH(QString, name);
        prepareCaFiles();
        makeWim(2, true, false, true);
        const QString outside = directory_.filePath("outside");
        const QString source = directory_.filePath("payload-link");
        put(outside, "preserved");
        QFile::remove(source);
        QVERIFY(QFile::link(outside, source));
        changeSetupFile(name.toUtf8().constData(), source);
        QCOMPARE(apply(26200, UNATTEND_USE_MS2023_BOOTLOADERS), EIO);
        QCOMPARE(read(outside), QByteArray("preserved"));
        QCOMPARE(read(target_ + "/efi/boot/bootx64.efi"), QByteArray("original firmware loader"));
        QCOMPARE(read(target_ + "/bootmgr.efi"), QByteArray("original manager"));
        QCOMPARE(overwriteCalls, 0);
    }

    void caMissingRequiredFiles_data()
    {
        QTest::addColumn<QString>("name");
        QTest::newRow("firmware") << QStringLiteral("Windows/Boot/EFI_EX/bootmgfw_EX.efi");
        QTest::newRow("manager") << QStringLiteral("Windows/Boot/EFI_EX/bootmgr_EX.efi");
        QTest::newRow("fonts") << QStringLiteral("Windows/Boot/Fonts_EX");
    }

    void caMissingRequiredFiles()
    {
        QFETCH(QString, name);
        prepareCaFiles();
        makeWim(2, true, false, true);
        changeSetupFile(name.toUtf8().constData());
        QCOMPARE(apply(26200, UNATTEND_USE_MS2023_BOOTLOADERS), EIO);
        QCOMPARE(read(target_ + "/bootmgr.efi"), QByteArray("original manager"));
        QCOMPARE(overwriteCalls, 0);
        QVERIFY(!QFile::exists(target_ + '/' + AnswerPath));
    }

    void caFontDirectories_data()
    {
        QTest::addColumn<bool>("empty");
        QTest::newRow("empty-fonts") << true;
        QTest::newRow("missing-destination") << false;
    }

    void caFontDirectories()
    {
        QFETCH(bool, empty);
        prepareCaFiles();
        makeWim(2, true, false, true);
        if (empty)
        {
            changeSetupFile("Windows/Boot/Fonts_EX/win_EXboot_EX.ttf");
            changeSetupFile("Windows/Boot/Fonts_EX/lower_ex.ttf");
        }
        QVERIFY(QDir(target_ + "/efi/microsoft/boot/fonts").removeRecursively());
        QCOMPARE(apply(26200, UNATTEND_USE_MS2023_BOOTLOADERS), empty ? 0 : ENOENT);
        QVERIFY(!QFile::exists(target_ + "/efi/microsoft/boot/fonts"));
    }

    void caInvalidFontPayloads_data()
    {
        QTest::addColumn<bool>("directoryIsFile");
        QTest::newRow("fonts-is-file") << true;
        QTest::newRow("empty-mapped-name") << false;
    }

    void caInvalidFontPayloads()
    {
        QFETCH(bool, directoryIsFile);
        prepareCaFiles();
        makeWim(2, true, false, true);
        const QString source = directory_.filePath("invalid-font");
        put(source, "invalid font payload");
        if (directoryIsFile)
        {
            changeSetupFile("Windows/Boot/Fonts_EX");
            changeSetupFile("Windows/Boot/Fonts_EX", source);
        }
        else
            changeSetupFile("Windows/Boot/Fonts_EX/_EX", source);
        QCOMPARE(apply(26200, UNATTEND_USE_MS2023_BOOTLOADERS), directoryIsFile ? EIO : EINVAL);
        QCOMPARE(read(target_ + "/bootmgr.efi"), QByteArray("original manager"));
        QCOMPARE(overwriteCalls, 0);
    }

    void caCombinedWithHardware_data()
    {
        QTest::addColumn<bool>("haveHive");
        QTest::addColumn<bool>("policy");
        QTest::newRow("native-registry") << true << false;
        QTest::newRow("answer-fallback") << false << false;
        QTest::newRow("native-registry-and-policy") << true << true;
        QTest::newRow("answer-fallback-and-policy") << false << true;
    }

    void caCombinedWithHardware()
    {
        QFETCH(bool, haveHive);
        QFETCH(bool, policy);
        prepareCaFiles();
        makeWim(2, haveHive, false, true);
        const int flags = UNATTEND_USE_MS2023_BOOTLOADERS | UNATTEND_SECUREBOOT_TPM_MINRAM |
            UNATTEND_NO_DATA_COLLECTION | (policy ? UNATTEND_APPLY_SKUSIPOLICY : 0);
        QCOMPARE(apply(26200, flags), 0);
        for (const QString& path : {QStringLiteral("sources/boot.wim"), QStringLiteral("sources/appraiserres.dll"),
            QStringLiteral("setup.exe"), QStringLiteral("setup.dll"), QStringLiteral("efi/boot/bootx64.efi")})
            QVERIFY2(modified_.contains(path), qPrintable(path));
        QCOMPARE(overwriteCalls, 1);
        QCOMPARE(read(target_ + "/efi/boot/bootx64.efi"), QByteArray(96 * 1024, 'F') + "-2");
        QCOMPARE(read(target_ + "/bootmgr.efi"), QByteArray("ca-manager-2"));
        QByteArray xml;
        if (haveHive)
        {
            const QByteArray hive = readWimFile(2, SystemPath);
            int64_t number = 0;
            QCOMPARE(ReadFixtureHiveNumber(hive.constData(), hive.size(), "Setup\\LabConfig", "BypassTPMCheck", &number), 0);
            QCOMPARE(number, int64_t(1));
            xml = read(target_ + '/' + AnswerPath);
            QVERIFY(xml.contains("pass=\"disabled\""));
        }
        else
        {
            xml = readWimFile(2, "Autounattend.xml");
            QVERIFY(xml.contains("pass=\"windowsPE\""));
            QVERIFY(xml.contains("BypassTPMCheck"));
            QVERIFY(xml.contains("<ProtectYourPC>3</ProtectYourPC>"));
        }
        QCOMPARE(xml.count("SkuSiPolicy.p7b"), policy ? 1 : 0);
        QCOMPARE(readWimFile(1, "keep.txt"), QByteArray("keep-1"));
    }

    void caFailuresPreserveCurrentLoader_data()
    {
        QTest::addColumn<int>("failure");
        QTest::addColumn<int>("expected");
        QTest::newRow("extract-failed") << int(ExtractFailure) << EIO;
        QTest::newRow("extract-cancelled") << int(CancelExtract) << ECANCELED;
        QTest::newRow("partial-write") << int(CaWriteFailure) << ENOSPC;
        QTest::newRow("write-cancelled") << int(CancelCaWrite) << ECANCELED;
        QTest::newRow("sync-failed") << int(CaSyncFailure) << EIO;
    }

    void caFailuresPreserveCurrentLoader()
    {
        QFETCH(int, failure);
        QFETCH(int, expected);
        prepareCaFiles();
        makeWim(2, true, false, true);
        const QByteArray before = read(target_ + "/sources/boot.wim");
        fault = Fault(failure);
        QCOMPARE(apply(26200, UNATTEND_USE_MS2023_BOOTLOADERS), expected);
        QCOMPARE(read(target_ + "/efi/boot/bootx64.efi"), QByteArray("original firmware loader"));
        QCOMPARE(read(target_ + "/bootmgr.efi"), QByteArray("original manager"));
        QCOMPARE(read(target_ + "/sources/boot.wim"), before);
        const auto entries = QDir(target_ + "/efi/boot").entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot);
        QCOMPARE(entries, QStringList({QStringLiteral("bootx64.efi")}));
        QVERIFY(!QFile::exists(target_ + '/' + AnswerPath));
        QCOMPARE(overwriteCalls, 0);
    }

    void caMissingPayloadFails()
    {
        prepareCaFiles();
        // init() created an ordinary WIM with no CA payload directories.
        QCOMPARE(apply(26200, UNATTEND_USE_MS2023_BOOTLOADERS), EIO);
        QCOMPARE(read(target_ + "/bootmgr.efi"), QByteArray("original manager"));
        QCOMPARE(read(target_ + "/efi/microsoft/boot/fonts/winboot.ttf"), QByteArray("original font"));
        QVERIFY(!QFile::exists(target_ + '/' + AnswerPath));
    }

    void caUnsafeDestinations_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<bool>("hardlink");
        QTest::newRow("font-link") << QStringLiteral("efi/microsoft/boot/fonts/winboot.ttf") << false;
        QTest::newRow("firmware-link") << QStringLiteral("efi/boot/bootx64.efi") << false;
        QTest::newRow("manager-link") << QStringLiteral("bootmgr.efi") << false;
        QTest::newRow("manager-hardlink") << QStringLiteral("bootmgr.efi") << true;
        QTest::newRow("parent-link") << QStringLiteral("efi/boot") << false;
    }

    void caUnsafeDestinations()
    {
        QFETCH(QString, name);
        QFETCH(bool, hardlink);
        prepareCaFiles();
        makeWim(2, true, false, true);
        const QString outside = directory_.filePath("outside");
        put(outside, "preserved");
        const QString path = target_ + '/' + name;
        if (QFileInfo(path).isDir())
            QVERIFY(QDir(path).removeRecursively());
        else
            QVERIFY(QFile::remove(path));
        if (hardlink)
            QCOMPARE(link(outside.toUtf8().constData(), path.toUtf8().constData()), 0);
        else
            QVERIFY(QFile::link(outside, path));
        QVERIFY(apply(26200, UNATTEND_USE_MS2023_BOOTLOADERS) != 0);
        QCOMPARE(read(outside), QByteArray("preserved"));
        QCOMPARE(overwriteCalls, 0);
    }

    void backupRulesMatchRufus()
    {
        put(target_ + "/sources/appraiserres.bak", "older appraiser");
        put(target_ + "/setup.dll", "existing setup backup");
        QCOMPARE(apply(), EEXIST);
        QCOMPARE(read(target_ + "/sources/appraiserres.bak"), QByteArray("original appraiser"));
        QCOMPARE(read(target_ + "/setup.dll"), QByteArray("existing setup backup"));
        QCOMPARE(read(target_ + "/setup.exe"), originalSetup_);
    }

    void unsupportedArchitectureKeepsOriginalSetup()
    {
        const QByteArray setup = peHeader(0x014c);
        put(target_ + "/setup.exe", setup);
        QCOMPARE(apply(), 0);
        QCOMPARE(read(target_ + "/setup.exe"), setup);
        QVERIFY(!QFile::exists(target_ + "/setup.dll"));
        QVERIFY(messages.contains("Unsupported setup.exe architecture"));
        QVERIFY(!modified_.contains(QStringLiteral("setup.exe")));
        QVERIFY(!modified_.contains(QStringLiteral("setup.dll")));
    }

    void existingAnswerPreventsWimUpdate()
    {
        const QByteArray before = read(target_ + "/sources/boot.wim");
        put(target_ + '/' + AnswerPath, "existing answer");
        QCOMPARE(apply(), EEXIST);
        QCOMPARE(read(target_ + '/' + AnswerPath), QByteArray("existing answer"));
        QCOMPARE(read(target_ + "/sources/boot.wim"), before);
        QCOMPARE(overwriteCalls, 0);
    }

    void unsafeFiles_data()
    {
        QTest::addColumn<QString>("name");
        QTest::newRow("appraiser") << QStringLiteral("sources/appraiserres.dll");
        QTest::newRow("appraiser-backup") << QStringLiteral("sources/appraiserres.bak");
        QTest::newRow("setup") << QStringLiteral("setup.exe");
        QTest::newRow("setup-backup") << QStringLiteral("setup.dll");
        QTest::newRow("wim") << QStringLiteral("sources/boot.wim");
    }

    void unsafeFiles()
    {
        QFETCH(QString, name);
        const QString outside = directory_.filePath("outside");
        put(outside, "preserved");
        const QString path = target_ + '/' + name;
        QFile::remove(path);
        QVERIFY(QFile::link(outside, path));
        QVERIFY(apply() != 0);
        QCOMPARE(read(outside), QByteArray("preserved"));
        QVERIFY(!QFile::exists(target_ + '/' + AnswerPath));
        QCOMPARE(overwriteCalls, 0);
    }
};

QTEST_GUILESS_MAIN(TestWindowsSetup)
#include "test_windows_setup.moc"
