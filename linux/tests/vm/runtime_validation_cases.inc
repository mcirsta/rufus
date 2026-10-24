// SPDX-License-Identifier: GPL-3.0-or-later
// Included only in the guarded disposable Linux guest driver.
void checkRuntimeManifest(const QString& name, bool knownBad)
{
    const QByteArray list = readFile(QStringLiteral("/mnt/readback/") + name);
    require(!list.isEmpty(), "runtime manifest published");
    int entries = 0, bad = 0;
    bool original = false, config = false, unicode = false;
    for (const QByteArray& line : list.split('\n')) {
        if (line.isEmpty() || line.startsWith('#')) continue;
        require(line.size() > 34 && line[32] == ' ', "independent manifest row");
        QByteArray path = line.mid(34);
        if (path.startsWith("./")) path.remove(0, 2);
        require(!path.contains("..") && !path.startsWith('/'), "relative manifest path");
        require(path.toLower() != "efi/boot/bootx64.efi", "validator excluded from its own list");
        const QByteArray bytes = readFile(QStringLiteral("/mnt/readback/") + QString::fromUtf8(path));
        const QByteArray actual = QCryptographicHash::hash(bytes, QCryptographicHash::Md5).toHex();
        const bool preservedBad = knownBad && path == "known-bad.txt";
        if (preservedBad) {
            require(line.left(32) == QByteArray(32, '0') && actual != line.left(32), "untouched bad source digest preserved"); ++bad;
        } else require(actual == line.left(32).toLower(), "independent target MD5 matches");
        original |= path.toLower() == "efi/boot/bootx64_original.efi";
        config |= path == "isolinux/isolinux.cfg";
        unicode |= path == QString::fromUtf8("payload/安装.txt").toUtf8();
        ++entries;
    }
    require(entries >= 4 && original && config && unicode && bad == int(knownBad), "complete runtime manifest inventory");
}

void runRuntimeValidationCases(const rufus::PrepareImageRequest& base)
{
    require(QDir().mkpath(QStringLiteral("/run/caller-home")) && QDir().mkpath(QStringLiteral("/mnt/readback")), "runtime directories");
    require(mount("tmpfs", "/run/caller-home", "tmpfs", 0, "size=32m") == 0, "independent runtime source backing");
    writeGuestFile(QStringLiteral("/etc/passwd"), "root:x:0:0:root:/root:/bin/bash\ncaller:x:1000:1000:Caller:/run/caller-home:/bin/bash\n");
    const QDBusMessage query = QDBusMessage::createMethodCall(base.serviceOwner,
        QStringLiteral("/org/freedesktop/UDisks2"), QStringLiteral("org.freedesktop.DBus.ObjectManager"), QStringLiteral("GetManagedObjects"));
    const QDBusReply<rufus::UDisksObjects> snapshot = QDBusConnection::systemBus().call(query);
    require(snapshot.isValid(), "runtime baseline metadata"); rawObjects = snapshot.value();
    struct stat partition;
    require(stat("/dev/sda1", &partition) == 0 && S_ISBLK(partition.st_mode), "runtime seed partition");
    rawPartition = quint64(partition.st_rdev);
    require(QDir().mkpath(QStringLiteral("/dev/block")), "runtime partition aliases");
    for (int number : {1, 2}) {
        const QByteArray alias = QByteArray("/dev/block/") + QByteArray::number(major(partition.st_rdev)) + ':'
            + QByteArray::number(minor(partition.st_rdev) + number - 1);
        const QByteArray node = QByteArray("../sda") + QByteArray::number(number);
        require(symlink(node.constData(), alias.constData()) == 0 || errno == EEXIST, "runtime kernel partition aliases");
    }
    for (const char* name : {"listed.iso", "uppercase.iso", "plain.iso", "conflict.iso"}) {
        const QString destination = QStringLiteral("/run/caller-home/") + QString::fromLatin1(name);
        require(QFile::copy(QLatin1Char('/') + QString::fromLatin1(name), destination)
            && chown(QFile::encodeName(destination).constData(), 1000, 1000) == 0, "runtime source ownership");
    }
    struct RuntimeCase { int filesystem; const char* iso; };
    const RuntimeCase cases[] = {{FS_FAT16, "listed.iso"}, {FS_FAT32, "uppercase.iso"}, {FS_NTFS, "plain.iso"}};
    for (const RuntimeCase& test : cases) {
        for (const char* marker : {"/run/format-launches", "/run/format-exit-held"}) QFile::remove(QString::fromLatin1(marker));
        rufus::PrepareImageRequest request;
        request.path = base.path; request.device = base.device; request.size = base.size;
        request.diskSequence = base.diskSequence; request.objectPath = base.objectPath;
        request.serviceOwner = base.serviceOwner; request.identity = base.identity;
        request.isDevice = request.runtimeValidation = true; request.fileSystem = test.filesystem;
        request.clusterSize = test.filesystem == FS_FAT16 ? 8192 : 4096; request.label = QStringLiteral("RUNTIME");
        require(rufus::CapturePrepareSource(QStringLiteral("/run/caller-home/") + QString::fromLatin1(test.iso), request) == 0, "runtime source snapshot");
        publishTargetMount(rawObjects, request, {}, rawPartition);
        QProcess client; client.setProcessChannelMode(QProcess::ForwardedChannels);
        client.start(QStringLiteral("/usr/bin/test_native_format_qt_guest"), {});
        require(client.waitForStarted(10000), "runtime ordinary Qt client");
        const QByteArray packet = QJsonDocument(rufus::EncodePrepareRequest(request)).toJson(QJsonDocument::Compact) + '\n';
        require(packet.size() > 3 && packet.size() < rufus::PrepareMessageBytes && client.write(packet) == packet.size(), "runtime identity pipe");
        client.closeWriteChannel();
        require(client.waitForFinished(180000) && client.exitCode() == 0, "runtime ordinary Qt joined");
        const char* filesystem = test.filesystem == FS_NTFS ? "ntfs" : "vfat";
        require(mount("/dev/sda1", "/mnt/readback", filesystem, MS_RDONLY | MS_NODEV | MS_NOSUID | MS_NOEXEC,
            test.filesystem == FS_NTFS ? "iocharset=utf8" : "utf8=1") == 0, "runtime independent kernel readback");
        const QByteArray validator = readFile(QStringLiteral("/validator.efi"));
        require(!validator.isEmpty(), "independent validator reference");
        require(readFile(QStringLiteral("/mnt/readback/efi/boot/bootx64.efi")) == validator, "validator bytes unchanged");
        require(readFile(QStringLiteral("/mnt/readback/efi/boot/bootx64_original.efi")) == readFile(QStringLiteral("/original.efi")), "preserved original EFI bytes");
        const QByteArray cfg = readFile(QStringLiteral("/mnt/readback/isolinux/isolinux.cfg"));
        require(cfg.contains("RUNTIME") && !cfg.contains("LABEL=SOURCE"), "config relabeled before hash repair");
        const bool knownBad = test.filesystem == FS_FAT32;
        checkRuntimeManifest(QStringLiteral("md5sum.txt"), false);
        if (knownBad) checkRuntimeManifest(QStringLiteral("MD5SUMS"), true);
        require(umount("/mnt/readback") == 0, "runtime independent unmount");
        const int claim = open("/dev/sda", O_RDWR | O_EXCL | O_CLOEXEC);
        require(claim >= 0 && close(claim) == 0, "runtime helper claim released");
        printf("RUFUS_RUNTIME_READBACK FS=%d HASHES=1 ORIGINAL=1 RESOURCE=1 CLAIM=1\n", test.filesystem);
    }
    rufus::PrepareImageRequest conflict = base;
    conflict.syslinux = false; conflict.runtimeValidation = true; conflict.mbr = false;
    conflict.fileSystem = FS_FAT32; conflict.clusterSize = 4096; conflict.label = QStringLiteral("RUNTIME");
    require(rufus::CapturePrepareSource(QStringLiteral("/run/caller-home/conflict.iso"), conflict) == 0, "conflict source snapshot");
    publishTargetMount(rawObjects, conflict, {}, rawPartition);
    for (const char* marker : {"/run/format-launches", "/run/format-exit-held"}) QFile::remove(QString::fromLatin1(marker));
    const QByteArray before = diskDigest();
    rufus::PrepareImageProcess helper(QStringLiteral("/usr/bin/rufus-prepare-helper"));
    QSignalSpy done(&helper, &rufus::PrepareImageProcess::finished);
    require(helper.start(conflict), "conflict helper start");
    QElapsedTimer deadline; deadline.start();
    while (done.isEmpty()) { QCoreApplication::processEvents(); require(deadline.elapsed() < 60000, "conflict deadline"); usleep(1000); }
    require(helper.outcome().error == EEXIST && !helper.outcome().started && diskDigest() == before, "runtime backup conflict refuses before disk writes");
    require(readFile(QStringLiteral("/run/format-launches")) == "launch\n", "one conflict authorization");
    require(umount("/run/caller-home") == 0, "runtime source cleanup");
    puts("RUFUS_RUNTIME_DONE JOBS=3 PREWRITE_CONFLICT=1 AUTH=1 JOINED=1 READBACK=1");
}
