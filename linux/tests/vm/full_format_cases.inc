// SPDX-License-Identifier: GPL-3.0-or-later
void seedFullFormatDisk(quint64 capacity)
{
    const int fd = open("/dev/sda", O_RDWR | O_EXCL | O_CLOEXEC);
    const QByteArray dirty(1024 * 1024, char(0xa5));
    require(fd >= 0, "full format dirty disk");
    for (quint64 offset = 0; offset < capacity; offset += quint64(dirty.size()))
        require(pwrite(fd, dirty.constData(), size_t(dirty.size()), off_t(offset)) == dirty.size(), "full format dirty seed");
    require(fsync(fd) == 0 && close(fd) == 0, "full format seed sync");
}

void runFullFormatCases(const rufus::PrepareImageRequest& base, const QString& rawPath, const QByteArray& expectedDisk)
{
    for (int filesystem : {FS_FAT16, FS_FAT32, FS_NTFS, FS_EXFAT})
    {
        seedFullFormatDisk(base.size);
        rufus::PrepareImageRequest request;
        request.path = base.path; request.device = base.device; request.size = base.size;
        request.diskSequence = base.diskSequence; request.objectPath = base.objectPath;
        request.serviceOwner = base.serviceOwner; request.identity = base.identity;
        request.isDevice = request.nonBootable = request.mbr = true;
        request.quickFormat = false; request.fileSystem = filesystem;
        request.label = QStringLiteral("FULLTEST");
        request.volumeId = filesystem == FS_FAT16 || filesystem == FS_FAT32 ? 0x1234abcd : 0;
        request.clusterSize = filesystem == FS_FAT16 ? 2048 : filesystem == FS_FAT32 ? 512 : 4096;
        publishTargetMount(rawObjects, request, {}, rawPartition);
        runFormatHelperCase({"full-format", 0, true, 4}, request);
        QFile disk(QStringLiteral("/dev/sda"));
        require(disk.open(QIODevice::ReadOnly) && disk.seek(49 * 1024 * 1024)
            && disk.read(65536) == QByteArray(65536, '\0'), "full format native free-data readback");
        disk.close();
        for (const char* marker : {"/run/format-launches", "/run/format-exit-held", "/run/format-exit-release"})
            QFile::remove(QString::fromLatin1(marker));
        printf("RUFUS_FULL_FORMAT_CASE FS=%d FULL=1 READBACK=1 HELPER=1 JOINED=1\n", filesystem);
    }
    for (int filesystem : {FS_EXT2, FS_EXT3, FS_EXT4})
    {
        const int claim = open("/dev/sda", O_RDWR | O_EXCL | O_CLOEXEC);
        const int fd = open("/dev/sda1", O_RDWR | O_CLOEXEC);
        uint64_t capacity = 0; struct stat partition;
        require(claim >= 0 && fd >= 0 && ioctl(fd, BLKGETSIZE64, &capacity) == 0 && fstat(fd, &partition) == 0,
            "full ext partition under whole-device claim");
        const QByteArray dirty(65536, char(0xa5));
        for (quint64 offset = 0; offset < capacity; offset += quint64(dirty.size()))
        {
            const size_t bytes = size_t(qMin(quint64(dirty.size()), capacity - offset));
            require(pwrite(fd, dirty.constData(), bytes, off_t(offset)) == ssize_t(bytes), "dirty ext partition seed");
        }
        require(fsync(fd) == 0 && close(claim) == 0, "dirty ext partition sync and native claim handoff");
        rufus::ExtFormatOptions options{capacity, filesystem, QStringLiteral("FULLTEST"), true, false};
        FORMAT_RESULT formatted;
        require(rufus::FormatExtDevice(fd, partition.st_rdev, QStringLiteral("/usr/bin/mke2fs"), options, formatted) == 0
            && formatted.started && formatted.filesystem_bytes == capacity && close(fd) == 0,
            "full ext format and descriptor cleanup");
        const int reacquired = open("/dev/sda", O_RDWR | O_EXCL | O_CLOEXEC);
        require(reacquired >= 0 && close(reacquired) == 0, "ext formatter released its claim before completion");
        QProcess check;
        check.start(QStringLiteral("/usr/bin/e2fsck"), {QStringLiteral("-fn"), QStringLiteral("/dev/sda1")});
        require(check.waitForFinished(30000) && check.exitCode() == 0, "full ext independent filesystem readback");
        if (filesystem != FS_EXT2)
        {
            QProcess journal;
            journal.start(QStringLiteral("/usr/bin/debugfs"), {QStringLiteral("-R"),
                QStringLiteral("dump <8> /run/full-journal"), QStringLiteral("/dev/sda1")});
            require(journal.waitForFinished(30000) && journal.exitCode() == 0, "full ext independent journal dump");
            const QByteArray bytes = readFile(QStringLiteral("/run/full-journal"));
            require(bytes.size() >= 1024 * 1024 && bytes.mid(1024) == QByteArray(bytes.size() - 1024, '\0'),
                "full ext journal initialized without lazy data");
            require(QFile::remove(QStringLiteral("/run/full-journal")), "journal fixture released");
        }
        printf("RUFUS_FULL_FORMAT_CASE FS=%d FULL=1 READBACK=1 HELPER=0 JOINED=1\n", filesystem);
    }
    auto restore = base;
    require(rufus::CapturePrepareSource(rawPath, restore) == 0, "full format representative restore source");
    publishTargetMount(rawObjects, restore, {}, rawPartition);
    runCompressedQtCase(restore);
    require(diskDigest() == expectedDisk && QFile::remove(rawPath), "representative bootable restore after full formats");
    printf("RUFUS_FULL_FORMAT_DONE SHA256=%s\n", expectedDisk.toHex().constData());
}
