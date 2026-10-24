// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/windows_bcd.h"
#include "platform/windows_bootloaders.h"
#include "pe_arch.h"
#include <hivex.h>
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QMap>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>

namespace
{
enum Fault { NoFault, CommitFailure, SyncFailure, CloseFailure, CancelCommit, ChangeSource };
Fault fault;
bool active, cancelled;
QByteArray sourcePath;
const char managerId[] = "{9dea862c-5cdd-4e70-acc1-f32b344d4795}";
const char efiId[] = "{11111111-0000-0000-0000-000000000001}";
const char biosId[] = "{22222222-0000-0000-0000-000000000002}";
const char peId[] = "{33333333-0000-0000-0000-000000000003}";
const char resumeId[] = "{44444444-0000-0000-0000-000000000004}";
const char inheritId[] = "{55555555-0000-0000-0000-000000000005}";
const char memoryId[] = "{b2721d73-1db4-4c62-bf78-c548a880142d}";

QByteArray utf16(const QString& text)
{
    QByteArray bytes((text.size() + 1) * 2, '\0');
    for (qsizetype i = 0; i < text.size(); ++i)
        qToLittleEndian<quint16>(text[i].unicode(), bytes.data() + i * 2);
    return bytes;
}

QByteArray readFile(const QByteArray& path)
{
    QFile file(QString::fromUtf8(path));
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool putValue(hive_h* hive, hive_node_h node, const char* key, hive_type type, const QByteArray& bytes)
{
    hive_set_value value = {const_cast<char*>(key), type, size_t(bytes.size()), const_cast<char*>(bytes.constData())};
    return node != 0 && hivex_node_set_value(hive, node, &value, 0) == 0;
}

bool putElement(hive_h* hive, hive_node_h object, const char* key, hive_type type, const QByteArray& bytes)
{
    hive_node_h elements = hivex_node_get_child(hive, object, "Elements");
    if (elements == 0) elements = hivex_node_add_child(hive, object, "Elements");
    return putValue(hive, hivex_node_add_child(hive, elements, key), "Element", type, bytes);
}

hive_node_h addObject(hive_h* hive, hive_node_h objects, const char* id, quint32 type)
{
    const hive_node_h object = hivex_node_add_child(hive, objects, id);
    const hive_node_h description = hivex_node_add_child(hive, object, "Description");
    QByteArray bytes(4, '\0');
    qToLittleEndian<quint32>(type, bytes.data());
    return putValue(hive, description, "Type", hive_t_REG_DWORD, bytes) ? object : 0;
}

QByteArray getElement(hive_h* hive, const QByteArray& id, const char* key, hive_type expected)
{
    const hive_node_h objects = hivex_node_get_child(hive, hivex_root(hive), "Objects");
    const hive_node_h object = hivex_node_get_child(hive, objects, id.constData());
    const hive_node_h elements = object != 0 ? hivex_node_get_child(hive, object, "Elements") : 0;
    const hive_node_h element = elements != 0 ? hivex_node_get_child(hive, elements, key) : 0;
    if (element == 0) return {};
    const hive_value_h value = hivex_node_get_value(hive, element, "Element");
    if (value == 0) return {};
    hive_type type;
    size_t size = 0;
    char* bytes = hivex_value_value(hive, value, &type, &size);
    if (bytes == nullptr) return {};
    const QByteArray result = type == expected ? QByteArray(bytes, size) : QByteArray();
    free(bytes);
    return result;
}

rufus::WindowsBcdOptions optionsFor(bool gpt, bool uefi)
{
    rufus::WindowsBcdOptions options;
    options.uefi = uefi;
    options.description = QString::fromUtf8("Windows Édition Ω");
    options.windows.gpt = options.system.gpt = gpt;
    if (gpt)
    {
        options.windows.diskGuid = options.system.diskGuid = QUuid("{12345678-9abc-def0-1122-334455667788}");
        options.windows.partitionGuid = QUuid("{f1e2d3c4-b5a6-8798-1020-304050607080}");
        options.system.partitionGuid = QUuid("{10203040-5060-7080-90a0-b0c0d0e0f000}");
    }
    else
    {
        options.windows.mbrSignature = options.system.mbrSignature = 0xa1b2c3d4;
        options.windows.offset = 0x123456000ULL;
        options.system.offset = 0x100000;
    }
    return options;
}

QByteArray expectedDevice(bool gpt, bool system)
{
    const QByteArray prefix = QByteArray::fromHex("0000000000000000000000000000000006000000000000004800000000000000");
    const QByteArray suffix(16, '\0');
    if (gpt)
        return prefix + QByteArray::fromHex(system ? "403020106050807090a0b0c0d0e0f000" : "c4d3e2f1a6b598871020304050607080")
            + QByteArray(8, '\0') + QByteArray::fromHex("78563412bc9af0de1122334455667788") + suffix;
    return prefix + QByteArray::fromHex(system ? "00001000000000000000000000000000" : "00604523010000000000000000000000")
        + QByteArray::fromHex("0000000001000000d4c3b2a1000000000000000000000000") + suffix;
}
}

extern "C" int IsRufusJobCancelled(void) { return cancelled; }
extern "C" int IsIsoScanCancelled(void) { return cancelled; }
extern "C" void uprintf(const char*, ...) {}
extern "C" void uprintfs(const char*) {}
extern "C" int __real_hivex_commit(hive_h*, const char*, int);
extern "C" int __real_hivex_close(hive_h*);
extern "C" int __real_fsync(int);
extern "C" int __wrap_hivex_commit(hive_h* hive, const char* output, int flags)
{
    if (active && fault == CommitFailure) { errno = EIO; return -1; }
    const int result = __real_hivex_commit(hive, output, flags);
    if (active && fault == CancelCommit) cancelled = true;
    if (active && fault == ChangeSource)
    {
        QFile file(QString::fromUtf8(sourcePath));
        if (!file.open(QIODevice::Append) || file.write("changed", 7) != 7 || !file.flush()) abort();
    }
    return result;
}
extern "C" int __wrap_hivex_close(hive_h* hive)
{
    const int result = __real_hivex_close(hive);
    if (active && fault == CloseFailure) { errno = EIO; return -1; }
    return result;
}
extern "C" int __wrap_fsync(int descriptor)
{
    if (active && fault == SyncFailure) { errno = EIO; return -1; }
    return __real_fsync(descriptor);
}

class WindowsBcdTest : public QObject
{
    Q_OBJECT
    QTemporaryDir temporary_;
    QByteArray source_, output_, original_;

    bool makeFixture(bool standard = false)
    {
        QFile::remove(QString::fromUtf8(source_));
        if (!QFile::copy(QStringLiteral(RUFUS_MINIMAL_HIVE), QString::fromUtf8(source_))) return false;
        hive_h* hive = hivex_open(source_.constData(), HIVEX_OPEN_WRITE);
        if (hive == nullptr) return false;
        const hive_node_h objects = hivex_node_add_child(hive, hivex_root(hive), "Objects");
        const hive_node_h manager = addObject(hive, objects, managerId, 0x10100002);
        const hive_node_h efi = addObject(hive, objects, efiId, 0x10200003);
        const hive_node_h bios = addObject(hive, objects, biosId, 0x10200003);
        const hive_node_h pe = addObject(hive, objects, peId, 0x10200003);
        const hive_node_h resume = addObject(hive, objects, resumeId, 0x10200004);
        const hive_node_h inherit = addObject(hive, objects, inheritId, 0x20200003);
        const hive_node_h memory = addObject(hive, objects, memoryId, 0x10200005);
        bool ok = manager != 0 && efi != 0 && bios != 0 && pe != 0 && resume != 0 && inherit != 0 && memory != 0;
        ok = ok && putElement(hive, manager, "23000003", hive_t_REG_SZ, utf16(QString::fromLatin1(peId)));
        ok = ok && putElement(hive, manager, "25000004", hive_t_REG_BINARY, QByteArray(1, char(30)));
        const char* pathKey = standard ? "12000002" : "42000002";
        ok = ok && putElement(hive, efi, pathKey, hive_t_REG_SZ, utf16("\\system32\\winload.efi"));
        ok = ok && putElement(hive, bios, pathKey, hive_t_REG_SZ, utf16("\\system32\\winload.exe"));
        ok = ok && putElement(hive, efi, "23000003", hive_t_REG_SZ, utf16(QString::fromLatin1(resumeId)));
        ok = ok && putElement(hive, bios, "23000003", hive_t_REG_SZ, utf16(QString::fromLatin1(resumeId)));
        ok = ok && putElement(hive, resume, pathKey, hive_t_REG_SZ, utf16("\\system32\\winresume.efi"));
        ok = ok && putElement(hive, efi, "14000006", hive_t_REG_MULTI_SZ, utf16(QString::fromLatin1(inheritId)) + QByteArray(2, '\0'));
        ok = ok && putElement(hive, bios, "14000006", hive_t_REG_MULTI_SZ, utf16(QString::fromLatin1(inheritId)) + QByteArray(2, '\0'));
        ok = ok && putElement(hive, pe, pathKey, hive_t_REG_SZ, utf16("\\system32\\winload.efi"));
        ok = ok && putElement(hive, pe, "42000003", hive_t_REG_SZ, utf16("\\boot.wim"));
        ok = ok && putElement(hive, inherit, "16000048", hive_t_REG_BINARY, QByteArray(1, '\0'));
        if (ok) ok = hivex_commit(hive, source_.constData(), 0) == 0;
        if (hivex_close(hive) != 0) ok = false;
        return ok;
    }

    int prepare(const rufus::WindowsBcdOptions& options, rufus::WindowsBcdResult& result)
    {
        const int descriptor = open(source_.constData(), O_RDONLY | O_CLOEXEC);
        if (descriptor < 0) return errno;
        if (lseek(descriptor, 17, SEEK_SET) != 17) abort();
        sourcePath = source_;
        active = true;
        const int error = rufus::PrepareWindowsToGoBcd(descriptor, options, result);
        active = false;
        if (lseek(descriptor, 0, SEEK_CUR) != 17 || close(descriptor) != 0) abort();
        return error;
    }

    hive_h* readOutput(const rufus::WindowsBcdResult& result)
    {
        QFile file(QString::fromUtf8(output_));
        if (!file.open(QIODevice::WriteOnly) || file.write(result.bytes) != result.bytes.size() || !file.flush()) return nullptr;
        file.close();
        return hivex_open(output_.constData(), 0);
    }

private slots:
    void initTestCase()
    {
        QVERIFY(temporary_.isValid());
        source_ = temporary_.filePath("template").toUtf8();
        output_ = temporary_.filePath("BCD").toUtf8();
    }
    void init() { fault = NoFault; active = cancelled = false; }

    void generatedStores_data()
    {
        QTest::addColumn<bool>("gpt"); QTest::addColumn<bool>("uefi"); QTest::addColumn<bool>("standard");
        QTest::newRow("MBR-BIOS-factory") << false << false << false;
        QTest::newRow("MBR-EFI-factory") << false << true << false;
        QTest::newRow("GPT-EFI-factory") << true << true << false;
        QTest::newRow("GPT-EFI-standard") << true << true << true;
    }

    void generatedStores()
    {
        QFETCH(bool, gpt); QFETCH(bool, uefi); QFETCH(bool, standard);
        QVERIFY(makeFixture(standard));
        original_ = readFile(source_);
        const auto options = optionsFor(gpt, uefi);
        rufus::WindowsBcdResult result;
        QCOMPARE(prepare(options, result), 0);
        QCOMPARE(result.loader, QUuid(uefi ? efiId : biosId));
        QCOMPARE(readFile(source_), original_);
        hive_h* hive = readOutput(result);
        QVERIFY(hive != nullptr);
        const QByteArray loader = result.loader.toString().toUtf8();
        QCOMPARE(getElement(hive, managerId, "23000003", hive_t_REG_SZ), utf16(result.loader.toString()));
        QCOMPARE(getElement(hive, managerId, "24000001", hive_t_REG_MULTI_SZ), utf16(result.loader.toString()) + QByteArray(2, '\0'));
        QCOMPARE(getElement(hive, managerId, "11000001", hive_t_REG_BINARY), expectedDevice(gpt, true));
        QCOMPARE(getElement(hive, loader, "11000001", hive_t_REG_BINARY), expectedDevice(gpt, false));
        QCOMPARE(getElement(hive, loader, "21000001", hive_t_REG_BINARY), expectedDevice(gpt, false));
        QCOMPARE(getElement(hive, loader, "12000004", hive_t_REG_SZ), utf16(options.description));
        QCOMPARE(getElement(hive, loader, "16000009", hive_t_REG_BINARY), QByteArray(1, '\0'));
        QCOMPARE(getElement(hive, loader, "12000002", hive_t_REG_SZ), utf16(uefi ? "\\Windows\\system32\\winload.efi" : "\\Windows\\system32\\winload.exe"));
        QCOMPARE(getElement(hive, loader, "14000006", hive_t_REG_MULTI_SZ), utf16(QString::fromLatin1(inheritId)) + QByteArray(2, '\0'));
        QVERIFY(getElement(hive, loader, "42000002", hive_t_REG_SZ).isEmpty());
        QCOMPARE(getElement(hive, inheritId, "16000048", hive_t_REG_BINARY), QByteArray(1, '\0'));
        QCOMPARE(getElement(hive, resumeId, "11000001", hive_t_REG_BINARY), expectedDevice(gpt, false));
        QCOMPARE(getElement(hive, resumeId, "12000002", hive_t_REG_SZ), utf16(uefi ? "\\Windows\\system32\\winresume.efi" : "\\Windows\\system32\\winresume.exe"));
        QCOMPARE(getElement(hive, memoryId, "11000001", hive_t_REG_BINARY), expectedDevice(gpt, true));
        QCOMPARE(getElement(hive, managerId, "25000004", hive_t_REG_BINARY), QByteArray(1, char(30)));
        QCOMPARE(hivex_close(hive), 0);
    }

    void failures_data()
    {
        QTest::addColumn<QString>("mode");
        for (const char* mode : {"bad-options", "missing-loader", "ambiguous-loader", "cancel-before", "commit",
            "sync", "close", "cancel-commit", "source-change"}) QTest::newRow(mode) << QString::fromLatin1(mode);
    }

    void failures()
    {
        QFETCH(QString, mode);
        QVERIFY(makeFixture());
        if (mode == "missing-loader" || mode == "ambiguous-loader")
        {
            hive_h* hive = hivex_open(source_.constData(), HIVEX_OPEN_WRITE);
            QVERIFY(hive != nullptr);
            const hive_node_h objects = hivex_node_get_child(hive, hivex_root(hive), "Objects");
            if (mode == "missing-loader")
                QCOMPARE(hivex_node_delete_child(hive, hivex_node_get_child(hive, objects, efiId)), 0);
            else
            {
                const hive_node_h other = addObject(hive, objects, "{66666666-0000-0000-0000-000000000006}", 0x10200003);
                QVERIFY(putElement(hive, other, "12000002", hive_t_REG_SZ, utf16("\\Windows\\system32\\winload.efi")));
            }
            QCOMPARE(hivex_commit(hive, source_.constData(), 0), 0);
            QCOMPARE(hivex_close(hive), 0);
        }
        original_ = readFile(source_);
        auto options = optionsFor(true, true);
        if (mode == "bad-options") options.system.diskGuid = QUuid::createUuid();
        if (mode == "cancel-before") cancelled = true;
        if (mode == "commit") fault = CommitFailure;
        if (mode == "sync") fault = SyncFailure;
        if (mode == "close") fault = CloseFailure;
        if (mode == "cancel-commit") fault = CancelCommit;
        if (mode == "source-change") fault = ChangeSource;
        rufus::WindowsBcdResult result = {QByteArray("old"), QUuid::createUuid()};
        const int expected = mode.startsWith("cancel-") ? ECANCELED : mode == "missing-loader" ? ENOTSUP
            : mode == "bad-options" || mode == "ambiguous-loader" ? EINVAL : mode == "source-change" ? ESTALE : EIO;
        QCOMPARE(prepare(options, result), expected);
        QVERIFY(result.bytes.isEmpty() && result.loader.isNull());
        if (mode != "source-change") QCOMPARE(readFile(source_), original_);
    }

    void userTemplate()
    {
        const QByteArray retained = qgetenv("RUFUS_TEST_BCD_TEMPLATE");
        if (retained.isEmpty()) QSKIP("Optional user-owned BCD template was not selected.");
        QFile::remove(QString::fromUtf8(source_));
        QVERIFY(QFile::copy(QString::fromUtf8(retained), QString::fromUtf8(source_)));
        original_ = readFile(source_);
        for (bool uefi : {false, true})
        {
            rufus::WindowsBcdResult result;
            QCOMPARE(prepare(optionsFor(uefi, uefi), result), 0);
            hive_h* hive = readOutput(result);
            QVERIFY(hive != nullptr);
            const QByteArray loader = result.loader.toString().toUtf8();
            QCOMPARE(getElement(hive, loader, "11000001", hive_t_REG_BINARY), expectedDevice(uefi, false));
            QCOMPARE(getElement(hive, loader, "16000009", hive_t_REG_BINARY), QByteArray(1, '\0'));
            QCOMPARE(hivex_close(hive), 0);
        }
        QCOMPARE(readFile(source_), original_);
    }

    void bootTrees_data()
    {
        QTest::addColumn<QString>("mode");
        for (const char* mode : {"bios", "dual", "arm64-ca2023", "source-link", "target-link", "wrong-architecture"})
            QTest::newRow(mode) << QString::fromLatin1(mode);
    }

    void bootTrees()
    {
        QFETCH(QString, mode);
        QVERIFY(makeFixture());
        const QString windows = temporary_.filePath("windows"), stage = temporary_.filePath("stage");
        const QString destination = temporary_.filePath("destination"), outside = temporary_.filePath("outside");
        for (const QString& path : {windows, stage, destination, outside})
        {
            QVERIFY(QDir(path).removeRecursively());
            QVERIFY(QDir().mkpath(path));
        }
        const bool ex = mode == "arm64-ca2023";
        const bool bios = mode == "bios" || mode == "dual";
        const bool efi = mode != "bios";
        const QByteArray efiRoot = ex ? "/Windows/Boot/EFI_EX/" : "/Windows/Boot/EFI/";
        const QByteArray fonts = ex ? "/Windows/Boot/Fonts_EX/" : "/Windows/Boot/Fonts/";
        const QByteArray manager = ex ? "bootmgfw_EX.efi" : "bootmgfw.efi";
        QByteArray pe(512, '\0');
        pe[0] = 'M'; pe[1] = 'Z';
        qToLittleEndian<quint32>(128, pe.data() + 60);
        pe.replace(128, 4, QByteArray("PE\0\0", 4));
        qToLittleEndian<quint16>(ex ? 0xaa64 : 0x8664, pe.data() + 132);
        const QMap<QByteArray, QByteArray> payloads = {
            {"/Windows/Boot/PCAT/bootmgr", "BIOS manager"},
            {"/Windows/Boot/PCAT/bootnxt", "next"},
            {"/Windows/Boot/PCAT/en-US/bootmgr.exe.mui", "BIOS language"},
            {"/Windows/Boot/Fonts/font.ttf", "ordinary font"},
            {fonts + (ex ? "font_EX.ttf" : "font.ttf"), ex ? "CA2023 font" : "ordinary font"},
            {efiRoot + manager, pe},
            {efiRoot + "en-US/bootmgfw.efi.mui", "EFI language"},
            {"/Windows/Boot/Resources/bootres.dll", "resources"}
        };
        for (auto file = payloads.cbegin(); file != payloads.cend(); ++file)
        {
            const QString path = windows + QString::fromUtf8(file.key());
            QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
            QFile output(path);
            QVERIFY(output.open(QIODevice::WriteOnly));
            QCOMPARE(output.write(file.value()), file.value().size());
        }
        QVERIFY(QDir().mkpath(windows + "/Windows/System32/config"));
        QVERIFY(QFile::copy(QString::fromUtf8(source_), windows + "/Windows/System32/config/BCD-Template"));
        if (mode == "source-link")
            QVERIFY(QFile::link(outside, windows + QString::fromUtf8(efiRoot) + "escape"));
        if (mode == "target-link") QVERIFY(QFile::link(outside, destination + "/EFI"));
        QFile sentinel(outside + "/sentinel");
        QVERIFY(sentinel.open(QIODevice::WriteOnly));
        QCOMPARE(sentinel.write("keep"), 4);
        sentinel.close();
        const int source = open(windows.toUtf8().constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        const int staging = open(stage.toUtf8().constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        const int target = open(destination.toUtf8().constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        QVERIFY(source >= 0 && staging >= 0 && target >= 0);
        rufus::WindowsToGoBootOptions options;
        options.bcd = optionsFor(ex, efi);
        options.bios = bios; options.uefi = efi; options.ca2023 = ex;
        options.architecture = ex ? ARCH_ARM_64 : mode == "wrong-architecture" ? ARCH_ARM_32 : ARCH_X86_64;
        rufus::WindowsBootFilesResult staged, installed;
        const int error = rufus::StageWindowsToGoBootFiles(source, staging, options, staged);
        if (mode == "source-link") QCOMPARE(error, ENOTSUP);
        else if (mode == "wrong-architecture") QCOMPARE(error, EBADMSG);
        else
        {
            QCOMPARE(error, 0);
            QVERIFY(staged.files != 0 && staged.bytes != 0 && staged.path.isEmpty());
            const int copied = rufus::InstallWindowsToGoBootFiles(staging, target, installed);
            if (mode == "target-link") QVERIFY(copied != 0);
            else
            {
                QCOMPARE(copied, 0);
                QCOMPARE(installed.bytes, staged.bytes);
                QCOMPARE(installed.files, staged.files);
                if (bios)
                {
                    QCOMPARE(readFile((destination + "/bootmgr").toUtf8()), QByteArray("BIOS manager"));
                    QCOMPARE(readFile((destination + "/BOOTNXT").toUtf8()), QByteArray("next"));
                    QVERIFY(!QFileInfo::exists(destination + "/Boot/bootmgr"));
                    hive_h* bcd = hivex_open((destination + "/Boot/BCD").toUtf8().constData(), 0);
                    QVERIFY(bcd != nullptr);
                    QCOMPARE(getElement(bcd, managerId, "23000003", hive_t_REG_SZ), utf16(QString::fromLatin1(biosId)));
                    QCOMPARE(hivex_close(bcd), 0);
                }
                if (efi)
                {
                    const QString fallback = ex ? "/EFI/Boot/bootaa64.efi" : "/EFI/Boot/bootx64.efi";
                    QCOMPARE(readFile((destination + fallback).toUtf8()), pe);
                    QCOMPARE(readFile((destination + "/EFI/Microsoft/Boot/Fonts/font.ttf").toUtf8()),
                        ex ? QByteArray("CA2023 font") : QByteArray("ordinary font"));
                    hive_h* bcd = hivex_open((destination + "/EFI/Microsoft/Boot/BCD").toUtf8().constData(), 0);
                    QVERIFY(bcd != nullptr);
                    QCOMPARE(getElement(bcd, managerId, "23000003", hive_t_REG_SZ), utf16(QString::fromLatin1(efiId)));
                    QCOMPARE(hivex_close(bcd), 0);
                }
            }
        }
        QCOMPARE(readFile((outside + "/sentinel").toUtf8()), QByteArray("keep"));
        QCOMPARE(close(target), 0); QCOMPARE(close(staging), 0); QCOMPARE(close(source), 0);
    }
};

QTEST_GUILESS_MAIN(WindowsBcdTest)
#include "test_windows_bcd.moc"
