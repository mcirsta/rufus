// SPDX-License-Identifier: GPL-3.0-or-later
// Included only by the guarded, disposable block-device guest driver.
void readToGoAttributes(const char* path, quint32 expected)
{
    quint32 encoded = 0;
    require(getxattr(path, "system.ntfs_attrib", &encoded, sizeof(encoded)) == sizeof(encoded), "independent DOS attribute readback");
    // Check READONLY/HIDDEN/SYSTEM/DIRECTORY/NORMAL, without constraining the
    // filesystem's archive/index bookkeeping flags.
    require((qFromLittleEndian(encoded) & 0x97) == expected, "stored boot protection and directory type");
}

void readToGoPolicy(const QString& path)
{
    hive_h* hive = hivex_open(QFile::encodeName(path).constData(), 0);
    require(hive != nullptr, "independent SYSTEM readback");
    hive_node_h node = hivex_root(hive);
    for (const char* part : {"ControlSet001", "Services", "partmgr", "Parameters"}) {
        require(node != 0, "SYSTEM policy parent"); node = hivex_node_get_child(hive, node, part);
    }
    require(node != 0, "SYSTEM policy node");
    const hive_value_h value = hivex_node_get_value(hive, node, "SanPolicy");
    require(value != 0 && hivex_value_dword(hive, value) == 4, "installed offline-internal policy");
    require(hivex_close(hive) == 0, "SYSTEM readback close");
}

void readToGoBcd(const QString& path, const rufus::PrepareImageRequest& request)
{
    hive_h* hive = hivex_open(QFile::encodeName(path).constData(), 0);
    require(hive != nullptr, "independent BCD readback");
    hive_node_h node = hivex_node_get_child(hive, hivex_root(hive), "Objects");
    const char* loader = request.windowsToGoTarget == TT_BIOS
        ? "{22222222-0000-0000-0000-000000000002}" : "{11111111-0000-0000-0000-000000000001}";
    for (const char* part : {loader, "Elements", "11000001"}) {
        require(node != 0, "BCD parent"); node = hivex_node_get_child(hive, node, part);
    }
    require(node != 0, "BCD device node");
    const hive_value_h value = hivex_node_get_value(hive, node, "Element");
    hive_type type; size_t size = 0;
    char* buffer = value ? hivex_value_value(hive, value, &type, &size) : nullptr;
    require(buffer != nullptr && type == hive_t_REG_BINARY && size == 88, "qualified BCD device");
    const QByteArray bytes(buffer, qsizetype(size)); free(buffer);
    const int disk = open("/dev/sda", O_RDONLY | O_CLOEXEC);
    require(disk >= 0, "BCD identity disk");
    if (request.mbr) {
        const quint64 start = readFile(QStringLiteral("/sys/class/block/sda1/start")).trimmed().toULongLong() * 512;
        require(qFromLittleEndian<quint32>(bytes.constBegin() + 52) == 1
            && qFromLittleEndian<quint64>(bytes.constBegin() + 32) == start
            && bytes.mid(56, 4) == readBytes(disk, 440, 4), "MBR BCD actual identity");
    } else {
        require(qFromLittleEndian<quint32>(bytes.constBegin() + 52) == 0
            && bytes.mid(56, 16) == readBytes(disk, 512 + 56, 16)
            && bytes.mid(32, 16) == readBytes(disk, 1024 + 2 * 128 + 16, 16), "GPT BCD actual Windows partition");
    }
    require(close(disk) == 0 && hivex_close(hive) == 0, "BCD readback close");
}

void runWindowsToGoCases(rufus::PrepareImageRequest request)
{
    require(QDir().mkpath(QStringLiteral("/run/caller-home")) && QDir().mkpath(QStringLiteral("/mnt/target")), "To Go directories");
    require(mount("tmpfs", "/run/caller-home", "tmpfs", 0, "size=64m") == 0, "separate To Go source backing");
    require(QDir().mkpath(QStringLiteral("/dev/block")), "partition device directory");
    for (unsigned partition = 1; partition <= 3; ++partition) {
        const QByteArray name = QByteArray("/dev/block/") + QByteArray::number(major(request.device)) + ':'
            + QByteArray::number(minor(request.device) + partition);
        const QByteArray destination = QByteArray("../sda") + QByteArray::number(partition);
        require(symlink(destination.constData(), name.constData()) == 0, "future kernel partition link");
    }
    request.windowsToGo = true; request.fileSystem = FS_NTFS; request.clusterSize = 4096;
    request.extendedLabel = false; request.label = QStringLiteral("TOGO"); request.volumeId = 0;
    request.windowsEdition = 2; request.windowsFlags = UNATTEND_OFFLINE_INTERNAL_DRIVES;
    const char* const sources[] = {"source.wim", "source.esd", "wim.iso", "esd.iso"};
    for (unsigned index = 0; index < 4; ++index) {
        const QString name = QString::fromLatin1(sources[index]);
        const QString path = QStringLiteral("/run/caller-home/") + name;
        require(QFile::copy(QLatin1Char('/') + name, path) && chown(QFile::encodeName(path).constData(), 1000, 1000) == 0,
            "independent To Go source");
        request.mbr = index >= 2; request.windowsToGoTarget = index == 2 ? TT_BIOS : TT_UEFI;
        request.uefiNtfs = index == 3;
        request.windowsInstallPath = index < 2 ? QString() : index == 2
            ? QStringLiteral("/sources/install.wim") : QStringLiteral("/sources/install.esd");
        require(rufus::CapturePrepareSource(path, request) == 0, "To Go source identity");
        for (const char* marker : {"/run/format-launches", "/run/format-exit-held"}) QFile::remove(QString::fromLatin1(marker));
        QProcess client; client.setProcessChannelMode(QProcess::ForwardedChannels);
        client.start(QStringLiteral("/usr/bin/test_windows_to_go_qt_guest"), {});
        require(client.waitForStarted(10000), "To Go Qt client start");
        const QByteArray packet = QJsonDocument(rufus::EncodePrepareRequest(request)).toJson(QJsonDocument::Compact) + '\n';
        require(client.write(packet) == packet.size(), "To Go identity pipe"); client.closeWriteChannel();
        require(client.waitForFinished(300000) && client.exitCode() == 0, "ordinary To Go client completed");
        const QByteArray partition = request.mbr ? QByteArray("/dev/sda1") : QByteArray("/dev/sda3");
        require(mount(partition.constData(), "/mnt/target", "ntfs3", MS_RDONLY | MS_NODEV | MS_NOSUID | MS_NOEXEC, nullptr) == 0,
            "independent NTFS3 readback");
        require(readFile(QStringLiteral("/mnt/target/payload")) == QByteArray(128 * 1024, 'A')
            && readFile(QStringLiteral("/mnt/target/edition.txt")) == "2", "selected edition and payload readback");
        readToGoPolicy(QStringLiteral("/mnt/target/Windows/System32/config/SYSTEM"));
        if (request.mbr) {
            readToGoBcd(request.windowsToGoTarget == TT_BIOS ? QStringLiteral("/mnt/target/Boot/BCD")
                : QStringLiteral("/mnt/target/EFI/Microsoft/Boot/BCD"), request);
            readToGoAttributes("/mnt/target/Boot", 0x16);
            readToGoAttributes("/mnt/target/bootmgr", 7);
            readToGoAttributes("/mnt/target/Boot/BCD", 6);
        }
        require(umount("/mnt/target") == 0, "NTFS readback cleanup");
        if (!request.mbr) {
            require(mount("/dev/sda1", "/mnt/target", "vfat", MS_RDONLY | MS_NODEV | MS_NOSUID | MS_NOEXEC, nullptr) == 0,
                "separate ESP readback");
            readToGoBcd(QStringLiteral("/mnt/target/EFI/Microsoft/Boot/BCD"), request);
            require(!readFile(QStringLiteral("/mnt/target/EFI/Boot/bootx64.efi")).isEmpty(), "ESP EFI fallback");
            require(umount("/mnt/target") == 0, "ESP readback cleanup");
        }
        require(!readFile(QStringLiteral("/proc/self/mountinfo")).contains(" /run/rufus-"), "no shared helper mount");
        printf("RUFUS_TOGO_READBACK SOURCE=%s MBR=%d TARGET=%d EDITION=2 POLICY=4 BCD=1\n", sources[index], int(request.mbr), request.windowsToGoTarget);
    }
    puts("RUFUS_TOGO_DONE FORMATS=4 AUTH=1 JOINED=1");
}
