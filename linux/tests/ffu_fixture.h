// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "compressed_fixture.h"
#include <QList>

// Independent V2 serializer for small Qt/guest fixtures. Each store has one
// beginning-relative descriptor, and all store headers precede all payloads.
inline QByteArray FfuMultiStoreFixture(const QList<QByteArray>& disks)
{
    constexpr qsizetype block = 128 * 1024;
    if (disks.isEmpty() || disks.size() > 65535) return {};
    QByteArray manifest("[FullFlash]\r\nOSVersion=10.0.22621.0\r\n");
    for (const QByteArray& disk : disks)
    {
        if (disk.isEmpty() || disk.size() % block != 0) return {};
        manifest += "\r\n[Store]\r\nMinSectorCount=" + QByteArray::number(disk.size() / 512) + "\r\nSectorSize=512\r\n";
    }
    QByteArray metadata(24, '\0');
    PutVtsiFixtureValue(metadata, 0, 24, 4);
    metadata.replace(4, 12, "ImageFlash  ");
    PutVtsiFixtureValue(metadata, 16, quint64(manifest.size()), 4);
    PutVtsiFixtureValue(metadata, 20, 128, 4);
    metadata += manifest;
    metadata.append((block - metadata.size() % block) % block, '\0');
    for (qsizetype i = 0; i < disks.size(); i++)
    {
        const QByteArray& disk = disks[i];
        QByteArray store(262, '\0');
        PutVtsiFixtureValue(store, 4, 2, 2);
        PutVtsiFixtureValue(store, 8, 2, 2);
        PutVtsiFixtureValue(store, 204, block, 4);
        PutVtsiFixtureValue(store, 208, 1, 4);
        PutVtsiFixtureValue(store, 212, 16, 4);
        PutVtsiFixtureValue(store, 244, 1, 4);
        PutVtsiFixtureValue(store, 248, quint64(disks.size()), 2);
        PutVtsiFixtureValue(store, 250, quint64(i + 1), 2);
        PutVtsiFixtureValue(store, 252, quint64(disk.size()), 8);
        const QByteArray path = "VenHw(Store" + QByteArray::number(i + 1) + ')';
        PutVtsiFixtureValue(store, 260, quint64(path.size()), 2);
        for (char byte : path) { store += byte; store += '\0'; }
        QByteArray descriptor(16, '\0');
        PutVtsiFixtureValue(descriptor, 0, 1, 4);
        PutVtsiFixtureValue(descriptor, 4, quint64(disk.size() / block), 4);
        store += descriptor;
        store.append((block - store.size() % block) % block, '\0');
        metadata += store;
    }
    QByteArray hashes;
    for (qsizetype position = 0; position < metadata.size(); position += block)
        hashes += QCryptographicHash::hash(QByteArray::fromRawData(metadata.constData() + position, int(block)), QCryptographicHash::Sha256);
    for (const QByteArray& disk : disks)
        for (qsizetype position = 0; position < disk.size(); position += block)
            hashes += QCryptographicHash::hash(QByteArray::fromRawData(disk.constData() + position, int(block)), QCryptographicHash::Sha256);
    QByteArray security(32, '\0');
    PutVtsiFixtureValue(security, 0, 32, 4);
    security.replace(4, 12, "SignedImage ");
    PutVtsiFixtureValue(security, 16, 128, 4);
    PutVtsiFixtureValue(security, 20, 0x800c, 4);
    PutVtsiFixtureValue(security, 28, quint64(hashes.size()), 4);
    security += hashes;
    security.append((block - security.size() % block) % block, '\0');
    QByteArray packed = security + metadata;
    for (const QByteArray& disk : disks) packed += disk;
    return packed;
}
