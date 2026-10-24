// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/media_integrity.h"
#include "platform/iso_extract.h"
#include "image_file.h"
#include "format_policy.h"
#include "qt/prepare_image_protocol.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace
{
bool cancelled = false, syncFailure = false;
struct Directory
{
    QTemporaryDir temporary;
    int descriptor = open(QFile::encodeName(temporary.path()).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    ~Directory() { if (descriptor >= 0) ::close(descriptor); }
    QString path(const QString& name) const { return temporary.filePath(name); }
    bool put(const QString& name, const QByteArray& bytes) const
    {
        const QString location = path(name);
        if (!QDir().mkpath(QFileInfo(location).absolutePath())) return false;
        QFile file(location);
        return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.flush();
    }
    QByteArray get(const QString& name) const
    {
        QFile file(path(name));
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }
    bool clean() const { return QDir(temporary.path()).entryList({QStringLiteral(".rufus-md5-*")}, QDir::Files | QDir::Hidden).isEmpty(); }
};
QByteArray md5(const QByteArray& bytes) { return QCryptographicHash::hash(bytes, QCryptographicHash::Md5); }
QByteArray line(const QByteArray& path, const QByteArray& contents) { return md5(contents).toHex() + "  ./" + path + '\n'; }
int record(rufus::MediaIntegrity& integrity, const QByteArray& path, const QByteArray& bytes)
{
    const QByteArray digest = md5(bytes);
    return integrity.copied(path.constData(), quint64(bytes.size()), reinterpret_cast<const unsigned char*>(digest.constData()));
}
struct ExtractionIntegrity
{
    rufus::MediaIntegrity integrity;
    int begins = 0, copies = 0, edits = 0;
    static int begin(void* context, int root, unsigned kind)
    {
        auto* collector = static_cast<ExtractionIntegrity*>(context);
        ++collector->begins;
        return collector->integrity.begin(root, kind, true);
    }
    static int copied(void* context, const char* path, uint64_t size, const uint8_t* digest)
    {
        auto* collector = static_cast<ExtractionIntegrity*>(context);
        ++collector->copies;
        return collector->integrity.copied(path, size, digest);
    }
    static int modified(void* context, const char* path)
    {
        auto* collector = static_cast<ExtractionIntegrity*>(context);
        ++collector->edits;
        return collector->integrity.modified(path);
    }
};
}
extern "C" int __wrap_IsRufusJobCancelled() { return cancelled; }
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int descriptor)
{
    if (syncFailure) { errno = EIO; return -1; }
    return __real_fsync(descriptor);
}

class TestMediaIntegrity : public QObject
{
    Q_OBJECT
private slots:
    void cleanup() { cancelled = syncFailure = false; }
    void originalEligibility()
    {
        struct Selection { int boot; bool efi, dd, toGo, windows, bios, dual, allowed; };
        const Selection selections[] = {
            {BT_IMAGE, true, false, false, false, false, false, true},
            {BT_IMAGE, true, false, false, false, true, false, true},
            {BT_IMAGE, true, false, false, true, false, false, true},
            {BT_IMAGE, true, false, false, true, true, false, false},
            {BT_IMAGE, true, false, false, true, true, true, true},
            {BT_IMAGE, true, true, false, false, false, false, false},
            {BT_IMAGE, true, false, true, true, false, false, false},
            {BT_IMAGE, false, false, false, false, false, false, false},
            {BT_NON_BOOTABLE, true, false, false, false, false, false, false},
            {BT_GRUB2, true, false, false, false, true, false, false}
        };
        QVERIFY(!CanUseRuntimeUefiValidation(nullptr, BT_IMAGE, false, false, false));
        for (const auto& choice : selections) {
            RUFUS_IMG_REPORT report = {};
            report.is_iso = true; report.has_efi = choice.efi;
            report.has_bootmgr = choice.windows; report.is_bootable_img = choice.dd;
            report.disable_iso = choice.dd;
            QCOMPARE(CanUseRuntimeUefiValidation(&report, choice.boot, choice.toGo, choice.bios, choice.dual), choice.allowed);
        }
    }
    void strictRuntimeRequest()
    {
        rufus::PrepareImageRequest request;
        request.isDevice = true; request.path = QStringLiteral("/dev/fixture");
        request.device = 123; request.size = 1024ULL * 1024 * 1024;
        request.clusterSize = 4096; request.label = QStringLiteral("LIVE");
        request.diskSequence = 7; request.objectPath = QStringLiteral("/org/freedesktop/UDisks2/block_devices/fixture");
        request.serviceOwner = QStringLiteral(":1.2"); request.identity = {QStringLiteral("fixture")};
        request.imagePath = QStringLiteral("/unused/live.iso"); request.imageDevice = 1; request.imageInode = 2;
        request.imageSize = 16 * 1024 * 1024; request.imageModified = request.imageChanged = QStringLiteral("123:456");
        for (int filesystem : {FS_FAT16, FS_FAT32, FS_NTFS}) {
            request.fileSystem = filesystem; request.runtimeValidation = true;
            const QJsonObject packet = rufus::EncodePrepareRequest(request);
            QVERIFY(!packet.isEmpty()); QVERIFY(packet.value("runtimeValidation").isBool());
            rufus::PrepareImageRequest decoded;
            QVERIFY(rufus::DecodePrepareRequest(packet, decoded)); QVERIFY(decoded.runtimeValidation);
            for (QJsonValue invalid : {QJsonValue(false), QJsonValue(1), QJsonValue(QStringLiteral("true")), QJsonValue()}) {
                QJsonObject bad = packet; bad.insert("runtimeValidation", invalid);
                QVERIFY(!rufus::DecodePrepareRequest(bad, decoded));
            }
            request.runtimeValidation = false;
            QVERIFY(!rufus::EncodePrepareRequest(request).contains("runtimeValidation"));
        }
        request.runtimeValidation = true; request.nonBootable = true;
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty()); request.nonBootable = false;
        request.rawWrite = true; QVERIFY(rufus::EncodePrepareRequest(request).isEmpty()); request.rawWrite = false;
        request.windowsToGo = true; QVERIFY(rufus::EncodePrepareRequest(request).isEmpty()); request.windowsToGo = false;
        request.windowsBios = request.mbr = true; QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
    }
    void movedAndLinkedChecksumPaths()
    {
        for (bool generated : {false,true}) {
            Directory directory; rufus::MediaIntegrity integrity;
            const QByteArray contents = "final patched config\n";
            QVERIFY(directory.put(QStringLiteral("slax/boot/EFI/config.cfg"),contents));
            const QByteArray oldPath = "slax/boot/EFI/config.cfg";
            if (!generated) QVERIFY(directory.put(QStringLiteral("md5sum.txt"),line(oldPath,"old")+line("link.cfg","old")));
            QCOMPARE(integrity.begin(directory.descriptor,generated ? 0 : 1,generated),0);
            if (generated) {
                QCOMPARE(record(integrity,oldPath,contents),0);
                QVERIFY(directory.put(QStringLiteral("slax/boot/EFI/BOOT/BOOTX64.EFI"),"original EFI"));
                QCOMPARE(record(integrity,"slax/boot/EFI/BOOT/BOOTX64.EFI","original EFI"),0);
            }
            QCOMPARE(integrity.modified(oldPath.constData()),0);
            QVERIFY(QDir(directory.temporary.path()).rename(QStringLiteral("slax/boot/EFI"),QStringLiteral("EFI")));
            QCOMPARE(integrity.relocated("/slax/boot/EFI","/EFI"),0);
            // Metadata links are hashed through the resolved ordinary output,
            // never followed by the checksum reader.
            QVERIFY(directory.put(QStringLiteral("link.cfg"),QByteArray()));
            QCOMPARE(integrity.linked("/link.cfg","/EFI/config.cfg"),0);
            rufus::MediaIntegrityResult result;
            const int finished = integrity.finish(result);
            // Runtime wrapping needs an EFI entry when generating a new list.
            QCOMPARE(finished,0);
            {
                const QByteArray actual = directory.get(QStringLiteral("md5sum.txt"));
                QVERIFY(actual.contains(line("EFI/config.cfg",contents)));
                QVERIFY(actual.contains(line("link.cfg",contents)));
            }
        }
    }

    void generatedManifestAndAllValidators()
    {
        Directory directory; QVERIFY(directory.descriptor >= 0);
        rufus::MediaIntegrity integrity;
        QCOMPARE(integrity.begin(directory.descriptor, 0, true), 0);
        QVERIFY(integrity.generatesManifest());
        const QByteArray source = "append LABEL=old\n", changed = "append LABEL=new\n";
        QVERIFY(directory.put(QStringLiteral("cfg/配置.cfg"), source));
        QCOMPARE(record(integrity, QString::fromUtf8("cfg/配置.cfg").toUtf8(), source), 0);
        QVERIFY(directory.put(QStringLiteral("cfg/配置.cfg"), changed));
        QCOMPARE(integrity.modified("/cfg/配置.cfg"), 0);
        quint64 total = source.size();
        const int architectures[] = {ARCH_X86_32, ARCH_X86_64, ARCH_ARM_32, ARCH_ARM_64, ARCH_RISCV_64, ARCH_LOONGARCH_64};
        for (int architecture : architectures) {
            const QByteArray name = QByteArray("BOOT") + QByteArray(efi_archname[architecture]).toUpper() + ".EFI";
            const QByteArray path = QByteArray("EFI/BOOT/") + name;
            const QByteArray original = QByteArray("original-") + name;
            QVERIFY(directory.put(QString::fromLatin1(path), original));
            QCOMPARE(record(integrity, path, original), 0);
            total += quint64(original.size());
        }
        rufus::MediaIntegrityResult result;
        QCOMPARE(integrity.finish(result), 0);
        QCOMPARE(result.validators, 6u); QCOMPARE(result.files, quint64(7)); QCOMPARE(result.bytes, total);
        QCOMPARE(result.updated, 1u); QVERIFY(!integrity.active()); QVERIFY(directory.clean());
        const QByteArray manifest = directory.get(QStringLiteral("md5sum.txt"));
        QVERIFY(manifest.startsWith("# md5sum_totalbytes = 0x" + QByteArray::number(total, 16) + '\n'));
        QVERIFY(manifest.contains(line(QString::fromUtf8("cfg/配置.cfg").toUtf8(), changed)));
        for (int architecture : architectures) {
            const QByteArray name = QByteArray("BOOT") + QByteArray(efi_archname[architecture]).toUpper() + ".EFI";
            const QByteArray backup = name.left(name.size() - 4) + "_original.EFI";
            const QByteArray original = QByteArray("original-") + name;
            QCOMPARE(directory.get(QString::fromLatin1("EFI/BOOT/" + backup)), original);
            QByteArray validator; QCOMPARE(rufus::ReadMediaValidator(architecture, validator), 0);
            QFile resource(QStringLiteral(RUFUS_TEST_MD5_DIRECTORY) + QStringLiteral("/boot")
                + QString::fromLatin1(efi_archname[architecture]) + QStringLiteral(".efi"));
            QVERIFY(resource.open(QIODevice::ReadOnly)); QCOMPARE(validator, resource.readAll());
            QCOMPARE(directory.get(QString::fromLatin1("EFI/BOOT/" + name)), validator);
            QVERIFY(manifest.contains(line("EFI/BOOT/" + backup, original)));
        }
        struct stat root; QVERIFY(fstat(directory.descriptor, &root) == 0);
    }

    void extractionStreamsSourceHashesAndTracksEdits()
    {
        for (bool udf : {false, true}) {
            Directory tree, target; QTemporaryDir images;
            QVERIFY(tree.descriptor >= 0 && target.descriptor >= 0 && images.isValid());
            const QByteArray source = "append SOURCE\r\n", expected = "append DEST\n", loader = "original EFI fixture";
            QVERIFY(tree.put(QStringLiteral("isolinux/isolinux.cfg"), source));
            QVERIFY(tree.put(QStringLiteral("efi/boot/bootx64.efi"), loader));
            if (udf) QVERIFY(tree.put(QStringLiteral("md5sum.txt"), line("isolinux/isolinux.cfg", source)
                + line("efi/boot/bootx64.efi", loader)));
            const QString image = images.filePath(QStringLiteral("input.iso"));
            QStringList arguments = {QStringLiteral("-quiet"), QStringLiteral("-R"), QStringLiteral("-J"),
                QStringLiteral("-V"), QStringLiteral("SOURCE"), QStringLiteral("-o"), image};
            if (udf) arguments.append(QStringLiteral("-udf"));
            arguments.append(tree.temporary.path());
            QProcess process; process.start(QStringLiteral(RUFUS_TEST_MKISOFS), arguments);
            QVERIFY(process.waitForFinished(10000)); QCOMPARE(process.exitCode(), 0);
            ExtractionIntegrity collector;
            const ISO_EXTRACT_EVENTS events = {&collector, true, ExtractionIntegrity::begin,
                ExtractionIntegrity::copied, ExtractionIntegrity::modified, nullptr};
            const QByteArray encoded = QFile::encodeName(image), output = QFile::encodeName(target.temporary.path());
            const ISO_EXTRACT_OPTIONS options = {{true, true, {0, 0}}, "DEST", encoded.constData(), false, 0, &events};
            ISO_EXTRACT_RESULT planned = {}, extracted = {};
            QCOMPARE(CheckIsoTree(encoded.constData(), &options, 0, &planned), 0);
            ISO_ANALYSIS analysis = {}; ISO_ANALYSIS_SOURCE view = {};
            const int analyzed = AnalyzeIsoImageWithSource(encoded.constData(), QFileInfo(image).size(), &options.analysis, &analysis, &view);
            QVERIFY(analyzed == ISO_SCAN_OK || analyzed == ISO_SCAN_PARTIAL);
            quint64 reserve = 0; QString problem;
            const int checked = rufus::CheckMediaIntegritySource(view, analysis, true, 0, true, reserve, problem);
            CloseIsoAnalysisSource(&view);
            QCOMPARE(checked, 0); QVERIFY(reserve > 40000); QVERIFY(problem.isEmpty());
            QCOMPARE(collector.begins, 0); QCOMPARE(collector.copies, 0);
            QCOMPARE(ExtractIsoTree(encoded.constData(), output.constData(), &options, &extracted), 0);
            QCOMPARE(collector.begins, 1); QVERIFY(collector.edits >= 1);
            QCOMPARE(collector.copies, int(extracted.files)); QCOMPARE(extracted.patched_files, quint64(1));
            QCOMPARE(target.get(QStringLiteral("isolinux/isolinux.cfg")), expected);
            rufus::MediaIntegrityResult result; QCOMPARE(collector.integrity.finish(result), 0);
            const QByteArray manifest = target.get(QStringLiteral("md5sum.txt"));
            QVERIFY(manifest.contains(line("isolinux/isolinux.cfg", expected)));
            QVERIFY(manifest.contains(line("efi/boot/bootx64_original.efi", loader)));
            QVERIFY(target.clean());
            // These are source refusals, before the helper opens a target.
            for (bool backup : {true, false}) {
                const QString reserved = QStringLiteral("efi/boot/bootx64_original.efi");
                if (backup) QVERIFY(tree.put(reserved, "existing original"));
                else {
                    QVERIFY(QFile::remove(tree.path(reserved)));
                    QVERIFY(tree.put(QStringLiteral("md5sum.txt"), "# no validation entries\n"));
                }
                process.start(QStringLiteral(RUFUS_TEST_MKISOFS), arguments);
                QVERIFY(process.waitForFinished(10000)); QCOMPARE(process.exitCode(), 0);
                analysis = {}; view = {};
                const int status = AnalyzeIsoImageWithSource(encoded.constData(), QFileInfo(image).size(), &options.analysis, &analysis, &view);
                QVERIFY(status == ISO_SCAN_OK || status == ISO_SCAN_PARTIAL);
                const int refused = rufus::CheckMediaIntegritySource(view, analysis, true, 0, true, reserve, problem);
                CloseIsoAnalysisSource(&view);
                QCOMPARE(refused, backup ? EEXIST : EBADMSG); QVERIFY(!problem.isEmpty());
            }
        }
    }

    void preserveUnchangedSourceChecksums()
    {
        Directory directory; QVERIFY(directory.descriptor >= 0);
        const QByteArray original = "first\n", changed = "second\n";
        QVERIFY(directory.put(QStringLiteral("boot/grub/grub.cfg"), changed));
        QVERIFY(directory.put(QStringLiteral("payload.bin"), "payload"));
        const QByteArray badPayload = QByteArray(32, '0') + " *./payload.bin\r\n";
        const QByteArray untouched = "# retained comment\r\nmalformed line\r\n";
        const QByteArray manifest = line("boot/grub/grub.cfg", original) + badPayload + untouched;
        QVERIFY(directory.put(QStringLiteral("md5sum.txt"), manifest));
        rufus::MediaIntegrity integrity;
        QCOMPARE(integrity.begin(directory.descriptor, 1, false), 0);
        QCOMPARE(integrity.modified("boot/grub/grub.cfg"), 0);
        rufus::MediaIntegrityResult result; QCOMPARE(integrity.finish(result), 0);
        QCOMPARE(result.updated, 1u); QCOMPARE(result.validators, 0u);
        QCOMPARE(directory.get(QStringLiteral("md5sum.txt")), line("boot/grub/grub.cfg", changed) + badPayload + untouched);
        QVERIFY(directory.clean());
    }

    void generatedPartsReplaceMissingWimReference()
    {
        Directory target, stage; QVERIFY(target.descriptor >= 0 && stage.descriptor >= 0);
        const QByteArray first = "generated first WIM part", second = "generated second WIM part";
        QVERIFY(stage.put(QStringLiteral("install.swm"), first));
        QVERIFY(stage.put(QStringLiteral("install2.swm"), second));
        QVERIFY(target.put(QStringLiteral("md5sum.txt"), line("sources/install.wim", "original WIM") + "# keep\n"));
        QVERIFY(QDir().mkpath(target.path(QStringLiteral("sources"))));
        rufus::MediaIntegrity integrity; QCOMPARE(integrity.begin(target.descriptor, 1, false), 0);
        QCOMPARE(integrity.splitPart("/sources/install.wim", stage.descriptor, "install.swm"), 0);
        QCOMPARE(integrity.splitPart("/sources/install.wim", stage.descriptor, "install2.swm"), 0);
        QVERIFY(QFile::rename(stage.path(QStringLiteral("install.swm")), target.path(QStringLiteral("sources/install.swm"))));
        QVERIFY(QFile::rename(stage.path(QStringLiteral("install2.swm")), target.path(QStringLiteral("sources/install2.swm"))));
        rufus::MediaIntegrityResult result; QCOMPARE(integrity.finish(result), 0);
        QCOMPARE(target.get(QStringLiteral("md5sum.txt")), line("sources/install.swm", first) + line("sources/install2.swm", second) + "# keep\n");
        QCOMPARE(result.updated, 1u); QVERIFY(target.clean());
    }
    void uppercaseManifestAndRuntime()
    {
        Directory directory; QVERIFY(directory.descriptor >= 0);
        const QByteArray original = "old config", changed = "new config", loader = "loader";
        QVERIFY(directory.put(QStringLiteral("cfg.txt"), changed));
        QVERIFY(directory.put(QStringLiteral("efi/boot/bootx64.efi"), loader));
        const QByteArray manifest = line("cfg.txt", original);
        QVERIFY(directory.put(QStringLiteral("MD5SUMS"), manifest));
        rufus::MediaIntegrity integrity;
        QCOMPARE(integrity.begin(directory.descriptor, 2, true), 0);
        QCOMPARE(record(integrity, "cfg.txt", original), 0);
        QCOMPARE(record(integrity, "efi/boot/bootx64.efi", loader), 0);
        QCOMPARE(record(integrity, "MD5SUMS", manifest), 0);
        QCOMPARE(integrity.modified("cfg.txt"), 0);
        rufus::MediaIntegrityResult result; QCOMPARE(integrity.finish(result), 0);
        QCOMPARE(directory.get(QStringLiteral("MD5SUMS")), line("cfg.txt", changed));
        const QByteArray generated = directory.get(QStringLiteral("md5sum.txt"));
        QVERIFY(generated.contains(line("cfg.txt", changed)));
        QVERIFY(generated.contains(line("efi/boot/bootx64_original.efi", loader)));
        QVERIFY(!generated.contains("MD5SUMS")); QCOMPARE(result.files, quint64(2));
        QCOMPARE(result.validators, 1u); QVERIFY(directory.clean());
    }

    void prewriteRefusalsAndCleanup()
    {
        Directory directory; QVERIFY(directory.descriptor >= 0);
        QVERIFY(directory.put(QStringLiteral("efi/boot/bootx64.efi"), "original"));
        QVERIFY(directory.put(QStringLiteral("efi/boot/bootx64_original.efi"), "existing backup"));
        rufus::MediaIntegrity integrity;
        QCOMPARE(integrity.begin(directory.descriptor, 0, true), 0);
        QCOMPARE(integrity.modified("../../outside"), EINVAL);
        QCOMPARE(integrity.modified("a\nother"), EINVAL);
        rufus::MediaIntegrityResult result; QCOMPARE(integrity.finish(result), EEXIST);
        QCOMPARE(directory.get(QStringLiteral("efi/boot/bootx64.efi")), QByteArray("original"));
        QCOMPARE(directory.get(QStringLiteral("efi/boot/bootx64_original.efi")), QByteArray("existing backup"));
        QVERIFY(directory.clean()); QVERIFY(!integrity.active());
        Directory external; QVERIFY(external.put(QStringLiteral("list"), "external"));
        QVERIFY(QFile::link(external.path(QStringLiteral("list")), directory.path(QStringLiteral("md5sum.txt"))));
        QCOMPARE(integrity.begin(directory.descriptor, 1, false), 0);
        QCOMPARE(integrity.finish(result), ELOOP);
        QCOMPARE(external.get(QStringLiteral("list")), QByteArray("external")); QVERIFY(directory.clean());
    }

    void cancellationAndSyncFailures()
    {
        Directory directory; QVERIFY(directory.descriptor >= 0);
        QVERIFY(directory.put(QStringLiteral("md5sum.txt"), "# original\n"));
        rufus::MediaIntegrity integrity; rufus::MediaIntegrityResult result;
        QCOMPARE(integrity.begin(directory.descriptor, 1, false), 0);
        cancelled = true; QCOMPARE(integrity.finish(result), ECANCELED); cancelled = false;
        QVERIFY(directory.clean()); QVERIFY(!integrity.active());
        QCOMPARE(directory.get(QStringLiteral("md5sum.txt")), QByteArray("# original\n"));
        QCOMPARE(integrity.begin(directory.descriptor, 1, false), 0);
        syncFailure = true; QCOMPARE(integrity.finish(result), EIO); syncFailure = false;
        QCOMPARE(result.syncError, EIO); QVERIFY(!integrity.active()); QVERIFY(directory.clean());
        QCOMPARE(directory.get(QStringLiteral("md5sum.txt")), QByteArray("# original\n"));
        QByteArray missing;
        QCOMPARE(rufus::ReadMediaValidator(ARCH_IA_64, missing), ENOENT);
        QCOMPARE(rufus::ReadMediaValidator(ARCH_EBC, missing), ENOENT);
        QCOMPARE(rufus::ReadMediaValidator(ARCH_MAX, missing), EINVAL);
    }
};
QTEST_GUILESS_MAIN(TestMediaIntegrity)
#include "test_media_integrity.moc"
