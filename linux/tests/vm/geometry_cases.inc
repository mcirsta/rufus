// SPDX-License-Identifier: GPL-3.0-or-later
// Included only in the guarded, disposable Linux guest driver.
void geometryAlias(const QByteArray& node)
{
    struct stat info;
    require(stat(node.constData(), &info) == 0 && S_ISBLK(info.st_mode), "geometry partition node");
    const QByteArray alias = QByteArray("/dev/block/") + QByteArray::number(major(info.st_rdev)) + ':'
        + QByteArray::number(minor(info.st_rdev));
    require(symlink(node.constData(), alias.constData()) == 0 || errno == EEXIST, "geometry partition alias");
}

rufus::PrepareImageRequest geometryRequest(const rufus::PrepareImageRequest& base, int geometry)
{
    QList<QByteArray> matches;
    for (const QString& name : QDir(QStringLiteral("/sys/block")).entryList({QStringLiteral("sd*")}, QDir::Dirs)) {
        const QString queue = QStringLiteral("/sys/block/") + name + QStringLiteral("/queue/");
        const QByteArray logical = geometry == 2 ? QByteArray("4096") : QByteArray("512");
        const QByteArray physical = geometry ? QByteArray("4096") : QByteArray("512");
        if (readFile(queue + QStringLiteral("logical_block_size")).trimmed() == logical
            && readFile(queue + QStringLiteral("physical_block_size")).trimmed() == physical)
            matches.append(QFile::encodeName(QStringLiteral("/dev/") + name));
    }
    require(matches.size() == 1, "unique real geometry stick despite asynchronous USB enumeration");
    const QByteArray node = matches.first();
    const int disk = open(node.constData(), O_RDWR | O_EXCL | O_CLOEXEC);
    require(disk >= 0, "geometry seed claim");
    int logical = 0, physical = 0;
    quint64 capacity = 0;
    require(ioctl(disk, BLKSSZGET, &logical) == 0 && ioctl(disk, BLKPBSZGET, &physical) == 0
        && ioctl(disk, BLKGETSIZE64, &capacity) == 0, "actual device geometry");
    require(logical == (geometry == 2 ? 4096 : 512) && physical == (geometry ? 4096 : 512)
        && capacity == diskBytes, "three independently emulated geometries");
    struct stat info;
    require(fstat(disk, &info) == 0, "geometry disk identity");
    const GPT_IMAGE_OPTIONS seed = {capacity, quint32(logical), 63, quint32(logical), quint64(info.st_rdev),
        0, 0, false, false, quint32(physical)};
    GPT_IMAGE_RESULT seeded;
    require(CreateGptImagePartition(disk, &seed, &seeded) == 0 && ioctl(disk, BLKRRPART, 0) == 0
        && close(disk) == 0, "geometry seed partition and kernel refresh");
    geometryAlias(node + '1');
    const QDBusObjectPath member(QStringLiteral("/org/freedesktop/UDisks2/block_devices/") + QString::fromLatin1(node.mid(5)));
    const QDBusObjectPath drive(QStringLiteral("/org/freedesktop/UDisks2/drives/geometry"));
    rufus::UDisksObjects objects;
    objects[member][QStringLiteral("org.freedesktop.UDisks2.Block")] = {
        {QStringLiteral("Drive"), QVariant::fromValue(drive)}, {QStringLiteral("Device"), node + '\0'},
        {QStringLiteral("Size"), QVariant::fromValue(capacity)},
        {QStringLiteral("DeviceNumber"), QVariant::fromValue(quint64(info.st_rdev))},
        {QStringLiteral("ReadOnly"), false}, {QStringLiteral("HintSystem"), false},
        {QStringLiteral("HintIgnore"), false}, {QStringLiteral("Id"), QStringLiteral("geometry-fixture")}};
    objects[drive][QStringLiteral("org.freedesktop.UDisks2.Drive")] = {
        {QStringLiteral("ConnectionBus"), QStringLiteral("usb")}, {QStringLiteral("Removable"), true},
        {QStringLiteral("MediaRemovable"), true}, {QStringLiteral("MediaAvailable"), true},
        {QStringLiteral("Optical"), false}, {QStringLiteral("Serial"), QStringLiteral("RUFUS-GEOMETRY")},
        {QStringLiteral("TimeDetected"), QVariant::fromValue(quint64(1))},
        {QStringLiteral("TimeMediaDetected"), QVariant::fromValue(quint64(2))}};
    mountFixtureControl(QStringLiteral("SetObjects"), {QVariant::fromValue(objects)});
    rufus::Device device;
    require(rufus::DescribeUDisksDevice(objects, member.path(), device), "geometry discovery");
    rufus::PrepareImageRequest request;
    request.path = device.path; request.device = device.number; request.size = device.size;
    request.objectPath = device.objectPath; request.identity = device.identity; request.serviceOwner = base.serviceOwner;
    request.diskSequence = rufus::ReadDiskSequence(device.number); request.isDevice = request.nonBootable = true;
    request.logicalSectorSize = quint32(logical); request.physicalSectorSize = quint32(physical);
    require(rufus::CheckPrepareDeviceSelection(QDBusConnection::systemBus(), request) == 0, "geometry frozen snapshot");
    for (bool changeLogical : {true, false}) {
        rufus::PrepareImageRequest stale = request;
        if (changeLogical) stale.logicalSectorSize = logical == 512 ? 4096 : 512;
        else stale.physicalSectorSize = physical == 512 ? 4096 : physical * 2;
        require(rufus::CheckPrepareDeviceSelection(QDBusConnection::systemBus(), stale) == ESTALE,
            "stale logical or physical geometry refused before authorization");
    }
    printf("RUFUS_GEOMETRY_STALE LOGICAL=%d PHYSICAL=%d REFUSALS=2 BEFORE_AUTH=1\n", logical, physical);
    rawObjects = objects;
    return request;
}

void geometryReadback(const rufus::PrepareImageRequest& request, int job)
{
    GPT_IMAGE_RESULT layout;
    require(rufus::PlanFormatPartition(request, layout) == 0, "geometry readback extent");
    require(request.logicalSectorSize != 0 && request.size >= request.logicalSectorSize
        && layout.offset <= request.size && layout.size <= request.size - layout.offset && layout.size >= 4096,
        "bounded geometry readback range");
    const QByteArray path = QFile::encodeName(request.path);
    const int disk = open(path.constData(), O_RDONLY | O_EXCL | O_CLOEXEC);
    require(disk >= 0, "geometry joined helper releases whole device");
    const QByteArray table = readBytes(disk, 0, int(request.logicalSectorSize));
    if (request.mbr) {
        const uchar* entry = reinterpret_cast<const uchar*>(table.constData() + 446);
        require(quint64(qFromLittleEndian<quint32>(entry + 8)) * request.logicalSectorSize == layout.offset
            && quint64(qFromLittleEndian<quint32>(entry + 12)) * request.logicalSectorSize == layout.size,
            "MBR geometry uses logical sector units");
        require(table[510] == char(0x55) && table[511] == char(0xaa), "original MBR signature");
    } else if (!request.sfd) {
        const QByteArray header = readBytes(disk, request.logicalSectorSize, int(request.logicalSectorSize));
        const QByteArray backup = readBytes(disk, request.size - request.logicalSectorSize, int(request.logicalSectorSize));
        require(header.startsWith("EFI PART") && backup.startsWith("EFI PART"), "GPT at real logical sector boundaries");
        const uchar* h = reinterpret_cast<const uchar*>(header.constData());
        require(qFromLittleEndian<quint64>(h + 24) == 1
            && qFromLittleEndian<quint64>(h + 32) == request.size / request.logicalSectorSize - 1,
            "GPT reciprocal header addresses");
        const quint64 entriesLba = qFromLittleEndian<quint64>(h + 72);
        require(entriesLba < request.size / request.logicalSectorSize, "bounded GPT entries address");
        const quint64 entries = entriesLba * request.logicalSectorSize;
        const QByteArray first = readBytes(disk, entries, 128);
        const uchar* entry = reinterpret_cast<const uchar*>(first.constData());
        const quint64 begin = qFromLittleEndian<quint64>(entry + 32), end = qFromLittleEndian<quint64>(entry + 40);
        require(begin <= end && end < request.size / request.logicalSectorSize,
            "bounded GPT partition sector range");
        require(begin * request.logicalSectorSize == layout.offset
            && (end - begin + 1)
                * request.logicalSectorSize == layout.size, "GPT partition extent uses real sector units");
    } else require(layout.offset == 0 && layout.size == request.size, "SFD covers the exact whole device");
    const QByteArray boot = readBytes(disk, layout.offset, 4096);
    if (request.fileSystem == FS_FAT16 || request.fileSystem == FS_FAT32 || request.fileSystem == FS_NTFS) {
        const uchar* b = reinterpret_cast<const uchar*>(boot.constData());
        require(qFromLittleEndian<quint16>(b + 11) == request.logicalSectorSize
            && quint32(b[13]) * request.logicalSectorSize == request.clusterSize,
            "FAT/NTFS BPB preserves actual sector and selected cluster geometry");
        require(qFromLittleEndian<quint32>(b + 28) == layout.offset / request.logicalSectorSize,
            "FAT/NTFS hidden sectors use logical units");
    } else if (request.fileSystem == FS_EXFAT) {
        const unsigned shift = quint8(boot[108]), clusterShift = shift + quint8(boot[109]);
        require(shift < 32 && clusterShift < 32, "bounded exFAT geometry shifts");
        require(boot.mid(3, 8) == "EXFAT   " && (quint32(1) << shift) == request.logicalSectorSize
            && (quint32(1) << clusterShift) == request.clusterSize,
            "exFAT independently decoded geometry");
    } else if (request.fileSystem == FS_EXT2 || request.fileSystem == FS_EXT3) {
        const uchar* b = reinterpret_cast<const uchar*>(boot.constData() + 1024);
        const quint32 shift = qFromLittleEndian<quint32>(b + 24);
        require(shift <= 6, "bounded ext block exponent");
        require(qFromLittleEndian<quint16>(b + 56) == 0xef53
            && (quint32(1024) << shift) >= request.logicalSectorSize
            && bool(qFromLittleEndian<quint32>(b + 92) & 4) == (request.fileSystem == FS_EXT3),
            "ext block size and journal geometry");
    } else {
        const QByteArray anchor = readBytes(disk, layout.offset + 256 * request.logicalSectorSize, int(request.logicalSectorSize));
        const uchar* a = reinterpret_cast<const uchar*>(anchor.constData());
        require(qFromLittleEndian<quint16>(a) == 2 && qFromLittleEndian<quint32>(a + 12) == 256,
            "UDF default block follows actual device logical geometry");
    }
    require(close(disk) == 0, "geometry metadata readback close");
    const QByteArray node = path + (request.sfd ? QByteArray() : QByteArray("1"));
    const char* filesystem = request.fileSystem == FS_UDF ? "udf" : request.fileSystem == FS_EXT2 ? "ext2"
        : request.fileSystem == FS_EXT3 ? "ext3" : request.fileSystem == FS_NTFS ? "ntfs"
        : request.fileSystem == FS_EXFAT ? "exfat" : "vfat";
    require(mount(node.constData(), "/mnt/geometry", filesystem, MS_RDONLY | MS_NODEV | MS_NOSUID | MS_NOEXEC,
        request.fileSystem == FS_UDF ? "iocharset=utf8" : "") == 0, "independent kernel geometry mount");
    if (request.fileSystem == FS_EXT2 || request.fileSystem == FS_EXT3) {
        const QStringList entries = QDir(QStringLiteral("/mnt/geometry")).entryList(QDir::AllEntries | QDir::NoDotAndDotDot);
        struct stat found;
        require(entries == QStringList{QStringLiteral("lost+found")}
            && lstat("/mnt/geometry/lost+found", &found) == 0 && S_ISDIR(found.st_mode),
            "independent clean ext root directory and recovery directory");
    } else require(readFile(QStringLiteral("/mnt/geometry/geometry.txt")) == "B5 exact native geometry payload\n"
        && readFile(QStringLiteral("/mnt/geometry/EFI/BOOT/BOOTX64.EFI")) == readFile(QStringLiteral("/geometry-original.efi")),
        "independent filesystem payload bytes");
    require(umount("/mnt/geometry") == 0, "independent geometry mount cleanup");
    printf("RUFUS_GEOMETRY_READBACK JOB=%d FS=%d LOGICAL=%u PHYSICAL=%u LAYOUT=%s BYTES=1 CLAIM=1\n", job,
        request.fileSystem, request.logicalSectorSize, request.physicalSectorSize, request.sfd ? "SFD" : request.mbr ? "MBR" : "GPT");
}

void runGeometryCases(const rufus::PrepareImageRequest& base)
{
    require(QDir().mkpath(QStringLiteral("/dev/block")) && QDir().mkpath(QStringLiteral("/mnt/geometry")), "geometry fixture directories");
    require(mkdir("/run/caller-home", 0700) == 0 && mount("tmpfs", "/run/caller-home", "tmpfs", 0, "size=16m") == 0,
        "independent geometry source backing");
    require(QFile::copy(QStringLiteral("/geometry-overlay.zip"), QStringLiteral("/run/caller-home/geometry-overlay.zip"))
        && chown("/run/caller-home/geometry-overlay.zip", 1000, 1000) == 0, "geometry caller source ownership");
    const int filesystems[] = {FS_FAT16, FS_NTFS, FS_EXFAT, FS_UDF, FS_EXT2, FS_EXT3, FS_FAT32};
    int job = 0;
    // Leave the ordinary 512-byte stick as FAT32 for one representative EFI boot.
    for (int geometry : {2, 1, 0}) {
        rufus::PrepareImageRequest device = geometryRequest(base, geometry);
        for (int index = 0; index < 7; index++) {
            for (const char* marker : {"/run/format-launches", "/run/format-exit-held"}) QFile::remove(QString::fromLatin1(marker));
            rufus::PrepareImageRequest request = device;
            request.fileSystem = filesystems[index]; request.volumeId = 0; request.label = QStringLiteral("B5 GEOMETRY");
            request.clusterSize = request.fileSystem == FS_FAT16 ? 8192 : request.fileSystem == FS_EXFAT ? 128 * 1024
                : request.fileSystem == FS_UDF || request.fileSystem == FS_EXT2 || request.fileSystem == FS_EXT3 ? 256 : 4096;
            const int table = (index + geometry) % 3;
            request.mbr = table == 0; request.sfd = table == 2;
            request.quickFormat = !(geometry == 1 && request.fileSystem == FS_FAT32);
            printf("RUFUS_GEOMETRY_START JOB=%d FS=%d LOGICAL=%u PHYSICAL=%u LAYOUT=%s QUICK=%d\n", job,
                request.fileSystem, request.logicalSectorSize, request.physicalSectorSize,
                request.sfd ? "SFD" : request.mbr ? "MBR" : "GPT", int(request.quickFormat));
            fflush(stdout);
            if (request.fileSystem != FS_EXT2 && request.fileSystem != FS_EXT3)
                require(rufus::CapturePrepareArchive(QStringLiteral("/run/caller-home/geometry-overlay.zip"), request.archive) == 0,
                    "capture geometry overlay for eligible filesystem");
            publishTargetMount(rawObjects, request, {});
            QProcess client; client.setProcessChannelMode(QProcess::ForwardedChannels);
            client.start(QStringLiteral("/usr/bin/test_native_format_qt_guest"), {});
            require(client.waitForStarted(10000), "geometry ordinary Qt client start");
            const QByteArray packet = QJsonDocument(rufus::EncodePrepareRequest(request)).toJson(QJsonDocument::Compact) + '\n';
            require(packet.size() < rufus::PrepareMessageBytes && client.write(packet) == packet.size(), "geometry typed request pipe");
            client.closeWriteChannel();
            require(client.waitForFinished(240000) && client.exitCode() == 0, "geometry ordinary Qt client joined");
            if (!request.sfd) geometryAlias(QFile::encodeName(request.path) + '1');
            geometryReadback(request, job++);
        }
    }
    require(umount("/run/caller-home") == 0 && rmdir("/mnt/geometry") == 0, "geometry fixture cleanup");
    puts("RUFUS_GEOMETRY_DONE JOBS=21 GEOMETRIES=3 FORMATTERS=7 AUTH=1 JOINED=1 READBACK=1");
}
