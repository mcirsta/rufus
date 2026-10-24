// SPDX-License-Identifier: GPL-3.0-or-later
// Only included by the existing root/PID1/DMI/private-namespace guarded driver.
quint32 readNtfsOptionFlags(const QString& path)
{
    const int file = open(QFile::encodeName(path).constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    require(file >= 0, "NTFS option attribute descriptor");
    quint32 encoded = 0;
    require(fgetxattr(file, "system.ntfs_attrib", &encoded, sizeof(encoded)) == sizeof(encoded), "independent NTFS DOS flags");
    require(close(file) == 0, "NTFS option attribute close");
    return qFromLittleEndian(encoded);
}

void runNtfsOptionsCases(const rufus::PrepareImageRequest& base)
{
    require(QDir().mkpath(QStringLiteral("/run/caller-home")) && QDir().mkpath(QStringLiteral("/mnt/readback")), "NTFS option directories");
    require(mount("tmpfs", "/run/caller-home", "tmpfs", MS_NODEV | MS_NOSUID, "size=32m") == 0, "independent NTFS source backing");
    writeGuestFile(QStringLiteral("/etc/passwd"), "root:x:0:0:root:/root:/bin/bash\ncaller:x:1000:1000:Caller:/run/caller-home:/bin/bash\n");
    const QString source = QStringLiteral("/run/caller-home/ntfs-options.iso");
    require(QFile::copy(QStringLiteral("/ntfs-options.iso"), source) && chown(QFile::encodeName(source).constData(), 1000, 1000) == 0,
        "owned NTFS ISO source");
    const auto files = QJsonDocument::fromJson(readFile(QStringLiteral("/ntfs-options-files.json"))).object();
    require(files.size() == 6, "independent NTFS file manifest");
    const auto query = QDBusMessage::createMethodCall(base.serviceOwner, QStringLiteral("/org/freedesktop/UDisks2"),
        QStringLiteral("org.freedesktop.DBus.ObjectManager"), QStringLiteral("GetManagedObjects"));
    const QDBusReply<rufus::UDisksObjects> snapshot = QDBusConnection::systemBus().call(query);
    require(snapshot.isValid(), "NTFS option baseline metadata"); rawObjects = snapshot.value();
    struct stat partition;
    require(stat("/dev/sda1", &partition) == 0 && S_ISBLK(partition.st_mode), "NTFS option seed partition");
    rawPartition = quint64(partition.st_rdev);
    require(QDir().mkpath(QStringLiteral("/dev/block")), "NTFS option partition aliases");
    for (int number : {1, 2}) {
        const QByteArray alias = QByteArray("/dev/block/") + QByteArray::number(major(partition.st_rdev)) + ':'
            + QByteArray::number(minor(partition.st_rdev) + number - 1);
        const QByteArray node = QByteArray("../sda") + QByteArray::number(number);
        require(symlink(node.constData(), alias.constData()) == 0 || errno == EEXIST, "NTFS option kernel partition aliases");
    }
    for (int number = 0; number < 3; number++) {
        for (const char* marker : {"/run/format-launches", "/run/format-exit-held"}) QFile::remove(QString::fromLatin1(marker));
        auto job = base; job.volumeId = 0; job.fileSystem = FS_NTFS; job.label = QStringLiteral("OPTIONS");
        job.nonBootable = number != 2; job.ntfsCompression = true; job.fileIndexing = number != 0;
        job.clusterSize = number == 1 ? 8192 : 4096;
        if (!job.nonBootable) require(rufus::CapturePrepareSource(source, job) == 0, "NTFS option source snapshot");
        publishTargetMount(rawObjects, job, {}, rawPartition);
        QProcess client; client.setProcessChannelMode(QProcess::ForwardedChannels);
        client.start(QStringLiteral("/usr/bin/test_native_format_qt_guest"), {});
        require(client.waitForStarted(10000), "NTFS option ordinary Qt client");
        const QByteArray packet = QJsonDocument(rufus::EncodePrepareRequest(job)).toJson(QJsonDocument::Compact) + '\n';
        require(packet.size() > 3 && packet.size() < rufus::PrepareMessageBytes && client.write(packet) == packet.size(), "NTFS option identity pipe");
        client.closeWriteChannel();
        require(client.waitForFinished(180000) && client.exitCode() == 0, "NTFS option ordinary Qt joined");
        require(mount("/dev/sda1", "/mnt/readback", "ntfs", MS_RDONLY | MS_NODEV | MS_NOSUID | MS_NOEXEC, "iocharset=utf8") == 0,
            "independent NTFS kernel readback");
        const quint32 rootFlags = readNtfsOptionFlags(QStringLiteral("/mnt/readback"));
        require(bool(rootFlags & 0x800) == (number != 1), "effective NTFS root compression and large-cluster fallback");
        require(bool(rootFlags & 0x2000) == !job.fileIndexing, "NTFS root indexing preference");
        if (!job.nonBootable) {
            for (auto entry = files.begin(); entry != files.end(); ++entry) {
                const QString path = QStringLiteral("/mnt/readback/") + entry.key();
                require(QCryptographicHash::hash(readFile(path), QCryptographicHash::Sha256).toHex() == entry.value().toString().toLatin1(),
                    "independent compressed logical payload bytes");
            }
            for (const char* name : {"bootmgr", "bootmgr.efi", "efi/boot/bootx64.efi"})
                require(!(readNtfsOptionFlags(QStringLiteral("/mnt/readback/") + QString::fromLatin1(name)) & 0x800), "original uncompressed boot exceptions");
            require(readNtfsOptionFlags(QStringLiteral("/mnt/readback/payload.bin")) & 0x800, "ordinary payload compressed");
            struct stat payload;
            require(stat("/mnt/readback/payload.bin", &payload) == 0 && payload.st_size == 1024 * 1024
                && payload.st_blocks * 512 < payload.st_size / 2, "actual nonresident compressed allocation");
        }
        require(umount("/mnt/readback") == 0, "NTFS option readback unmount");
        const int claim = open("/dev/sda", O_RDWR | O_EXCL | O_CLOEXEC);
        require(claim >= 0 && close(claim) == 0, "NTFS option helper claim released");
        printf("RUFUS_NTFS_OPTIONS_READBACK JOB=%d FLAGS=1 BYTES=1 CLAIM=1\n", number);
    }
    // An unavailable required compression provider must fail before writes;
    // never retry a failed compressed write through a different driver.
    const QByteArray provider = readFile(QStringLiteral("/ntfs-provider-path")).trimmed();
    require(provider.startsWith("/usr/bin/") && !provider.contains(".."), "fixed compiled provider fixture");
    const QByteArray held = provider + ".held";
    require(rename(provider.constData(), held.constData()) == 0, "hold guest-only compression provider");
    auto refusal = base; refusal.volumeId = 0; refusal.fileSystem = FS_NTFS; refusal.clusterSize = 4096;
    refusal.ntfsCompression = true; refusal.fileIndexing = true; refusal.label = QStringLiteral("OPTIONS");
    require(rufus::CapturePrepareSource(source, refusal) == 0, "provider refusal source snapshot");
    publishTargetMount(rawObjects, refusal, {}, rawPartition);
    for (const char* marker : {"/run/format-launches", "/run/format-exit-held"}) QFile::remove(QString::fromLatin1(marker));
    const QByteArray before = diskDigest();
    rufus::PrepareImageProcess helper(QStringLiteral("/usr/bin/rufus-prepare-helper"));
    QSignalSpy done(&helper, &rufus::PrepareImageProcess::finished);
    require(helper.start(refusal), "missing provider helper start");
    QElapsedTimer deadline; deadline.start();
    while (done.isEmpty()) { QCoreApplication::processEvents(); require(deadline.elapsed() < 60000, "provider refusal deadline"); usleep(1000); }
    require(helper.outcome().error == ENOENT && !helper.outcome().started && diskDigest() == before, "missing provider refuses before target writes");
    require(readFile(QStringLiteral("/run/format-launches")) == "launch\n", "one provider-refusal authorization");
    require(rename(held.constData(), provider.constData()) == 0, "restore guest-only provider");
    require(umount("/run/caller-home") == 0, "NTFS option source cleanup");
    puts("RUFUS_NTFS_OPTIONS_DONE JOBS=3 PREWRITE_PROVIDER=1");
}
