// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/application_devices.h"
#include "platform/udisks_client.h"
#include <QCoreApplication>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QElapsedTimer>
#include <QFile>
#include <QSignalSpy>
#include <QTextStream>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace
{
void require(bool valid,const char* message) { if (!valid) qFatal("Discovery guest: %s",message); }
QByteArray readFile(const QString& path)
{
    QFile file(path); require(file.open(QIODevice::ReadOnly),"fixture identity"); return file.readAll();
}
}
int main(int argc,char** argv)
{
    QCoreApplication app(argc,argv);
    require(app.arguments().size()==2,"listed/self argument");
    require(readFile("/sys/class/dmi/id/sys_vendor").trimmed()=="RufusTest","disposable guest DMI");
    require(readFile("/sys/class/dmi/id/product_name").trimmed()=="RufusDiscoveryFixture","disposable guest product");
    require(readFile("/etc/rufus-discovery-fixture")=="discovery\n","owned fixture marker");
    require(geteuid()==1000,"unprivileged discovery");
    const bool self=app.arguments().at(1)=="self";
    qDBusRegisterMetaType<rufus::UDisksObjects>(); qDBusRegisterMetaType<rufus::UDisksInterfaces>();
    qDBusRegisterMetaType<QList<QByteArray>>();
    const QString block="org.freedesktop.UDisks2.Block",drive="org.freedesktop.UDisks2.Drive";
    const QString partition="org.freedesktop.UDisks2.Partition",filesystem="org.freedesktop.UDisks2.Filesystem";
    const QDBusObjectPath disk("/org/freedesktop/UDisks2/block_devices/sda"),usb("/org/freedesktop/UDisks2/drives/discovery");
    struct stat facts={}; require(stat("/dev/sda",&facts)==0 && S_ISBLK(facts.st_mode),"real USB block node");
    const quint64 number=quint64(facts.st_rdev);
    rufus::UDisksObjects objects;
    objects[disk][block]={{"Drive",QVariant::fromValue(usb)},{"Device",QByteArray("/dev/sda\0",9)},
        {"Size",quint64(320)*1024*1024},{"DeviceNumber",number},{"ReadOnly",true},
        {"HintSystem",false},{"HintIgnore",false},{"Id","discovery-fixture"}};
    objects[usb][drive]={{"ConnectionBus","usb"},{"Removable",true},{"MediaRemovable",true},
        {"MediaAvailable",true},{"Optical",false},{"Serial","RUFUS-DISCOVERY"},
        {"TimeDetected",quint64(1)},{"TimeMediaDetected",quint64(2)}};
    QList<rufus::Device> singles;
    int index=0;
    for (const QString& name : {QStringLiteral("fat"),QStringLiteral("ntfs"),QStringLiteral("exfat")})
    {
        ++index;
        const QString node=QStringLiteral("/dev/sda%1").arg(index),mount="/mnt/"+name;
        require(stat(QFile::encodeName(node).constData(),&facts)==0 && S_ISBLK(facts.st_mode),"real partition node");
        const QDBusObjectPath volume(QStringLiteral("/org/freedesktop/UDisks2/block_devices/sda%1").arg(index));
        objects[volume][block]={{"IdUsage","filesystem"},{"IdLabel",name.toUpper()},
            {"IdType",name=="fat" ? "vfat" : name},{"DeviceNumber",quint64(facts.st_rdev)},
            {"HintSystem",false},{"HintIgnore",false},{"ReadOnly",true}};
        objects[volume][partition]={{"Table",QVariant::fromValue(disk)},{"Offset",quint64(index)*1024*1024}};
        objects[volume][filesystem]={{"MountPoints",QVariant::fromValue(QList<QByteArray>{QFile::encodeName(mount)+'\0'})}};
        rufus::Device item; item.objectPath=disk.path(); item.volumeLabel="fallback";
        const auto content=rufus::ReadUDisksDeviceContent({{volume,objects[volume]}},{item}).first();
        require(content.volumeLabel=="Long "+name+" installer" && content.mountedVolumes==1,"mounted native filesystem extended label");
        QTextStream(stdout)<<"RUFUS_DISCOVERY_LABEL "<<name<<" OK\n";
    }
    const auto backings=rufus::NativeApplicationDevices();
    require(backings.contains(number)==self,"running app backing whole disk");
    const QDBusConnection connection=QDBusConnection::systemBus();
    QDBusMessage request=QDBusMessage::createMethodCall("org.rufus.TestUDisks","/org/freedesktop/UDisks2","org.rufus.TestUDisks","SetObjects");
    request.setArguments({QVariant::fromValue(objects)});
    require(connection.call(request).type()==QDBusMessage::ReplyMessage,"publish real device metadata");
    rufus::UDisksClient client(connection); QSignalSpy spy(&client,&rufus::UDisksClient::devicesChanged);
    client.start(); QElapsedTimer time; time.start(); bool ready=false;
    while (!ready && time.elapsed()<10000)
    {
        if (spy.isEmpty()) spy.wait(100);
        while (!spy.isEmpty()) {
            const auto arguments=spy.takeFirst(); require(arguments[1].toString().isEmpty(),"discovery status");
            const auto devices=qvariant_cast<QList<rufus::Device>>(arguments[0]);
            if (self) ready=devices.isEmpty();
            else if (devices.size()==1) ready=devices.first().mountedVolumes==3 && devices.first().volumeLabel=="Long fat installer" && devices.first().readOnly;
        }
    }
    require(ready,"application exclusion / asynchronous display metadata");
    QTextStream(stdout)<<"RUFUS_DISCOVERY_DONE "<<(self ? "self" : "listed")<<" UID="<<geteuid()<<"\n";
    return 0;
}
