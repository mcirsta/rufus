// SPDX-License-Identifier: GPL-3.0-or-later
void runZeroDeviceQtCases(const rufus::PrepareImageRequest& base, const QString& rawPath, const QByteArray& expectedDisk)
{
    constexpr qint64 regionBytes = 32 * 1024 * 1024;
    constexpr qsizetype ioBytes = 1024 * 1024;
    const int disk = open("/dev/sda", O_RDWR | O_EXCL | O_CLOEXEC);
    require(disk >= 0, "seed zeroing fixture disk");
    for (qint64 offset = 0; offset < qint64(base.size); offset += ioBytes)
    {
        const QByteArray bytes(ioBytes, offset < regionBytes ? '\0' : char(0xa5));
        require(pwrite(disk, bytes.constData(), size_t(bytes.size()), off_t(offset)) == bytes.size(), "mixed zeroing seed");
    }
    require(fsync(disk) == 0 && close(disk) == 0, "zeroing seed synchronized");
    rufus::PrepareImageRequest request;
    request.rawWrite = request.isDevice = true;
    request.path = base.path; request.device = base.device; request.size = base.size;
    request.objectPath = base.objectPath; request.serviceOwner = base.serviceOwner;
    request.identity = base.identity; request.diskSequence = base.diskSequence;
    request.zeroMode = ZERO_FAST;
    runCompressedQtCase(request, true);
    QCryptographicHash mixed(QCryptographicHash::Sha256);
    const QByteArray zeros(ioBytes, '\0'), erased(ioBytes, char(0xff));
    for (qint64 offset = 0; offset < qint64(base.size); offset += ioBytes)
        mixed.addData(offset < regionBytes ? zeros : erased);
    require(diskDigest() == mixed.result(), "fast zero preserves zero region and erases dirty region");
    runCompressedQtCase(request, true);
    require(diskDigest() == mixed.result(), "all-empty fast zero preserves zeros and erased bytes");
    request.zeroMode = ZERO_FULL;
    runCompressedQtCase(request, true);
    QCryptographicHash cleared(QCryptographicHash::Sha256);
    for (quint64 offset = 0; offset < base.size; offset += ioBytes) cleared.addData(zeros);
    require(diskDigest() == cleared.result(), "full zero writes every device byte");
    auto restore = base;
    require(rufus::CapturePrepareSource(rawPath, restore) == 0, "representative restore source identity");
    runCompressedQtCase(restore);
    require(diskDigest() == expectedDisk && QFile::remove(rawPath), "ordinary restore after zeroing and source cleanup");
    printf("RUFUS_ZERO_DEVICE_DONE SHA256=%s\n", expectedDisk.toHex().constData());
}
