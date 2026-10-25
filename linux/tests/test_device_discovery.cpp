// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/application_devices.h"
#include "platform/device_content.h"
#include "platform/udisks_client.h"
#include "qt/device_size.h"
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>
#include <cerrno>
#include <fcntl.h>
#include <linux/magic.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/sysmacros.h>
#include <unistd.h>

namespace
{
QByteArray utf16(const QString& text)
{
    QByteArray bytes;
    for (const QChar character : text) {
        bytes.append(char(character.unicode() & 255)); bytes.append(char(character.unicode() >> 8));
    }
    return bytes;
}
bool writeFile(const QString& path,const QByteArray& bytes)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    QFile file(path);
    return file.open(QIODevice::WriteOnly|QIODevice::Truncate) && file.write(bytes)==bytes.size();
}
const QString block = QStringLiteral("org.freedesktop.UDisks2.Block");
const QString filesystem = QStringLiteral("org.freedesktop.UDisks2.Filesystem");
const QString partition = QStringLiteral("org.freedesktop.UDisks2.Partition");
}
class TestDeviceDiscovery : public QObject
{
    Q_OBJECT
private slots:
    void labelRules_data()
    {
        QTest::addColumn<QString>("text"); QTest::addColumn<QString>("expected");
        QTest::newRow("normal") << "[autorun]\r\nlabel = Long installer name\r\n" << "Long installer name";
        QTest::newRow("case-quotes") << "\tLaBeL\t= \"Windows 安装\" ; ignored\n" << "Windows 安装";
        QTest::newRow("xml") << " < label >Hello</label>\r\n" << "Hello";
        QTest::newRow("first-nonempty") << "label=\nlabel=\"\"\nlabel=First\nlabel=Second" << "First";
        QTest::newRow("section-independent") << "[other]\nlabel=Other section" << "Other section";
        QTest::newRow("trailing-spaces") << "label=   Keep  \r\n" << "Keep  ";
        QTest::newRow("unquoted-semicolon") << "label=Name ; data" << "Name ; data";
        QTest::newRow("literal-quotes") << "label=Unquoted\"quote" << "Unquoted\"quote";
        QTest::newRow("not-prefix") << "labels=bad\n;label=bad\nxlabel=bad\nlabel=Good" << "Good";
        QTest::newRow("unterminated-quote") << "label=\"Original accepts this\n" << "Original accepts this";
        QTest::newRow("surrogate-pair") << QString::fromUtf8("label=Installer 💾") << QString::fromUtf8("Installer 💾");
    }
    void labelRules()
    {
        QFETCH(QString,text); QFETCH(QString,expected);
        for (const QByteArray& bytes : {utf16(text),QByteArray::fromHex("fffe")+utf16(text),QByteArray::fromHex("efbbbf")+text.toUtf8()}) {
            QString label; QCOMPARE(rufus::ParseDriveLabel(bytes,label),0); QCOMPARE(label,expected);
        }
    }
    void invalidMetadata_data()
    {
        QTest::addColumn<QByteArray>("bytes"); QTest::addColumn<int>("error");
        QTest::newRow("no-label") << utf16("[autorun]\nicon=test.ico\n") << ENOENT;
        QTest::newRow("odd-utf16") << QByteArray("l") << EBADMSG;
        QTest::newRow("utf16be") << QByteArray::fromHex("feff006c006100620065006c003d0041") << EBADMSG;
        QTest::newRow("bad-utf8") << QByteArray::fromHex("efbbbf6c6162656c3dc0af") << EBADMSG;
        QTest::newRow("bad-surrogate") << QByteArray::fromHex("6c006100620065006c003d0000d8") << EBADMSG;
        QTest::newRow("embedded-nul") << utf16(QStringLiteral("label=A")+QChar::Null+"B") << EBADMSG;
        QTest::newRow("control") << utf16("label=Bad\tname") << EBADMSG;
        QTest::newRow("oversized") << QByteArray(65537,'x') << EFBIG;
    }
    void invalidMetadata()
    {
        QFETCH(QByteArray,bytes); QFETCH(int,error); QString label="old";
        QCOMPARE(rufus::ParseDriveLabel(bytes,label),error); QVERIFY(label.isEmpty());
    }
    void originalLineBound()
    {
        QString label;
        QCOMPARE(rufus::ParseDriveLabel(utf16(QString(1023,'x')+"label=Second chunk\n"),label),0);
        QCOMPARE(label,QStringLiteral("Second chunk"));
    }
    void mountedFileSafety()
    {
        QTemporaryDir root; QVERIFY(root.isValid());
        const QByteArray path=QFile::encodeName(root.path());
        const int directory=open(path.constData(),O_RDONLY|O_DIRECTORY|O_CLOEXEC); QVERIFY(directory>=0);
        struct stat facts={}; QCOMPARE(fstat(directory,&facts),0);
        QString label;
        QCOMPARE(rufus::ReadMountedDriveLabel(directory,quint64(facts.st_dev),label),ENOENT);
        const QString inf=root.filePath("AUTORUN.INF");
        QVERIFY(writeFile(inf,QByteArray::fromHex("efbbbf")+"[autorun]\r\nlabel=Long USB label\r\n"));
        QCOMPARE(lseek(directory,0,SEEK_CUR),off_t(0));
        QCOMPARE(rufus::ReadMountedDriveLabel(directory,quint64(facts.st_dev),label),0);
        QCOMPARE(label,QStringLiteral("Long USB label")); QCOMPARE(lseek(directory,0,SEEK_CUR),off_t(0));
        QCOMPARE(rufus::ReadMountedDriveLabel(directory,quint64(facts.st_dev)+1,label),ESTALE);
        QVERIFY(label.isEmpty());
        QVERIFY(writeFile(root.filePath("autorun.inf"),utf16("label=Ambiguous")));
        QCOMPARE(rufus::ReadMountedDriveLabel(directory,quint64(facts.st_dev),label),EBADMSG);
        QVERIFY(QFile::remove(root.filePath("autorun.inf"))); QVERIFY(QFile::remove(inf));
        QVERIFY(QFile::link("/etc/passwd",inf));
        QCOMPARE(rufus::ReadMountedDriveLabel(directory,quint64(facts.st_dev),label),ELOOP);
        QVERIFY(QFile::remove(inf)); QCOMPARE(mkfifo(QFile::encodeName(inf).constData(),0600),0);
        QCOMPARE(rufus::ReadMountedDriveLabel(directory,quint64(facts.st_dev),label),ENOTSUP);
        QVERIFY(QFile::remove(inf)); QVERIFY(QDir().mkdir(inf));
        QCOMPARE(rufus::ReadMountedDriveLabel(directory,quint64(facts.st_dev),label),ENOTSUP);
        QVERIFY(QDir().rmdir(inf)); QVERIFY(writeFile(inf,QByteArray(65537,'x')));
        QCOMPARE(rufus::ReadMountedDriveLabel(directory,quint64(facts.st_dev),label),EFBIG);
        QCOMPARE(close(directory),0);
    }
    void backingGraph()
    {
        QTemporaryDir sys; QVERIFY(sys.isValid()); const QString root=sys.path();
        QVERIFY(writeFile(root+"/devices/sdz/dev","8:240\n"));
        QVERIFY(writeFile(root+"/devices/sdz/sdz1/dev","8:241\n"));
        QVERIFY(writeFile(root+"/devices/sdz/sdz1/partition","1\n"));
        QVERIFY(writeFile(root+"/devices/dm-0/dev","253:0\n"));
        QVERIFY(QDir().mkpath(root+"/devices/dm-0/slaves")); QVERIFY(QDir().mkpath(root+"/dev/block"));
        QVERIFY(QFile::link(root+"/devices/sdz/sdz1",root+"/devices/dm-0/slaves/sdz1"));
        QVERIFY(QFile::link(root+"/devices/dm-0",root+"/dev/block/253:0"));
        QVERIFY(QFile::link(root+"/devices/sdz",root+"/dev/block/8:240"));
        QVERIFY(QFile::link(root+"/devices/sdz/sdz1",root+"/dev/block/8:241"));
        QSet<quint64> devices;
        QCOMPARE(rufus::NativeBlockBackings(quint64(makedev(253,0)),devices,root),0);
        QCOMPARE(devices,(QSet<quint64>{quint64(makedev(253,0)),quint64(makedev(8,241)),quint64(makedev(8,240))}));
        // A cycle terminates while preserving the positive backing set.
        QVERIFY(QDir().mkpath(root+"/devices/sdz/slaves"));
        QVERIFY(QFile::link(root+"/devices/dm-0",root+"/devices/sdz/slaves/dm-0"));
        QCOMPARE(rufus::NativeBlockBackings(quint64(makedev(253,0)),devices,root),0);
        QCOMPARE(devices.size(),3);
        QCOMPARE(rufus::NativeBlockBackings(quint64(makedev(8,240)),devices,root+"/missing"),ENOENT); QVERIFY(devices.isEmpty());
        QCOMPARE(rufus::NativeBlockBackings(0,devices,root),ENOTSUP); QVERIFY(devices.isEmpty());
        QCOMPARE(rufus::NativeBlockBackings(quint64(makedev(8,16)),devices,root),ENOENT);
        QVERIFY(devices.contains(quint64(makedev(8,16))));
    }
    void loopBackingPath()
    {
        QTemporaryDir sys; QVERIFY(sys.isValid()); const QString root=sys.path();
        const QString backing=root+"/image with spaces "; QVERIFY(writeFile(backing,"inert data"));
        struct stat facts={}; QCOMPARE(::stat(QFile::encodeName(backing).constData(),&facts),0);
        struct statfs filesystemFacts={}; QCOMPARE(::statfs(QFile::encodeName(backing).constData(),&filesystemFacts),0);
        QVERIFY(writeFile(root+"/devices/loop0/loop/backing_file",QFile::encodeName(backing)+'\n'));
        QVERIFY(QDir().mkpath(root+"/dev/block")); QVERIFY(QFile::link(root+"/devices/loop0",root+"/dev/block/7:0"));
        QSet<quint64> devices; const int error=rufus::NativeBlockBackings(quint64(makedev(7,0)),devices,root);
        // Memory filesystems have no physical block parent to resolve in the mock sysfs tree.
        if (filesystemFacts.f_type==TMPFS_MAGIC || filesystemFacts.f_type==RAMFS_MAGIC) QCOMPARE(error,0);
        else QVERIFY(error==ENOENT || error==ENOTSUP);
        QVERIFY(devices.contains(quint64(makedev(7,0))));
        if (major(facts.st_dev)!=0) QVERIFY(devices.contains(quint64(facts.st_dev)));
    }
    void mountedVolumeSelection()
    {
        QTemporaryDir root; QVERIFY(root.isValid());
        struct stat facts={}; QCOMPARE(::stat(QFile::encodeName(root.path()).constData(),&facts),0);
        QVERIFY(writeFile(root.filePath("autorun.inf"),utf16("label=Extended installer")));
        rufus::Device device; device.objectPath="/disk"; device.volumeLabel="SHORT"; device.path="/dev/example"; device.name="USB";
        rufus::UDisksObjects objects;
        const QDBusObjectPath volume("/part");
        objects[volume][block]={{"IdUsage","filesystem"},{"IdLabel","FIRST"},{"DeviceNumber",quint64(facts.st_dev)},
                               {"HintSystem",false},{"HintIgnore",false}};
        objects[volume][partition]={{"Table",QVariant::fromValue(QDBusObjectPath("/disk"))},{"Offset",quint64(1024)}};
        objects[volume][filesystem]={{"MountPoints",QVariant::fromValue(QList<QByteArray>{QFile::encodeName(root.path())+'\0',QFile::encodeName(root.path())+'\0'})}};
        auto result=rufus::ReadUDisksDeviceContent(objects,{device}).first();
        QCOMPARE(result.volumeLabel,QStringLiteral("Extended installer")); QCOMPARE(result.mountedVolumes,1);
        QVERIFY(rufus::DeviceCaption(result).startsWith("Extended installer — /dev/example"));
        objects[QDBusObjectPath("/part2")]=objects[volume];
        objects[QDBusObjectPath("/part2")][block]["DeviceNumber"]=quint64(facts.st_dev)+1;
        objects[QDBusObjectPath("/part2")][partition]["Offset"]=quint64(2048);
        result=rufus::ReadUDisksDeviceContent(objects,{device}).first();
        QCOMPARE(result.mountedVolumes,2); QVERIFY(rufus::DeviceCaption(result).startsWith("Multiple Volumes —"));
        objects.remove(QDBusObjectPath("/part2"));
        QVERIFY(writeFile(root.filePath("EFI/Rufus/ntfs_x64.efi"),"marker"));
        result=rufus::ReadUDisksDeviceContent(objects,{device}).first();
        QCOMPARE(result.mountedVolumes,0); QCOMPARE(result.volumeLabel,QStringLiteral("SHORT"));
        objects[volume][filesystem]["MountPoints"]=QVariant::fromValue(QList<QByteArray>{"/../bad\0"});
        result=rufus::ReadUDisksDeviceContent(objects,{device}).first();
        QCOMPARE(result.mountedVolumes,0); QCOMPARE(result.volumeLabel,QStringLiteral("SHORT"));
    }
};
QTEST_GUILESS_MAIN(TestDeviceDiscovery)
#include "test_device_discovery.moc"
