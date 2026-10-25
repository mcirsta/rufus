// SPDX-License-Identifier: GPL-3.0-or-later
void dropCaptureUser()
{
    if (setgroups(0, nullptr) != 0 || setgid(1000) != 0 || setuid(1000) != 0) _exit(126);
}
void captureGuestImage(const QString& mode, const QString& path, const QByteArray& expected)
{
    QProcess client;
    client.setProcessChannelMode(QProcess::ForwardedChannels);
    client.setChildProcessModifier(dropCaptureUser);
    client.start(QStringLiteral("/usr/bin/test_image_save_qt_guest"), {mode, path, QString::fromLatin1(expected.toHex())});
    require(client.waitForStarted(processTimeoutMs) && client.waitForFinished(360000)
        && client.exitStatus() == QProcess::NormalExit && client.exitCode() == 0, "unprivileged capture process joined");
    const int claim = open("/dev/sda", O_RDONLY | O_EXCL | O_CLOEXEC);
    if (mode == QLatin1String("busy")) require(claim < 0 && errno == EBUSY, "busy desktop mount retained");
    else require(claim >= 0 && close(claim) == 0, "read claim released after joined result");
    require(QDir(QStringLiteral("/run/save-output")).entryList({QStringLiteral(".rufus-image-*")}, QDir::Files | QDir::Hidden).isEmpty(),
        "temporary capture files removed");
}
void runImageSaveCases(rufus::PrepareImageRequest base, const QByteArray& expectedDisk)
{
    require(QDir().mkpath(QStringLiteral("/run/save-output")) && mount("/dev/vda", "/run/save-output", "ext4", 0, nullptr) == 0,
        "disposable capture export filesystem");
    require(chown("/run/save-output", 1000, 1000) == 0, "caller-owned export directory");
    mountFixtureControl(QStringLiteral("OpenFile"), {QStringLiteral("/dev/sda")});
    publishTargetMount(rawObjects, base, {}, rawPartition);
    for (const char* mode : {"cancel", "deny"}) {
        const QString path = QStringLiteral("/run/save-output/%1.vhdx").arg(QLatin1String(mode));
        writeGuestFile(path, "OLD BACKUP");
        require(chown(QFile::encodeName(path).constData(), 1000, 1000) == 0, "old backup owner");
        captureGuestImage(QLatin1String(mode), path, expectedDisk);
        require(QFile::remove(path), "test backup removed");
    }
    mountFixtureControl(QStringLiteral("OpenMode"), {QString()});
    captureGuestImage(QStringLiteral("vhd"), QStringLiteral("/run/save-output/captured.vhd"), expectedDisk);
    captureGuestImage(QStringLiteral("vhdx"), QStringLiteral("/run/save-output/captured.vhdx"), expectedDisk);
    QFile gpt(QStringLiteral("/save-gpt.raw"));
    require(gpt.open(QIODevice::ReadOnly), "independent GPT capture source");
    const int disk = open("/dev/sda", O_RDWR | O_EXCL | O_CLOEXEC);
    require(disk >= 0, "GPT disposable disk acquisition");
    quint64 offset = 0;
    while (!gpt.atEnd()) {
        const QByteArray bytes = gpt.read(65536);
        require(!bytes.isEmpty() && pwrite(disk, bytes.constData(), size_t(bytes.size()), off_t(offset)) == bytes.size(), "GPT source write");
        offset += quint64(bytes.size());
    }
    require(offset == base.size && fsync(disk) == 0 && close(disk) == 0, "GPT source synced");
    gpt.close();
    const QByteArray gptHash = diskDigest();
    captureGuestImage(QStringLiteral("ffu"), QStringLiteral("/run/save-output/captured.ffu"), gptHash);
    auto restore = compressedRequest(base, QStringLiteral("/run/save-output/captured.vhdx"), base.size, expectedDisk, IMG_COMPRESSION_VHDX);
    publishTargetMount(rawObjects, restore, {}, rawPartition);
    runCompressedQtCase(restore);
    require(diskDigest() == expectedDisk, "native saved VHDX restored whole bootable disk");
    require(umount("/run/save-output") == 0, "export filesystem synced and released");
    printf("RUFUS_SAVE_BATCH_DONE SHA256=%s FFUSHA256=%s\n", expectedDisk.toHex().constData(), gptHash.toHex().constData());
}

QMap<QString, QByteArray> exportFiles(const QString& root)
{
    QMap<QString, QByteArray> files;
    QDirIterator iterator(root, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden, QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const QString path = iterator.next();
        const QString relative = QDir(root).relativeFilePath(path);
        if (iterator.fileInfo().isDir()) files[relative] = "directory";
        else {
            QFile file(path);
            require(file.open(QIODevice::ReadOnly), "independent filesystem file open");
            QCryptographicHash hash(QCryptographicHash::Sha256);
            require(hash.addData(&file), "independent filesystem file hash");
            files[relative] = hash.result().toHex();
        }
    }
    return files;
}
void mountExportSource(const rufus::PrepareImageRequest& base, bool readOnly, const QString& type = QStringLiteral("vfat"))
{
    require(QDir().mkpath(QStringLiteral("/mnt/target")), "desktop source mountpoint");
    if (type == QLatin1String("vfat")) require(mount("/dev/sda1", "/mnt/target", "vfat", readOnly ? static_cast<unsigned long>(MS_RDONLY) : 0UL, "uid=1000,gid=1000,utf8=1") == 0,
        "desktop source filesystem mounted");
    auto objects = rawObjects;
    const QDBusObjectPath member(base.objectPath + QStringLiteral("1"));
    auto& block = objects[member][QStringLiteral("org.freedesktop.UDisks2.Block")];
    block[QStringLiteral("IdUsage")] = QStringLiteral("filesystem");
    block[QStringLiteral("IdType")] = type;
    block[QStringLiteral("IdLabel")] = QStringLiteral("RUFUS TEST");
    block[QStringLiteral("Device")] = QByteArray("/dev/sda1\0", 10);
    auto& partition = objects[member][QStringLiteral("org.freedesktop.UDisks2.Partition")];
    partition[QStringLiteral("Offset")] = QVariant::fromValue(quint64(1048576));
    block[QStringLiteral("Drive")] = objects[QDBusObjectPath(base.objectPath)][QStringLiteral("org.freedesktop.UDisks2.Block")].value(QStringLiteral("Drive"));
    block[QStringLiteral("DeviceNumber")] = QVariant::fromValue(rawPartition);
    block[QStringLiteral("ReadOnly")] = false;
    block[QStringLiteral("HintSystem")] = false;
    block[QStringLiteral("HintIgnore")] = false;
    partition[QStringLiteral("Table")] = QVariant::fromValue(QDBusObjectPath(base.objectPath));
    objects[member][QStringLiteral("org.freedesktop.UDisks2.Filesystem")][QStringLiteral("MountPoints")] =
        QVariant::fromValue(QList<QByteArray>{QByteArray("/mnt/target") + '\0'});
    mountFixtureControl(QStringLiteral("SetObjects"), {QVariant::fromValue(objects)});
    mountFixtureControl(QStringLiteral("RefreshMounts"), {true});
}
void runImageExportCases(rufus::PrepareImageRequest base, const QByteArray& originalDisk)
{
    require(QDir().mkpath(QStringLiteral("/run/save-output")) && mount("/dev/vda", "/run/save-output", "ext4", 0, nullptr) == 0,
        "export batch destination filesystem");
    require(chown("/run/save-output", 1000, 1000) == 0, "export batch caller directory");
    mountExportSource(base, true);
    const QString cancelledPath = QStringLiteral("/run/save-output/helper-cancel.vhdx");
    writeGuestFile(cancelledPath, "OLD BACKUP");
    require(chown(QFile::encodeName(cancelledPath).constData(), 1000, 1000) == 0, "cancelled backup owner");
    captureGuestImage(QStringLiteral("helper-cancel"), cancelledPath, originalDisk);
    require(!targetMounted(rawPartition) && QFile::remove(cancelledPath), "cancelled helper released unmounted source");
    mountExportSource(base, true);
    captureGuestImage(QStringLiteral("mounted-vhdx"), QStringLiteral("/run/save-output/captured.vhdx"), originalDisk);
    require(!targetMounted(rawPartition), "mounted VHDX source automatically unmounted");
    mountExportSource(base, false);
    require(QDir().mkpath(QStringLiteral("/mnt/target/.hidden/empty directory")), "ISO source hidden/empty directories");
    writeGuestFile(QStringLiteral("/mnt/target/.hidden/ș-name.txt"), "Unicode hidden export payload\n");
    const auto expectedFiles = exportFiles(QStringLiteral("/mnt/target"));
    const int busy = open("/mnt/target", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    require(busy >= 0, "busy source directory retained");
    for (const char* mode : {"busy", "iso-cleanup"}) {
        const QString path = QStringLiteral("/run/save-output/%1.iso").arg(QLatin1String(mode));
        writeGuestFile(path, "OLD BACKUP");
        require(chown(QFile::encodeName(path).constData(), 1000, 1000) == 0, "export old backup owner");
        if (strcmp(mode, "iso-cleanup") == 0) writeGuestFile(QStringLiteral("/run/save-helper-fault"), "cleanup");
        captureGuestImage(QLatin1String(mode), path, originalDisk);
        require(QFile::remove(path), "export test backup removed");
        if (strcmp(mode, "busy") == 0) {
            require(targetMounted(rawPartition) && close(busy) == 0, "busy failure preserves mounted source");
        } else {
            require(QFile::remove(QStringLiteral("/run/save-helper-fault")), "cleanup fault released");
            const QStringList directories = QDir(QStringLiteral("/run")).entryList({QStringLiteral("rufus-save-*")}, QDir::Dirs);
            require(directories.size() == 1 && QDir().rmdir(QStringLiteral("/run/") + directories.first()), "failed private cleanup directory retained and removed");
        }
    }
    mountExportSource(base, false);
    captureGuestImage(QStringLiteral("iso"), QStringLiteral("/run/save-output/captured.iso"), originalDisk);
    require(!targetMounted(rawPartition), "ISO desktop source automatically unmounted");
    require(QDir(QStringLiteral("/run")).entryList({QStringLiteral("rufus-save-*")}, QDir::Dirs).isEmpty(), "successful ISO private source removed");
    require(QDir().mkpath(QStringLiteral("/run/iso-readback")), "kernel UDF readback directory");
    QProcess mountIso;
    mountIso.setProcessChannelMode(QProcess::ForwardedChannels);
    mountIso.start(QStringLiteral("/usr/bin/mount"), {QStringLiteral("-t"), QStringLiteral("udf"), QStringLiteral("-o"), QStringLiteral("loop,ro"),
        QStringLiteral("/run/save-output/captured.iso"), QStringLiteral("/run/iso-readback")});
    require(mountIso.waitForFinished(10000) && mountIso.exitCode() == 0, "independent kernel UDF-only mount");
    const auto actualFiles = exportFiles(QStringLiteral("/run/iso-readback"));
    require(actualFiles == expectedFiles, "all ISO paths/directories/files match native filesystem");
    require(umount("/run/iso-readback") == 0, "kernel UDF readback released");
    QJsonObject files;
    for (auto item = actualFiles.cbegin(); item != actualFiles.cend(); ++item) files.insert(item.key(), QString::fromLatin1(item.value()));
    writeGuestFile(QStringLiteral("/run/save-output/iso-files.json"), QJsonDocument(files).toJson());
    // Qualify the distinct read-only NTFS-3G source/daemon cleanup path once.
    QProcess formatter;
    formatter.setProcessChannelMode(QProcess::ForwardedChannels);
    formatter.start(QStringLiteral("/usr/bin/mkntfs"), {QStringLiteral("-Q"), QStringLiteral("-F"), QStringLiteral("/dev/sda1")});
    require(formatter.waitForFinished(60000) && formatter.exitCode() == 0, "NTFS export fixture formatted");
    QProcess ntfs;
    ntfs.setProcessChannelMode(QProcess::ForwardedChannels);
    ntfs.start(QStringLiteral("/usr/bin/lowntfs-3g"), {QStringLiteral("-o"),
        QStringLiteral("rw,no_detach,no_def_opts,norecover,uid=1000,gid=1000,allow_other"), QStringLiteral("/dev/sda1"), QStringLiteral("/mnt/target")});
    require(ntfs.waitForStarted(processTimeoutMs), "desktop NTFS daemon started");
    QElapsedTimer mountTimer; mountTimer.start();
    while (!targetMounted(rawPartition)) { require(mountTimer.elapsed() < 10000, "desktop NTFS mount deadline"); usleep(25000); }
    writeGuestFile(QStringLiteral("/mnt/target/NTFS ș-file.txt"), "NTFS export payload\n");
    const auto expectedNtfs = exportFiles(QStringLiteral("/mnt/target"));
    mountExportSource(base, false, QStringLiteral("ntfs"));
    captureGuestImage(QStringLiteral("ntfs-iso"), QStringLiteral("/run/save-output/captured-ntfs.iso"), originalDisk);
    require(!targetMounted(rawPartition) && ntfs.waitForFinished(5000) && ntfs.exitCode() == 0, "desktop NTFS source unmounted and daemon exited");
    mountIso.start(QStringLiteral("/usr/bin/mount"), {QStringLiteral("-t"), QStringLiteral("udf"), QStringLiteral("-o"), QStringLiteral("loop,ro"),
        QStringLiteral("/run/save-output/captured-ntfs.iso"), QStringLiteral("/run/iso-readback")});
    require(mountIso.waitForFinished(10000) && mountIso.exitCode() == 0, "NTFS export kernel UDF mount");
    const auto actualNtfs = exportFiles(QStringLiteral("/run/iso-readback"));
    require(actualNtfs == expectedNtfs && umount("/run/iso-readback") == 0, "NTFS export complete independent readback");
    QJsonObject ntfsFiles;
    for (auto item = actualNtfs.cbegin(); item != actualNtfs.cend(); ++item) ntfsFiles.insert(item.key(), QString::fromLatin1(item.value()));
    writeGuestFile(QStringLiteral("/run/save-output/ntfs-files.json"), QJsonDocument(ntfsFiles).toJson());
    const QByteArray authorizations = readFile(QStringLiteral("/run/save-authorizations"));
    require(authorizations == "launch\nlaunch\nlaunch\nlaunch\nlaunch\nlaunch\n", "one source authorization per whole job");
    auto restore = compressedRequest(base, QStringLiteral("/run/save-output/captured.vhdx"), base.size, originalDisk, IMG_COMPRESSION_VHDX);
    publishTargetMount(rawObjects, restore, {}, rawPartition);
    runCompressedQtCase(restore);
    require(diskDigest() == originalDisk, "mounted-source VHDX restored original bootable disk");
    require(umount("/run/save-output") == 0, "export batch destination synced and released");
    printf("RUFUS_SAVE_EXPORT_DONE SHA256=%s FILES=%lld NTFSFILES=%lld AUTHORIZATIONS=6\n", originalDisk.toHex().constData(), static_cast<long long>(actualFiles.size()), static_cast<long long>(actualNtfs.size()));
}

// Only the guarded guest sets BLKROSET; production capture never clears it.
void setCaptureReadOnly(bool readOnly)
{
    for (const char* path : {"/dev/sda", "/dev/sda1"}) {
        const int descriptor = open(path, O_RDONLY | O_CLOEXEC);
        int value = readOnly;
        require(descriptor >= 0 && ioctl(descriptor, BLKROSET, &value) == 0, "guest source write-protection set");
        require(ioctl(descriptor, BLKROGET, &value) == 0 && value == int(readOnly) && close(descriptor) == 0,
            "guest source write-protection verified");
    }
}
void publishCaptureReadOnly(const rufus::PrepareImageRequest& base, bool readOnly, bool gpt = false)
{
    const QDBusMessage query = QDBusMessage::createMethodCall(base.serviceOwner, QStringLiteral("/org/freedesktop/UDisks2"),
        QStringLiteral("org.freedesktop.DBus.ObjectManager"), QStringLiteral("GetManagedObjects"));
    const QDBusReply<rufus::UDisksObjects> snapshot = QDBusConnection::systemBus().call(query);
    require(snapshot.isValid(), "capture metadata snapshot");
    auto objects = snapshot.value();
    for (auto item = objects.begin(); item != objects.end(); ++item) {
        auto block = item->find(QStringLiteral("org.freedesktop.UDisks2.Block"));
        if (block != item->end()) (*block)[QStringLiteral("ReadOnly")] = readOnly;
    }
    if (gpt) objects[QDBusObjectPath(base.objectPath)][QStringLiteral("org.freedesktop.UDisks2.PartitionTable")][QStringLiteral("Type")] = QStringLiteral("gpt");
    mountFixtureControl(QStringLiteral("SetObjects"), {QVariant::fromValue(objects)});
}
void checkCaptureReadOnly(const QByteArray& expected)
{
    const int descriptor = open("/dev/sda", O_RDONLY | O_CLOEXEC);
    int value = 0;
    require(descriptor >= 0 && ioctl(descriptor, BLKROGET, &value) == 0 && value == 1 && close(descriptor) == 0,
        "capture retained kernel write-protection");
    require(diskDigest() == expected, "read-only source unchanged by capture");
}
void runReadOnlySaveCases(rufus::PrepareImageRequest base, const QByteArray& originalDisk)
{
    require(QDir().mkpath(QStringLiteral("/run/save-output")) && mount("/dev/vda", "/run/save-output", "ext4", 0, nullptr) == 0,
        "read-only capture destination mounted");
    require(chown("/run/save-output", 1000, 1000) == 0, "capture destination belongs to caller");
    mountFixtureControl(QStringLiteral("OpenFile"), {QStringLiteral("/dev/sda")});
    publishTargetMount(rawObjects, base, {}, rawPartition);
    setCaptureReadOnly(true);
    publishCaptureReadOnly(base, true);
    require(rufus::CheckPrepareDeviceSelection(QDBusConnection::systemBus(), base) == ENOTSUP,
        "write snapshot refuses known read-only device");
    captureGuestImage(QStringLiteral("vhd"), QStringLiteral("/run/save-output/captured.vhd"), originalDisk);
    checkCaptureReadOnly(originalDisk);
    mountExportSource(base, true);
    publishCaptureReadOnly(base, true);
    captureGuestImage(QStringLiteral("mounted-vhdx"), QStringLiteral("/run/save-output/captured.vhdx"), originalDisk);
    checkCaptureReadOnly(originalDisk);
    mountExportSource(base, true);
    const auto expectedFiles = exportFiles(QStringLiteral("/mnt/target"));
    publishCaptureReadOnly(base, true);
    captureGuestImage(QStringLiteral("iso"), QStringLiteral("/run/save-output/captured.iso"), originalDisk);
    checkCaptureReadOnly(originalDisk);
    require(QDir().mkpath(QStringLiteral("/run/iso-readback")), "read-only export oracle directory");
    QProcess mountIso;
    mountIso.setProcessChannelMode(QProcess::ForwardedChannels);
    mountIso.start(QStringLiteral("/usr/bin/mount"), {QStringLiteral("-t"), QStringLiteral("udf"), QStringLiteral("-o"), QStringLiteral("loop,ro"),
        QStringLiteral("/run/save-output/captured.iso"), QStringLiteral("/run/iso-readback")});
    require(mountIso.waitForFinished(10000) && mountIso.exitCode() == 0, "read-only ISO kernel UDF mount");
    const auto actualFiles = exportFiles(QStringLiteral("/run/iso-readback"));
    require(actualFiles == expectedFiles && umount("/run/iso-readback") == 0, "read-only ISO independent file readback");
    QJsonObject files;
    for (auto item = actualFiles.cbegin(); item != actualFiles.cend(); ++item) files.insert(item.key(), QString::fromLatin1(item.value()));
    writeGuestFile(QStringLiteral("/run/save-output/iso-files.json"), QJsonDocument(files).toJson());
    // Stale writable metadata must not bypass the existing kernel write check.
    publishCaptureReadOnly(base, false);
    rufus::PrepareDeviceTarget writer(base, rufus::PrepareDeviceTarget::Table::Raw);
    require(writer.open(1000) == EROFS, "kernel read-only state refuses stale writable metadata");
    writer.finishRaw();
    checkCaptureReadOnly(originalDisk);
    setCaptureReadOnly(false);
    QFile gpt(QStringLiteral("/save-gpt.raw"));
    require(gpt.open(QIODevice::ReadOnly), "GPT test source opened");
    const int disk = open("/dev/sda", O_RDWR | O_EXCL | O_CLOEXEC);
    require(disk >= 0, "guest GPT source preparation");
    quint64 offset = 0;
    while (!gpt.atEnd()) {
        const QByteArray bytes = gpt.read(65536);
        require(!bytes.isEmpty() && pwrite(disk, bytes.constData(), size_t(bytes.size()), off_t(offset)) == bytes.size(), "guest GPT source written");
        offset += quint64(bytes.size());
    }
    require(offset == base.size && fsync(disk) == 0 && close(disk) == 0, "guest GPT source synced");
    gpt.close();
    const QByteArray gptHash = diskDigest();
    publishTargetMount(rawObjects, base, {}, rawPartition);
    setCaptureReadOnly(true);
    publishCaptureReadOnly(base, true, true);
    captureGuestImage(QStringLiteral("ffu"), QStringLiteral("/run/save-output/captured.ffu"), gptHash);
    checkCaptureReadOnly(gptHash);
    require(readFile(QStringLiteral("/run/save-authorizations")) == "launch\nlaunch\n", "one helper authorization per mounted source job");
    require(QDir(QStringLiteral("/run")).entryList({QStringLiteral("rufus-save-*")}, QDir::Dirs).isEmpty(), "private source mounts cleaned");
    setCaptureReadOnly(false);
    auto restore = compressedRequest(base, QStringLiteral("/run/save-output/captured.vhdx"), base.size, originalDisk, IMG_COMPRESSION_VHDX);
    publishTargetMount(rawObjects, restore, {}, rawPartition);
    runCompressedQtCase(restore);
    require(diskDigest() == originalDisk && umount("/run/save-output") == 0, "representative read-only capture restored and destination released");
    printf("RUFUS_SAVE_READ_ONLY_DONE SHA256=%s FFUSHA256=%s FILES=%lld HELPERS=2 UDISKS=2\n", originalDisk.toHex().constData(), gptHash.toHex().constData(), static_cast<long long>(actualFiles.size()));
}
