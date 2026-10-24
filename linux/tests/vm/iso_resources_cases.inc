// SPDX-License-Identifier: GPL-3.0-or-later
// This driver is guarded by PID1, fixture DMI and a private mount namespace.
void runIsoResourcesCases(const rufus::PrepareImageRequest& base)
{
    require(QDir().mkpath("/run/caller-home") && QDir().mkpath("/mnt/readback"), "B3 directories");
    require(mount("tmpfs","/run/caller-home","tmpfs",MS_NODEV|MS_NOSUID,"size=192m") == 0,"B3 independent source backing");
    writeGuestFile("/etc/passwd","root:x:0:0:root:/root:/bin/bash\ncaller:x:1000:1000:Caller:/run/caller-home:/bin/bash\n");
    require(chown("/run/caller-home",1000,1000) == 0,"B3 caller home");
    require(QDir().mkpath("/run/caller-home/files"),"B3 cache directory");
    QDirIterator cache("/b3-cache",QDir::Files,QDirIterator::Subdirectories);
    while (cache.hasNext()) {
        const QString path = cache.next(), destination = QStringLiteral("/run/caller-home/files/") + path.mid(10);
        require(QDir().mkpath(QFileInfo(destination).path()) && QFile::copy(path,destination)
            && chown(QFile::encodeName(destination).constData(),1000,1000) == 0,"B3 selected cache resources");
    }
    const auto query = QDBusMessage::createMethodCall(base.serviceOwner,"/org/freedesktop/UDisks2","org.freedesktop.DBus.ObjectManager","GetManagedObjects");
    const QDBusReply<rufus::UDisksObjects> snapshot = QDBusConnection::systemBus().call(query);
    require(snapshot.isValid(),"B3 target metadata"); rawObjects = snapshot.value();
    struct stat partition; require(stat("/dev/sda1",&partition) == 0 && S_ISBLK(partition.st_mode),"B3 seed partition");
    rawPartition = quint64(partition.st_rdev);
    require(QDir().mkpath("/dev/block"),"B3 device aliases");
    for (int number : {1,2}) {
        const QByteArray alias = QByteArray("/dev/block/")+QByteArray::number(major(partition.st_rdev))+':'+QByteArray::number(minor(partition.st_rdev)+number-1);
        const QByteArray node = QByteArray("../sda")+QByteArray::number(number);
        require(symlink(node.constData(),alias.constData()) == 0 || errno == EEXIST,"B3 partition alias");
    }
    for (int number = 0; number < 4; ++number) {
        for (const char* name : {"/run/format-launches","/run/format-exit-held"}) QFile::remove(QString::fromLatin1(name));
        auto job = base; job.volumeId = 0; job.fileSystem = number % 2 ? FS_NTFS : FS_FAT32;
        job.mbr = false; job.syslinux = true; job.uefiNtfs = number % 2; job.clusterSize = 4096; job.label = "B3SOURCE";
        const QString source = QStringLiteral("/run/caller-home/resources-%1.iso").arg(number);
        require(QFile::copy(QStringLiteral("/resources-%1.iso").arg(number),source)
            && chown(QFile::encodeName(source).constData(),1000,1000) == 0,"B3 owned source");
        require(rufus::CapturePrepareSource(source,job) == 0,"B3 source snapshot");
        publishTargetMount(rawObjects,job,{},rawPartition);
        QProcess client; client.setProcessChannelMode(QProcess::ForwardedChannels);
        client.start("/usr/bin/test_native_format_qt_guest",{}); require(client.waitForStarted(10000),"B3 Qt client");
        const QByteArray packet = QJsonDocument(rufus::EncodePrepareRequest(job)).toJson(QJsonDocument::Compact)+'\n';
        require(packet.size() > 3 && client.write(packet) == packet.size(),"B3 typed pipe"); client.closeWriteChannel();
        require(client.waitForFinished(240000) && client.exitCode() == 0,"B3 joined Qt job");
        const bool ntfs = job.fileSystem == FS_NTFS;
        require(mount("/dev/sda1","/mnt/readback",ntfs ? "ntfs" : "vfat",MS_RDONLY|MS_NODEV|MS_NOSUID|MS_NOEXEC,ntfs ? "iocharset=utf8" : "utf8=1") == 0,"B3 independent kernel readback");
        require(readFile("/mnt/readback/EFI/BOOT/BOOTX64.EFI") == readFile("/original.efi"),"B3 embedded EFI bytes");
        require(readFile("/mnt/readback/data/file.txt") == "B3 source-only resolved payload\n","B3 regular payload");
        if (ntfs) {
            require(readFile("/mnt/readback/notes-link") == readFile("/mnt/readback/data/file.txt")
                && readFile("/mnt/readback/dir-link/file.txt") == readFile("/mnt/readback/data/file.txt"),"B3 resolved file/directory links");
        } else {
            require(readFile("/mnt/readback/notes-link") == "data/file.txt","B3 FAT literal link");
            require(readFile("/mnt/readback/syslinux/syslnx64.cfg").contains("CONFIG /isolinux/syslnx64.cfg"),"B3 Syslinux link redirect");
        }
        if (number >= 2) {
            require(readFile("/mnt/readback/isolinux/menu.c32") == readFile("/menu.c32")
                && readFile("/mnt/readback/isolinux/vesamenu.c32") == readFile("/vesamenu.c32"),"B3 obsolete COM32 replacements");
        }
        const QByteArray manifest = readFile("/mnt/readback/md5sum.txt");
        const QByteArray linkBytes = ntfs ? readFile("/mnt/readback/data/file.txt") : QByteArray("data/file.txt");
        require(manifest.contains(QCryptographicHash::hash(linkBytes,QCryptographicHash::Md5).toHex()+"  ./notes-link"),"B3 final link checksum");
        require(umount("/mnt/readback") == 0,"B3 readback unmount");
        if (ntfs) {
            // Reuse CP20k's readback-copy normalization for ntfsprogs-plus,
            // which rejects the original Syslinux VBR jump. The USB is read-only.
            const int input = open("/dev/sda1",O_RDONLY|O_CLOEXEC);
            const int copy = open("/run/b3-readback.ntfs",O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC,0600);
            require(input >= 0 && copy >= 0,"B3 metadata copy descriptors");
            quint64 offset = 0; unsigned char buffer[65536];
            for (;;) {
                const ssize_t bytes = read(input,buffer,sizeof(buffer));
                if (bytes < 0 && errno == EINTR) continue;
                require(bytes >= 0,"B3 metadata copy read"); if (!bytes) break;
                bool nonzero = false;
                for (ssize_t index = 0; index < bytes; ++index) nonzero |= buffer[index] != 0;
                ssize_t written = 0;
                while (nonzero && written < bytes) {
                    const ssize_t count = pwrite(copy,buffer+written,size_t(bytes-written),off_t(offset+quint64(written)));
                    if (count < 0 && errno == EINTR) continue;
                    require(count > 0,"B3 sparse metadata copy"); written += count;
                }
                offset += quint64(bytes);
            }
            require(ftruncate(copy,off_t(offset)) == 0 && pwrite(copy,"\xeb\x52\x90",3,0) == 3
                && close(copy) == 0 && close(input) == 0,"B3 copy-only jump normalization");
            QProcess metadata; metadata.start("/usr/bin/ntfsinfo",{"-F","/notes-link","/run/b3-readback.ntfs"});
            require(metadata.waitForFinished(10000),"B3 reparse reader deadline");
            const QByteArray description = metadata.readAllStandardOutput();
            printf("RUFUS_B3_REPARSE_READER exit=%d error=%s\n",metadata.exitCode(),metadata.readAllStandardError().constData());
            printf("RUFUS_B3_REPARSE_INFO %s\n",description.constData());
            require(metadata.exitCode() == 0 && description.toLower().contains("0xa000000c"),"B3 Windows reparse tag on disk");
            require(QFile::remove("/run/b3-readback.ntfs"),"B3 remove metadata copy");
        }
        const int claim = open("/dev/sda",O_RDWR|O_EXCL|O_CLOEXEC); require(claim >= 0 && close(claim) == 0,"B3 released claim");
        printf("RUFUS_ISO_RESOURCES_READBACK JOB=%d BYTES=1 LINKS=1 CLAIM=1\n",number);
    }
    require(umount("/run/caller-home") == 0,"B3 source cleanup");
    puts("RUFUS_ISO_RESOURCES_DONE JOBS=4");
}
