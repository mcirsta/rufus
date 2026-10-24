// SPDX-License-Identifier: GPL-3.0-or-later
// Only included by the PID1-child/DMI guarded disposable guest driver.
void sourceBatchCommand(const QString& program, const QStringList& arguments)
{
    QProcess process; process.setProcessChannelMode(QProcess::ForwardedChannels);
    process.start(program, arguments);
    require(process.waitForStarted(10000) && process.waitForFinished(120000) && process.exitCode() == 0, "source batch tool");
}
QString sourceBatchLoop(const QString& file)
{
    QProcess process; process.start(QStringLiteral("/usr/bin/losetup"), {"--find", "--show", "--partscan", file});
    require(process.waitForStarted(10000) && process.waitForFinished(30000) && process.exitCode() == 0, "file-backed loop attach");
    const QString node = QString::fromLocal8Bit(process.readAllStandardOutput()).trimmed();
    require(node.startsWith(QStringLiteral("/dev/loop")), "positive loop node"); return node;
}
void sourceBatchPublish(const rufus::UDisksObjects& objects)
{
    auto message = QDBusMessage::createMethodCall(QStringLiteral("org.rufus.TestUDisks"), QStringLiteral("/org/freedesktop/UDisks2"),
        QStringLiteral("org.rufus.TestUDisks"), QStringLiteral("SetObjects"));
    message.setArguments({QVariant::fromValue(objects)});
    require(QDBusConnection::systemBus().call(message).type() == QDBusMessage::ReplyMessage, "source batch publish");
}
rufus::PrepareImageRequest sourceBatchDevice(const rufus::PrepareImageRequest& base, rufus::UDisksObjects& objects,
    const QString& node, quint64 bytes, bool optical = false)
{
    struct stat info = {}; require(stat(QFile::encodeName(node).constData(), &info) == 0 && S_ISBLK(info.st_mode), "source batch block node");
    const QString objectPath = QStringLiteral("/org/freedesktop/UDisks2/block_devices/") + node.mid(5);
    const QDBusObjectPath object(objectPath), drive(objectPath + QStringLiteral("_drive"));
    objects[object][QStringLiteral("org.freedesktop.UDisks2.Block")] = {{"Drive", QVariant::fromValue(optical ? drive : QDBusObjectPath("/"))},
        {"Device", QFile::encodeName(node) + '\0'}, {"Size", bytes}, {"DeviceNumber", quint64(info.st_rdev)},
        {"ReadOnly", optical}, {"HintSystem", true}, {"HintIgnore", false}};
    if (optical) objects[drive][QStringLiteral("org.freedesktop.UDisks2.Drive")] = {{"ConnectionBus", "scsi"},
        {"Removable", true}, {"MediaRemovable", true}, {"MediaAvailable", true}, {"Optical", true}};
    rufus::Device device;
    const bool described = rufus::DescribeUDisksDevice(objects, objectPath, device, false, optical ? rufus::DeviceAccess::Read : rufus::DeviceAccess::Write,
        rufus::AllDeviceClasses);
    if (!described) {
        rufus::NativeDiskFacts facts;
        const int error = rufus::ReadNativeDiskFacts(quint64(info.st_rdev), bytes, facts);
        fprintf(stderr, "SOURCE_DISCOVERY node=%s error=%d\n", qPrintable(node), error);
        const QString sysfs = QStringLiteral("/sys/dev/block/%1:%2").arg(major(info.st_rdev)).arg(minor(info.st_rdev));
        for (const char* field : {"dev", "size", "diskseq", "pid", "queue/logical_block_size"})
            fprintf(stderr, "SOURCE_FACT %s=%s\n", field, readFile(sysfs + QLatin1Char('/') + QLatin1String(field)).trimmed().constData());
    }
    require(described, "native drive-less/optical discovery");
    auto request = base; request.path = node; request.objectPath = objectPath; request.device = device.number; request.size = device.size;
    request.identity = device.identity; request.deviceClass = device.deviceClass; request.diskSequence = rufus::ReadDiskSequence(device.number);
    request.nonBootable = true; request.volumeId = 0; request.label = QStringLiteral("SOURCE BATCH"); request.clusterSize = 4096;
    sourceBatchPublish(objects); return request;
}
void sourceBatchClient(rufus::PrepareImageRequest request, const QString& command)
{
    for (const char* marker : {"/run/format-launches", "/run/format-exit-held", "/run/format-exit-release"}) QFile::remove(QString::fromLatin1(marker));
    QProcess client; client.setProcessChannelMode(QProcess::ForwardedChannels);
    const bool format = command == QLatin1String("format");
    if (command == QLatin1String("capture")) {
        require(chown("/run/caller-home", 1000, 1000) == 0 && chown("/run/caller-home/rufus.ini", 1000, 1000) == 0, "owned capture profile");
        client.start(QStringLiteral("/usr/bin/setpriv"), {"--reuid=1000", "--regid=1000", "--clear-groups", "/usr/bin/test_device_source_qt_guest", command});
    } else client.start(format ? QStringLiteral("/usr/bin/test_native_format_qt_guest") : QStringLiteral("/usr/bin/test_device_source_qt_guest"),
        format ? QStringList() : QStringList{command});
    require(client.waitForStarted(10000), "source batch Qt start");
    if (command == QLatin1String("capture")) request.deviceClass = rufus::DeviceClass::Usb; // Fixture envelope only; client independently identifies optical.
    const QByteArray frame = QJsonDocument(rufus::EncodePrepareRequest(request)).toJson(QJsonDocument::Compact) + '\n';
    require(frame.size() > 3 && client.write(frame) == frame.size(), "source batch frozen request"); client.closeWriteChannel();
    require(client.waitForFinished(180000) && client.exitCode() == 0, "source batch Qt joined");
}
void runDeviceSourceCases(const rufus::PrepareImageRequest& base)
{
    require(QDir().mkpath(QStringLiteral("/var/lock")), "NBD private socket directory");
    require(QDir().mkpath(QStringLiteral("/run/caller-home")) && QDir().mkpath(QStringLiteral("/mnt/readback")), "source batch directories");
    require(mount("tmpfs", "/run/caller-home", "tmpfs", MS_NODEV | MS_NOSUID, "size=32m") == 0, "source batch caller home");
    writeGuestFile(QStringLiteral("/etc/passwd"), "root:x:0:0:root:/root:/bin/bash\ncaller:x:1000:1000:Caller:/run/caller-home:/bin/bash\n");
    QProcess aliases;
    aliases.start(QStringLiteral("/usr/bin/bash"), {"-c", "mkdir -p /dev/block; while :; do for path in /sys/class/block/*/dev; do read numbers < \"$path\"; name=${path%/dev}; name=${name##*/}; ln -sfn /dev/\"$name\" /dev/block/\"$numbers\"; done; sleep .03; done"});
    require(aliases.waitForStarted(10000), "guest device aliases");
    auto snapshot = QDBusConnection::systemBus().call(QDBusMessage::createMethodCall(base.serviceOwner,
        QStringLiteral("/org/freedesktop/UDisks2"), QStringLiteral("org.freedesktop.DBus.ObjectManager"), QStringLiteral("GetManagedObjects")));
    const QDBusReply<rufus::UDisksObjects> reply(snapshot); require(reply.isValid(), "source batch baseline");
    auto objects = reply.value();
    QStringList nodes;
    QList<rufus::PrepareImageRequest> requests;
    for (const QString& kind : {QStringLiteral("loop"), QStringLiteral("nbd")}) {
        const QString path = QStringLiteral("/run/") + kind + QStringLiteral(".raw");
        QFile file(path); require(file.open(QIODevice::ReadWrite) && file.resize(64 * 1024 * 1024), "owned virtual backing"); file.close();
        QString node;
        if (kind == QLatin1String("loop")) node = sourceBatchLoop(path);
        else {
            sourceBatchCommand(QStringLiteral("/usr/bin/qemu-nbd"), {"--fork", "--persistent", "--format=raw", "--connect=/dev/nbd0", path}); node = QStringLiteral("/dev/nbd0");
            QElapsedTimer ready; ready.start();
            for (;;) {
                struct stat info = {}; rufus::NativeDiskFacts facts;
                if (stat("/dev/nbd0", &info) == 0 && rufus::ReadNativeDiskFacts(quint64(info.st_rdev), 64 * 1024 * 1024, facts) == 0) break;
                if (ready.elapsed() > 10000) break; // Detailed native refusal is emitted by sourceBatchDevice.
                usleep(25000);
            }
        }
        nodes.append(node);
        requests.append(sourceBatchDevice(base, objects, node, 64 * 1024 * 1024));
    }
    for (int index = 0; index < nodes.size(); ++index) {
        const QString node = nodes.at(index), kind = index == 0 ? QStringLiteral("loop") : QStringLiteral("nbd");
        auto request = requests.at(index);
        sourceBatchClient(request, QStringLiteral("format"));
        const QByteArray partition = QFile::encodeName(node + QStringLiteral("p1"));
        require(mount(partition.constData(), "/mnt/readback", "vfat", MS_RDONLY | MS_NODEV | MS_NOSUID, "utf8=1") == 0, "virtual independent FAT readback");
        require(umount("/mnt/readback") == 0, "virtual readback release");
        const int disk = open(QFile::encodeName(node).constData(), O_RDWR | O_EXCL | O_CLOEXEC); require(disk >= 0, "virtual claim after joined READY");
        unsigned char entry[128]; require(pread(disk, entry, sizeof(entry), 1024) == sizeof(entry), "virtual GPT identity");
        const QString guid = QUuid(qFromLittleEndian<quint32>(entry + 16), qFromLittleEndian<quint16>(entry + 20), qFromLittleEndian<quint16>(entry + 22),
            entry[24], entry[25], entry[26], entry[27], entry[28], entry[29], entry[30], entry[31]).toString();
        bool started = false;
        require(rufus::ToggleDeviceEsp(disk, {guid}, sourceBatchRecord, nullptr, started) == 0 && started, "fixture initial ESP");
        unsigned char header[92]; require(pread(disk, header, sizeof(header), 512) == sizeof(header), "excluded disk header");
        const QString diskGuid = QUuid(qFromLittleEndian<quint32>(header + 56), qFromLittleEndian<quint16>(header + 60), qFromLittleEndian<quint16>(header + 62),
            header[64], header[65], header[66], header[67], header[68], header[69], header[70], header[71]).toString();
        rufus::DeviceExclusions exclusions; exclusions.disks = {diskGuid};
        require(rufus::CheckDeviceExclusions(node, request.device, request.size, exclusions, disk) == EPERM, "live GPT exclusion");
        require(fsync(disk) == 0 && ioctl(disk, BLKRRPART) == 0 && close(disk) == 0, "fixture ESP sync");
        auto excluded = request; excluded.exclusions = exclusions;
        rufus::PrepareDeviceTarget excludedTarget(excluded, rufus::PrepareDeviceTarget::Table::Raw);
        require(excludedTarget.open(1000) == EPERM, "frozen GPT exclusion before acquisition/unmount");
        // A real competing whole-disk claim distinguishes the session override.
        // Identity and mounted-source checks still run when our claim is disabled.
        const int held = open(QFile::encodeName(node).constData(), O_RDONLY | O_EXCL | O_CLOEXEC);
        require(held >= 0, "competing exclusive claim");
        {
            rufus::PrepareDeviceTarget locked(request, rufus::PrepareDeviceTarget::Table::Raw);
            require(locked.open(1000) == EBUSY, "default native locking refuses competing claim");
            auto optOut = request; optOut.lockDevice = false;
            rufus::PrepareDeviceTarget unlocked(optOut, rufus::PrepareDeviceTarget::Table::Raw);
            require(unlocked.open(1000) == 0 && unlocked.finishRaw() == 0, "native locking override retains checks and opens");
        }
        require(close(held) == 0, "competing claim released");
        request.deviceOperation = rufus::ToggleEspType;
        sourceBatchClient(request, QStringLiteral("toggle"));
        sourceBatchClient(request, QStringLiteral("unlocked"));
        printf("RUFUS_DEVICE_SOURCE_NATIVE KIND=%s READBACK=1 ESP=1 LOCK_OVERRIDE=1\n", qPrintable(kind));
    }
    // The same filesystem under a file-backed loop must be recognized as source
    // and caller-home storage, including anonymous Btrfs and overlay layers.
    require(QDir().mkpath(QStringLiteral("/mnt/backing")) && QDir().mkpath(QStringLiteral("/mnt/overlay")), "backing fixture directories");
    QFile btrfs(QStringLiteral("/run/btrfs.raw")); require(btrfs.open(QIODevice::ReadWrite) && btrfs.resize(256 * 1024 * 1024), "Btrfs fixture"); btrfs.close();
    const QString backing = sourceBatchLoop(btrfs.fileName());
    sourceBatchCommand(QStringLiteral("/usr/bin/mkfs.btrfs"), {"-q", "-f", backing});
    require(mount(QFile::encodeName(backing).constData(), "/mnt/backing", "btrfs", MS_NODEV | MS_NOSUID, nullptr) == 0, "actual Btrfs mount");
    writeGuestFile(QStringLiteral("/mnt/backing/source.iso"), "retained source bytes");
    const int disk = open(QFile::encodeName(backing).constData(), O_RDONLY | O_CLOEXEC);
    const int source = open("/mnt/backing/source.iso", O_RDONLY | O_CLOEXEC);
    const int home = open("/run/caller-home", O_PATH | O_DIRECTORY | O_CLOEXEC);
    require(disk >= 0 && source >= 0 && home >= 0, "retained backing descriptors");
    rufus::NativeTargetMounts mounts;
    require(rufus::CheckNativeTargetMounts(disk, source, mounts, home) == EBUSY, "Btrfs source refused before unmount"); close(source);
    require(QDir().mkpath(QStringLiteral("/run/upper")) && QDir().mkpath(QStringLiteral("/run/work")), "overlay directories");
    require(mount("overlay", "/mnt/overlay", "overlay", 0, "lowerdir=/mnt/backing,upperdir=/run/upper,workdir=/run/work") == 0, "positive overlay backing");
    const int overlay = open("/mnt/overlay/source.iso", O_RDONLY | O_CLOEXEC); require(overlay >= 0, "overlay source");
    require(rufus::CheckNativeTargetMounts(disk, overlay, mounts, home) == EBUSY, "overlay protects Btrfs member"); close(overlay);
    require(umount("/mnt/overlay") == 0, "overlay release");
    require(rufus::CheckNativeTargetMounts(disk, -1, mounts, home) == 0 && mounts.volumes.size() == 1, "Btrfs automatic unmount plan");
    auto mountedSource = sourceBatchDevice(base, objects, backing, 256 * 1024 * 1024);
    const QDBusObjectPath mountedObject(mountedSource.objectPath);
    objects[mountedObject][QStringLiteral("org.freedesktop.UDisks2.Filesystem")] = {{"MountPoints", QVariant::fromValue(QList<QByteArray>{QByteArray("/mnt/backing\0", 13)})}};
    objects[mountedObject][QStringLiteral("org.freedesktop.UDisks2.Block")].insert(QStringLiteral("IdUsage"), QStringLiteral("filesystem"));
    sourceBatchPublish(objects);
    rufus::Device mountedDevice; require(rufus::DescribeUDisksDevice(objects, mountedSource.objectPath, mountedDevice), "mounted Btrfs description");
    require(rufus::MatchUDisksTargetMounts(objects, mountedDevice, 0, mountedSource.diskSequence, mounts).isEmpty(), "Btrfs anonymous filesystem and physical UDisks member match");
    int unmounted = 0;
    const int unmountError = rufus::UnmountNativeTarget(disk, -1, mounts, unmounted, home);
    fprintf(stderr, "BTRFS_UNMOUNT error=%d count=%d\n", unmountError, unmounted);
    require(unmountError == 0 && unmounted == 1, "Btrfs automatic unmount");
    QFile secondBtrfs(QStringLiteral("/run/btrfs-second.raw"));
    require(secondBtrfs.open(QIODevice::ReadWrite) && secondBtrfs.resize(256 * 1024 * 1024), "second Btrfs member fixture"); secondBtrfs.close();
    const QString secondBacking = sourceBatchLoop(secondBtrfs.fileName());
    sourceBatchCommand(QStringLiteral("/usr/bin/mkfs.btrfs"), {"-q", "-f", "-d", "raid1", "-m", "raid1", backing, secondBacking});
    require(mount(QFile::encodeName(backing).constData(), "/mnt/backing", "btrfs", MS_NODEV | MS_NOSUID, nullptr) == 0, "multi-member Btrfs mount");
    require(rufus::CheckNativeTargetMounts(disk, -1, mounts, home) == EBUSY, "multi-member target refused before unmount");
    require(umount("/mnt/backing") == 0, "owned multi-member fixture release");
    require(close(home) == 0 && close(disk) == 0, "backing release");
    sourceBatchCommand(QStringLiteral("/usr/bin/losetup"), {"--detach", secondBacking});
    QString optical;
    for (const QString& name : QDir(QStringLiteral("/sys/block")).entryList({QStringLiteral("sr*")}, QDir::Dirs)) optical = QStringLiteral("/dev/") + name;
    require(!optical.isEmpty(), "actual optical block device");
    auto capture = sourceBatchDevice(base, objects, optical, 2 * 1024 * 1024, true);
    auto openFile = QDBusMessage::createMethodCall(QStringLiteral("org.rufus.TestUDisks"), QStringLiteral("/org/freedesktop/UDisks2"), QStringLiteral("org.rufus.TestUDisks"), QStringLiteral("OpenFile"));
    openFile.setArguments({optical}); require(QDBusConnection::systemBus().call(openFile).type() == QDBusMessage::ReplyMessage, "optical acquisition setup");
    sourceBatchClient(capture, QStringLiteral("capture"));
    QFile saved(QStringLiteral("/run/caller-home/disc.iso")); require(saved.open(QIODevice::ReadOnly), "optical published image");
    QByteArray expected; expected.reserve(2 * 1024 * 1024); for (int i = 0; i < 2 * 1024 * 1024; ++i) expected.append(char(i & 255));
    require(saved.readAll() == expected, "raw optical exact independent readback"); saved.close();
    for (const char* path : {"/mapped-parent.vhdx", "/mapped-child.vhdx", "/mapped-child.vhdx.parents"})
        require(chown(path, 1000, 1000) == 0, "mapped source caller ownership");
    auto restore = base; restore.nonBootable = true;
    sourceBatchClient(restore, QStringLiteral("restore"));
    QFile restored(base.path); require(restored.open(QIODevice::ReadOnly), "mapped restore independent raw readback");
    QCryptographicHash restoredHash(QCryptographicHash::Sha256);
    require(restoredHash.addData(&restored) && restoredHash.result().toHex() == readFile(QStringLiteral("/etc/rufus-mapped-expected")).trimmed(), "complete mapped parent device bytes"); restored.close();
    puts("RUFUS_DEVICE_SOURCE_MAPPED_RESTORE BYTES=1 AUTH=1 JOINED=1");
    rufus::NativeDiskFacts usbFacts;
    require(rufus::ReadNativeDiskFacts(base.device, base.size, usbFacts) == 0 && usbFacts.usbIdsKnown, "native USB identifiers");
    rufus::DeviceExclusions usbExclusions; usbExclusions.usb.append((quint32(usbFacts.usbVendor) << 16) | usbFacts.usbProduct);
    require(rufus::CheckDeviceExclusions(base.path, base.device, base.size, usbExclusions) == EPERM, "original VID/PID exclusion");
    auto reset = base; reset.nonBootable = true; reset.deviceOperation = rufus::ResetUsbPort;
    sourceBatchClient(reset, QStringLiteral("reset"));
    const int refreshed = open(QFile::encodeName(base.path).constData(), O_RDONLY | O_CLOEXEC);
    require(refreshed >= 0 && ioctl(refreshed, BLKRRPART) == 0 && close(refreshed) == 0, "restored partition refresh");
    require(mount(QFile::encodeName(base.path + QLatin1Char('1')).constData(), "/mnt/readback", "vfat", MS_RDONLY | MS_NODEV | MS_NOSUID, "utf8=1") == 0, "restored FAT kernel readback");
    require(readFile(QStringLiteral("/mnt/readback/marker.txt")) == "B2 mapped parent device restore\n", "restored payload kernel bytes");
    require(umount("/mnt/readback") == 0, "restored payload release");
    aliases.terminate(); require(aliases.waitForFinished(10000), "alias worker joined");
    sourceBatchCommand(QStringLiteral("/usr/bin/qemu-nbd"), {"--disconnect", "/dev/nbd0"});
    for (const QString& node : {nodes.first(), backing}) sourceBatchCommand(QStringLiteral("/usr/bin/losetup"), {"--detach", node});
    require(umount("/run/caller-home") == 0, "caller home release");
    puts("RUFUS_DEVICE_SOURCE_DONE LOOP=1 NBD=1 OPTICAL=1 RESET=1 ESP=1 BACKING=1");
}
