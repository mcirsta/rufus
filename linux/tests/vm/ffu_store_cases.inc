// SPDX-License-Identifier: GPL-3.0-or-later
#include "../ffu_fixture.h"

static QByteArray AddGuestFfuCatalog(const QByteArray& packed)
{
    QFile file(QStringLiteral("/etc/rufus-ffu-test.cat"));
    if (!file.exists()) return packed;
    require(file.open(QIODevice::ReadOnly), "read independent test-signed catalog");
    const QByteArray catalog = file.readAll();
    require(!catalog.isEmpty() && catalog.size() < 16 * 1024 * 1024, "bounded test catalog");
    const auto* header = reinterpret_cast<const uchar*>(packed.constData());
    const quint32 block = qFromLittleEndian<quint32>(header + 16) * 1024;
    const quint32 oldCatalog = qFromLittleEndian<quint32>(header + 24);
    const quint32 hashes = qFromLittleEndian<quint32>(header + 28);
    require(block != 0 && oldCatalog == 0, "independent catalog-free fixture");
    const quint64 imageOffset = (32 + quint64(hashes) + block - 1) / block * block;
    QByteArray result = packed.first(32);
    qToLittleEndian<quint32>(quint32(catalog.size()), reinterpret_cast<uchar*>(result.data()) + 24);
    result += catalog;
    result += packed.mid(32, hashes);
    result += QByteArray((block - result.size() % block) % block, '\0');
    result += packed.mid(qsizetype(imageOffset));
    printf("RUFUS_FFU_CATALOG SHA256=%s\n", QCryptographicHash::hash(catalog, QCryptographicHash::Sha256).toHex().constData());
    return result;
}

void runFfuStoreQtCases(rufus::PrepareImageRequest base, const QString& rawPath, const QByteArray& expectedDisk)
{
    const QByteArray first(256 * 1024, 'A'), second(384 * 1024, 'B');
    const QString path = QStringLiteral("/run/caller-home/stores.ffu");
    const QByteArray packed = AddGuestFfuCatalog(FfuMultiStoreFixture({first, second}));
    require(!packed.isEmpty(), "independent multi-store FFU");
    writeGuestFile(path, packed);
    require(chown(QFile::encodeName(path).constData(), 1000, 1000) == 0, "multi-store caller ownership");
    for (unsigned store : {1u, 2u})
    {
        const QByteArray& expected = store == 1 ? first : second;
        auto request = compressedRequest(base, path, quint64(expected.size()),
            QCryptographicHash::hash(expected, QCryptographicHash::Sha256), IMG_COMPRESSION_FFU, store);
        publishTargetMount(rawObjects, request, {}, rawPartition);
        runCompressedQtCase(request);
        QFile disk(QStringLiteral("/dev/sda"));
        require(disk.open(QIODevice::ReadOnly) && disk.read(expected.size()) == expected, "selected FFU store independent readback");
        require(disk.seek(qint64(base.size - 512)) && disk.read(512) == QByteArray(512, char(0xa5)), "unlisted FFU target tail preserved");
        disk.close();
        QFile source(path);
        require(source.open(QIODevice::ReadOnly) && source.readAll() == packed, "multi-store source unchanged");
        printf("RUFUS_FFU_STORE_CASE INDEX=%u BYTES=%lld READBACK=1 SOURCE_UNCHANGED=1\n", store, static_cast<long long>(expected.size()));
    }
    QFile raw(rawPath);
    require(raw.open(QIODevice::ReadOnly), "full multi-store boot source");
    QByteArray boot = raw.readAll();
    raw.close();
    require(quint64(boot.size()) == base.size && QCryptographicHash::hash(boot, QCryptographicHash::Sha256) == expectedDisk,
        "full bootable store identity");
    const QByteArray full = AddGuestFfuCatalog(FfuMultiStoreFixture({first, boot}));
    boot.clear(); boot.squeeze();
    writeGuestFile(path, full);
    require(chown(QFile::encodeName(path).constData(), 1000, 1000) == 0, "full multi-store caller ownership");
    require(QFile::remove(rawPath), "full raw source removed before restore");
    auto request = compressedRequest(base, path, base.size, expectedDisk, IMG_COMPRESSION_FFU, 2);
    publishTargetMount(rawObjects, request, {}, rawPartition);
    runCompressedQtCase(request);
    require(diskDigest() == expectedDisk, "full selected store disk readback");
    QFile source(path);
    require(source.open(QIODevice::ReadOnly) && source.readAll() == full, "full multi-store source unchanged");
    source.close();
    require(QFile::remove(path), "multi-store source removed after verification");
    printf("RUFUS_FFU_STORES_DONE SHA256=%s\n", expectedDisk.toHex().constData());
}
