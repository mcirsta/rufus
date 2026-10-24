// SPDX-License-Identifier: GPL-3.0-or-later
// Only included by the DMI/parent guarded disposable guest driver.
void runBadBlocksCases(const rufus::PrepareImageRequest& base, const QString& path, const QByteArray& expectedDisk)
{
    for (int choice : {rufus::BadBlocksAbort, rufus::BadBlocksRetry, rufus::BadBlocksIgnore})
    {
        for (const char* marker : {"/run/format-launches", "/run/format-exit-held", "/run/format-exit-release"})
            QFile::remove(QString::fromLatin1(marker));
        writeGuestFile(QStringLiteral("/run/badblocks-fault"), choice == rufus::BadBlocksRetry ? "alias-once" : "alias");
        auto request = base;
        require(rufus::CapturePrepareSource(path, request) == 0, "bad-block retained source");
        request.badBlocksPattern = 0;
        publishTargetMount(rawObjects, request, {}, rawPartition);
        rufus::PrepareImageProcess process(QStringLiteral("/usr/bin/compressed_helper_guest_probe"));
        process.setReportDirectory(QStringLiteral("/run/caller-home"));
        QSignalSpy done(&process, &rufus::PrepareImageProcess::finished);
        QSignalSpy review(&process, &rufus::PrepareImageProcess::badBlocksReviewRequired);
        require(process.start(request), "bad-block helper start");
        QElapsedTimer timer; timer.start();
        bool answered = false, held = false;
        while (done.isEmpty())
        {
            QCoreApplication::processEvents();
            require(timer.elapsed() < 180000, "bad-block helper deadline");
            if (!review.isEmpty() && !answered)
            {
                const auto report = qvariant_cast<rufus::BadBlocksReport>(review.first().first());
                require(report.count == 120 && report.corruptionErrors == 120 && report.error == 0, "real direct-I/O aliased media detection");
                checkExclusiveClaim();
                require(process.isActive() && QFile::exists(review.first()[1].toString()), "review holds claim and report");
                process.answerBadBlocks(choice); answered = true;
            }
            if (QFile::exists(QStringLiteral("/run/format-exit-held")) && done.isEmpty())
            {
                require(process.isActive(), "bad-block READY waits for joined launcher"); held = true;
            }
            usleep(1000);
        }
        if (process.outcome().error != (choice == rufus::BadBlocksAbort ? ECANCELED : 0))
            qFatal("Bad-block choice %d: error=%d problem=%s", choice, process.outcome().error, qPrintable(process.problem()));
        require(answered && held && process.outcome().started, "bad-block decision and joined completion");
        require(process.outcome().syncError == 0 && process.outcome().cleanupError == 0, "bad-block sync and cleanup");
        require(readFile(QStringLiteral("/run/format-launches")) == "launch\n", "one authorization across retry/review/creation");
        if (choice == rufus::BadBlocksAbort)
        {
            const int fd = open("/dev/sda", O_RDONLY | O_CLOEXEC);
            QByteArray headers(8 * 1024 * 1024, '\x01');
            require(fd >= 0 && pread(fd, headers.data(), size_t(headers.size()), 0) == headers.size()
                && headers == QByteArray(headers.size(), '\0') && close(fd) == 0, "abort clears test partition signatures");
            require(process.outcome().raw.bytes_written == 0, "abort never starts image copy");
        }
        else require(diskDigest() == expectedDisk, "retry/ignore exact image restore");
        const int claim = open("/dev/sda", O_RDWR | O_EXCL | O_CLOEXEC);
        require(claim >= 0 && close(claim) == 0, "bad-block claim released before completion");
        printf("RUFUS_BADBLOCK_CASE CHOICE=%d BAD=120 JOINED=1 AUTH=1\n", choice);
    }
    for (const char* marker : {"/run/format-launches", "/run/format-exit-held", "/run/format-exit-release"})
        QFile::remove(QString::fromLatin1(marker));
    auto restore = base;
    require(rufus::CapturePrepareSource(path, restore) == 0, "clean bad-block restore source");
    restore.badBlocksPattern = 4;
    publishTargetMount(rawObjects, restore, {}, rawPartition);
    runCompressedQtCase(restore);
    require(diskDigest() == expectedDisk && QFile::remove(path), "clean TLC test and ordinary bootable restore");
    printf("RUFUS_BADBLOCK_DONE SHA256=%s\n", expectedDisk.toHex().constData());
}
