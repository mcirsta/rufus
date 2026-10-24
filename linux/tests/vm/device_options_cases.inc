// SPDX-License-Identifier: GPL-3.0-or-later
// Included only by the existing PID1-child/DMI/private-namespace guarded driver.
void runDeviceOptionsCases(const rufus::PrepareImageRequest& base)
{
    require(QDir().mkpath(QStringLiteral("/run/caller-home")) && QDir().mkpath(QStringLiteral("/mnt/readback")), "device class directories");
    require(mount("tmpfs", "/run/caller-home", "tmpfs", MS_NODEV | MS_NOSUID, "size=16m") == 0, "known memory-backed caller home");
    writeGuestFile(QStringLiteral("/etc/passwd"), "root:x:0:0:root:/root:/bin/bash\ncaller:x:1000:1000:Caller:/run/caller-home:/bin/bash\n");
    const QString blockInterface = QStringLiteral("org.freedesktop.UDisks2.Block");
    const QString driveInterface = QStringLiteral("org.freedesktop.UDisks2.Drive");
    const QDBusReply<rufus::UDisksObjects> snapshot = QDBusConnection::systemBus().call(QDBusMessage::createMethodCall(
        base.serviceOwner, QStringLiteral("/org/freedesktop/UDisks2"), QStringLiteral("org.freedesktop.DBus.ObjectManager"), QStringLiteral("GetManagedObjects")));
    require(snapshot.isValid(), "device class baseline");
    auto objects = snapshot.value();
    const int kinds[] = {2, 4, 8};
    for (int index = 0; index < 3; index++) {
        QString node;
        for (const QString& name : QDir(QStringLiteral("/sys/block")).entryList({QStringLiteral("sd*")}, QDir::Dirs)) {
            struct stat candidate; rufus::NativeDiskFacts facts;
            const QString path = QStringLiteral("/dev/") + name;
            if (stat(QFile::encodeName(path).constData(), &candidate) == 0
                && rufus::ReadNativeDiskFacts(quint64(candidate.st_rdev), 64 * 1024 * 1024, facts) == 0
                && int(facts.deviceClass) == kinds[index]) {
                require(node.isEmpty(), "unambiguous native class"); node = path;
            }
        }
        require(!node.isEmpty(), "actual native class device");
        const QString objectPath = QStringLiteral("/org/freedesktop/UDisks2/block_devices/") + node.mid(5);
        const QDBusObjectPath object(objectPath), drive(objectPath + QStringLiteral("_drive"));
        struct stat info;
        require(stat(QFile::encodeName(node).constData(), &info) == 0 && S_ISBLK(info.st_mode), "native class disk node");
        objects[object][blockInterface] = {{"Drive", QVariant::fromValue(drive)}, {"Device", QFile::encodeName(node) + '\0'},
            {"Size", quint64(64 * 1024 * 1024)}, {"DeviceNumber", quint64(info.st_rdev)},
            {"ReadOnly", false}, {"HintSystem", true}, {"HintIgnore", false}};
        objects[drive][driveInterface] = {{"ConnectionBus", "scsi"}, {"Removable", index == 2},
            {"MediaRemovable", index == 2}, {"MediaAvailable", true}, {"Optical", false}};
        require(QDir().mkpath(QStringLiteral("/dev/block")), "native class device aliases");
        for (int part : {1, 2}) {
            const QByteArray alias = QByteArray("/dev/block/") + QByteArray::number(major(info.st_rdev)) + ':'
                + QByteArray::number(minor(info.st_rdev) + part);
            const QByteArray destination = QByteArray("../") + QFile::encodeName(node.mid(5)) + QByteArray::number(part);
            require(symlink(destination.constData(), alias.constData()) == 0 || errno == EEXIST, "native class partition aliases without udev");
        }
        rufus::Device device;
        require(rufus::DescribeUDisksDevice(objects, objectPath, device, true, rufus::DeviceAccess::Write, 31), "native class discovery");
        require(int(device.deviceClass) == kinds[index] && device.requiresNativeProtection, "verified native disk class");
        auto publish = QDBusMessage::createMethodCall(QStringLiteral("org.rufus.TestUDisks"), QStringLiteral("/org/freedesktop/UDisks2"),
            QStringLiteral("org.rufus.TestUDisks"), QStringLiteral("SetObjects"));
        publish.setArguments({QVariant::fromValue(objects)});
        require(QDBusConnection::systemBus().call(publish).type() == QDBusMessage::ReplyMessage, "native class snapshot publication");
        auto job = base; job.path = node; job.objectPath = objectPath; job.device = device.number;
        job.size = device.size; job.identity = device.identity; job.deviceClass = device.deviceClass;
        job.diskSequence = rufus::ReadDiskSequence(device.number); job.nonBootable = true; job.volumeId = 0;
        job.label = QStringLiteral("CLASS%1").arg(kinds[index]); job.clusterSize = 4096; job.fileSystem = FS_FAT32;
        job.allowFixedUsb = index == 2;
        rufus::UDisksObjects checked; rufus::Device current;
        auto wrong = job; wrong.deviceClass = rufus::DeviceClass::Usb;
        require(rufus::ReadPrepareDeviceSnapshot(QDBusConnection::systemBus(), wrong, checked, current) != 0, "wrong class refused before writes");
        for (const char* marker : {"/run/format-launches", "/run/format-exit-held"}) QFile::remove(QString::fromLatin1(marker));
        QProcess client; client.setProcessChannelMode(QProcess::ForwardedChannels);
        client.start(QStringLiteral("/usr/bin/test_native_format_qt_guest"), {});
        require(client.waitForStarted(10000), "device class Qt client");
        const QByteArray packet = QJsonDocument(rufus::EncodePrepareRequest(job)).toJson(QJsonDocument::Compact) + '\n';
        require(packet.size() > 3 && client.write(packet) == packet.size(), "device class frozen request");
        client.closeWriteChannel();
        require(client.waitForFinished(120000) && client.exitCode() == 0, "native class Qt joined");
        const QByteArray partition = QFile::encodeName(node + QLatin1Char('1'));
        require(mount(partition.constData(), "/mnt/readback", "vfat", MS_RDONLY | MS_NODEV | MS_NOSUID | MS_NOEXEC, "utf8=1") == 0,
            "independent kernel FAT32 readback");
        struct statfs filesystem;
        require(statfs("/mnt/readback", &filesystem) == 0 && filesystem.f_type == MSDOS_SUPER_MAGIC, "native class FAT filesystem");
        require(umount("/mnt/readback") == 0, "native class readback cleanup");
        require(rufus::ReadSaveDeviceSnapshot(QDBusConnection::systemBus(), job, checked, current) == 0, "fresh native class save source");
        const int claim = open(QFile::encodeName(node).constData(), O_RDONLY | O_EXCL | O_CLOEXEC);
        unsigned sector = 0;
        require(claim >= 0 && rufus::CheckImageSaveDevice(claim, job.device, job.size, job.diskSequence, sector) == 0
            && sector == 512 && close(claim) == 0, "native class read claim and capture geometry");
        if (index == 0) {
            require(mount(partition.constData(), "/run/caller-home", "vfat", MS_RDONLY, "utf8=1") == 0, "native virtual home backing");
            QFile disk(node); require(disk.open(QIODevice::ReadOnly), "home refusal initial snapshot");
            QCryptographicHash before(QCryptographicHash::Sha256); require(before.addData(&disk), "home refusal initial hash"); disk.close();
            rufus::PrepareImageProcess helper(QStringLiteral("/usr/bin/rufus-prepare-helper"));
            QSignalSpy done(&helper, &rufus::PrepareImageProcess::finished);
            require(helper.start(job), "virtual home refusal helper");
            QElapsedTimer timer; timer.start();
            while (done.isEmpty()) { QCoreApplication::processEvents(); require(timer.elapsed() < 30000, "virtual home refusal deadline"); usleep(1000); }
            require(helper.outcome().error != 0 && !helper.outcome().started, "native home protection before writes with empty UDisks mounts");
            require(disk.open(QIODevice::ReadOnly), "home refusal final snapshot");
            QCryptographicHash after(QCryptographicHash::Sha256); require(after.addData(&disk) && after.result() == before.result(), "home disk unchanged"); disk.close();
            require(umount("/run/caller-home") == 0, "virtual home refusal cleanup");
            puts("RUFUS_DEVICE_OPTIONS_HOME_PROTECTED=1");
        }
        printf("RUFUS_DEVICE_OPTIONS_READBACK CLASS=%d FILESYSTEM=1 CLAIM=1 READ=1\n", kinds[index]);
    }
    require(umount("/run/caller-home") == 0, "caller home fixture cleanup");
    puts("RUFUS_DEVICE_OPTIONS_DONE JOBS=3");
}
