// SPDX-License-Identifier: GPL-3.0-or-later
void runImageEntryQtCases(rufus::PrepareImageRequest base, const QString& rawPath, const QByteArray& expectedDisk)
{
    const QString alias = QStringLiteral("/run/caller-home/source.WIC");
    const QString packed = QStringLiteral("/run/caller-home/source.gz");
    require(QFile::copy(rawPath, alias), "marker override raw source");
    QFile marker(alias);
    require(marker.open(QIODevice::ReadWrite) && marker.seek(510)
        && marker.write(QByteArray(2, '\0')) == 2 && marker.flush() && marker.seek(0), "markerless raw bytes");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    require(hash.addData(&marker), "independent markerless disk hash");
    const QByteArray changed = hash.result();
    marker.close();
    require(chown(QFile::encodeName(alias).constData(), 1000, 1000) == 0, "markerless caller ownership");
    auto request = base;
    require(rufus::CapturePrepareSource(alias, request) == 0, "raw override source identity");
    runCompressedHelperCase({"raw-marker-required", ENOTSUP, 0, 0}, request);
    require(diskDigest() == expectedDisk, "default raw marker refusal preserves disk");
    request.ignoreBootMarker = true;
    runCompressedQtCase(request);
    require(diskDigest() == changed, "raw override whole-disk readback");
    gzipSource(alias, packed);
    request = compressedRequest(base, packed, base.size, changed);
    runCompressedHelperCase({"gzip-marker-required", ENOTSUP, 0, 0}, request);
    require(diskDigest() == changed, "default compressed marker refusal preserves disk");
    request.ignoreBootMarker = true;
    runCompressedQtCase(request);
    require(diskDigest() == changed, "compressed override whole-disk readback");
    require(QFile::remove(alias) && QFile::copy(rawPath, alias)
        && chown(QFile::encodeName(alias).constData(), 1000, 1000) == 0, "representative bootable raw source");
    request = base;
    require(rufus::CapturePrepareSource(alias, request) == 0, "normal raw source identity");
    runCompressedQtCase(request);
    require(diskDigest() == expectedDisk, "normal raw restore whole-disk readback");
    require(QFile::remove(alias) && QFile::remove(packed) && QFile::remove(rawPath), "image-entry sources released");
    printf("RUFUS_IMAGE_ENTRY_DONE SHA256=%s\n", expectedDisk.toHex().constData());
}
