// SPDX-License-Identifier: GPL-3.0-or-later
// Included only by the guarded disposable block-device driver.
void resetArchiveMarkers()
{
    for (const char* name : {"/run/format-launches", "/run/format-exit-held", "/run/format-exit-release"}) QFile::remove(QString::fromLatin1(name));
}

void runArchiveHelper(const rufus::PrepareImageRequest& request, int expected)
{
    resetArchiveMarkers();
    rufus::PrepareImageProcess process(QStringLiteral("/usr/bin/rufus-prepare-helper"));
    QSignalSpy done(&process, &rufus::PrepareImageProcess::finished);
    QSignalSpy logs(&process, &rufus::PrepareImageProcess::logMessage);
    require(process.start(request), "start archive helper");
    QElapsedTimer timer; timer.start();
    while (done.isEmpty()) { QCoreApplication::processEvents(); require(timer.elapsed() < 240000, "archive helper deadline"); usleep(1000); }
    if (process.outcome().error != expected) {
        for (const auto& entry : logs) fprintf(stderr, "ARCHIVE_LOG %s\n", qPrintable(entry.first().toString()));
        qFatal("Archive helper: expected %d, got %d: %s", expected, process.outcome().error, qPrintable(process.problem()));
    }
    require(readFile(QStringLiteral("/run/format-launches")) == "launch\n", "one archive authorization");
    if (expected == 0) require(process.state() == rufus::PrepareImageProcess::State::Complete && process.outcome().archiveDone,
        "archive helper joined success");
    else require(!process.outcome().started && !process.outcome().archiveDone, "archive refused before writes");
}

void runArchiveCases(rufus::PrepareImageRequest request)
{
    require(QDir().mkpath(QStringLiteral("/run/caller-home")) && QDir().mkpath(QStringLiteral("/mnt/target")), "archive directories");
    require(mount("tmpfs", "/run/caller-home", "tmpfs", 0, "size=32m") == 0, "separate caller home");
    for (const char* name : {"supplement.zip", "bad-supplement.zip"}) {
        const QString destination = QStringLiteral("/run/caller-home/") + QString::fromLatin1(name);
        require(QFile::copy(QLatin1Char('/') + QString::fromLatin1(name), destination)
            && chown(QFile::encodeName(destination).constData(), 1000, 1000) == 0, "independent archive source");
    }
    writeGuestFile(QStringLiteral("/etc/passwd"), "root:x:0:0:root:/root:/bin/bash\ncaller:x:1000:1000:Caller:/run/caller-home:/bin/bash\n");
    {
        rufus::PrepareDeviceTarget seed(request, Table::Mbr);
        require(seed.open(1000) == 0, "archive seed claim"); prepareDisk(seed, request, Table::Mbr);
    }
    const QDBusMessage message = QDBusMessage::createMethodCall(request.serviceOwner,
        QStringLiteral("/org/freedesktop/UDisks2"), QStringLiteral("org.freedesktop.DBus.ObjectManager"), QStringLiteral("GetManagedObjects"));
    const QDBusReply<rufus::UDisksObjects> snapshot = QDBusConnection::systemBus().call(message);
    require(snapshot.isValid(), "archive baseline metadata"); rawObjects = snapshot.value();
    struct stat partition;
    require(stat("/dev/sda1", &partition) == 0, "archive partition node"); rawPartition = quint64(partition.st_rdev);
    auto format = request;
    format.syslinux = false; format.nonBootable = true; format.mbr = true; format.extendedLabel = false;
    format.fileSystem = FS_FAT32; format.label = QStringLiteral("EXTRA"); format.clusterSize = 512;
    auto bad = format;
    require(rufus::CapturePrepareArchive(QStringLiteral("/run/caller-home/bad-supplement.zip"), bad.archive) == 0, "bad archive snapshot");
    publishTargetMount(rawObjects, bad, {}, rawPartition);
    QByteArray before = diskDigest(); runArchiveHelper(bad, EBADMSG);
    require(diskDigest() == before, "bad archive leaves complete disk unchanged");
    require(rufus::CapturePrepareArchive(QStringLiteral("/run/caller-home/supplement.zip"), format.archive) == 0, "archive selection");
    publishTargetMount(rawObjects, format, {}, rawPartition); resetArchiveMarkers();
    QProcess client; client.setProcessChannelMode(QProcess::ForwardedChannels);
    client.start(QStringLiteral("/usr/bin/test_archive_qt_guest"), {});
    require(client.waitForStarted(10000), "archive Qt client start");
    const QByteArray packet = QJsonDocument(rufus::EncodePrepareRequest(format)).toJson(QJsonDocument::Compact) + '\n';
    require(client.write(packet) == packet.size(), "archive identity pipe"); client.closeWriteChannel();
    require(client.waitForFinished(240000) && client.exitCode() == 0, "ordinary archive client completed");
    require(mount("/dev/sda1", "/mnt/target", "vfat", MS_NODEV | MS_NOSUID | MS_NOEXEC, "uid=1000,gid=1000,utf8=1") == 0, "archive independent readback mount");
    require(readFile(QStringLiteral("/mnt/target/keep.txt")) == "ZIP REPLACEMENT\n"
        && readFile(QString::fromUtf8("/mnt/target/drivers/fișiere.txt")) == "Unicode supplement\n", "archive independent file bytes");
    require(QFile::copy(QStringLiteral("/supplement.zip"), QStringLiteral("/mnt/target/source.zip")), "source-on-target fixture");
    const int mounted = open("/mnt/target", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    require(mounted >= 0 && syncfs(mounted) == 0 && close(mounted) == 0, "archive fixture sync");
    auto protectedSource = format;
    require(rufus::CapturePrepareArchive(QStringLiteral("/mnt/target/source.zip"), protectedSource.archive) == 0, "source-on-target snapshot");
    publishTargetMount(rawObjects, protectedSource, QByteArray("/mnt/target"), rawPartition);
    before = diskDigest(); runArchiveHelper(protectedSource, EBUSY);
    require(diskDigest() == before && QFile::exists(QStringLiteral("/mnt/target/source.zip")), "source-on-target unchanged and still mounted");
    require(umount("/mnt/target") == 0, "archive independent unmount");
    publishTargetMount(rawObjects, request, {}, rawPartition);
    request.syslinux = true; request.nonBootable = false; request.extendedLabel = false;
    require(QFile::copy(QStringLiteral("/unknown.iso"), QStringLiteral("/run/caller-home/source.iso"))
        && chown("/run/caller-home/source.iso", 1000, 1000) == 0
        && rufus::CapturePrepareSource(QStringLiteral("/run/caller-home/source.iso"), request) == 0,
        "archive bootable ISO source snapshot");
    require(rufus::CapturePrepareArchive(QStringLiteral("/run/caller-home/supplement.zip"), request.archive) == 0, "archive bootable supplement snapshot");
    runArchiveHelper(request, 0);
    puts("RUFUS_ARCHIVE_DONE PREWRITE_CRC=1 SOURCE_PROTECTED=1 AUTH=1 JOINED=1 BOOTABLE_ISO=1");
}
