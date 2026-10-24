// SPDX-License-Identifier: GPL-3.0-or-later
// Included only in the guarded disposable Linux guest driver.
void runNativeFormatCases(const rufus::PrepareImageRequest& base)
{
    const bool udfIndexing = readFile(QStringLiteral("/etc/rufus-udf-indexing-fixture")) == "udf-indexing\n";
    require(mkdir("/run/caller-home", 0700) == 0, "native caller-home directory");
    require(mount("tmpfs", "/run/caller-home", "tmpfs", 0, "size=16m") == 0, "independent caller home");
    writeGuestFile(QStringLiteral("/etc/passwd"), "root:x:0:0:root:/root:/bin/bash\ncaller:x:1000:1000:Caller:/run/caller-home:/bin/bash\n");
    const QDBusMessage query = QDBusMessage::createMethodCall(base.serviceOwner,
        QStringLiteral("/org/freedesktop/UDisks2"), QStringLiteral("org.freedesktop.DBus.ObjectManager"), QStringLiteral("GetManagedObjects"));
    const QDBusReply<rufus::UDisksObjects> snapshot = QDBusConnection::systemBus().call(query);
    require(snapshot.isValid(), "native baseline metadata"); rawObjects = snapshot.value();
    // The minimal initramfs has no udev. The seeded disk already has a kernel
    // partition; supply its ordinary /dev/block alias, as the existing guests do.
    struct stat partition;
    require(stat("/dev/sda1", &partition) == 0 && S_ISBLK(partition.st_mode), "native seed partition node");
    rawPartition = quint64(partition.st_rdev);
    require(QDir().mkpath(QStringLiteral("/dev/block")), "native partition link directory");
    const QByteArray alias = QByteArray("/dev/block/") + QByteArray::number(major(partition.st_rdev)) + ':'
        + QByteArray::number(minor(partition.st_rdev));
    require(symlink("../sda1", alias.constData()) == 0 || errno == EEXIST, "native kernel partition link");
    require(QFile::copy(QStringLiteral("/native-supplement.zip"), QStringLiteral("/run/caller-home/native-supplement.zip"))
        && chown("/run/caller-home/native-supplement.zip", 1000, 1000) == 0, "independent supplement ownership");
    struct NativeCase { int filesystem; bool sfd, mbr, quick, archive, indexing = false; };
    const QList<NativeCase> cases = udfIndexing
        ? QList<NativeCase>{{FS_UDF, false, false, true, true, false}, {FS_UDF, true, false, true, true, true}}
        : QList<NativeCase>{{FS_UDF, false, false, true, true}, {FS_UDF, true, false, false, false},
            {FS_EXT2, true, false, true, false}, {FS_EXT3, false, true, false, false}};
    for (const NativeCase& test : cases) {
        for (const char* marker : {"/run/format-launches", "/run/format-exit-held"}) QFile::remove(QString::fromLatin1(marker));
        rufus::PrepareImageRequest request;
        request.path = base.path; request.device = base.device; request.size = base.size;
        request.diskSequence = base.diskSequence; request.objectPath = base.objectPath;
        request.serviceOwner = base.serviceOwner; request.identity = base.identity;
        request.isDevice = request.nonBootable = true; request.fileSystem = test.filesystem;
        request.sfd = test.sfd; request.mbr = test.mbr; request.quickFormat = test.quick;
        request.fileIndexing = test.indexing;
        request.clusterSize = 256; request.label = test.filesystem == FS_UDF ? QString::fromUtf8("Native Ω") : QStringLiteral("Native ext");
        request.extendedLabel = test.filesystem == FS_UDF; if (request.extendedLabel) request.displayLabel = request.label;
        if (test.archive) {
            require(rufus::CapturePrepareArchive(QStringLiteral("/run/caller-home/native-supplement.zip"), request.archive) == 0, "capture supplement");
        }
        publishTargetMount(rawObjects, request, {}, rawPartition);
        QProcess client; client.setProcessChannelMode(QProcess::ForwardedChannels);
        client.start(QStringLiteral("/usr/bin/test_native_format_qt_guest"), {});
        require(client.waitForStarted(10000), "native Qt client start");
        const QByteArray packet = QJsonDocument(rufus::EncodePrepareRequest(request)).toJson(QJsonDocument::Compact) + '\n';
        require(packet.size() < rufus::PrepareMessageBytes && client.write(packet) == packet.size(), "native request pipe");
        client.closeWriteChannel();
        require(client.waitForFinished(180000) && client.exitCode() == 0, "ordinary native Qt client joined");
        const char* node = test.sfd ? "/dev/sda" : "/dev/sda1";
        const char* filesystem = test.filesystem == FS_UDF ? "udf" : test.filesystem == FS_EXT2 ? "ext2" : "ext3";
        require(QDir().mkpath(QStringLiteral("/mnt/native")), "native independent mountpoint");
        require(mount(node, "/mnt/native", filesystem, MS_RDONLY | MS_NODEV | MS_NOSUID | MS_NOEXEC,
            test.filesystem == FS_UDF ? "iocharset=utf8" : "") == 0, "independent kernel readback");
        if (test.filesystem == FS_UDF) {
            const QByteArray inf = readFile(QStringLiteral("/mnt/native/autorun.inf"));
            const QByteArray icon = readFile(QStringLiteral("/mnt/native/autorun.ico"));
            QByteArray expected; require(rufus::ReadMediaIcon(expected) == 0 && icon == expected, "native icon readback");
            require(inf.startsWith(QByteArray::fromHex("fffe")), "native metadata encoding");
            QByteArray label;
            for (const QChar character : request.label) {
                label.append(char(character.unicode() & 255)); label.append(char(character.unicode() >> 8));
            }
            require(inf.contains(label), "native exact extended label");
            if (test.archive) require(readFile(QString::fromUtf8("/mnt/native/安装/native.txt")) == "native UDF overlay\n", "UDF ZIP overlay readback");
            struct stat info;
            require(stat("/mnt/native/autorun.inf", &info) == 0 && (info.st_mode & 0444) == 0444, "UDF metadata readable after root helper");
            if (udfIndexing) {
                const int volume = open(node, O_RDONLY | O_CLOEXEC);
                require(volume >= 0, "independent UDF label descriptor");
                const QByteArray anchor = readBytes(volume, 256 * 512, 512);
                require(anchor.size() == 512 && qFromLittleEndian<quint16>(anchor.constData()) == 2, "independent UDF anchor");
                const quint32 extentBytes = qFromLittleEndian<quint32>(anchor.constData() + 16);
                const quint32 extentBlock = qFromLittleEndian<quint32>(anchor.constData() + 20);
                require(extentBytes >= 512 && extentBytes <= 64 * 1024 && extentBytes % 512 == 0
                    && quint64(extentBlock) * 512 + extentBytes <= request.size, "independent UDF descriptor bounds");
                bool foundLabel = false;
                const QByteArray expected = QByteArray::fromHex("10004e00610074006900760065002003a9");
                for (quint32 offset = 0; offset < extentBytes; offset += 512) {
                    const QByteArray descriptor = readBytes(volume, quint64(extentBlock) * 512 + offset, 512);
                    require(descriptor.size() == 512, "independent UDF descriptor read");
                    if (qFromLittleEndian<quint16>(descriptor.constData()) != 6) continue;
                    require(descriptor.mid(84, expected.size()) == expected && quint8(descriptor[211]) == expected.size(),
                        "independent UDF on-disk Unicode label");
                    foundLabel = true;
                }
                require(foundLabel && close(volume) == 0, "independent UDF label complete");
            }
        } else {
            const int volume = open(node, O_RDONLY | O_CLOEXEC);
            require(volume >= 0, "native ext readback descriptor");
            const QByteArray superblock = readBytes(volume, 1024, 1024); close(volume);
            require(quint8(superblock[56]) == 0x53 && quint8(superblock[57]) == 0xef, "independent ext superblock");
            require(bool(quint8(superblock[92]) & 4) == (test.filesystem == FS_EXT3), "native journal choice");
            require(superblock.mid(120, 10) == "Native ext", "native ext label");
        }
        require(umount("/mnt/native") == 0 && rmdir("/mnt/native") == 0, "independent native cleanup");
        const int claim = open("/dev/sda", O_RDWR | O_EXCL | O_CLOEXEC);
        require(claim >= 0 && close(claim) == 0, "all native claims released");
        printf("RUFUS_NATIVE_READBACK FS=%d SFD=%d MBR=%d QUICK=%d\n", test.filesystem, int(test.sfd), int(test.mbr), int(test.quick));
        if (udfIndexing) printf("RUFUS_UDF_INDEXING_READBACK INDEXING=%d LABEL=1 ICON=1 ZIP=1 CLAIM=1\n", int(test.indexing));
    }
    require(umount("/run/caller-home") == 0, "caller-home cleanup");
    if (udfIndexing) puts("RUFUS_UDF_INDEXING_DONE JOBS=2 AUTH=1 JOINED=1 READBACK=1");
    else puts("RUFUS_NATIVE_FORMATS_DONE JOBS=4 AUTH=1 JOINED=1 READBACK=1");
}
