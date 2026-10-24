// SPDX-License-Identifier: GPL-3.0-or-later
// Included only in the DMI/parent guarded disposable guest driver.
void resetAppearanceMarkers()
{
    for (const char* marker : {"/run/format-launches", "/run/format-exit-held", "/run/format-exit-release", "/run/appearance-force-fuse"})
        QFile::remove(QString::fromLatin1(marker));
}

void runAppearanceHelper(rufus::PrepareImageRequest request, bool fallback = false)
{
    resetAppearanceMarkers();
    if (fallback) writeGuestFile(QStringLiteral("/run/appearance-force-fuse"), "fallback\n");
    publishTargetMount(rawObjects, request, {}, rawPartition);
    const QString helper = fallback ? QStringLiteral("/usr/bin/compressed_helper_guest_probe")
        : QStringLiteral("/usr/bin/rufus-prepare-helper");
    rufus::PrepareImageProcess process(helper);
    QSignalSpy done(&process, &rufus::PrepareImageProcess::finished);
    QSignalSpy logs(&process, &rufus::PrepareImageProcess::logMessage);
    require(process.start(request), "start appearance helper");
    QElapsedTimer timer; timer.start(); bool held = false;
    while (done.isEmpty())
    {
        QCoreApplication::processEvents(); require(timer.elapsed() < 90000, "appearance helper deadline");
        if (QFile::exists(QStringLiteral("/run/format-exit-held")) && done.isEmpty())
        { require(process.isActive(), "appearance waits for joined exit"); held = true; }
        usleep(1000);
    }
    if (process.outcome().error != 0)
    {
        for (const auto& entry : logs) fprintf(stderr, "APPEARANCE_LOG %s\n", qPrintable(entry.first().toString()));
        qFatal("Appearance helper: %s", qPrintable(process.problem()));
    }
    require(held && process.state() == rufus::PrepareImageProcess::State::Complete && process.outcome().appearanceDone,
        "appearance successful joined result");
    require(readFile(QStringLiteral("/run/format-launches")) == "launch\n", "one appearance authorization");
    if (request.fileSystem == FS_NTFS)
    {
        QString transcript;
        for (const auto& entry : logs) transcript += entry.first().toString();
        require(transcript.contains(fallback ? QStringLiteral("NTFS file copying uses ntfs-3g")
            : QStringLiteral("NTFS file copying uses the kernel ntfs driver")), "actual NTFS provider selected");
    }
}

void readAppearanceFiles(int filesystem, bool sfd, const QString& label, bool preserved = false)
{
    require(mkdir("/mnt/appearance", 0700) == 0 || errno == EEXIST, "independent appearance mountpoint");
    const char* node = sfd ? "/dev/sda" : "/dev/sda1";
    const char* type = filesystem == FS_NTFS ? "ntfs" : filesystem == FS_EXFAT ? "exfat" : "vfat";
    const char* options = filesystem == FS_NTFS ? "iocharset=utf8" : filesystem == FS_EXFAT ? "iocharset=utf8" : "utf8=1";
    require(mount(node, "/mnt/appearance", type, MS_RDONLY | MS_NODEV | MS_NOSUID | MS_NOEXEC, options) == 0,
        "independent kernel filesystem readback");
    const QByteArray inf = readFile(QStringLiteral("/mnt/appearance/autorun.inf"));
    const QByteArray icon = readFile(QStringLiteral("/mnt/appearance/autorun.ico"));
    if (preserved)
    {
        require(inf == readFile(QStringLiteral("/expected-autorun.inf")) && icon == "custom icon bytes",
            "existing uppercase autorun and custom icon preserved exactly");
    }
    else
    {
        require(inf.size() >= 2 && inf.startsWith(QByteArray::fromHex("fffe")) && inf.size() % 2 == 0,
            "UTF-16LE metadata BOM");
        const QString text = QString::fromUtf16(reinterpret_cast<const char16_t*>(inf.constData() + 2), (inf.size() - 2) / 2);
        require(text.contains(QStringLiteral("[autorun]\r\nicon  = autorun.ico\r\nlabel = %1\r\n").arg(label)), "original Unicode label readback");
        QByteArray expected; require(rufus::ReadMediaIcon(expected) == 0 && icon == expected, "original icon readback");
    }
    require(umount("/mnt/appearance") == 0 && rmdir("/mnt/appearance") == 0, "independent readback cleanup");
    const int claim = open("/dev/sda", O_RDWR | O_EXCL | O_CLOEXEC);
    require(claim >= 0 && close(claim) == 0, "appearance job released every device claim");
}

void runMediaAppearanceCases(const rufus::PrepareImageRequest& base, const QString& rawPath, const QByteArray& expectedDisk)
{
    const QString label = QString::fromUtf8("Măriuș 安装 💾 / original label");
    for (int filesystem : {FS_FAT16, FS_FAT32, FS_NTFS, FS_EXFAT})
        for (bool sfd : {false, true})
        {
            rufus::PrepareImageRequest request;
            request.path = base.path; request.device = base.device; request.size = base.size;
            request.diskSequence = base.diskSequence; request.objectPath = base.objectPath;
            request.serviceOwner = base.serviceOwner; request.identity = base.identity;
            request.isDevice = request.nonBootable = request.extendedLabel = true;
            request.sfd = sfd; request.mbr = !sfd && (filesystem == FS_FAT16 || filesystem == FS_NTFS);
            request.fileSystem = filesystem; request.label = QStringLiteral("APPEARANCE"); request.displayLabel = label;
            request.volumeId = filesystem == FS_FAT16 || filesystem == FS_FAT32 ? 0x1234abcd : 0;
            request.clusterSize = filesystem == FS_FAT16 ? 2048 : filesystem == FS_FAT32 ? 512 : 4096;
            if (filesystem == FS_FAT32 && !sfd)
            {
                resetAppearanceMarkers(); publishTargetMount(rawObjects, request, {}, rawPartition);
                QProcess client; client.setProcessChannelMode(QProcess::ForwardedChannels);
                client.start(QStringLiteral("/usr/bin/test_media_appearance_qt_guest"), {});
                require(client.waitForStarted(10000), "start ordinary default-option Qt client");
                const QByteArray packet = QJsonDocument(rufus::EncodePrepareRequest(request)).toJson(QJsonDocument::Compact) + '\n';
                require(client.write(packet) == packet.size(), "appearance identity pipe"); client.closeWriteChannel();
                require(client.waitForFinished(120000) && client.exitCode() == 0, "ordinary appearance Qt client joined");
            }
            else runAppearanceHelper(request, filesystem == FS_NTFS && sfd);
            readAppearanceFiles(filesystem, sfd, label);
            printf("RUFUS_APPEARANCE_CASE FS=%d SFD=%d READBACK=1 AUTH=1 JOINED=1\n", filesystem, int(sfd));
        }
    auto image = base;
    image.rawWrite = false; image.syslinux = true; image.volumeId = 0x1234abcd; image.label = QStringLiteral("RUFUS TEST"); image.clusterSize = 512;
    image.extendedLabel = true; image.displayLabel = QStringLiteral("KEEP THE IMAGE METADATA");
    require(chown("/appearance.iso", 1000, 1000) == 0 && rufus::CapturePrepareSource(QStringLiteral("/appearance.iso"), image) == 0,
        "existing autorun source ownership and snapshot");
    runAppearanceHelper(image); readAppearanceFiles(FS_FAT32, false, {}, true);
    puts("RUFUS_APPEARANCE_PRESERVED ISO=1 CASE=1 ICON=1");
    resetAppearanceMarkers();
    auto restore = base;
    require(rufus::CapturePrepareSource(rawPath, restore) == 0, "appearance representative restore source");
    publishTargetMount(rawObjects, restore, {}, rawPartition);
    runCompressedQtCase(restore);
    require(diskDigest() == expectedDisk && QFile::remove(rawPath), "representative full restore after appearance jobs");
    printf("RUFUS_APPEARANCE_DONE SHA256=%s\n", expectedDisk.toHex().constData());
}
