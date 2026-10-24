// SPDX-License-Identifier: GPL-3.0-or-later
// Included in the guarded guest driver to share its real bus, source/home,
// mounted-target and helper-client fixtures. This is never a host CTest.
void gzipSource(const QString& sourcePath, const QString& gzipPath)
{
    QFile source(sourcePath);
    require(source.open(QIODevice::ReadOnly), "gzip fixture source");
    const QByteArray path = QFile::encodeName(gzipPath);
    gzFile output = gzopen(path.constData(), "wb1");
    require(output != nullptr, "gzip fixture output");
    constexpr int bufferBytes = 64 * 1024;
    while (!source.atEnd())
    {
        const QByteArray bytes = source.read(bufferBytes);
        require(!bytes.isEmpty() && gzwrite(output, bytes.constData(), unsigned(bytes.size())) == bytes.size(), "gzip fixture encode");
    }
    require(gzclose(output) == Z_OK, "gzip fixture close");
    require(chown(path.constData(), 1000, 1000) == 0, "gzip caller ownership");
}

void appendZip16(QByteArray& bytes, quint16 value)
{
    for (unsigned index = 0; index < 2; index++) bytes.append(char(value >> (8 * index)));
}

void appendZip32(QByteArray& bytes, quint32 value)
{
    for (unsigned index = 0; index < 4; index++) bytes.append(char(value >> (8 * index)));
}

void zipFixtureSource(const QString& sourcePath, const QString& packedPath, unsigned method = 8)
{
    const QByteArray decoded = readFile(sourcePath);
    require(decoded.size() >= 512 && quint64(decoded.size()) <= UINT32_MAX, "ZIP fixture decoded bounds");
    QByteArray packed;
    if (method == 0)
        packed = decoded;
    else if (method == 8)
    {
        packed.resize(qsizetype(compressBound(uLong(decoded.size()))));
        uLongf size = uLongf(packed.size());
        require(compress2(reinterpret_cast<Bytef*>(packed.data()), &size,
            reinterpret_cast<const Bytef*>(decoded.constData()), uLong(decoded.size()), Z_BEST_SPEED) == Z_OK
            && size >= 6, "independent guest ZIP deflate encoder");
        packed = packed.mid(2, qsizetype(size - 6)); // Discard the zlib wrapper and Adler32.
    }
    else
    {
        require(method == 12 || method == 14 || method == 95, "qualified ZIP fixture method");
        const QString streamPath = packedPath + QStringLiteral(".stream");
        QProcess encoder;
        encoder.setStandardInputFile(sourcePath);
        encoder.setStandardOutputFile(streamPath);
        if (method == 12)
            encoder.start(QStringLiteral("/usr/bin/bzip2"), {QStringLiteral("--stdout")});
        else if (method == 14)
            encoder.start(QStringLiteral("/usr/bin/xz"), {QStringLiteral("--stdout"), QStringLiteral("--threads=1"),
                QStringLiteral("--format=lzma"), QStringLiteral("--lzma1=dict=64KiB")});
        else
            encoder.start(QStringLiteral("/usr/bin/xz"), {QStringLiteral("--stdout"), QStringLiteral("--threads=1"),
                QStringLiteral("--check=crc64"), QStringLiteral("--lzma2=dict=64KiB")});
        require(encoder.waitForFinished(120000) && encoder.exitStatus() == QProcess::NormalExit
            && encoder.exitCode() == 0, "independent guest ZIP entry encoder");
        packed = readFile(streamPath);
        require(!packed.isEmpty() && QFile::remove(streamPath), "ZIP entry stream cleanup");
        if (method == 14)
        {
            require(packed.size() >= 18 && packed.mid(5, 8) == QByteArray(8, char(0xff)), "ZIP-LZMA EOS stream");
            packed = QByteArray::fromHex("09140500") + packed.left(5) + packed.mid(13);
        }
    }
    require(quint64(packed.size()) <= UINT32_MAX, "ZIP fixture packed bounds");
    const QByteArray name("disk.img");
    const quint16 flags = method == 14 ? 2 : 0; // ZIP-LZMA EOS marker.
    const quint32 crc = quint32(crc32(0, reinterpret_cast<const Bytef*>(decoded.constData()), uInt(decoded.size())));
    QByteArray archive;
    appendZip32(archive, 0x04034b50);
    appendZip16(archive, 20); appendZip16(archive, flags); appendZip16(archive, quint16(method));
    appendZip16(archive, 0); appendZip16(archive, 0);
    appendZip32(archive, crc); appendZip32(archive, quint32(packed.size())); appendZip32(archive, quint32(decoded.size()));
    appendZip16(archive, quint16(name.size())); appendZip16(archive, 0);
    archive += name;
    archive += packed;
    const quint32 directoryOffset = quint32(archive.size());
    appendZip32(archive, 0x02014b50);
    appendZip16(archive, 20); appendZip16(archive, 20);
    appendZip16(archive, flags); appendZip16(archive, quint16(method));
    appendZip16(archive, 0); appendZip16(archive, 0);
    appendZip32(archive, crc); appendZip32(archive, quint32(packed.size())); appendZip32(archive, quint32(decoded.size()));
    appendZip16(archive, quint16(name.size())); appendZip16(archive, 0); appendZip16(archive, 0);
    appendZip16(archive, 0); appendZip16(archive, 0);
    appendZip32(archive, 0); appendZip32(archive, 0);
    archive += name;
    const quint32 directorySize = quint32(archive.size()) - directoryOffset;
    appendZip32(archive, 0x06054b50);
    appendZip16(archive, 0); appendZip16(archive, 0); appendZip16(archive, 1); appendZip16(archive, 1);
    appendZip32(archive, directorySize); appendZip32(archive, directoryOffset); appendZip16(archive, 0);
    writeGuestFile(packedPath, archive);
    require(chown(QFile::encodeName(packedPath).constData(), 1000, 1000) == 0, "ZIP caller ownership");
    printf("RUFUS_ZIP_SOURCE METHOD=%u BYTES=%lld SHA256=%s\n", method,
        static_cast<long long>(archive.size()), QCryptographicHash::hash(archive, QCryptographicHash::Sha256).toHex().constData());
}

void compressedFixtureSource(const QString& sourcePath, const QString& packedPath, int compression,
    const QString& check = QStringLiteral("crc64"), unsigned lzmaDictionaryBytes = 65536)
{
    if (compression == BLED_COMPRESSION_GZIP)
    {
        gzipSource(sourcePath, packedPath);
        return;
    }
    if (compression == BLED_COMPRESSION_ZIP)
    {
        zipFixtureSource(sourcePath, packedPath);
        return;
    }
    const bool bzip2 = compression == BLED_COMPRESSION_BZIP2;
    const bool lzma = compression == BLED_COMPRESSION_LZMA;
    const bool lzw = compression == BLED_COMPRESSION_LZW;
    const bool zstd = compression == BLED_COMPRESSION_ZSTD;
    require(compression == BLED_COMPRESSION_XZ || bzip2 || lzma || lzw || zstd, "known compressed guest fixture provider");
    QProcess encoder;
    encoder.setStandardInputFile(sourcePath);
    encoder.setStandardOutputFile(packedPath);
    if (bzip2)
        encoder.start(QStringLiteral("/usr/bin/bzip2"), {QStringLiteral("--stdout")});
    else if (zstd)
        encoder.start(QStringLiteral("/usr/bin/zstd"), {QStringLiteral("--stdout"), QStringLiteral("--quiet")});
    else if (lzw)
        encoder.start(QStringLiteral("/usr/bin/rufus-test-compress"), {QStringLiteral("-c"), QStringLiteral("-f"),
            QStringLiteral("-b"), QStringLiteral("16")});
    else if (lzma)
        encoder.start(QStringLiteral("/usr/bin/xz"), {QStringLiteral("--stdout"), QStringLiteral("--threads=1"),
            QStringLiteral("--format=lzma"), QStringLiteral("--lzma1=dict=%1").arg(lzmaDictionaryBytes)});
    else
        encoder.start(QStringLiteral("/usr/bin/xz"), {QStringLiteral("--stdout"), QStringLiteral("--threads=1"),
            QStringLiteral("--check=") + check, QStringLiteral("--lzma2=dict=64KiB")});
    const int encoderTimeoutMs = bzip2 || lzma || lzw ? 120000 : 30000;
    require(encoder.waitForFinished(encoderTimeoutMs) && encoder.exitStatus() == QProcess::NormalExit
        && encoder.exitCode() == 0, "independent guest compressed encoder");
    const QByteArray path = QFile::encodeName(packedPath);
    require(chown(path.constData(), 1000, 1000) == 0, "compressed caller ownership");
    const QByteArray packed = readFile(packedPath);
    if (lzma)
    {
        require(packed.size() >= 18 && quint8(packed[0]) < 225, "independent LZMA header");
        quint32 dictionary = 0;
        for (unsigned index = 0; index < 4; index++) dictionary |= quint32(quint8(packed[1 + int(index)])) << (8 * index);
        require(dictionary == lzmaDictionaryBytes && packed.mid(5, 8) == QByteArray(8, char(0xff)), "LZMA dictionary and unknown-size header");
    }
    else require(lzw ? packed.startsWith(QByteArray::fromHex("1f9d90"))
        : bzip2 ? packed.startsWith("BZh") : zstd ? packed.startsWith(QByteArray::fromHex("28b52ffd"))
        : packed.startsWith(QByteArray::fromHex("fd377a585a00")),
        "independent compressed stream magic");
    printf("RUFUS_%s_SOURCE CHECK=%s BYTES=%lld SHA256=%s\n", zstd ? "ZSTD" : lzw ? "LZW" : lzma ? "LZMA" : bzip2 ? "BZIP2" : "XZ",
        lzw || lzma ? "none" : bzip2 ? "combined" : zstd ? "xxh64" : qPrintable(check),
        static_cast<long long>(packed.size()), QCryptographicHash::hash(packed, QCryptographicHash::Sha256).toHex().constData());
}

void corruptCompressedCheck(const QString& path, int compression)
{
    QFile source(path);
    require(source.open(QIODevice::ReadWrite) && source.size() >= 24, "compressed check fault source");
    qint64 offset = source.size() - 8; // Gzip's data CRC32 precedes its ISIZE.
    if (compression == BLED_COMPRESSION_XZ)
    {
        require(source.seek(7), "XZ check ID");
        const QByteArray id = source.read(1);
        require(id.size() == 1, "XZ check ID byte");
        const int checkBytes = id[0] == 1 ? 4 : id[0] == 4 ? 8 : id[0] == 10 ? 32 : 0;
        require(checkBytes != 0 && source.seek(source.size() - 8), "XZ footer backward size");
        const QByteArray backward = source.read(4);
        require(backward.size() == 4, "XZ footer index size bytes");
        quint64 indexWords = 0;
        for (unsigned i = 0; i < 4; i++) indexWords |= quint64(quint8(backward[int(i)])) << (8 * i);
        const quint64 indexBytes = (indexWords + 1) * 4;
        require(indexBytes + 12 + quint64(checkBytes) < quint64(source.size()), "XZ last block check bounds");
        offset = source.size() - 12 - qint64(indexBytes) - checkBytes;
    }
    if (compression == BLED_COMPRESSION_BZIP2) offset = 10; // First actual block CRC after BZh/magic.
    if (compression == BLED_COMPRESSION_ZIP) offset = 14; // Local entry CRC32.
    if (compression == BLED_COMPRESSION_ZSTD) offset = source.size() - 1; // XXH64 trailer.
    require(source.seek(offset), "compressed data check position");
    const QByteArray byte = source.read(1);
    require(byte.size() == 1 && source.seek(offset)
        && source.write(QByteArray(1, char(byte[0] ^ 1))) == 1, "compressed data check fault");
}

rufus::PrepareImageRequest compressedRequest(rufus::PrepareImageRequest request,
    const QString& path, quint64 decodedBytes, const QByteArray& hash, int compression = BLED_COMPRESSION_GZIP,
    unsigned ffuStore = 0)
{
    require(rufus::CapturePrepareSource(path, request) == 0, "capture compressed guest source");
    request.imageCompression = compression;
    request.ffuStore = ffuStore;
    request.decodedImageSize = decodedBytes;
    request.imageSectorSize = 512;
    request.decodedImageSha256 = hash;
    request.ignoreBootMarker = false;
    request.imageLayoutSha256.clear();
    if (compression == IMG_COMPRESSION_FFU) {
        QFile image(path); require(image.open(QIODevice::ReadOnly), "FFU layout source");
        const QByteArray header = image.read(32);
        require(header.size() == 32, "FFU layout header");
        quint32 catalog = 0, hashes = 0;
        for (unsigned i = 0; i < 4; i++) {
            catalog |= quint32(quint8(header[24 + i])) << (8 * i);
            hashes |= quint32(quint8(header[28 + i])) << (8 * i);
        }
        const QByteArray catalogBytes = image.read(catalog);
        require(catalogBytes.size() == catalog, "FFU catalog bytes");
        require(image.seek(32 + catalog), "FFU hash table offset");
        QByteArray table = image.read(hashes);
        require(table.size() == hashes, "FFU hash table bytes");
        if (catalog != 0)
            table.prepend(QByteArray("Rufus FFU catalog", 18) + QCryptographicHash::hash(catalogBytes, QCryptographicHash::Sha256));
        if (ffuStore != 0) { table.append(char(ffuStore)); table.append(char(ffuStore >> 8)); }
        request.imageLayoutSha256 = QCryptographicHash::hash(table, QCryptographicHash::Sha256);
    }
    return request;
}

QByteArray diskRangeDigest(quint64 offset)
{
    QFile disk(QStringLiteral("/dev/sda"));
    require(disk.open(QIODevice::ReadOnly) && disk.seek(qint64(offset)), "independent disk range");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    constexpr quint64 bufferBytes = 1024 * 1024;
    while (offset < diskBytes)
    {
        const QByteArray bytes = disk.read(qint64(qMin(bufferBytes, diskBytes - offset)));
        require(!bytes.isEmpty(), "independent range read");
        hash.addData(bytes);
        offset += quint64(bytes.size());
    }
    return hash.result();
}

unsigned observationValue(const QByteArray& report, const char* name)
{
    const QByteArray prefix = QByteArray(name) + '=';
    for (const QByteArray& field : report.simplified().split(' '))
        if (field.startsWith(prefix))
        {
            bool valid;
            const unsigned value = field.mid(prefix.size()).toUInt(&valid);
            require(valid, "numeric kernel claim observation");
            return value;
        }
    qFatal("Missing compressed observation: %s", name);
}

struct CompressedHelperCase
{
    const char* name;
    int error;
    quint64 written;
    quint64 verified;
    const char* fault = nullptr;
    bool poison = false;
};

void runCompressedHelperCase(const CompressedHelperCase& test, const rufus::PrepareImageRequest& request,
    const QByteArray& decoded = {}, const QByteArray& sparseExpected = {})
{
    const QByteArray name(test.name);
    const QByteArray reportName = (request.imageCompression == BLED_COMPRESSION_XZ ? QByteArray("xz-")
        : request.imageCompression == BLED_COMPRESSION_BZIP2 ? QByteArray("bzip2-")
        : request.imageCompression == BLED_COMPRESSION_LZMA ? QByteArray("lzma-")
        : request.imageCompression == BLED_COMPRESSION_LZW ? QByteArray("lzw-")
        : request.imageCompression == BLED_COMPRESSION_ZIP ? QByteArray("zip-")
        : request.imageCompression == BLED_COMPRESSION_ZSTD ? QByteArray("zstd-")
        : request.imageCompression == BLED_COMPRESSION_VTSI ? QByteArray("vtsi-")
        : request.imageCompression == IMG_COMPRESSION_VHD ? QByteArray("vhd-")
        : QFile::exists(QStringLiteral("/etc/rufus-lzw-qt-fixture")) ? QByteArray("lzw-gzip-") : QByteArray()) + name;
    const quint64 paddedBytes = rufus::PrepareRawWriteBytes(request);
    if (test.fault || test.poison)
    {
        const int disk = open("/dev/sda", O_RDWR | O_EXCL | O_CLOEXEC);
        const QByteArray poison(qsizetype(paddedBytes), char(0xa5));
        require(disk >= 0 && pwrite(disk, poison.constData(), size_t(poison.size()), 0) == poison.size()
            && fsync(disk) == 0 && close(disk) == 0, "seed distinct bytes before real compressed write");
    }
    const QByteArray before = diskDigest();
    const quint64 preservedTailStart = sparseExpected.isEmpty() ? test.written : paddedBytes;
    const QByteArray tail = test.written == 0 ? QByteArray() : diskRangeDigest(preservedTailStart);
    const int descriptors = descriptorCount();
    const QString childList = QStringLiteral("/proc/self/task/%1/children").arg(getpid());
    const QByteArray children = readFile(childList).simplified();
    publishTargetMount(rawObjects, request, {}, rawPartition);
    if (test.fault)
        writeGuestFile(QStringLiteral("/run/compressed-fault"), QByteArray(test.fault));
    const QString helper = test.fault ? QStringLiteral("/usr/bin/compressed_helper_guest_probe")
        : QStringLiteral("/usr/bin/rufus-prepare-helper");
    rufus::PrepareImageProcess process(helper);
    QSignalSpy completed(&process, &rufus::PrepareImageProcess::finished);
    QSignalSpy progress(&process, &rufus::PrepareImageProcess::rawProgressChanged);
    QSignalSpy logs(&process, &rufus::PrepareImageProcess::logMessage);
    require(process.start(request), "start compressed native guest helper");
    QElapsedTimer timer, held;
    timer.start();
    const bool completeXzImage = request.imageCompression == BLED_COMPRESSION_XZ && name == "bootable-restore";
    const bool completeBzip2Image = request.imageCompression == BLED_COMPRESSION_BZIP2 && name == "bootable-restore";
    const bool completeLzmaImage = request.imageCompression == BLED_COMPRESSION_LZMA && name == "bootable-restore";
    const bool completeLzwImage = request.imageCompression == BLED_COMPRESSION_LZW && name == "bootable-restore";
    const bool completeZipImage = request.imageCompression == BLED_COMPRESSION_ZIP && name == "bootable-restore";
    const bool completeZstdImage = request.imageCompression == BLED_COMPRESSION_ZSTD && name == "bootable-restore";
    const bool completeVtsiImage = request.imageCompression == BLED_COMPRESSION_VTSI && name == "bootable-restore";
    const int deadlineMs = completeLzmaImage || completeLzwImage ? 600000
        : completeXzImage || completeBzip2Image || completeZipImage || completeZstdImage || completeVtsiImage ? 300000 : 90000;
    qint64 nextDiagnosticMs = 15000;
    bool cancelledCopy = false, releasedCopy = false, waitedForExit = false;
    while (completed.isEmpty())
    {
        QCoreApplication::processEvents();
        // Four real decoder/hash passes over the complete image on TCG. XZ's
        // original byte-wise LZMA2/CRC64 and BZIP2 BWT/CRC loops cost more under Debug/sanitizers.
        if (timer.elapsed() >= nextDiagnosticMs)
        {
            const QList<QVariant> last = progress.isEmpty() ? QList<QVariant>() : progress.last();
            printf("RUFUS_COMPRESSED_HELPER_WAIT %s ELAPSED_MS=%lld STATE=%d PHASE=%d PROCESSED=%llu\n",
                reportName.constData(), static_cast<long long>(timer.elapsed()), int(process.state()),
                last.isEmpty() ? -1 : last[0].toInt(), last.isEmpty() ? 0ULL : last[1].toULongLong());
            fflush(stdout);
            nextDiagnosticMs += 15000;
        }
        require(timer.elapsed() < deadlineMs, "compressed guest helper deadline");
        if (!cancelledCopy && name == "cancel" && QFile::exists(QStringLiteral("/run/compressed-copy-held")))
        {
            checkExclusiveClaim();
            process.cancel();
            cancelledCopy = true;
            held.start();
        }
        if (cancelledCopy && !releasedCopy && held.elapsed() >= 100)
        {
            writeGuestFile(QStringLiteral("/run/compressed-copy-release"), "release\n");
            releasedCopy = true;
        }
        if (QFile::exists(QStringLiteral("/run/format-exit-held")) && completed.isEmpty())
        {
            require(process.isActive(), "compressed client waits for launcher exit");
            waitedForExit = true;
        }
        usleep(1000);
    }
    for (const QList<QVariant>& log : logs)
        printf("RUFUS_COMPRESSED_HELPER_LOG %s %s\n", reportName.constData(), qPrintable(log.first().toString()));
    const auto outcome = process.outcome();
    if (outcome.error != test.error)
        qFatal("compressed helper %s: error=%d expected=%d problem=%s", reportName.constData(), outcome.error, test.error, qPrintable(process.problem()));
    const auto state = test.error == ECANCELED ? rufus::PrepareImageProcess::State::Cancelled
        : test.error == 0 ? rufus::PrepareImageProcess::State::Complete : rufus::PrepareImageProcess::State::Failed;
    require(process.state() == state && completed.size() == 1 && waitedForExit, "compressed joined result");
    require(outcome.rawWrite && outcome.raw.bytes_written == test.written
        && outcome.raw.bytes_verified == test.verified, "compressed physical byte counters");
    require(outcome.started == (test.written != 0) && outcome.cleanupError == 0, "compressed started and cleanup facts");
    const int syncError = name == "sync" ? EIO : 0;
    require(outcome.syncError == syncError && outcome.raw.sync_error == syncError, "compressed sync failure retained");
    if (test.error == 0)
        require(outcome.raw.phase == RAW_WRITE_DONE && progress.size() >= 5, "compressed verified completion progress");
    if (test.written == 0)
        require(diskDigest() == before, "compressed refusal preserves entire disk");
    else
    {
        require(diskDigest() != before, "compressed job changed actual disk bytes");
        require(diskRangeDigest(preservedTailStart) == tail, "compressed preserves every byte beyond actual writes");
        if (!sparseExpected.isEmpty())
        {
            QFile disk(QStringLiteral("/dev/sda"));
            require(disk.open(QIODevice::ReadOnly) && disk.read(qint64(paddedBytes)) == sparseExpected,
                "VTSI independent sparse extent and gap readback");
        }
        if (!decoded.isEmpty())
        {
            QFile disk(QStringLiteral("/dev/sda"));
            require(disk.open(QIODevice::ReadOnly), "compressed independent prefix readback");
            QByteArray expected = decoded;
            expected.append(QByteArray(qsizetype(paddedBytes) - decoded.size(), '\0'));
            require(disk.read(qint64(test.written)) == expected.left(qsizetype(test.written)), "compressed decoded bytes and zero padding");
        }
    }
    require(readFile(QStringLiteral("/run/format-launches")) == "launch\n", "one compressed authorization launch");
    // A parent-on-target refusal deliberately leaves the caller mount attached.
    // Release that fixture before checking the helper's block claim.
    if (name == "parent-target")
        require(QFile::remove(QStringLiteral("/mnt/target/parent.vhd")) && umount("/mnt/target") == 0,
            "caller parent mount released after refusal");
    const int claim = open("/dev/sda", O_RDWR | O_EXCL | O_CLOEXEC);
    require(claim >= 0 && close(claim) == 0, "compressed helper released real block claim");
    if (test.fault)
    {
        const QByteArray observations = readFile(QStringLiteral("/run/compressed-observations"));
        require(observationValue(observations, "WRITES") > 0 && observationValue(observations, "SYNCS") == 1
            && observationValue(observations, "CLAIMS") >= 5 && observationValue(observations, "CLOSES") == 1,
            "real writes, dirty flush and claim through checked close");
        require((observationValue(observations, "READS") != 0) == (test.verified != 0 || name == "verify"),
            "readback only after successful copy and sync");
        if (name == "source-read" || name == "source-eof")
            require(observationValue(observations, "SOURCE_READS") >= 4, "actual source refill after committed output");
        printf("RUFUS_COMPRESSED_CLAIM_CASE %s %s", reportName.constData(), observations.constData());
    }
    for (const char* path : {"/run/format-exit-held", "/run/format-launches", "/run/compressed-fault",
             "/run/compressed-copy-held", "/run/compressed-copy-release", "/run/compressed-observations", "/run/lzw-flip-control"})
        if (QFile::exists(QString::fromLatin1(path))) require(QFile::remove(QString::fromLatin1(path)), "clear compressed guest control");
    require(descriptorCount() == descriptors && readFile(childList).simplified() == children, "compressed descriptors and descendants joined");
    printf("RUFUS_COMPRESSED_HELPER_CASE %s ERROR=%d WRITTEN=%llu VERIFIED=%llu SYNC=%d\n",
        reportName.constData(), outcome.error, static_cast<unsigned long long>(outcome.raw.bytes_written),
        static_cast<unsigned long long>(outcome.raw.bytes_verified), outcome.syncError);
}

void runBzip2StreamCases(const rufus::PrepareImageRequest& base, const QString& packedPath, const QByteArray& decoded)
{
    const QString emptyPath = QStringLiteral("/run/caller-home/empty.img");
    const QString emptyPackedPath = QStringLiteral("/run/caller-home/empty.bz2");
    writeGuestFile(emptyPath, {});
    compressedFixtureSource(emptyPath, emptyPackedPath, BLED_COMPRESSION_BZIP2);
    const QByteArray empty = readFile(emptyPackedPath);
    require(empty.size() == 14, "independent BZIP2 empty stream");
    const QByteArray valid = readFile(packedPath);
    QByteArray combined = valid;
    combined[combined.size() - 3] ^= 1; // Entirely inside the final combined CRC.
    writeGuestFile(packedPath, combined);
    const QByteArray hash = QCryptographicHash::hash(decoded, QCryptographicHash::Sha256);
    auto request = compressedRequest(base, packedPath, quint64(decoded.size()), hash, BLED_COMPRESSION_BZIP2);
    runCompressedHelperCase({"combined-crc", EBADMSG, 0, 0}, request);
    QByteArray badEmpty = empty;
    badEmpty[badEmpty.size() - 1] ^= 1;
    for (const char* name : {"empty-crc-only", "empty-crc-first", "empty-crc-last", "empty-crc-middle"})
    {
        const QByteArray mode(name);
        const bool prefix = mode == "empty-crc-last" || mode == "empty-crc-middle";
        const bool suffix = mode == "empty-crc-first" || mode == "empty-crc-middle";
        writeGuestFile(packedPath, (prefix ? valid : QByteArray()) + badEmpty + (suffix ? valid : QByteArray()));
        request = compressedRequest(base, packedPath, quint64(decoded.size()), hash, BLED_COMPRESSION_BZIP2);
        runCompressedHelperCase({name, EBADMSG, 0, 0}, request);
    }
    constexpr quint64 paddedBytes = 1024 * 1024 + 512;
    writeGuestFile(packedPath, empty + valid + empty);
    request = compressedRequest(base, packedPath, quint64(decoded.size()), hash, BLED_COMPRESSION_BZIP2);
    runCompressedHelperCase({"concatenated-success", 0, paddedBytes, paddedBytes, nullptr, true}, request, decoded);
    writeGuestFile(packedPath, valid + "ignored non-BZ tail" + empty);
    request = compressedRequest(base, packedPath, quint64(decoded.size()), hash, BLED_COMPRESSION_BZIP2);
    runCompressedHelperCase({"tail-success", 0, paddedBytes, paddedBytes, nullptr, true}, request, decoded);
    require(QFile::remove(emptyPath) && QFile::remove(emptyPackedPath), "BZIP2 extra sources removed");
}

void runZipEntryCases(const rufus::PrepareImageRequest& base, const QString& rawPath,
    const QString& packedPath, const QByteArray& decoded)
{
    const QByteArray hash = QCryptographicHash::hash(decoded, QCryptographicHash::Sha256);
    const quint64 padded = quint64(decoded.size() + 511) / 512 * 512;
    for (unsigned method : {0u, 8u, 12u, 14u, 95u})
    {
        zipFixtureSource(rawPath, packedPath, method);
        auto request = compressedRequest(base, packedPath, quint64(decoded.size()), hash, BLED_COMPRESSION_ZIP);
        const QByteArray success = "method" + QByteArray::number(method) + "-success";
        runCompressedHelperCase({success.constData(), 0, padded, padded, nullptr, true}, request, decoded);
        corruptCompressedCheck(packedPath, BLED_COMPRESSION_ZIP);
        request = compressedRequest(base, packedPath, quint64(decoded.size()), hash, BLED_COMPRESSION_ZIP);
        const QByteArray refused = "method" + QByteArray::number(method) + "-crc";
        runCompressedHelperCase({refused.constData(), EBADMSG, 0, 0}, request);
    }
}

QByteArray lzmaSizedStream(QByteArray packed, quint64 size)
{
    require(packed.size() >= 13, "LZMA size field bounds");
    for (unsigned index = 0; index < 8; index++) packed[5 + int(index)] = char(size >> (8 * index));
    return packed;
}

void runLzmaStreamCases(const rufus::PrepareImageRequest& base, const QString& packedPath, const QByteArray& decoded)
{
    const QByteArray valid = readFile(packedPath);
    const QByteArray known = lzmaSizedStream(valid, quint64(decoded.size()));
    QByteArray properties = valid;
    properties[0] = char(225);
    QByteArray dictionary = valid;
    // Little-endian 1 GiB + 1 exceeds the original cap.
    dictionary[1] = 1; dictionary[2] = 0; dictionary[3] = 0; dictionary[4] = char(0x40);
    QByteArray maximumDictionary = known;
    maximumDictionary[1] = maximumDictionary[2] = maximumDictionary[3] = 0;
    maximumDictionary[4] = char(0x40);
    // Initial match/rep/rep0/short-rep reads unwritten history in the original.
    const QByteArray history = QByteArray::fromHex("5d00100000010000000000000000c0000000") + QByteArray(32, '\0');
    const QString repeatedPath = QStringLiteral("/run/caller-home/lzma-repeated.img");
    const QString repeatedPacked = QStringLiteral("/run/caller-home/lzma-repeated.lzma");
    writeGuestFile(repeatedPath, QByteArray(20000, '\0'));
    compressedFixtureSource(repeatedPath, repeatedPacked, BLED_COMPRESSION_LZMA, QStringLiteral("none"), 4096);
    const QByteArray overshoot = lzmaSizedStream(readFile(repeatedPacked), 4097);
    const QByteArray withoutEos = known.left(known.size() - 5);
    // Independent xz must refuse the incomplete EOS even though the original
    // known-size contract allows all declared output to finish before it.
    writeGuestFile(packedPath, withoutEos);
    QProcess decoder;
    decoder.start(QStringLiteral("/usr/bin/xz"), {QStringLiteral("--decompress"), QStringLiteral("--stdout"),
        QStringLiteral("--format=lzma"), packedPath});
    require(decoder.waitForFinished(30000) && decoder.exitStatus() == QProcess::NormalExit
        && decoder.exitCode() != 0 && decoder.readAllStandardOutput() == decoded, "independent incomplete-EOS fixture");
    struct LzmaStreamCase { const char* name; QByteArray bytes; int error; };
    const QList<LzmaStreamCase> cases = {
        {"known-success", known, 0}, {"known-no-eos-success", withoutEos, 0},
        {"second-stream-success", valid + valid, 0}, {"tail-success", valid + "ignored LZMA tail", 0},
        {"properties", properties, EBADMSG}, {"dictionary", dictionary, ENOTSUP},
        {"history", history, EBADMSG}, {"declared-over-4gib", lzmaSizedStream(valid, (quint64(1) << 32) + 512), EBADMSG},
        {"known-overshoot", overshoot, EBADMSG}, {"truncated-eos", valid.left(valid.size() - 8), EBADMSG},
        {"maximum-dictionary-known-success", maximumDictionary, 0}};
    const quint64 padded = rufus::PrepareRawWriteBytes(base);
    for (const LzmaStreamCase& test : cases)
    {
        writeGuestFile(packedPath, test.bytes);
        const auto request = compressedRequest(base, packedPath, quint64(decoded.size()), base.decodedImageSha256, BLED_COMPRESSION_LZMA);
        const quint64 count = test.error == 0 ? padded : 0;
        runCompressedHelperCase({test.name, test.error, count, count, nullptr, test.error == 0}, request,
            test.error == 0 ? decoded : QByteArray());
    }
    require(QFile::remove(repeatedPath) && QFile::remove(repeatedPacked), "LZMA extra sources removed");
}

void runLzwDataFaultCases(const rufus::PrepareImageRequest& base, const QString& packedPath, const QByteArray& decoded)
{
    const QString directory = QStringLiteral("/lzw-fixtures/");
    const QJsonObject manifest = QJsonDocument::fromJson(readFile(directory + QStringLiteral("manifest.json"))).object();
    require(manifest.value("ordinary_sha256").toString().toLatin1()
        == QCryptographicHash::hash(decoded, QCryptographicHash::Sha256).toHex(), "LZW data-fault fixture identity");
    const QByteArray clear = readFile(directory + QStringLiteral("clear-tail.Z"));
    writeGuestFile(packedPath, clear);
    auto request = compressedRequest(base, packedPath, quint64(decoded.size()), base.decodedImageSha256, base.imageCompression);
    constexpr quint64 fullChunks = 1024 * 1024;
    runCompressedHelperCase({"copy-late-code", EBADMSG, fullChunks, 0, "late-code"}, request, decoded);
    const QByteArray literal = readFile(directory + QStringLiteral("literal.Z"));
    writeGuestFile(packedPath, literal);
    request = compressedRequest(base, packedPath, quint64(decoded.size()), base.decodedImageSha256, base.imageCompression);
    const quint64 index = quint64(manifest.value("decoded_flip_index").toInteger());
    require(index > 2 * 65536 && index < fullChunks, "LZW data flip after committed buffered output");
    QByteArray changed = decoded;
    changed[qsizetype(index)] ^= 1;
    const QByteArray control = QByteArray::number(manifest.value("flip_offset").toInteger()) + ' '
        + QByteArray::number(manifest.value("flip_xor").toInt()) + '\n';
    writeGuestFile(QStringLiteral("/run/lzw-flip-control"), control);
    runCompressedHelperCase({"copy-data", ESTALE, fullChunks, 0, "copy-data"}, request, changed);
    writeGuestFile(QStringLiteral("/run/lzw-flip-control"), control);
    const quint64 verified = index / 65536 * 65536;
    const quint64 padded = rufus::PrepareRawWriteBytes(request);
    runCompressedHelperCase({"verify-data", EBADMSG, padded, verified, "verify-data"}, request, decoded);
    require(readFile(packedPath) == literal, "LZW in-memory faults preserve actual source file");
}

void runLzwStreamCases(const rufus::PrepareImageRequest& base, const QString& packedPath, const QByteArray& decoded)
{
    const QString directory = QStringLiteral("/lzw-fixtures/");
    const QJsonObject manifest = QJsonDocument::fromJson(readFile(directory + QStringLiteral("manifest.json"))).object();
    require(manifest.value("ordinary_sha256").toString().toLatin1()
        == QCryptographicHash::hash(decoded, QCryptographicHash::Sha256).toHex(), "independent LZW fixture raw identity");
    const QJsonArray cases = manifest.value("cases").toArray();
    require(cases.size() == 20, "complete LZW independent fixture inventory");
    for (const QJsonValue& value : cases)
    {
        const QJsonObject test = value.toObject();
        const QByteArray packed = readFile(directory + test.value("file").toString());
        require(QCryptographicHash::hash(packed, QCryptographicHash::Sha256).toHex()
            == test.value("stored_sha256").toString().toLatin1(), "independent LZW encoded identity");
        QByteArray expected = decoded;
        const QString kind = test.value("kind").toString();
        if (kind == QStringLiteral("prefix")) expected = decoded.left(512);
        if (kind == QStringLiteral("appended")) expected = decoded.left(512) + QByteArray(1, '\0');
        if (kind == QStringLiteral("repeated")) expected = readFile(directory + QStringLiteral("repeated.img"));
        const QByteArray hash = QCryptographicHash::hash(expected, QCryptographicHash::Sha256);
        require(hash.toHex() == test.value("sha256").toString().toLatin1()
            && expected.size() == test.value("decoded").toInteger(), "independent LZW decoded identity");
        writeGuestFile(packedPath, packed);
        const auto request = compressedRequest(base, packedPath, quint64(expected.size()), hash, BLED_COMPRESSION_LZW);
        const int error = test.value("error").toInt();
        const quint64 count = error == 0 ? rufus::PrepareRawWriteBytes(request) : 0;
        const QByteArray name = test.value("name").toString().toLatin1();
        runCompressedHelperCase({name.constData(), error, count, count, nullptr, error == 0}, request,
            error == 0 ? expected : QByteArray());
    }
    runLzwDataFaultCases(base, packedPath, decoded);
}

QByteArray compressedGuestPayload(int compression)
{
    constexpr int decodedBytes = 1024 * 1024 + 37;
    QByteArray decoded(decodedBytes, Qt::Uninitialized);
    for (int index = 0; index < decoded.size(); index++) decoded[index] = char((index * 31 + index / 257) & 255);
    if (compression == BLED_COMPRESSION_LZMA || compression == BLED_COMPRESSION_LZW
        || compression == BLED_COMPRESSION_ZIP || compression == BLED_COMPRESSION_ZSTD)
    {
        quint32 state = compression == BLED_COMPRESSION_LZW ? 28
            : compression == BLED_COMPRESSION_ZIP ? 29 : compression == BLED_COMPRESSION_ZSTD ? 30 : 27;
        for (int index = 0; index < decoded.size(); index++)
        {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            decoded[index] = char(state);
        }
    }
    decoded[510] = char(0x55);
    decoded[511] = char(0xaa);
    return decoded;
}

void runLzwFallbackFaultCases(const rufus::PrepareImageRequest& base)
{
    const QByteArray decoded = compressedGuestPayload(BLED_COMPRESSION_LZW);
    const QString rawPath = QStringLiteral("/run/caller-home/fallback-small.img");
    const QString packedPath = QStringLiteral("/run/caller-home/fallback-small.gz");
    writeGuestFile(rawPath, decoded);
    compressedFixtureSource(rawPath, packedPath, BLED_COMPRESSION_LZW);
    const auto request = compressedRequest(base, packedPath, quint64(decoded.size()),
        QCryptographicHash::hash(decoded, QCryptographicHash::Sha256), BLED_COMPRESSION_GZIP);
    runCompressedHelperCase({"source-read", EIO, 65536, 0, "source-read"}, request, decoded);
    runCompressedHelperCase({"source-eof", EIO, 65536, 0, "source-eof"}, request, decoded);
    runLzwDataFaultCases(request, packedPath, decoded);
    require(QFile::remove(rawPath) && QFile::remove(packedPath), "fallback small sources removed");
}

void runCompressedQtCase(const rufus::PrepareImageRequest& request, bool preserveBase = false)
{
    if (!preserveBase)
    {
        const int target = open("/dev/sda", O_RDWR | O_EXCL | O_CLOEXEC);
        const QByteArray poison(4096, char(0xa5));
        require(target >= 0 && pwrite(target, poison.constData(), size_t(poison.size()), 0) == poison.size()
            && pwrite(target, poison.constData(), 512, off_t(request.size - 512)) == 512
            && fsync(target) == 0 && close(target) == 0, "distinct prefix and final sector before ordinary Qt restore");
    }
    QProcess client;
    client.setProcessChannelMode(QProcess::ForwardedChannels);
    client.start(QStringLiteral("/usr/bin/test_compressed_qt_guest"), QStringList());
    require(client.waitForStarted(processTimeoutMs), "start ordinary compressed Qt client");
    const QByteArray packet = QJsonDocument(rufus::EncodePrepareRequest(request)).toJson(QJsonDocument::Compact) + '\n';
    require(client.write(packet) == packet.size(), "Qt fixture identity pipe");
    client.closeWriteChannel();
    const bool largerImage = request.imageCompression == BLED_COMPRESSION_XZ
        || request.imageCompression == BLED_COMPRESSION_BZIP2
        || request.imageCompression == BLED_COMPRESSION_ZIP
        || request.imageCompression == BLED_COMPRESSION_ZSTD
        || request.imageCompression == IMG_COMPRESSION_VHDX || request.imageCompression == IMG_COMPRESSION_FFU;
    const bool lzwFixture = QFile::exists(QStringLiteral("/etc/rufus-lzw-qt-fixture"));
    const int clientTimeoutMs = request.imageCompression == BLED_COMPRESSION_LZMA || lzwFixture
        || request.imageCompression == BLED_COMPRESSION_ZIP
        || request.imageCompression == BLED_COMPRESSION_ZSTD ? 960000 : largerImage ? 600000 : 180000;
    require(client.waitForFinished(clientTimeoutMs) && client.exitStatus() == QProcess::NormalExit
        && client.exitCode() == 0, "ordinary compressed Qt process joined");
}

void runCompressedQtRestore(rufus::PrepareImageRequest request, const QString& rawPath,
    const QByteArray& expectedDisk, int compression)
{
    const bool lzma = compression == BLED_COMPRESSION_LZMA;
    const bool lzw = compression == BLED_COMPRESSION_LZW;
    const bool zip = compression == BLED_COMPRESSION_ZIP;
    const bool zstd = compression == BLED_COMPRESSION_ZSTD;
    require(lzma || lzw || zip || zstd || compression == BLED_COMPRESSION_BZIP2, "focused ordinary compressed restore provider");
    const QString packedPath = lzw ? QStringLiteral("/run/caller-home/source.Z")
        : lzma ? QStringLiteral("/run/caller-home/source.lzma")
        : zip ? QStringLiteral("/run/caller-home/source.zip")
        : zstd ? QStringLiteral("/run/caller-home/source.zst") : QStringLiteral("/run/caller-home/source.bz2");
    compressedFixtureSource(rawPath, packedPath, compression);
    request = compressedRequest(request, packedPath, request.size, expectedDisk, compression);
    publishTargetMount(rawObjects, request, {}, rawPartition);
    runCompressedQtCase(request);
    require(diskDigest() == expectedDisk, "ordinary compressed full disk equality");
    if (lzw)
    {
        runLzwFallbackFaultCases(request);
        const QString alias = QStringLiteral("/run/caller-home/source.gz");
        writeGuestFile(alias, readFile(packedPath));
        require(chown(QFile::encodeName(alias).constData(), 1000, 1000) == 0, "fallback caller ownership");
        const auto fallback = compressedRequest(request, alias, request.size, expectedDisk, BLED_COMPRESSION_GZIP);
        runCompressedQtCase(fallback);
        require(diskDigest() == expectedDisk && QFile::remove(alias), "ordinary gzip fallback whole-disk equality and source cleanup");
    }
    require(QFile::remove(packedPath), "ordinary compressed source removed");
    printf("RUFUS_%s_QT_HELPER_DONE SHA256=%s\n", lzw ? "LZW" : lzma ? "LZMA" : zip ? "ZIP" : zstd ? "ZSTD" : "BZIP2",
        expectedDisk.toHex().constData());
}

void runCompressedHelperCases(rufus::PrepareImageRequest request, const QString& rawPath, const QByteArray& expectedDisk,
    int compression = BLED_COMPRESSION_GZIP)
{
    const QString smallPath = QStringLiteral("/run/caller-home/small.img");
    const bool xz = compression == BLED_COMPRESSION_XZ;
    const bool bzip2 = compression == BLED_COMPRESSION_BZIP2;
    const bool lzma = compression == BLED_COMPRESSION_LZMA;
    const bool lzw = compression == BLED_COMPRESSION_LZW;
    const bool zip = compression == BLED_COMPRESSION_ZIP;
    const bool zstd = compression == BLED_COMPRESSION_ZSTD;
    const char* reportPrefix = xz ? "xz" : bzip2 ? "bzip2" : lzma ? "lzma" : lzw ? "lzw" : zip ? "zip" : zstd ? "zstd" : "gzip";
    const QString packedPath = QStringLiteral("/run/caller-home/source.")
        + QString::fromLatin1(xz ? "xz" : bzip2 ? "bz2" : lzma ? "lzma" : lzw ? "Z" : zip ? "zip" : zstd ? "zst" : "gz");
    const QByteArray decoded = compressedGuestPayload(compression);
    const quint64 decodedBytes = quint64(decoded.size());
    constexpr quint64 paddedBytes = 1024 * 1024 + 512;
    writeGuestFile(smallPath, decoded);
    compressedFixtureSource(smallPath, packedPath, compression);
    const auto small = compressedRequest(request, packedPath, decodedBytes, QCryptographicHash::hash(decoded, QCryptographicHash::Sha256), compression);
    auto mismatch = small;
    mismatch.decodedImageSha256[0] ^= 1;
    runCompressedHelperCase({"digest", ESTALE, 0, 0}, mismatch);
    mismatch = small;
    mismatch.decodedImageSize--;
    runCompressedHelperCase({"size", ESTALE, 0, 0}, mismatch);
    mismatch = small;
    mismatch.imageSectorSize = 4096;
    runCompressedHelperCase({"sector", ESTALE, 0, 0}, mismatch);
    if (lzma)
        writeGuestFile(packedPath, lzmaSizedStream(readFile(packedPath), decodedBytes + 1));
    else if (lzw)
        writeGuestFile(packedPath, QByteArray::fromHex("1f9d90ff01"));
    else corruptCompressedCheck(packedPath, compression);
    mismatch = compressedRequest(request, packedPath, decodedBytes, small.decodedImageSha256, compression);
    runCompressedHelperCase({lzw ? "code" : lzma ? "size-header" : "crc", EBADMSG, 0, 0}, mismatch);
    compressedFixtureSource(smallPath, packedPath, compression);
    request = compressedRequest(request, packedPath, decodedBytes, small.decodedImageSha256, compression);
    for (const MountedHelperCase& test : {MountedHelperCase{"busy", EBUSY, false},
             MountedHelperCase{"home-target", EBUSY, false}, MountedHelperCase{"raw-cancel-after", ECANCELED, true}})
        runMountedHelperCase(test, request, rawObjects, rawPartition, nullptr, reportPrefix);
    runMountedHelperCase({"raw-success", 0, true}, request, rawObjects, rawPartition, nullptr, reportPrefix);
    // The small image has replaced the partition table. All remaining jobs are
    // unmounted DD, until the original bootable image is completely restored.
    for (const CompressedHelperCase& test : {
             CompressedHelperCase{"write", EIO, 17, 0, "write"},
             CompressedHelperCase{"cancel", ECANCELED, 65536, 0, "cancel"},
             CompressedHelperCase{"sync", EIO, paddedBytes, 0, "sync"},
             CompressedHelperCase{"verify", EBADMSG, paddedBytes, 0, "verify"},
             CompressedHelperCase{"close", EIO, paddedBytes, paddedBytes, "close"},
             CompressedHelperCase{"observed-success", 0, paddedBytes, paddedBytes, "observe"}})
        runCompressedHelperCase(test, request, decoded);
    if (lzma || lzw || zip || zstd)
    {
        runCompressedHelperCase({"source-read", EIO, 65536, 0, "source-read"}, request, decoded);
        runCompressedHelperCase({"source-eof", EIO, 65536, 0, "source-eof"}, request, decoded);
        if (lzma) runLzmaStreamCases(request, packedPath, decoded);
        if (lzw) runLzwStreamCases(request, packedPath, decoded);
    }
    if (xz)
    {
        for (const QString& check : {QStringLiteral("crc32"), QStringLiteral("sha256")})
        {
            compressedFixtureSource(smallPath, packedPath, compression, check);
            corruptCompressedCheck(packedPath, compression);
            request = compressedRequest(request, packedPath, decodedBytes, small.decodedImageSha256, compression);
            const QByteArray refused = check.toLatin1() + "-crc";
            runCompressedHelperCase({refused.constData(), EBADMSG, 0, 0}, request);
            compressedFixtureSource(smallPath, packedPath, compression, check);
            request = compressedRequest(request, packedPath, decodedBytes, small.decodedImageSha256, compression);
            const QByteArray success = check.toLatin1() + "-success";
            runCompressedHelperCase({success.constData(), 0, paddedBytes, paddedBytes, nullptr, true}, request, decoded);
        }
        compressedFixtureSource(smallPath, packedPath, compression, QStringLiteral("none"));
        request = compressedRequest(request, packedPath, decodedBytes, small.decodedImageSha256, compression);
        runCompressedHelperCase({"none-success", 0, paddedBytes, paddedBytes, nullptr, true}, request, decoded);
    }
    if (bzip2) runBzip2StreamCases(request, packedPath, decoded);
    if (zip) runZipEntryCases(request, smallPath, packedPath, decoded);
    compressedFixtureSource(rawPath, packedPath, compression);
    request = compressedRequest(request, packedPath, request.size, expectedDisk, compression);
    runCompressedHelperCase({"bootable-restore", 0, request.size, request.size, nullptr, bzip2 || lzma || lzw || zip || zstd}, request);
    const bool qtAdmitted = compression == BLED_COMPRESSION_GZIP
        || (xz && QFile::exists(QStringLiteral("/etc/rufus-xz-qt-fixture")));
    if (QFile::exists(QStringLiteral("/etc/rufus-compressed-qt-fixture")) && qtAdmitted)
    {
        runCompressedQtCase(request);
    }
    require(diskDigest() == expectedDisk, "complete compressed boot image equality");
    printf("RUFUS_%s_HELPER_DONE SHA256=%s\n", xz ? "XZ" : bzip2 ? "BZIP2" : lzma ? "LZMA" : lzw ? "LZW" : zip ? "ZIP" : zstd ? "ZSTD" : "COMPRESSED", expectedDisk.toHex().constData());
    require(QFile::remove(smallPath) && QFile::remove(packedPath), "compressed source fixtures removed");
}
