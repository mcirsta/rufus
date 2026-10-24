// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/windows_registry.h"
#include "unattend.h"
#include <hivex.h>
#include <wimlib.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
#include <cerrno>
#include <cstring>
#include <endian.h>
#include <sys/stat.h>
#include <unistd.h>

enum Fault {
    NoFault, LookupFailure, SetFailure, CommitFailure, SyncFailure, CloseFailure,
    HiveCloseFailure, CancelDuringValues, CancelBeforeCommit, CancelAfterCommit
};
static int fault, valueCalls, outputFd = -1;
static bool active, cancelled;

extern "C" int ReadFixtureHiveNumber(const void*, size_t, const char*, const char*, int64_t*);
extern "C" int IsRufusJobCancelled(void) { return cancelled; }
extern "C" int IsIsoScanCancelled(void) { return cancelled; }
extern "C" void uprintf(const char*, ...) {}
extern "C" void uprintfs(const char*) {}
extern "C" hive_node_h __real_hivex_node_get_child(hive_h*, hive_node_h, const char*);
extern "C" int __real_hivex_node_set_value(hive_h*, hive_node_h, const hive_set_value*, int);
extern "C" int __real_hivex_commit(hive_h*, const char*, int);
extern "C" int __real_hivex_close(hive_h*);
extern "C" int __real_fsync(int);
extern "C" int __real_close(int);

extern "C" hive_node_h __wrap_hivex_node_get_child(hive_h* hive, hive_node_h node, const char* name)
{
    if (active && fault == LookupFailure && strcmp(name, "LabConfig") == 0)
    {
        errno = EIO;
        return 0;
    }
    return __real_hivex_node_get_child(hive, node, name);
}

extern "C" int __wrap_hivex_node_set_value(hive_h* hive, hive_node_h node,
    const hive_set_value* value, int flags)
{
    if (active)
    {
        ++valueCalls;
        if (fault == SetFailure && valueCalls == 2)
        {
            errno = ENOMEM;
            return -1;
        }
        const bool duringValues = fault == CancelDuringValues && valueCalls == 1;
        const bool beforeCommit = fault == CancelBeforeCommit && valueCalls == 3;
        if (duringValues || beforeCommit)
            cancelled = true;
    }
    return __real_hivex_node_set_value(hive, node, value, flags);
}

extern "C" int __wrap_hivex_commit(hive_h* hive, const char* path, int flags)
{
    if (active)
    {
        if (sscanf(path, "/proc/self/fd/%d", &outputFd) != 1)
            qFatal("Commit must use the retained destination descriptor");
        if (fault == CommitFailure)
        {
            if (write(outputFd, "partial", 7) != 7)
                qFatal("Could not inject a partial output");
            errno = ENOSPC;
            return -1;
        }
    }
    const int result = __real_hivex_commit(hive, path, flags);
    if (active && fault == CancelAfterCommit)
        cancelled = true;
    return result;
}

extern "C" int __wrap_hivex_close(hive_h* hive)
{
    const int result = __real_hivex_close(hive);
    if (active && fault == HiveCloseFailure)
    {
        errno = EIO;
        return -1;
    }
    return result;
}

extern "C" int __wrap_fsync(int fd)
{
    if (active && fault == SyncFailure)
    {
        errno = EIO;
        return -1;
    }
    return __real_fsync(fd);
}

extern "C" int __wrap_close(int fd)
{
    const int result = __real_close(fd);
    if (active && fault == CloseFailure && fd == outputFd)
    {
        errno = EIO;
        return -1;
    }
    return result;
}

static int SetDword(hive_h* hive, hive_node_h node, const char* name, uint32_t number)
{
    uint32_t encoded = htole32(number);
    hive_set_value value = { const_cast<char*>(name), hive_t_REG_DWORD, sizeof(encoded),
        reinterpret_cast<char*>(&encoded) };
    return hivex_node_set_value(hive, node, &value, 0);
}

class WindowsRegistryTest : public QObject
{
    Q_OBJECT
private:
    QTemporaryDir directory_;
    QByteArray source_, destination_, sourceBytes_;

    QByteArray readFile(const QByteArray& path)
    {
        QFile file(QString::fromUtf8(path));
        if (!file.open(QIODevice::ReadOnly))
            return {};
        return file.readAll();
    }

    bool makeFixture(bool existingConfig)
    {
        hive_h* hive = hivex_open(source_.constData(), HIVEX_OPEN_WRITE);
        if (!hive)
            return false;
        const hive_node_h root = hivex_root(hive);
        const hive_node_h setup = hivex_node_add_child(hive, root, "sETuP");
        bool valid = setup != 0 && SetDword(hive, setup, "UnrelatedSetupValue", 17) == 0;
        if (valid && existingConfig)
        {
            const hive_node_h config = hivex_node_add_child(hive, setup, "lAbCoNfIg");
            char key[] = "bypassramcheck", oldValue[] = "old!";
            hive_set_value value = { key, hive_t_REG_BINARY, 4, oldValue };
            valid = config != 0 && hivex_node_set_value(hive, config, &value, 0) == 0 &&
                SetDword(hive, config, "KeepMe", 42) == 0;
            if (valid)
            {
                const hive_node_h child = hivex_node_add_child(hive, config, "PreservedChild");
                valid = child != 0 && SetDword(hive, child, "KeepMeToo", 77) == 0;
            }
        }
        const int written = valid ? hivex_commit(hive, source_.constData(), 0) : -1;
        const int closed = hivex_close(hive);
        return valid && written == 0 && closed == 0;
    }

    int prepare()
    {
        active = true;
        const int error = PrepareWindowsSetupHive(source_.constData(), destination_.constData());
        active = false;
        return error;
    }

    void verifyNumber(const QByteArray& bytes, const char* path, const char* name, int64_t expected)
    {
        int64_t value = 0;
        QCOMPARE(ReadFixtureHiveNumber(bytes.constData(), size_t(bytes.size()), path, name, &value), 0);
        QCOMPARE(value, expected);
    }

    void verifyResult()
    {
        const QByteArray bytes = readFile(destination_);
        // Literal expectations stay independent of Rufus's shared name array.
        verifyNumber(bytes, "Setup\\LabConfig", "BypassTPMCheck", 1);
        verifyNumber(bytes, "Setup\\LabConfig", "BypassSecureBootCheck", 1);
        verifyNumber(bytes, "Setup\\LabConfig", "BypassRAMCheck", 1);
        verifyNumber(bytes, "Setup", "UnrelatedSetupValue", 17);
        QCOMPARE(readFile(source_), sourceBytes_);
        struct stat info = {};
        QCOMPARE(stat(destination_.constData(), &info), 0);
        QCOMPARE(info.st_mode & 0777, mode_t(0600));
    }

private slots:
    void initTestCase()
    {
        QVERIFY(directory_.isValid());
        source_ = directory_.filePath("SYSTEM").toUtf8();
        destination_ = directory_.filePath("SYSTEM.updated").toUtf8();
        QCOMPARE(wimlib_global_init(WIMLIB_INIT_FLAG_ASSUME_UTF8), 0);
    }

    void cleanupTestCase() { wimlib_global_cleanup(); }

    void init()
    {
        active = cancelled = false;
        fault = NoFault;
        valueCalls = 0;
        outputFd = -1;
        QFile::remove(QString::fromUtf8(source_));
        QFile::remove(QString::fromUtf8(destination_));
        QVERIFY(QFile::copy(QStringLiteral(RUFUS_MINIMAL_HIVE), QString::fromUtf8(source_)));
        QVERIFY(makeFixture(true));
        sourceBytes_ = readFile(source_);
        QVERIFY(!sourceBytes_.isEmpty());
    }

    void preservesUnrelatedData()
    {
        QCOMPARE(prepare(), 0);
        verifyResult();
        const QByteArray bytes = readFile(destination_);
        verifyNumber(bytes, "Setup\\LabConfig", "KeepMe", 42);
        verifyNumber(bytes, "Setup\\LabConfig\\PreservedChild", "KeepMeToo", 77);
        hive_h* hive = hivex_open(destination_.constData(), 0);
        QVERIFY(hive != nullptr);
        hive_node_h setup = hivex_node_get_child(hive, hivex_root(hive), "setup");
        hive_node_h config = hivex_node_get_child(hive, setup, "labconfig");
        const size_t count = hivex_node_nr_values(hive, config);
        const hive_value_h value = hivex_node_get_value(hive, config, "bypassramcheck");
        hive_type type = hive_t_REG_NONE;
        size_t length = 0;
        const int typed = hivex_value_type(hive, value, &type, &length);
        const int closed = hivex_close(hive);
        QCOMPARE(count, size_t(4)); // Three requested DWORDs plus KeepMe, no duplicate names.
        QCOMPARE(typed, 0);
        QCOMPARE(type, hive_t_REG_DWORD);
        QCOMPARE(length, size_t(4));
        QCOMPARE(closed, 0);
    }

    void createsMissingConfig()
    {
        QVERIFY(QFile::remove(QString::fromUtf8(source_)));
        QVERIFY(QFile::copy(QStringLiteral(RUFUS_MINIMAL_HIVE), QString::fromUtf8(source_)));
        QVERIFY(makeFixture(false));
        sourceBytes_ = readFile(source_);
        QCOMPARE(prepare(), 0);
        verifyResult();
    }

    void windowsToGo_data()
    {
        QTest::addColumn<QString>("change");
        QTest::addColumn<int>("expected");
        QTest::addColumn<int>("flags");
        QTest::newRow("selected-second-set") << QString() << 0 << int(UNATTEND_OFFLINE_INTERNAL_DRIVES);
        QTest::newRow("create-parameters") << QStringLiteral("no-parameters") << 0 << int(UNATTEND_OFFLINE_INTERNAL_DRIVES);
        QTest::newRow("missing-select") << QStringLiteral("no-select") << ENOENT << int(UNATTEND_OFFLINE_INTERNAL_DRIVES);
        QTest::newRow("bad-default-type") << QStringLiteral("bad-type") << EBADMSG << int(UNATTEND_OFFLINE_INTERNAL_DRIVES);
        QTest::newRow("missing-control-set") << QStringLiteral("no-control-set") << ENOENT << int(UNATTEND_OFFLINE_INTERNAL_DRIVES);
        QTest::newRow("missing-service") << QStringLiteral("no-service") << ENOENT << int(UNATTEND_OFFLINE_INTERNAL_DRIVES);
        QTest::newRow("s-mode") << QString() << 0 << int(UNATTEND_FORCE_S_MODE);
        QTest::newRow("s-mode-create-policy") << QStringLiteral("no-ci") << 0 << int(UNATTEND_FORCE_S_MODE);
        QTest::newRow("s-mode-missing-control") << QStringLiteral("no-control") << ENOENT << int(UNATTEND_FORCE_S_MODE);
        QTest::newRow("both-offline-policies") << QString() << 0 << int(UNATTEND_OFFLINE_SERVICING_MASK);
    }

    void windowsToGo()
    {
        QFETCH(QString, change);
        QFETCH(int, expected);
        QFETCH(int, flags);
        hive_h* hive = hivex_open(source_.constData(), HIVEX_OPEN_WRITE);
        QVERIFY(hive != nullptr);
        const hive_node_h root = hivex_root(hive);
        if (change != "no-select")
        {
            const hive_node_h select = hivex_node_add_child(hive, root, "Select");
            QVERIFY(select != 0);
            QCOMPARE(SetDword(hive, select, "Current", 1), 0);
            QCOMPARE(SetDword(hive, select, "Default", 2), 0);
            if (change == "bad-type")
            {
                char key[] = "Default", bytes[] = "2";
                hive_set_value value = {key, hive_t_REG_SZ, sizeof(bytes), bytes};
                QCOMPARE(hivex_node_set_value(hive, select, &value, 0), 0);
            }
        }
        for (int number = 1; number <= 2; ++number)
        {
            if (number == 2 && change == "no-control-set") continue;
            const QByteArray name = "ControlSet00" + QByteArray::number(number);
            hive_node_h node = hivex_node_add_child(hive, root, name.constData());
            QVERIFY(node != 0);
            if (number != 2 || change != "no-control")
            {
                hive_node_h policy = hivex_node_add_child(hive, node, "Control");
                QVERIFY(policy != 0);
                if (number != 2 || change != "no-ci")
                {
                    policy = hivex_node_add_child(hive, policy, "CI");
                    QVERIFY(policy != 0);
                    policy = hivex_node_add_child(hive, policy, "Policy");
                    QVERIFY(policy != 0);
                    QCOMPARE(SetDword(hive, policy, "SkuPolicyRequired", 0), 0);
                    QCOMPARE(SetDword(hive, policy, "KeepMe", 63), 0);
                }
            }
            node = hivex_node_add_child(hive, node, "Services");
            QVERIFY(node != 0);
            if (number == 2 && change == "no-service") continue;
            node = hivex_node_add_child(hive, node, "partmgr");
            QVERIFY(node != 0);
            if (number == 2 && change == "no-parameters") continue;
            node = hivex_node_add_child(hive, node, "Parameters");
            QVERIFY(node != 0);
            QCOMPARE(SetDword(hive, node, "SanPolicy", quint32(number)), 0);
            QCOMPARE(SetDword(hive, node, "KeepMe", 97), 0);
        }
        QCOMPARE(hivex_commit(hive, source_.constData(), 0), 0);
        QCOMPARE(hivex_close(hive), 0);
        sourceBytes_ = readFile(source_);
        const qsizetype descriptors = QDir("/proc/self/fd").entryList().size();
        QCOMPARE(PrepareWindowsToGoHive(source_.constData(), destination_.constData(), flags), expected);
        QCOMPARE(QDir("/proc/self/fd").entryList().size(), descriptors);
        QCOMPARE(readFile(source_), sourceBytes_);
        if (expected != 0)
        {
            QVERIFY(!QFile::exists(QString::fromUtf8(destination_)));
            return;
        }
        const QByteArray result = readFile(destination_);
        verifyNumber(result, "ControlSet001\\Services\\partmgr\\Parameters", "SanPolicy", 1);
        verifyNumber(result, "ControlSet002\\Services\\partmgr\\Parameters", "SanPolicy",
            flags & UNATTEND_OFFLINE_INTERNAL_DRIVES ? 4 : 2);
        verifyNumber(result, "ControlSet001\\Control\\CI\\Policy", "SkuPolicyRequired", 0);
        verifyNumber(result, "ControlSet002\\Control\\CI\\Policy", "SkuPolicyRequired",
            flags & UNATTEND_FORCE_S_MODE ? 1 : 0);
        if (change != "no-ci") verifyNumber(result, "ControlSet002\\Control\\CI\\Policy", "KeepMe", 63);
        verifyNumber(result, "Setup\\LabConfig", "KeepMe", 42);
        if (change != "no-parameters")
            verifyNumber(result, "ControlSet002\\Services\\partmgr\\Parameters", "KeepMe", 97);
    }

    void missingSetupIsRefused()
    {
        QVERIFY(QFile::remove(QString::fromUtf8(source_)));
        QVERIFY(QFile::copy(QStringLiteral(RUFUS_MINIMAL_HIVE), QString::fromUtf8(source_)));
        const QByteArray before = readFile(source_);
        QCOMPARE(prepare(), ENOENT);
        QCOMPARE(readFile(source_), before);
        QVERIFY(!QFile::exists(QString::fromUtf8(destination_)));
    }

    void faults_data()
    {
        QTest::addColumn<int>("injection");
        QTest::addColumn<int>("error");
        QTest::newRow("lookup") << int(LookupFailure) << EIO;
        QTest::newRow("second-value") << int(SetFailure) << ENOMEM;
        QTest::newRow("partial-commit") << int(CommitFailure) << ENOSPC;
        QTest::newRow("flush") << int(SyncFailure) << EIO;
        QTest::newRow("file-close") << int(CloseFailure) << EIO;
        QTest::newRow("hive-close") << int(HiveCloseFailure) << EIO;
        QTest::newRow("cancel-during-values") << int(CancelDuringValues) << ECANCELED;
        QTest::newRow("cancel-before-commit") << int(CancelBeforeCommit) << ECANCELED;
        QTest::newRow("cancel-after-commit") << int(CancelAfterCommit) << ECANCELED;
    }

    void faults()
    {
        QFETCH(int, injection);
        QFETCH(int, error);
        fault = injection;
        const qsizetype descriptors = QDir("/proc/self/fd").entryList().size();
        QCOMPARE(prepare(), error);
        QCOMPARE(QDir("/proc/self/fd").entryList().size(), descriptors);
        QCOMPARE(readFile(source_), sourceBytes_);
        QVERIFY(!QFile::exists(QString::fromUtf8(destination_)));
    }

    void existingDestinationIsPreserved()
    {
        QVERIFY(QFile::copy(QString::fromUtf8(source_), QString::fromUtf8(destination_)));
        QCOMPARE(prepare(), EEXIST);
        QCOMPARE(readFile(destination_), sourceBytes_);
        QCOMPARE(readFile(source_), sourceBytes_);
        QCOMPARE(PrepareWindowsSetupHive(source_.constData(), source_.constData()), EEXIST);
        QCOMPARE(readFile(source_), sourceBytes_);
    }

    void rejectsLinksAndNonFiles()
    {
        const QByteArray linkPath = directory_.filePath("source-link").toUtf8();
        QCOMPARE(symlink(source_.constData(), linkPath.constData()), 0);
        QCOMPARE(PrepareWindowsSetupHive(linkPath.constData(), destination_.constData()), ELOOP);
        QCOMPARE(unlink(linkPath.constData()), 0);
        QCOMPARE(symlink(source_.constData(), destination_.constData()), 0);
        QCOMPARE(prepare(), EEXIST);
        QCOMPARE(readFile(source_), sourceBytes_);
        QCOMPARE(unlink(destination_.constData()), 0);
        QCOMPARE(mkfifo(linkPath.constData(), 0600), 0);
        QCOMPARE(PrepareWindowsSetupHive(linkPath.constData(), destination_.constData()), EINVAL);
        QCOMPARE(unlink(linkPath.constData()), 0);
        QCOMPARE(PrepareWindowsSetupHive(directory_.path().toUtf8().constData(), destination_.constData()), EINVAL);
        QVERIFY(!QFile::exists(QString::fromUtf8(destination_)));
    }

    void invalidInputAndEarlyCancellation()
    {
        QCOMPARE(PrepareWindowsSetupHive(nullptr, destination_.constData()), EINVAL);
        QCOMPARE(PrepareWindowsSetupHive(source_.constData(), ""), EINVAL);
        cancelled = true;
        QCOMPARE(prepare(), ECANCELED);
        cancelled = false;
        QFile file(QString::fromUtf8(source_));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("not a hive"), qint64(10));
        file.close();
        QVERIFY(prepare() != 0);
        QCOMPARE(readFile(source_), QByteArray("not a hive"));
        QVERIFY(!QFile::exists(QString::fromUtf8(destination_)));
    }
};

QTEST_GUILESS_MAIN(WindowsRegistryTest)
#include "test_windows_registry.moc"
