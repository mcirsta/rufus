// SPDX-License-Identifier: GPL-3.0-or-later
// Included only in the guarded disposable Linux guest driver.
void runLegacyBootCases(const rufus::PrepareImageRequest& base)
{
    require(QDir().mkpath(QStringLiteral("/run/caller-home")) && QDir().mkpath(QStringLiteral("/mnt/readback")),"legacy directories");
    require(mount("tmpfs","/run/caller-home","tmpfs",MS_NODEV|MS_NOSUID,"size=16m")==0,"legacy caller-home tmpfs");
    writeGuestFile(QStringLiteral("/etc/passwd"),"root:x:0:0:root:/root:/bin/bash\ncaller:x:1000:1000:Caller:/run/caller-home:/bin/bash\n");
    const QDBusMessage query = QDBusMessage::createMethodCall(base.serviceOwner,
        QStringLiteral("/org/freedesktop/UDisks2"),QStringLiteral("org.freedesktop.DBus.ObjectManager"),QStringLiteral("GetManagedObjects"));
    const QDBusReply<rufus::UDisksObjects> snapshot = QDBusConnection::systemBus().call(query);
    require(snapshot.isValid(),"legacy baseline metadata"); rawObjects = snapshot.value();
    struct stat partition;
    require(stat("/dev/sda1",&partition)==0 && S_ISBLK(partition.st_mode),"legacy seed partition");
    rawPartition = quint64(partition.st_rdev);
    require(QDir().mkpath(QStringLiteral("/dev/block")),"legacy partition alias directory");
    const QByteArray alias = QByteArray("/dev/block/")+QByteArray::number(major(partition.st_rdev))+':'+QByteArray::number(minor(partition.st_rdev));
    require(symlink("../sda1",alias.constData())==0 || errno==EEXIST,"legacy kernel partition alias");
    require(chown("/run/caller-home",1000,1000)==0 && mkdir("/run/caller-home/files",0700)==0,"legacy cache directory");
    require(chown("/run/caller-home/files",1000,1000)==0,"legacy cache ownership");
    const QString cache = QStringLiteral("/run/caller-home/files/diskcopy.dll");
    require(QFile::copy(QStringLiteral("/diskcopy.dll"),cache) && chown(QFile::encodeName(cache).constData(),1000,1000)==0,"legacy cache input");
    const QByteArray original = readFile(cache);
    const auto manifest = QJsonDocument::fromJson(readFile(QStringLiteral("/legacy-files.json"))).object();
    require(manifest.size()==13,"independent DOS manifest");
    struct LegacyCase { bool msdos; int filesystem; };
    const LegacyCase cases[] = {{true,FS_FAT16},{true,FS_FAT32},{false,FS_FAT16},{false,FS_FAT32}};
    for (const auto& test : cases) {
        for (const char* marker : {"/run/format-launches","/run/format-exit-held"}) QFile::remove(QString::fromLatin1(marker));
        rufus::PrepareImageRequest job = base;
        job.msDos = test.msdos; job.reactOs = !test.msdos; job.mbr = true; job.fileSystem = test.filesystem;
        job.clusterSize = test.filesystem==FS_FAT16 ? 8192 : 4096; job.label = QStringLiteral("LEGACY");
        if (test.msdos) {
            job.dosImagePath = cache;
            job.dosKeyboard = test.filesystem==FS_FAT16 ? QStringLiteral("us") : QStringLiteral("ro");
            job.dosCodepage = test.filesystem==FS_FAT16 ? 437 : 852;
        }
        publishTargetMount(rawObjects,job,{},rawPartition);
        QProcess client; client.setProcessChannelMode(QProcess::ForwardedChannels);
        client.start(QStringLiteral("/usr/bin/test_native_format_qt_guest"),{});
        require(client.waitForStarted(10000),"legacy ordinary Qt client");
        const QByteArray packet = QJsonDocument(rufus::EncodePrepareRequest(job)).toJson(QJsonDocument::Compact)+'\n';
        require(packet.size()>3 && client.write(packet)==packet.size(),"legacy identity pipe"); client.closeWriteChannel();
        require(client.waitForFinished(180000) && client.exitCode()==0,"legacy ordinary Qt joined");
        require(mount("/dev/sda1","/mnt/readback","vfat",MS_RDONLY|MS_NODEV|MS_NOSUID|MS_NOEXEC,"utf8=1")==0,"legacy independent mount");
        if (test.msdos) {
            for (auto entry = manifest.begin(); entry != manifest.end(); ++entry) {
                const QString path = QStringLiteral("/mnt/readback/")+entry.key();
                const QByteArray bytes = readFile(path); const auto expected = entry.value().toObject();
                require(bytes.size()==expected.value(QStringLiteral("size")).toInt(),"DOS independent file size");
                require(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex()==expected.value(QStringLiteral("sha256")).toString().toLatin1(),"DOS independent file bytes/patches");
                const int file = open(QFile::encodeName(path).constData(),O_RDONLY|O_CLOEXEC); require(file>=0,"DOS attribute descriptor");
                uint32_t attributes = 0;
                require(ioctl(file,FAT_IOCTL_GET_ATTRIBUTES,&attributes)==0 && attributes==uint32_t(expected.value(QStringLiteral("attributes")).toInt()),"DOS original FAT attributes");
                require(close(file)==0,"DOS attribute descriptor close");
            }
            const QByteArray startup = readFile(QStringLiteral("/mnt/readback/AUTOEXEC.BAT"));
            const QByteArray config = readFile(QStringLiteral("/mnt/readback/CONFIG.SYS"));
            const bool us = job.dosCodepage==437 && startup.contains("US-English") && config.isEmpty();
            const bool romanian = job.dosCodepage==852 && startup.contains("keyb ro") && startup.contains("852")
                && config.contains("MENUITEM=1") && config.contains("US-English");
            require(us || romanian,"explicit MS-DOS startup and fallback");
        } else {
            const QDir root(QStringLiteral("/mnt/readback"));
            require(root.entryList(QDir::AllEntries|QDir::NoDotAndDotDot|QDir::Hidden|QDir::System).isEmpty(),"ReactOS standalone boot records only");
        }
        require(umount("/mnt/readback")==0,"legacy independent unmount");
        require(readFile(cache)==original,"Microsoft source retained unchanged");
        const int claim = open("/dev/sda",O_RDWR|O_EXCL|O_CLOEXEC);
        require(claim>=0 && close(claim)==0,"legacy claim released");
        printf("RUFUS_LEGACY_READBACK DOS=%d FS=%d FILES=%d ATTRS=1 CLAIM=1\n",int(test.msdos),test.filesystem,test.msdos?13:0);
    }
    const QString invalid = QStringLiteral("/run/caller-home/changed-diskcopy.dll");
    QByteArray changed = original; changed[10] ^= 1; writeGuestFile(invalid,changed);
    require(chown(QFile::encodeName(invalid).constData(),1000,1000)==0,"changed source ownership");
    auto refusal = base; refusal.msDos = refusal.mbr = true; refusal.fileSystem = FS_FAT32;
    refusal.clusterSize = 4096; refusal.dosImagePath = invalid;
    refusal.dosKeyboard = QStringLiteral("us"); refusal.dosCodepage = 437;
    publishTargetMount(rawObjects,refusal,{},rawPartition);
    const QByteArray before = diskDigest();
    rufus::PrepareImageProcess helper(QStringLiteral("/usr/bin/rufus-prepare-helper"));
    QSignalSpy done(&helper,&rufus::PrepareImageProcess::finished);
    require(helper.start(refusal),"changed source helper start");
    QElapsedTimer deadline; deadline.start();
    while (done.isEmpty()) { QCoreApplication::processEvents(); require(deadline.elapsed()<60000,"refusal deadline"); usleep(1000); }
    require(helper.outcome().error==EBADMSG && !helper.outcome().started && diskDigest()==before,"changed DOS resource refuses before target writes");
    require(umount("/run/caller-home")==0,"legacy caller-home cleanup");
    puts("RUFUS_LEGACY_DONE JOBS=4 PREWRITE_REFUSAL=1 AUTH=1 JOINED=1 READBACK=1");
}
