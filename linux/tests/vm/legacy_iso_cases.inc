// SPDX-License-Identifier: GPL-3.0-or-later
// Included only in the guarded PID 1 disposable-device driver.
void saveLegacyIsoBootDisk(const char* destination)
{
    const int source = open("/dev/sda",O_RDONLY|O_CLOEXEC);
    const int target = open(destination,O_RDWR|O_CLOEXEC);
    require(source >= 0 && target >= 0,"B4 sparse boot snapshot descriptors");
    unsigned char buffer[65536]; quint64 offset = 0;
    while (offset < diskBytes) {
        const ssize_t bytes = read(source,buffer,sizeof(buffer));
        if (bytes < 0 && errno == EINTR) continue;
        require(bytes > 0 && quint64(bytes) <= diskBytes-offset,"B4 sparse boot snapshot read");
        bool nonzero = false;
        for (ssize_t index = 0; index < bytes; ++index) nonzero |= buffer[index] != 0;
        ssize_t written = 0;
        while (nonzero && written < bytes) {
            const ssize_t count = pwrite(target,buffer+written,size_t(bytes-written),off_t(offset+quint64(written)));
            if (count < 0 && errno == EINTR) continue;
            require(count > 0,"B4 sparse boot snapshot write"); written += count;
        }
        offset += quint64(bytes);
    }
    require(fsync(target) == 0 && close(target) == 0 && close(source) == 0,"B4 sparse boot snapshot sync");
}

void runLegacyIsoCases(const rufus::PrepareImageRequest& base)
{
    require(QDir().mkpath("/run/caller-home") && QDir().mkpath("/mnt/readback"),"B4 directories");
    require(mount("tmpfs","/run/caller-home","tmpfs",MS_NODEV|MS_NOSUID,"size=64m") == 0,"B4 independent source backing");
    writeGuestFile("/etc/passwd","root:x:0:0:root:/root:/bin/bash\ncaller:x:1000:1000:Caller:/run/caller-home:/bin/bash\n");
    require(chown("/run/caller-home",1000,1000) == 0 && QDir().mkpath("/run/caller-home/files"),"B4 caller home/cache");
    const auto query = QDBusMessage::createMethodCall(base.serviceOwner,"/org/freedesktop/UDisks2","org.freedesktop.DBus.ObjectManager","GetManagedObjects");
    const QDBusReply<rufus::UDisksObjects> snapshot = QDBusConnection::systemBus().call(query);
    require(snapshot.isValid(),"B4 target metadata"); rawObjects = snapshot.value();
    struct stat partition; require(stat("/dev/sda1",&partition) == 0 && S_ISBLK(partition.st_mode),"B4 seed partition");
    rawPartition = quint64(partition.st_rdev);
    require(QDir().mkpath("/dev/block"),"B4 device aliases");
    for (int number : {1,2}) {
        const QByteArray alias = QByteArray("/dev/block/")+QByteArray::number(major(partition.st_rdev))+':'+QByteArray::number(minor(partition.st_rdev)+number-1);
        const QByteArray node = QByteArray("../sda")+QByteArray::number(number);
        require(symlink(node.constData(),alias.constData()) == 0 || errno == EEXIST,"B4 partition alias");
    }
    const QJsonArray manifest = QJsonDocument::fromJson(readFile("/legacy-iso-manifest.json")).array();
    require(manifest.size() == 5,"B4 source manifest");
    for (int number : {0,1,2,4,3}) {
        for (const char* name : {"/run/format-launches","/run/format-exit-held"}) QFile::remove(QString::fromLatin1(name));
        auto job = base; job.volumeId = 0; job.fileSystem = number == 4 ? FS_NTFS : number < 2 ? FS_FAT16 : FS_FAT32;
        job.mbr = number != 2; job.syslinux = number == 2; job.windowsBios = number < 2 || number == 4;
        job.kolibriOs = number == 3; job.enhancedInstall = number < 2 || number == 4; job.uefiNtfs = number == 4;
        job.clusterSize = number < 2 ? 8192 : 4096; job.label = "B4SOURCE";
        const QString source = QStringLiteral("/run/caller-home/legacy-%1.iso").arg(number);
        require(QFile::copy(QStringLiteral("/legacy-%1.iso").arg(number),source)
            && chown(QFile::encodeName(source).constData(),1000,1000) == 0,"B4 owned source");
        require(rufus::CapturePrepareSource(source,job) == 0,"B4 source identity");
        publishTargetMount(rawObjects,job,{},rawPartition);
        QProcess client; client.setProcessChannelMode(QProcess::ForwardedChannels);
        client.start("/usr/bin/test_native_format_qt_guest",{}); require(client.waitForStarted(10000),"B4 Qt client");
        const QByteArray packet = QJsonDocument(rufus::EncodePrepareRequest(job)).toJson(QJsonDocument::Compact)+'\n';
        require(packet.size() > 3 && client.write(packet) == packet.size(),"B4 typed pipe"); client.closeWriteChannel();
        require(client.waitForFinished(240000) && client.exitCode() == 0,"B4 joined Qt job");
        require(mount("/dev/sda1","/mnt/readback",number == 4 ? "ntfs" : "vfat",MS_RDONLY|MS_NODEV|MS_NOSUID|MS_NOEXEC,number == 4 ? "iocharset=utf8" : "utf8=1") == 0,"B4 independent readback");
        const QJsonObject files = manifest.at(number).toObject().value("files").toObject();
        for (auto entry = files.begin(); entry != files.end(); ++entry) {
            const QByteArray data = readFile(QStringLiteral("/mnt/readback/")+entry.key());
            require(QCryptographicHash::hash(data,QCryptographicHash::Sha256).toHex() == entry.value().toString().toLatin1(),"B4 copied/generated file bytes");
        }
        if (number == 2) {
            require(readFile("/mnt/readback/mboot.c32") == readFile("/original-mboot.c32"),"B4 original ReactOS mboot resource");
            require(!readFile("/mnt/readback/ldlinux.sys").isEmpty(),"B4 ReactOS native Syslinux installed");
            const QByteArray config = readFile("/mnt/readback/syslinux.cfg");
            const QByteArray prefix = "DEFAULT ReactOS\nLABEL ReactOS\n  KERNEL mboot.c32\n  APPEND ";
            require(config == prefix+"/freeldr.sys" || config == prefix+"/loader/setupldr.sys","B4 original ReactOS selected loader configuration");
        }
        if (number == 3) {
            const int loader = open("/mnt/readback/MTLD_F32",O_RDONLY|O_CLOEXEC); uint32_t attributes = 0;
            require(loader >= 0 && ioctl(loader,FAT_IOCTL_GET_ATTRIBUTES,&attributes) == 0
                && (attributes & (ATTR_HIDDEN|ATTR_SYS)) == (ATTR_HIDDEN|ATTR_SYS),"B4 Kolibri real FAT hidden/system attributes");
            require(close(loader) == 0,"B4 Kolibri attribute descriptor close");
            require(!QFile::exists("/mnt/readback/ldlinux.sys"),"B4 Kolibri PBR owns loader");
        }
        require(umount("/mnt/readback") == 0,"B4 independent readback unmount");
        const int device = open("/dev/sda1",O_RDONLY|O_CLOEXEC); char sector[512];
        require(device >= 0 && pread(device,sector,sizeof(sector),0) == sizeof(sector) && close(device) == 0,"B4 original PBR readback");
        const QByteArray boot(sector,sizeof(sector));
        const int disk = open("/dev/sda",O_RDONLY|O_CLOEXEC); char firstSector[512];
        require(disk >= 0 && pread(disk,firstSector,sizeof(firstSector),0) == sizeof(firstSector) && close(disk) == 0,"B4 original MBR readback");
        const QByteArray mbr(firstSector,sizeof(firstSector));
        const QString bootstrap = number == 3 ? QStringLiteral("kolibri") : QStringLiteral("rufus");
        const QByteArray originalMbr = readFile(QStringLiteral("/mbr-")+bootstrap+QStringLiteral(".bin"));
        require(originalMbr.size() == 440 && mbr.left(440) == originalMbr
            && uint8_t(mbr[446]) == (number == 0 || number == 4 ? 0x81 : 0x80),"B4 original MBR dispatch and legacy masquerading");
        if (number < 2) {
            const QByteArray head = readFile("/fat16_0x0.bin"), body = readFile("/fat16_0x3e.bin");
            require(boot.left(head.size()) == head && boot.mid(0x3e,body.size()) == body
                && boot.mid(43,11) == "NO NAME    " && uint8_t(boot[36]) == 0x80,"B4 original standard FAT16 PBR, no BOOTMGR alias");
        } else if (number == 3) {
            const QByteArray body = readFile("/fat32kos_0x52.bin");
            require(boot.mid(0x52,body.size()) == body,"B4 original Kolibri FAT32 PBR");
            const int partition = open("/dev/sda1",O_RDONLY|O_CLOEXEC); char backup[512];
            require(partition >= 0 && pread(partition,backup,sizeof(backup),6*512) == sizeof(backup)
                && close(partition) == 0 && QByteArray(backup,sizeof(backup)) == boot,"B4 original Kolibri backup PBR");
        } else if (number == 4) {
            const QByteArray head = readFile("/ntfs_0x0.bin"), body = readFile("/ntfs_0x54.bin");
            const int partition = open("/dev/sda1",O_RDONLY|O_CLOEXEC); QByteArray record(0x54+body.size(),'\0');
            require(partition >= 0 && pread(partition,record.data(),size_t(record.size()),0) == record.size()
                && close(partition) == 0 && record.left(head.size()) == head && record.mid(0x54) == body,"B4 original NTFS PBR after finalization");
            const quint32 bootStart = qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(mbr.constBegin()+470));
            const quint32 mainStart = qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(mbr.constBegin()+454));
            const quint32 mainSectors = qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(mbr.constBegin()+458));
            const QByteArray payload = readFile("/uefi-ntfs.img");
            constexpr quint64 trackBytes = 63*512;
            const quint64 bootBytes = (quint64(payload.size())+trackBytes-1)/trackBytes*trackBytes;
            const quint64 expectedStart = (diskBytes-bootBytes)/trackBytes*trackBytes/512;
            require(mainStart == 2048 && bootStart == expectedStart && bootStart >= mainStart+mainSectors && uint8_t(mbr[462]) == 0
                && uint8_t(mbr[466]) == 0xef,"B4 final MBR retains UEFI boot partition and only masquerades main");
            const int target = open("/dev/sda",O_RDONLY|O_CLOEXEC); QByteArray actual(payload.size(),'\0');
            require(target >= 0 && pread(target,actual.data(),size_t(actual.size()),quint64(bootStart)*512) == actual.size()
                && close(target) == 0 && actual == payload,"B4 original UEFI NTFS tail payload after final masquerading");
            QProcess metadata; metadata.start("/usr/bin/ntfsinfo",{"-m","/dev/sda1"});
            require(metadata.waitForFinished(10000) && metadata.exitCode() == 0,"B4 independent NTFS metadata reader");
            const QByteArray description = metadata.readAllStandardOutput().simplified();
            require(description.contains("Volume Flags: 0x0000"),"B4 NTFS finalization leaves clean volume");
            puts("RUFUS_LEGACY_ISO_NTFS_DUAL LAYOUT=1 FLAGS=1 PAYLOAD=1 CLEAN=1");
        }
        if (number == 1) saveLegacyIsoBootDisk("/dev/vda");
        if (number == 2) saveLegacyIsoBootDisk("/dev/vdb");
        const int claim = open("/dev/sda",O_RDWR|O_EXCL|O_CLOEXEC); require(claim >= 0 && close(claim) == 0,"B4 target claim released");
        printf("RUFUS_LEGACY_ISO_READBACK JOB=%d FILES=%d PBR=1 ATTRS=1 CLAIM=1\n",number,int(files.size()));
    }
    require(umount("/run/caller-home") == 0,"B4 source cleanup");
    puts("RUFUS_LEGACY_ISO_DONE JOBS=5");
}
