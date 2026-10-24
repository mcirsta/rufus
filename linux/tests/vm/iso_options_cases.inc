// SPDX-License-Identifier: GPL-3.0-or-later
// Included only by the root/PID1/DMI/namespace guarded disposable guest driver.
void runIsoOptionsCases(const rufus::PrepareImageRequest& base)
{
    require(QDir().mkpath(QStringLiteral("/run/caller-home")) && QDir().mkpath(QStringLiteral("/mnt/readback")), "ISO option directories");
    require(mount("tmpfs", "/run/caller-home", "tmpfs", MS_NODEV | MS_NOSUID, "size=32m") == 0, "independent source backing");
    writeGuestFile(QStringLiteral("/etc/passwd"), "root:x:0:0:root:/root:/bin/bash\ncaller:x:1000:1000:Caller:/run/caller-home:/bin/bash\n");
    require(chown("/run/caller-home", 1000, 1000) == 0 && mkdir("/run/caller-home/files", 0700) == 0
        && chown("/run/caller-home/files", 1000, 1000) == 0, "caller cache ownership");
    for (const char* name : {"diskcopy.dll", "options.iso", "options-udf.iso"}) {
        const QString destination = QStringLiteral("/run/caller-home/") + (strcmp(name, "diskcopy.dll") == 0 ? QStringLiteral("files/") : QString())
            + QString::fromLatin1(name);
        require(QFile::copy(QLatin1Char('/') + QString::fromLatin1(name), destination)
            && chown(QFile::encodeName(destination).constData(), 1000, 1000) == 0, "owned source copy");
    }
    const QDBusMessage query = QDBusMessage::createMethodCall(base.serviceOwner,
        QStringLiteral("/org/freedesktop/UDisks2"), QStringLiteral("org.freedesktop.DBus.ObjectManager"), QStringLiteral("GetManagedObjects"));
    const QDBusReply<rufus::UDisksObjects> snapshot = QDBusConnection::systemBus().call(query);
    require(snapshot.isValid(), "option baseline metadata"); rawObjects = snapshot.value();
    struct stat partition;
    require(stat("/dev/sda1", &partition) == 0 && S_ISBLK(partition.st_mode), "option seed partition");
    rawPartition = quint64(partition.st_rdev);
    require(QDir().mkpath(QStringLiteral("/dev/block")), "option partition aliases");
    for (int number : {1, 2}) {
        const QByteArray alias = QByteArray("/dev/block/") + QByteArray::number(major(partition.st_rdev)) + ':'
            + QByteArray::number(minor(partition.st_rdev) + number - 1);
        const QByteArray node = QByteArray("../sda") + QByteArray::number(number);
        require(symlink(node.constData(), alias.constData()) == 0 || errno == EEXIST, "option kernel partition aliases");
    }
    const auto manifest = QJsonDocument::fromJson(readFile(QStringLiteral("/legacy-files.json"))).object();
    require(manifest.size() == 13, "independent DOS manifest");
    for (int filesystem : {FS_FAT16, FS_FAT32, FS_NTFS}) {
        for (const char* marker : {"/run/format-launches", "/run/format-exit-held"}) QFile::remove(QString::fromLatin1(marker));
        auto job = base; job.volumeId = 0; job.fileSystem = filesystem; job.label = QStringLiteral("OPTIONS");
        job.clusterSize = filesystem == FS_FAT16 ? 65536 : 4096;
        if (filesystem == FS_FAT16) {
            job.msDos = job.mbr = true; job.sizeChecks = false;
            job.dosImagePath = QStringLiteral("/run/caller-home/files/diskcopy.dll");
            job.dosKeyboard = QStringLiteral("us"); job.dosCodepage = 437;
        } else {
            job.preserveTimestamps = true;
            job.joliet = false; // RR for ISO9660; UDF must remain preferred regardless.
            require(rufus::CapturePrepareSource(QStringLiteral("/run/caller-home/")
                + (filesystem == FS_NTFS ? QStringLiteral("options-udf.iso") : QStringLiteral("options.iso")), job) == 0, "option source snapshot");
        }
        publishTargetMount(rawObjects, job, {}, rawPartition);
        QProcess client; client.setProcessChannelMode(QProcess::ForwardedChannels);
        client.start(QStringLiteral("/usr/bin/test_native_format_qt_guest"), {});
        require(client.waitForStarted(10000), "option ordinary Qt client");
        const QByteArray packet = QJsonDocument(rufus::EncodePrepareRequest(job)).toJson(QJsonDocument::Compact) + '\n';
        require(packet.size() > 3 && packet.size() < rufus::PrepareMessageBytes && client.write(packet) == packet.size(), "option identity pipe");
        client.closeWriteChannel();
        require(client.waitForFinished(180000) && client.exitCode() == 0, "option ordinary Qt joined");
        const bool ntfs = filesystem == FS_NTFS;
        require(mount("/dev/sda1", "/mnt/readback", ntfs ? "ntfs" : "vfat", MS_RDONLY | MS_NODEV | MS_NOSUID | MS_NOEXEC,
            ntfs ? "iocharset=utf8" : "utf8=1,tz=UTC") == 0, "option independent kernel readback");
        if (job.msDos) {
            for (auto entry = manifest.begin(); entry != manifest.end(); ++entry) {
                const QByteArray bytes = readFile(QStringLiteral("/mnt/readback/") + entry.key());
                require(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()
                    == entry.value().toObject().value(QStringLiteral("sha256")).toString().toLatin1(), "DOS64 independent bytes");
            }
            const int disk = open("/dev/sda1", O_RDONLY | O_CLOEXEC); require(disk >= 0, "DOS64 BPB descriptor");
            unsigned char bpb[512]; require(pread(disk, bpb, sizeof(bpb), 0) == sizeof(bpb) && close(disk) == 0, "DOS64 BPB read");
            require(bpb[11] == 0 && bpb[12] == 2 && bpb[13] == 128, "DOS64 real 65536 byte clusters");
        } else {
            for (const char* name : {"Mixed-Case.txt", "sub/child.txt", "sub", "empty", "efi/boot/bootx64.efi"}) {
                struct stat info;
                require(stat((QByteArray("/mnt/readback/") + name).constData(), &info) == 0 && info.st_mtim.tv_sec == 1234567890,
                    "independent preserved ISO/UDF file and directory modification time");
                const time_t access = ntfs ? 1234567890 : 1234483200; // FAT access time stores only the UTC date.
                require(info.st_atim.tv_sec == access, "independent preserved access time and native FAT precision");
            }
            require(readFile(QStringLiteral("/mnt/readback/Mixed-Case.txt")) == "ISO option payload\n"
                && readFile(QStringLiteral("/mnt/readback/sub/child.txt")) == "Nested timestamp payload\n"
                && readFile(QStringLiteral("/mnt/readback/efi/boot/bootx64.efi")) == readFile(QStringLiteral("/original.efi")), "independent option payload bytes");
        }
        require(umount("/mnt/readback") == 0, "option independent unmount");
        const int claim = open("/dev/sda", O_RDWR | O_EXCL | O_CLOEXEC);
        require(claim >= 0 && close(claim) == 0, "option helper claim released");
        printf("RUFUS_ISO_OPTIONS_READBACK FS=%d TIMES=1 BYTES=1 CLAIM=1\n", filesystem);
    }
    require(umount("/run/caller-home") == 0, "option source cleanup");
    puts("RUFUS_ISO_OPTIONS_DONE JOBS=3");
}
