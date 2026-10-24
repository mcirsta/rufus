// SPDX-License-Identifier: GPL-3.0-or-later
// Included only in the guarded disposable guest driver. /dev/vda is its scratch disk.
void resetOldBiosMarkers()
{
    for (const char* path : {"/run/format-launches", "/run/format-exit-held", "/run/format-exit-release"})
        QFile::remove(QString::fromLatin1(path));
}

void checkOldBiosTable(const QString& node, const MBR_IMAGE_RESULT& planned, int filesystem)
{
    QFile disk(node); require(disk.open(QIODevice::ReadOnly), "old-BIOS independent table readback");
    const QByteArray mbr = disk.read(512);
    require(mbr.size() == 512 && quint8(mbr[446]) == 0x80 && quint8(mbr[466]) == 0xea && mbr[462] == 0,
        "old-BIOS active data and inactive compatibility track");
    require(qFromLittleEndian<quint32>(mbr.constData() + 454) * quint64(512) == planned.offset
        && qFromLittleEndian<quint32>(mbr.constData() + 458) * quint64(512) == planned.size,
        "old-BIOS main raw extent");
    require(qFromLittleEndian<quint32>(mbr.constData() + 470) * quint64(512) == planned.compatibility_offset
        && qFromLittleEndian<quint32>(mbr.constData() + 474) == 63, "old-BIOS trailing raw extent");
    require(disk.seek(qint64(planned.compatibility_offset)) && disk.read(32256) == QByteArray(32256, '\0'),
        "compatibility track cleared");
    if (filesystem == FS_FAT16 || filesystem == FS_FAT32 || filesystem == FS_NTFS)
    {
        require(disk.seek(qint64(planned.offset)), "old-BIOS volume offset");
        const QByteArray boot = disk.read(512);
        require(boot.size() == 512 && qFromLittleEndian<quint32>(boot.constData() + 28) == planned.offset / 512,
            "old-BIOS filesystem hidden sectors match layout");
    }
}

void runOldBiosExtCases()
{
    constexpr quint64 bytes = 320 * 1024 * 1024;
    for (int filesystem : {FS_EXT2, FS_EXT3})
    {
        int disk = open("/dev/vda", O_RDWR | O_EXCL | O_CLOEXEC);
        struct stat whole = {}; quint64 size = 0;
        require(disk >= 0 && fstat(disk, &whole) == 0 && ioctl(disk, BLKGETSIZE64, &size) == 0 && size == bytes,
            "old-BIOS ext scratch device claim");
        const MBR_IMAGE_OPTIONS options = {bytes, 512, 63, 256, 0x78563412, quint64(whole.st_rdev),
            filesystem, MBR_BOOT_GRUB2, 0, 0, true};
        MBR_IMAGE_RESULT planned = {};
        require(PrepareMbrTarget(disk, &options, &planned) == 0 && planned.offset == 64512,
            "old-BIOS ext native table and default sentinel alignment");
        require(ioctl(disk, BLKRRPART) == 0 && close(disk) == 0, "ext partition refresh and formatter claim handoff");
        const int partition = open("/dev/vda1", O_RDWR | O_CLOEXEC);
        struct stat info = {}; require(partition >= 0 && fstat(partition, &info) == 0, "old-BIOS ext partition");
        const rufus::ExtFormatOptions format{planned.size, filesystem, QStringLiteral("OLD BIOS"), false, true};
        FORMAT_RESULT result = {};
        require(rufus::FormatExtDevice(partition, quint64(info.st_rdev), QStringLiteral("/usr/bin/mke2fs"), format, result) == 0
            && close(partition) == 0, "native old-BIOS ext format and metadata verification");
        disk = open("/dev/vda", O_RDWR | O_EXCL | O_CLOEXEC);
        QByteArray core; MBR_CORE_RESULT installed = {};
        require(disk >= 0 && rufus::ReadEmbeddedGrub2Core(planned.offset, core) == 0
            && WriteGrub2CoreImage(disk, &options, reinterpret_cast<const unsigned char*>(core.constData()),
                quint64(core.size()), &installed) == 0 && close(disk) == 0, "ext claim reacquired and GRUB fits legacy gap");
        QProcess check; check.start(QStringLiteral("/usr/bin/e2fsck"), {QStringLiteral("-fn"), QStringLiteral("/dev/vda1")});
        require(check.waitForFinished(30000) && check.exitCode() == 0, "old-BIOS ext independent filesystem readback");
        checkOldBiosTable(QStringLiteral("/dev/vda"), planned, filesystem);
        printf("RUFUS_OLD_BIOS_CASE FS=%d OFFSET=%llu READBACK=1 HELPER=0\n", filesystem,
            static_cast<unsigned long long>(planned.offset));
    }
}

void runOldBiosCases(const rufus::PrepareImageRequest& base, const QString& rawPath, const QByteArray& expectedDisk)
{
    runOldBiosExtCases();
    for (int filesystem : {FS_FAT16, FS_NTFS, FS_FAT32})
    {
        rufus::PrepareImageRequest request;
        request.path = base.path; request.device = base.device; request.size = base.size;
        request.diskSequence = base.diskSequence; request.objectPath = base.objectPath;
        request.serviceOwner = base.serviceOwner; request.identity = base.identity;
        request.isDevice = request.mbr = request.oldBiosFixes = true;
        request.grub2 = filesystem != FS_FAT32; request.syslinux = !request.grub2;
        request.mbr = request.grub2; // Syslinux already implies MBR in the strict protocol.
        request.syslinuxSelection = request.syslinux ? BT_SYSLINUX_V6 : BT_IMAGE;
        request.fileSystem = filesystem; request.label = QStringLiteral("OLD BIOS");
        request.volumeId = filesystem == FS_NTFS ? 0 : 0x1234abcd;
        request.clusterSize = filesystem == FS_FAT16 ? 2048 : filesystem == FS_FAT32 ? 512 : 4096;
        resetOldBiosMarkers(); publishTargetMount(rawObjects, request, {}, rawPartition);
        rufus::PrepareImageProcess process(QStringLiteral("/usr/bin/rufus-prepare-helper"));
        QSignalSpy done(&process, &rufus::PrepareImageProcess::finished);
        QSignalSpy logs(&process, &rufus::PrepareImageProcess::logMessage);
        require(process.start(request), "old-BIOS complete helper job");
        QElapsedTimer timer; timer.start(); bool held = false;
        while (done.isEmpty())
        {
            QCoreApplication::processEvents(); require(timer.elapsed() < 90000, "old-BIOS helper deadline");
            if (QFile::exists(QStringLiteral("/run/format-exit-held"))) held = true;
            usleep(1000);
        }
        if (process.outcome().error != 0)
        {
            for (const auto& entry : logs) fprintf(stderr, "OLD_BIOS_LOG %s\n", qPrintable(entry.first().toString()));
            qFatal("Old-BIOS helper: %s", qPrintable(process.problem()));
        }
        require(held && process.state() == rufus::PrepareImageProcess::State::Complete
            && readFile(QStringLiteral("/run/format-launches")) == "launch\n", "old-BIOS one authorization and joined result");
        const MBR_IMAGE_OPTIONS options{base.size, 512, 63, request.clusterSize, 0, 0, filesystem,
            rufus::FormatMbrBootCode(request), 0, 0, true};
        MBR_IMAGE_RESULT planned = {}; require(PlanMbrImageLayout(&options, &planned) == 0, "old-BIOS expected geometry");
        checkOldBiosTable(QStringLiteral("/dev/sda"), planned, filesystem);
        printf("RUFUS_OLD_BIOS_CASE FS=%d OFFSET=%llu READBACK=1 HELPER=1 AUTH=1 JOINED=1\n", filesystem,
            static_cast<unsigned long long>(planned.offset));
    }
    auto request = base;
    request.rawWrite = false; request.imagePath.clear(); request.imageDevice = request.imageInode = request.imageSize = 0;
    request.imageModified.clear(); request.imageChanged.clear(); request.isDevice = request.mbr = request.grub2 = request.oldBiosFixes = true;
    request.fileSystem = FS_FAT32; request.label = QStringLiteral("OLD BIOS"); request.clusterSize = 512; request.volumeId = 0x1234abcd;
    resetOldBiosMarkers(); publishTargetMount(rawObjects, request, {}, rawPartition);
    QProcess client; client.setProcessChannelMode(QProcess::ForwardedChannels);
    client.start(QStringLiteral("/usr/bin/test_media_appearance_qt_guest"), {QStringLiteral("--old-bios")});
    require(client.waitForStarted(10000), "ordinary old-BIOS Qt client");
    const QByteArray packet = QJsonDocument(rufus::EncodePrepareRequest(request)).toJson(QJsonDocument::Compact) + '\n';
    require(client.write(packet) == packet.size(), "old-BIOS identity pipe"); client.closeWriteChannel();
    require(client.waitForFinished(120000) && client.exitCode() == 0, "ordinary old-BIOS Qt creation joined");
    QFile original(QStringLiteral("/dev/sda")), boot(QStringLiteral("/dev/vda"));
    require(original.open(QIODevice::ReadOnly) && boot.open(QIODevice::ReadWrite), "old-BIOS boot snapshot devices");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (quint64 offset = 0; offset < base.size; offset += 65536)
    {
        const QByteArray bytes = original.read(65536); require(bytes.size() == 65536, "boot snapshot read");
        hash.addData(bytes); require(boot.write(bytes) == bytes.size(), "boot snapshot write");
    }
    require(boot.flush() && fsync(boot.handle()) == 0, "boot snapshot sync"); original.close(); boot.close();
    printf("RUFUS_OLD_BIOS_BOOT_SNAPSHOT BYTES=%llu SHA256=%s\n", static_cast<unsigned long long>(base.size), hash.result().toHex().constData());
    resetOldBiosMarkers(); auto restore = base;
    require(rufus::CapturePrepareSource(rawPath, restore) == 0, "old-BIOS representative restore source");
    publishTargetMount(rawObjects, restore, {}, rawPartition); runCompressedQtCase(restore);
    require(diskDigest() == expectedDisk && QFile::remove(rawPath), "representative restore after compatibility jobs");
    printf("RUFUS_OLD_BIOS_DONE SHA256=%s\n", expectedDisk.toHex().constData());
}
