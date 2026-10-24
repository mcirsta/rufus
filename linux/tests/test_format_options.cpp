// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/format_options.h"
#include "platform/ext_format.h"
#include "platform/fat_format.h"
#include "platform/exfat_format.h"
#include "platform/ntfs_format.h"
#include "platform/udf_format.h"
#include "platform/partition_image.h"
#include "qt/prepare_image_protocol.h"
#include "libfat.h"
#include <QTest>

namespace
{
constexpr quint64 MiB = 1024 * 1024;
constexpr quint64 GiB = 1024 * MiB;

rufus::PrepareImageRequest geometryRequest(quint32 logical, quint32 physical, int filesystem,
    rufus::PartitionScheme scheme, quint64 capacity = 512 * MiB)
{
    rufus::PrepareImageRequest request;
    request.path = QStringLiteral("/test/geometry-disk");
    request.size = capacity; request.device = 2064; request.isDevice = true; request.diskSequence = 10;
    request.objectPath = QStringLiteral("/org/freedesktop/UDisks2/block_devices/geometry");
    request.serviceOwner = QStringLiteral(":1.23"); request.identity = QStringList{QStringLiteral("geometry")};
    request.logicalSectorSize = logical; request.physicalSectorSize = physical;
    request.nonBootable = true; request.fileSystem = filesystem;
    request.mbr = scheme == rufus::PartitionScheme::Mbr; request.sfd = scheme == rufus::PartitionScheme::Sfd;
    request.label = QStringLiteral("GEOMETRY");
    return request;
}
}

class TestFormatOptions : public QObject
{
    Q_OBJECT
private slots:
    void nativeGeometryLayouts_data()
    {
        QTest::addColumn<quint32>("logical"); QTest::addColumn<quint32>("physical");
        QTest::addColumn<int>("filesystem"); QTest::addColumn<int>("layout");
        const struct Geometry { const char* name; quint32 logical, physical; } geometries[] = {
            {"512", 512, 512}, {"512e", 512, 4096}, {"4Kn", 4096, 4096}
        };
        for (const Geometry& geometry : geometries)
        for (int filesystem : {FS_FAT16, FS_FAT32, FS_NTFS, FS_EXFAT, FS_UDF, FS_EXT2, FS_EXT3})
        for (int layout = 0; layout < 3; ++layout)
            QTest::newRow(qPrintable(QStringLiteral("%1-fs%2-layout%3").arg(geometry.name).arg(filesystem).arg(layout)))
                << geometry.logical << geometry.physical << filesystem << layout;
    }

    void nativeGeometryLayouts()
    {
        QFETCH(quint32, logical); QFETCH(quint32, physical); QFETCH(int, filesystem); QFETCH(int, layout);
        const auto scheme = layout == 0 ? rufus::PartitionScheme::Mbr
            : layout == 1 ? rufus::PartitionScheme::Gpt : rufus::PartitionScheme::Sfd;
        const auto choices = rufus::NativeFileSystemClusterSizes(512 * MiB, filesystem, false,
            BT_NON_BOOTABLE, scheme, false, MBR_BOOT_ZERO, false, false, TT_UEFI, true, logical, physical);
        QVERIFY(choices.Allowed != 0);
        QVERIFY((choices.Default & choices.Allowed) != 0);
        quint32 candidates = choices.Allowed;
        while (candidates != 0) {
            const quint32 cluster = candidates & (0u - candidates);
            candidates &= ~cluster;
            auto request = geometryRequest(logical, physical, filesystem, scheme);
            request.clusterSize = cluster;
            QVERIFY(rufus::ValidFormatGeometry(request));
            GPT_IMAGE_RESULT partition = {};
            QCOMPARE(rufus::PlanFormatPartition(request, partition), 0);
            QCOMPARE(partition.offset, scheme == rufus::PartitionScheme::Sfd ? quint64(0) : MiB);
            QCOMPARE(partition.offset % physical, quint64(0));
            QCOMPARE(partition.size % logical, quint64(0));
            QVERIFY(partition.size <= request.size - partition.offset);
            QCOMPARE(partition.uefi_ntfs_size, quint64(0));
            QCOMPARE(rufus::CheckFormatSettings(request, partition), 0);
            if (filesystem == FS_UDF || filesystem == FS_EXT2 || filesystem == FS_EXT3)
                QCOMPARE(cluster, quint32(256));
            else {
                QVERIFY(cluster >= logical);
                QCOMPARE(partition.size % cluster, quint64(0));
            }
            if (cluster != choices.Default) continue;
            const QJsonObject encoded = rufus::EncodePrepareRequest(request);
            QVERIFY(!encoded.isEmpty());
            const bool explicitGeometry = logical != 512 || physical != 512;
            QCOMPARE(encoded.contains("geometry"), explicitGeometry);
            if (explicitGeometry) QCOMPARE(encoded.value("geometry").toObject(),
                (QJsonObject{{"logical", int(logical)}, {"physical", int(physical)}}));
            rufus::PrepareImageRequest decoded;
            QVERIFY(rufus::DecodePrepareRequest(encoded, decoded));
            QCOMPARE(decoded.logicalSectorSize, logical); QCOMPARE(decoded.physicalSectorSize, physical);
            QCOMPARE(decoded.clusterSize, cluster);
            QCOMPARE(decoded.mbr, request.mbr); QCOMPARE(decoded.sfd, request.sfd);
            QCOMPARE(rufus::EncodePrepareRequest(decoded), encoded);
        }
    }

    void geometryProtocolRefusals()
    {
        auto request = geometryRequest(4096, 4096, FS_NTFS, rufus::PartitionScheme::Gpt);
        request.clusterSize = 4096;
        const QJsonObject encoded = rufus::EncodePrepareRequest(request);
        QVERIFY(!encoded.isEmpty());
        const QJsonObject invalidGeometry[] = {
            {}, {{"logical", 4096}}, {{"physical", 4096}},
            {{"logical", "4096"}, {"physical", 4096}},
            {{"logical", 4096.5}, {"physical", 4096}},
            {{"logical", 4096}, {"physical", 4096.5}},
            {{"logical", 4096}, {"physical", 512}},
            {{"logical", 0}, {"physical", 4096}},
            {{"logical", 513}, {"physical", 4096}},
            {{"logical", 8192}, {"physical", 8192}},
            {{"logical", 4096}, {"physical", 131072}},
            {{"logical", 4096}, {"physical", 4096}, {"unused", true}},
            // Defaults are represented only by the absence of geometry.
            {{"logical", 512}, {"physical", 512}}
        };
        for (const QJsonObject& geometry : invalidGeometry) {
            QJsonObject invalid = encoded; invalid.insert("geometry", geometry);
            rufus::PrepareImageRequest decoded;
            QVERIFY(!rufus::DecodePrepareRequest(invalid, decoded));
        }
        for (const QJsonValue& geometry : {QJsonValue(true), QJsonValue(4096), QJsonValue(QStringLiteral("4096"))}) {
            QJsonObject invalid = encoded; invalid.insert("geometry", geometry);
            rufus::PrepareImageRequest decoded;
            QVERIFY(!rufus::DecodePrepareRequest(invalid, decoded));
        }
        for (quint32 logical : {quint32(0), quint32(256), quint32(513), quint32(8192)}) {
            request.logicalSectorSize = logical;
            QVERIFY(!rufus::ValidFormatGeometry(request));
            QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
            GPT_IMAGE_RESULT partition = {};
            QCOMPARE(rufus::PlanFormatPartition(request, partition), ENOTSUP);
        }
        request.logicalSectorSize = 4096;
        for (quint32 physical : {quint32(0), quint32(512), quint32(4097), quint32(131072)}) {
            request.physicalSectorSize = physical;
            QVERIFY(!rufus::ValidFormatGeometry(request));
            QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        }
    }

    void geometryPreservesBootLimits()
    {
        const struct BootMode { const char* name; int mode; } modes[] = {
            {"FreeDOS", BT_FREEDOS}, {"MSDOS", BT_MSDOS}, {"ReactOS", BT_REACTOS},
            {"Syslinux4", BT_SYSLINUX_V4}, {"Syslinux6", BT_SYSLINUX_V6},
            {"GRUB2", BT_GRUB2}, {"GRUB4DOS", BT_GRUB4DOS}, {"UEFI:NTFS", BT_UEFI_NTFS}
        };
        for (const BootMode& mode : modes) {
            const bool syslinux = mode.mode == BT_SYSLINUX_V4 || mode.mode == BT_SYSLINUX_V6;
            const int filesystem = mode.mode == BT_UEFI_NTFS ? FS_NTFS : FS_FAT32;
            const auto choices = rufus::NativeFileSystemClusterSizes(512 * MiB, filesystem, syslinux,
                mode.mode, rufus::PartitionScheme::Mbr, false, MBR_BOOT_ZERO, false,
                false, TT_UEFI, true, 4096, 4096);
            QVERIFY2(choices.Allowed == 0, mode.name);
        }
        auto request = geometryRequest(4096, 4096, FS_NTFS, rufus::PartitionScheme::Mbr);
        request.clusterSize = 4096; request.nonBootable = false;
        request.windowsBios = true;
        QVERIFY(!rufus::ValidFormatGeometry(request));
        request.windowsBios = false; request.kolibriOs = true;
        QVERIFY(!rufus::ValidFormatGeometry(request));
        request.kolibriOs = false; request.windowsToGo = true; request.windowsToGoTarget = TT_BIOS;
        QVERIFY(!rufus::ValidFormatGeometry(request));
        request.windowsToGo = false; request.uefiNtfs = true;
        QVERIFY(!rufus::ValidFormatGeometry(request));
    }

    void mbrSectorCapacityBoundaries_data()
    {
        QTest::addColumn<quint32>("logical"); QTest::addColumn<quint32>("physical");
        QTest::newRow("512") << quint32(512) << quint32(512);
        QTest::newRow("512e") << quint32(512) << quint32(4096);
        QTest::newRow("4Kn") << quint32(4096) << quint32(4096);
    }

    void mbrSectorCapacityBoundaries()
    {
        QFETCH(quint32, logical); QFETCH(quint32, physical);
        const quint64 mbrLimit = (quint64(1) << 32) * logical;
        QCOMPARE(mbrLimit, logical == 512 ? 2048 * GiB : 16384 * GiB);
        auto request = geometryRequest(logical, physical, FS_NTFS, rufus::PartitionScheme::Mbr, mbrLimit);
        request.clusterSize = 65536;
        GPT_IMAGE_RESULT partition = {};
        QCOMPARE(rufus::PlanFormatPartition(request, partition), 0);
        QCOMPARE(rufus::CheckFormatSettings(request, partition), 0);
        QVERIFY(partition.offset / logical + partition.size / logical <= (quint64(1) << 32));
        const auto last = rufus::NativeFileSystemClusterSizes(mbrLimit, FS_NTFS, false, BT_NON_BOOTABLE,
            rufus::PartitionScheme::Mbr, false, MBR_BOOT_ZERO, false, false, TT_UEFI, true, logical, physical);
        QVERIFY(last.Allowed != 0);
        request.size += logical;
        QCOMPARE(rufus::PlanFormatPartition(request, partition), EINVAL);
        QCOMPARE(rufus::NativeFileSystemClusterSizes(request.size, FS_NTFS, false, BT_NON_BOOTABLE,
            rufus::PartitionScheme::Mbr, false, MBR_BOOT_ZERO, false, false, TT_UEFI, true, logical, physical).Allowed, quint32(0));
        request.mbr = false;
        QCOMPARE(rufus::PlanFormatPartition(request, partition), 0);
        QCOMPARE(rufus::CheckFormatSettings(request, partition), 0);
        request.mbr = true; request.size = 2048 * GiB + 4096;
        QCOMPARE(rufus::PlanFormatPartition(request, partition), logical == 512 ? EINVAL : 0);
    }

    void fat32LargeClusters_data()
    {
        QTest::addColumn<quint32>("logical"); QTest::addColumn<quint32>("physical");
        QTest::newRow("512") << quint32(512) << quint32(512);
        QTest::newRow("512e") << quint32(512) << quint32(4096);
        QTest::newRow("4Kn") << quint32(4096) << quint32(4096);
    }

    void fat32LargeClusters()
    {
        QFETCH(quint32, logical); QFETCH(quint32, physical);
        for (const auto scheme : {rufus::PartitionScheme::Mbr, rufus::PartitionScheme::Gpt, rufus::PartitionScheme::Sfd})
        for (quint64 capacity : {8 * GiB, 16 * GiB, 32 * GiB, 64 * GiB}) {
            const auto choices = rufus::NativeFileSystemClusterSizes(capacity, FS_FAT32, false, BT_NON_BOOTABLE,
                scheme, false, MBR_BOOT_ZERO, false, false, TT_UEFI, true, logical, physical);
            QVERIFY(choices.Allowed & 65536);
            auto request = geometryRequest(logical, physical, FS_FAT32, scheme, capacity);
            request.clusterSize = 65536;
            GPT_IMAGE_RESULT partition = {};
            QCOMPARE(rufus::PlanFormatPartition(request, partition), 0);
            QCOMPARE(rufus::CheckFormatSettings(request, partition), 0);
            const QJsonObject encoded = rufus::EncodePrepareRequest(request);
            QVERIFY(!encoded.isEmpty());
            rufus::PrepareImageRequest decoded;
            QVERIFY(rufus::DecodePrepareRequest(encoded, decoded));
            QCOMPARE(decoded.clusterSize, quint32(65536));
        }
    }

    void sharedMbrProviderSelection()
    {
        // Native requests still represent one admitted provider. Their output
        // must match the pre-extraction bootstrap choice for every current route.
        for (bool source : {false, true})
        for (bool enhanced : {false, true})
        for (bool rufusMbr : {false, true})
        {
            rufus::PrepareImageRequest request;
            request.mbr = true;
            request.enhancedInstall = enhanced;
            if (source) request.imagePath = QStringLiteral("/unused/source.iso");
            QCOMPARE(rufus::FormatMbrBootCode(request), MBR_BOOT_ZERO);
            request.windowsBios = true;
            request.windowsRufusMbr = rufusMbr;
            QCOMPARE(rufus::FormatMbrBootCode(request), rufusMbr ? MBR_BOOT_RUFUS : MBR_BOOT_WIN7);
            request.windowsBios = false;
            request.freeDos = true;
            request.dosRufusMbr = rufusMbr;
            QCOMPARE(rufus::FormatMbrBootCode(request), rufusMbr ? MBR_BOOT_RUFUS : MBR_BOOT_WIN7);
            request.freeDos = false;
            request.grub2 = true;
            QCOMPARE(rufus::FormatMbrBootCode(request), MBR_BOOT_GRUB2);
            request.grub2 = false;
            request.syslinux = true;
            for (int selection : {BT_IMAGE, BT_SYSLINUX_V4, BT_SYSLINUX_V6})
            {
                request.syslinuxSelection = selection;
                QCOMPARE(rufus::FormatMbrBootCode(request), MBR_BOOT_SYSLINUX);
            }
            request.syslinux = false;
            request.nonBootable = true;
            QCOMPARE(rufus::FormatMbrBootCode(request), MBR_BOOT_ZERO);
            request.nonBootable = false;
            request.uefiNtfs = true;
            QCOMPARE(rufus::FormatMbrBootCode(request), MBR_BOOT_ZERO);
        }
    }

    void standaloneSyslinuxClusters_data()
    {
        QTest::addColumn<int>("bootType");
        QTest::newRow("v4") << int(BT_SYSLINUX_V4);
        QTest::newRow("v6") << int(BT_SYSLINUX_V6);
    }

    void standaloneSyslinuxClusters()
    {
        QFETCH(int, bootType);
        for (int fs : {FS_FAT16, FS_FAT32, FS_NTFS, FS_EXFAT})
        {
            for (auto scheme : {rufus::PartitionScheme::Mbr, rufus::PartitionScheme::Gpt, rufus::PartitionScheme::Sfd})
            {
                const auto sizes = rufus::NativeFileSystemClusterSizes(64 * MiB, fs, true, bootType, scheme);
                const bool ntfs = fs == FS_NTFS && bootType == BT_SYSLINUX_V6;
                const bool supported = (fs == FS_FAT16 || fs == FS_FAT32 || ntfs) && scheme == rufus::PartitionScheme::Mbr;
                const quint32 allowed = ntfs ? 0x1f000 : fs == FS_FAT16 ? 0x3800 : 0x200;
                const quint32 defaultCluster = ntfs ? 4096 : fs == FS_FAT16 ? 2048 : 512;
                QCOMPARE(sizes.Allowed, supported ? allowed : quint32(0));
                QCOMPARE(sizes.Default, supported ? defaultCluster : quint32(0));
                QCOMPARE(rufus::NativeFileSystemClusterSizes(64 * MiB, fs, false, bootType, scheme).Allowed, quint32(0));
                QCOMPARE(rufus::NativeFileSystemClusterSizes(64 * MiB, fs, true, BT_GRUB2, scheme).Allowed, quint32(0));
                QCOMPARE(rufus::NativeFileSystemClusterSizes(2048 * GiB + 512, fs, true, bootType, scheme).Allowed, quint32(0));
            }
        }
    }

    void freeDosClusters()
    {
        for (int fs : {FS_FAT16, FS_FAT32, FS_NTFS, FS_EXFAT})
        {
            for (auto scheme : {rufus::PartitionScheme::Mbr, rufus::PartitionScheme::Gpt, rufus::PartitionScheme::Sfd})
            {
                const auto sizes = rufus::NativeFileSystemClusterSizes(64 * MiB, fs, false, BT_FREEDOS, scheme);
                const bool supported = (fs == FS_FAT16 || fs == FS_FAT32) && scheme == rufus::PartitionScheme::Mbr;
                if (supported)
                {
                    // Independent values for this MBR/FAT extent, also used by the GUI.
                    QCOMPARE(sizes.Allowed, quint32(fs == FS_FAT16 ? 0x3800 : 0x200));
                    QCOMPARE(sizes.Default, quint32(fs == FS_FAT16 ? 2048 : 512));
                }
                else
                {
                    QCOMPARE(sizes.Allowed, quint32(0));
                    QCOMPARE(sizes.Default, quint32(0));
                }
                QCOMPARE(rufus::NativeFileSystemClusterSizes(64 * MiB, fs, true, BT_FREEDOS, scheme).Allowed, quint32(0));
                QCOMPARE(rufus::NativeFileSystemClusterSizes(2048 * GiB + 512, fs, false, BT_FREEDOS, scheme).Allowed, quint32(0));
            }
        }
    }

    void extLabels()
    {
        QCOMPARE(rufus::NormalizeExtFormatLabel(QStringLiteral("usb.Name*:.test")), QStringLiteral("usb.Name*:.test"));
        QCOMPARE(rufus::NormalizeExtFormatLabel(QStringLiteral("1234567890123456")), QStringLiteral("123456789012345"));
        QCOMPARE(rufus::NormalizeExtFormatLabel(QString::fromUtf8("12345678901234😀!")), QStringLiteral("12345678901234"));
        QCOMPARE(rufus::NormalizeExtFormatLabel(QString::fromUtf8("12345678901😀!")), QString::fromUtf8("12345678901😀"));
    }

    void standaloneGrub4dosClusters()
    {
        for (int fs : {FS_FAT16, FS_FAT32, FS_NTFS, FS_EXFAT, FS_EXT2, FS_EXT3})
        for (auto scheme : {rufus::PartitionScheme::Mbr, rufus::PartitionScheme::Gpt, rufus::PartitionScheme::Sfd})
        {
            const auto sizes = rufus::NativeFileSystemClusterSizes(64 * MiB, fs, false, BT_GRUB4DOS, scheme);
            const bool fat = fs == FS_FAT16 || fs == FS_FAT32;
            const bool ntfs = fs == FS_NTFS;
            const bool supported = (fat || ntfs) && scheme == rufus::PartitionScheme::Mbr;
            const quint32 allowed = ntfs ? 4096 | 8192 | 16384 | 32768 | 65536
                : fs == FS_FAT16 ? 2048 | 4096 | 8192 : 512;
            const quint32 preferred = ntfs ? 4096 : fs == FS_FAT16 ? 2048 : 512;
            QCOMPARE(sizes.Allowed, supported ? allowed : quint32(0));
            QCOMPARE(sizes.Default, supported ? preferred : quint32(0));
            QCOMPARE(rufus::NativeFileSystemClusterSizes(64 * MiB, fs, true, BT_GRUB4DOS, scheme).Allowed, quint32(0));
        }
        for (int fs : {FS_FAT16, FS_FAT32, FS_NTFS})
            QCOMPARE(rufus::NativeFileSystemClusterSizes(2048 * GiB + 512, fs, false,
                BT_GRUB4DOS, rufus::PartitionScheme::Mbr).Allowed, quint32(0));
    }

    void standaloneGrub2Clusters()
    {
        for (int fs : {FS_FAT16, FS_FAT32, FS_NTFS, FS_EXFAT})
        {
            for (auto scheme : {rufus::PartitionScheme::Mbr, rufus::PartitionScheme::Gpt, rufus::PartitionScheme::Sfd})
            {
                const auto sizes = rufus::NativeFileSystemClusterSizes(64 * MiB, fs, false, BT_GRUB2, scheme);
                const bool supported = fs != FS_EXFAT && scheme == rufus::PartitionScheme::Mbr;
                const quint32 allowed = fs == FS_FAT16 ? 0x3800 : fs == FS_NTFS ? 0x1f000 : 0x200;
                const quint32 preferred = fs == FS_FAT16 ? 2048 : fs == FS_NTFS ? 4096 : 512;
                QCOMPARE(sizes.Allowed, supported ? allowed : quint32(0));
                QCOMPARE(sizes.Default, supported ? preferred : quint32(0));
                QCOMPARE(rufus::NativeFileSystemClusterSizes(64 * MiB, fs, true, BT_GRUB2, scheme).Allowed, quint32(0));
                QCOMPARE(rufus::NativeFileSystemClusterSizes(2048 * GiB + 512, fs, false, BT_GRUB2, scheme).Allowed, quint32(0));
            }
        }
        for (int fs : {FS_EXT2, FS_EXT3, FS_EXT4})
        {
            const auto sizes = rufus::NativeFileSystemClusterSizes(320 * MiB, fs, false,
                BT_GRUB2, rufus::PartitionScheme::Mbr);
            QCOMPARE(sizes.Allowed, fs == FS_EXT4 ? quint32(0) : quint32(0x100));
            QCOMPARE(sizes.Default, sizes.Allowed);
            QCOMPARE(rufus::NativeFileSystemClusterSizes(256 * MiB - 512, fs, false,
                BT_GRUB2, rufus::PartitionScheme::Mbr).Allowed, quint32(0));
            QCOMPARE(rufus::NativeFileSystemClusterSizes(320 * MiB, fs, false,
                BT_GRUB2, rufus::PartitionScheme::Gpt).Allowed, quint32(0));
            QCOMPARE(rufus::NativeFileSystemClusterSizes(320 * MiB, fs, true,
                BT_GRUB2, rufus::PartitionScheme::Mbr).Allowed, quint32(0));
            for (int boot : {BT_IMAGE, BT_NON_BOOTABLE, BT_UEFI_NTFS, BT_FREEDOS})
                QCOMPARE(rufus::NativeFileSystemClusterSizes(320 * MiB, fs, false,
                    boot, rufus::PartitionScheme::Mbr).Allowed,
                    boot == BT_NON_BOOTABLE && fs != FS_EXT4 ? quint32(0x100) : quint32(0));
        }
    }

    void standaloneLayout_data()
    {
        QTest::addColumn<int>("fileSystem");
        QTest::addColumn<bool>("mbr");
        QTest::newRow("GPT-exFAT") << int(FS_EXFAT) << false;
        QTest::newRow("MBR-exFAT") << int(FS_EXFAT) << true;
        QTest::newRow("GPT-NTFS") << int(FS_NTFS) << false;
        QTest::newRow("MBR-NTFS") << int(FS_NTFS) << true;
    }

    void standaloneLayout()
    {
        QFETCH(int, fileSystem);
        QFETCH(bool, mbr);
        rufus::PrepareImageRequest request;
        request.fileSystem = fileSystem;
        request.mbr = mbr;
        request.nonBootable = true;
        request.size = 64 * MiB;
        request.clusterSize = fileSystem == FS_EXFAT ? 128 * 1024 : 4096;
        GPT_IMAGE_RESULT plain = {}, bootable = {};
        QCOMPARE(rufus::PlanFormatPartition(request, plain), 0);
        QCOMPARE(plain.uefi_ntfs_size, quint64(0));
        request.uefiNtfs = true;
        request.nonBootable = false;
        QCOMPARE(rufus::PlanFormatPartition(request, bootable), 0);
        // Independent Rufus track/cluster rounding with the 1-MiB embedded image.
        constexpr quint64 trackBytes = 63 * 512;
        const quint64 backupBytes = mbr ? 0 : 33 * 512;
        const quint64 bootSize = (MiB + trackBytes - 1) / trackBytes * trackBytes;
        const quint64 bootOffset = (request.size - backupBytes - bootSize) / trackBytes * trackBytes;
        const quint64 mainSize = (bootOffset - MiB) / trackBytes * trackBytes / request.clusterSize * request.clusterSize;
        QCOMPARE(bootable.offset, MiB);
        QCOMPARE(bootable.size, mainSize);
        QCOMPARE(bootable.uefi_ntfs_offset, bootOffset);
        QCOMPARE(bootable.uefi_ntfs_size, bootSize);
        QVERIFY(bootable.size < plain.size);
        QCOMPARE(rufus::CheckFormatSettings(request, bootable), 0);
        const auto scheme = mbr ? rufus::PartitionScheme::Mbr : rufus::PartitionScheme::Gpt;
        const auto sizes = rufus::NativeFileSystemClusterSizes(request.size, fileSystem, false, BT_UEFI_NTFS, scheme);
        QCOMPARE(sizes.Default, quint32(4096));
        QVERIFY(sizes.Allowed & request.clusterSize);
        quint32 candidates = sizes.Allowed;
        while (candidates != 0)
        {
            const quint32 cluster = candidates & (0u - candidates);
            candidates &= ~cluster;
            const quint64 size = (bootOffset - MiB) / trackBytes * trackBytes / cluster * cluster;
            if (fileSystem == FS_NTFS)
            {
                const rufus::NtfsFormatOptions format = {size, MiB, cluster, {}};
                QCOMPARE(rufus::CheckNtfsFormatOptions(format), 0);
            }
            else
            {
                const rufus::ExfatFormatOptions format = {size, cluster, {}, MiB};
                QCOMPARE(rufus::CheckExfatFormatOptions(format), 0);
            }
        }
        QCOMPARE(rufus::NativeFileSystemClusterSizes(request.size, fileSystem, false, BT_UEFI_NTFS,
            rufus::PartitionScheme::Sfd).Allowed, quint32(0));
        QCOMPARE(rufus::NativeFileSystemClusterSizes(request.size, fileSystem, true, BT_UEFI_NTFS,
            scheme).Allowed, quint32(0));
    }

    void labels_data()
    {
        QTest::addColumn<QString>("input");
        QTest::addColumn<QString>("expected");
        QTest::newRow("lowercase") << QStringLiteral("rufus test") << QStringLiteral("RUFUS TEST");
        QTest::newRow("punctuation") << QStringLiteral("usb.name*:long") << QStringLiteral("USB_NAMELON");
        QTest::newRow("utf16-units") << QString::fromUtf8("ABCD😀ș") << QStringLiteral("ABCD___");
        QTest::newRow("fallback") << QString::fromUtf8("😀") << QStringLiteral("64 MB");
        QTest::newRow("empty") << QString() << QString();
        QTest::newRow("original-spaces") << QStringLiteral(" disk ") << QStringLiteral(" DISK ");
        QTest::newRow("fat-padding") << QStringLiteral("disk ") << QStringLiteral("DISK");
        QTest::newRow("truncated-alpine") << QStringLiteral("alpine-std 3.22.5 x86_64") << QStringLiteral("ALPINE-STD");
    }

    void labels()
    {
        QFETCH(QString, input);
        QFETCH(QString, expected);
        QCOMPARE(rufus::NormalizeFormatLabel(input, 64 * MiB, false), expected);
    }

    void ntfsLabelsAndClusters()
    {
        const QString label = QString::fromUtf8("Windows 11 ș😀.test");
        QCOMPARE(rufus::NormalizeFormatLabel(label, 64 * MiB, true), QString::fromUtf8("Windows 11 ș😀_test"));
        QCOMPARE(rufus::NormalizeFormatLabel(QString(40, u'a'), 64 * MiB, true), QString(32, u'a'));
        const RUFUS_CLUSTER_SIZES small = rufus::NativeClusterSizes(MiB, true);
        QCOMPARE(small.Allowed, quint32(0));
        QCOMPARE(small.Default, quint32(0));
        for (quint64 disk : {64 * MiB, 8 * GiB})
        {
            const RUFUS_CLUSTER_SIZES sizes = rufus::NativeClusterSizes(disk, true);
            QCOMPARE(sizes.Allowed, quint32(0x1f000));
            QCOMPARE(sizes.Default, quint32(4096));
        }
    }

    void clusterChoices_data()
    {
        QTest::addColumn<quint64>("disk");
        QTest::addColumn<quint32>("allowed");
        QTest::addColumn<quint32>("standard");
        QTest::newRow("empty") << quint64(0) << quint32(0) << quint32(0);
        QTest::newRow("partition-too-small") << 33 * MiB << quint32(0) << quint32(0);
        QTest::newRow("64-MiB") << 64 * MiB << quint32(0x200) << quint32(512);
        QTest::newRow("128-MiB") << 128 * MiB << quint32(0x600) << quint32(1024);
        QTest::newRow("256-MiB") << 256 * MiB << quint32(0xe00) << quint32(512);
        QTest::newRow("259-MiB") << 259 * MiB << quint32(0x1e00) << quint32(4096);
        QTest::newRow("16-GiB") << 16 * GiB << quint32(0x1f000) << quint32(8192);
        QTest::newRow("32-GiB") << 32 * GiB << quint32(0x1c000) << quint32(32768);
        QTest::newRow("2-TiB") << 2048 * GiB << quint32(0) << quint32(0);
    }

    void fat16ClusterChoices_data()
    {
        QTest::addColumn<quint64>("disk");
        QTest::addColumn<quint32>("allowed");
        QTest::addColumn<quint32>("standard");
        QTest::newRow("empty") << quint64(0) << quint32(0) << quint32(0);
        QTest::newRow("too-small") << 2 * MiB << quint32(0) << quint32(0);
        QTest::newRow("16-MiB") << 16 * MiB << quint32(0xe00) << quint32(512);
        QTest::newRow("32-MiB") << 32 * MiB << quint32(0x1c00) << quint32(1024);
        QTest::newRow("64-MiB") << 64 * MiB << quint32(0x3800) << quint32(2048);
        QTest::newRow("128-MiB") << 128 * MiB << quint32(0x7000) << quint32(4096);
        QTest::newRow("256-MiB") << 256 * MiB << quint32(0xe000) << quint32(8192);
        QTest::newRow("512-MiB") << 512 * MiB << quint32(0x1c000) << quint32(16384);
        QTest::newRow("1-GiB") << GiB << quint32(0x18000) << quint32(32768);
        QTest::newRow("2-GiB") << 2 * GiB << quint32(0x10000) << quint32(65536);
        QTest::newRow("last-supported-sector") << 4 * GiB - 512 << quint32(0x10000) << quint32(65536);
        QTest::newRow("4-GiB") << 4 * GiB << quint32(0) << quint32(0);
    }

    void fat16ClusterChoices()
    {
        QFETCH(quint64, disk);
        QFETCH(quint32, allowed);
        QFETCH(quint32, standard);
        for (bool syslinux : {false, true})
        {
            const RUFUS_CLUSTER_SIZES sizes = rufus::NativeFileSystemClusterSizes(disk, FS_FAT16, syslinux);
            QCOMPARE(sizes.Allowed, allowed);
            QCOMPARE(sizes.Default, standard);
            quint32 candidates = sizes.Allowed;
            while (candidates != 0)
            {
                const quint32 cluster = candidates & (0u - candidates);
                candidates &= ~cluster;
                // Independently check every offered cluster against its extent.
                const quint64 trackBytes = 63 * 512;
                const quint64 reservedTail = syslinux ? 0 : 33 * 512;
                const quint64 size = (disk - MiB - reservedTail) / trackBytes * trackBytes / cluster * cluster;
                const FAT16_FORMAT_OPTIONS format = {MiB, size, cluster, 0, {0}};
                QCOMPARE(CheckFat16FormatOptions(&format), 0);
            }
        }
    }

    void explicitFileSystemSelection()
    {
        for (int fs : {FS_UNKNOWN, FS_UDF, FS_MAX})
        {
            const RUFUS_CLUSTER_SIZES unsupported = rufus::NativeFileSystemClusterSizes(64 * MiB, fs);
            QCOMPARE(unsupported.Allowed, quint32(0));
            QCOMPARE(unsupported.Default, quint32(0));
        }
        for (int fs : {FS_FAT32, FS_NTFS})
        {
            for (bool syslinux : {false, true})
            {
                const RUFUS_CLUSTER_SIZES explicitSizes = rufus::NativeFileSystemClusterSizes(64 * MiB, fs, syslinux);
                const RUFUS_CLUSTER_SIZES existing = rufus::NativeClusterSizes(64 * MiB, fs == FS_NTFS, syslinux);
                QCOMPARE(explicitSizes.Allowed, existing.Allowed);
                QCOMPARE(explicitSizes.Default, existing.Default);
            }
        }
    }

    void nonBootableNtfsClusters()
    {
        QCOMPARE(rufus::NativeFileSystemClusterSizes(17 * MiB, FS_NTFS, false, BT_NON_BOOTABLE).Allowed, quint32(0));
        // The data volume fits without UEFI:NTFS, but not with its boot partition.
        QCOMPARE(rufus::NativeFileSystemClusterSizes(18 * MiB, FS_NTFS).Allowed, quint32(0));
        for (quint64 disk : {18 * MiB, 64 * MiB, 8 * GiB, 2048 * GiB})
        {
            const RUFUS_CLUSTER_SIZES sizes = rufus::NativeFileSystemClusterSizes(disk, FS_NTFS, false, BT_NON_BOOTABLE);
            QCOMPARE(sizes.Allowed, quint32(0x1f000));
            QCOMPARE(sizes.Default, quint32(4096));
            quint32 candidates = sizes.Allowed;
            while (candidates != 0)
            {
                const quint32 cluster = candidates & (0u - candidates);
                candidates &= ~cluster;
                const quint64 trackBytes = 63 * 512;
                const quint64 size = (disk - MiB - 33 * 512) / trackBytes * trackBytes / cluster * cluster;
                const rufus::NtfsFormatOptions format = {size, MiB, cluster, {}};
                QCOMPARE(rufus::CheckNtfsFormatOptions(format), 0);
            }
        }
    }

    void nonBootableMbrClusters()
    {
        constexpr quint64 trackBytes = 63 * 512;
        for (int fs : {FS_FAT16, FS_FAT32, FS_NTFS, FS_EXFAT})
        {
            const auto image = rufus::NativeFileSystemClusterSizes(64 * MiB, fs, false, BT_IMAGE, rufus::PartitionScheme::Mbr);
            QVERIFY(image.Allowed != 0);
            const auto sizes = rufus::NativeFileSystemClusterSizes(64 * MiB, fs, false, BT_NON_BOOTABLE, rufus::PartitionScheme::Mbr);
            QVERIFY(sizes.Allowed != 0 && (sizes.Allowed & sizes.Default) != 0);
            quint32 candidates = sizes.Allowed;
            while (candidates != 0)
            {
                const quint32 cluster = candidates & (0u - candidates);
                candidates &= ~cluster;
                const quint64 size = (64 * MiB - MiB) / trackBytes * trackBytes / cluster * cluster;
                int error;
                if (fs == FS_NTFS)
                    error = rufus::CheckNtfsFormatOptions({size, MiB, cluster, {}});
                else if (fs == FS_EXFAT)
                    error = rufus::CheckExfatFormatOptions({size, cluster, {}, MiB});
                else
                {
                    const FAT_FORMAT_OPTIONS options = {MiB, size, cluster, 0, {0}};
                    error = fs == FS_FAT16 ? CheckFat16FormatOptions(&options) : CheckFat32FormatOptions(&options);
                }
                QCOMPARE(error, 0);
            }
            QCOMPARE(rufus::NativeFileSystemClusterSizes(2048 * GiB + 512, fs, false, BT_NON_BOOTABLE, rufus::PartitionScheme::Mbr).Allowed, quint32(0));
        }
        QVERIFY(rufus::NativeFileSystemClusterSizes(2048 * GiB, FS_NTFS, false, BT_NON_BOOTABLE, rufus::PartitionScheme::Mbr).Allowed != 0);
        QVERIFY(rufus::NativeFileSystemClusterSizes(2048 * GiB + 512, FS_NTFS, false, BT_NON_BOOTABLE).Allowed != 0);
        // A whole 16-MiB NTFS volume fits this MBR extent, but not GPT's tail.
        constexpr quint64 ntfsBoundary = MiB + 521 * trackBytes;
        QVERIFY(rufus::NativeFileSystemClusterSizes(ntfsBoundary, FS_NTFS, false, BT_NON_BOOTABLE, rufus::PartitionScheme::Mbr).Allowed != 0);
        QCOMPARE(rufus::NativeFileSystemClusterSizes(ntfsBoundary, FS_NTFS, false, BT_NON_BOOTABLE).Allowed, quint32(0));
    }

    void sfdClusters()
    {
        for (quint64 disk : {33 * MiB, 64 * MiB, 128 * MiB, 64 * GiB})
        {
            const auto sizes = rufus::NativeFileSystemClusterSizes(disk, FS_FAT32, false, BT_NON_BOOTABLE, rufus::PartitionScheme::Sfd);
            QVERIFY(sizes.Allowed != 0 && (sizes.Allowed & sizes.Default) != 0);
            quint32 candidates = sizes.Allowed;
            while (candidates != 0)
            {
                const quint32 cluster = candidates & (0u - candidates);
                candidates &= ~cluster;
                const FAT_FORMAT_OPTIONS format = {0, disk, cluster, 0, {0}};
                QCOMPARE(CheckFat32FormatOptions(&format), 0);
            }
        }
        // A whole 33-MiB FAT32 volume fits, unlike its smaller GPT partition.
        QCOMPARE(rufus::NativeFileSystemClusterSizes(33 * MiB, FS_FAT32, false, BT_NON_BOOTABLE).Allowed, quint32(0));
        for (quint64 disk : {quint64(0), 16 * MiB, 64 * MiB + 1, 2048 * GiB})
            QCOMPARE(rufus::NativeFileSystemClusterSizes(disk, FS_FAT32, false, BT_NON_BOOTABLE, rufus::PartitionScheme::Sfd).Allowed, quint32(0));
        for (int fs : {FS_REFS, FS_UNKNOWN})
            QCOMPARE(rufus::NativeFileSystemClusterSizes(64 * MiB, fs, false, BT_NON_BOOTABLE, rufus::PartitionScheme::Sfd).Allowed, quint32(0));
        QCOMPARE(rufus::NativeFileSystemClusterSizes(64 * MiB, FS_FAT32, false, BT_IMAGE, rufus::PartitionScheme::Sfd).Allowed, quint32(0));
        QCOMPARE(rufus::NativeFileSystemClusterSizes(64 * MiB, FS_FAT32, true, BT_NON_BOOTABLE, rufus::PartitionScheme::Sfd).Allowed, quint32(0));
    }

    void sfdFat16Clusters()
    {
        for (quint64 disk : {3 * MiB, 16 * MiB, 64 * MiB, 4095 * MiB})
        {
            const auto sizes = rufus::NativeFileSystemClusterSizes(disk, FS_FAT16, false, BT_NON_BOOTABLE, rufus::PartitionScheme::Sfd);
            QVERIFY(sizes.Allowed != 0 && (sizes.Allowed & sizes.Default) != 0);
            quint32 candidates = sizes.Allowed;
            while (candidates != 0)
            {
                const quint32 cluster = candidates & (0u - candidates);
                candidates &= ~cluster;
                const FAT_FORMAT_OPTIONS format = {0, disk, cluster, 0, {0}};
                QCOMPARE(CheckFat16FormatOptions(&format), 0);
            }
        }
        // Whole-device FAT16 fits where the GPT partition prefix would not.
        QCOMPARE(rufus::NativeFileSystemClusterSizes(3 * MiB, FS_FAT16, false, BT_NON_BOOTABLE).Allowed, quint32(0));
        // Without that prefix, the last sector below 4 GiB exceeds native FAT16's cluster limit.
        for (quint64 disk : {quint64(0), 2 * MiB, 16 * MiB + 1, 4096 * MiB - 512, 4096 * MiB})
            QCOMPARE(rufus::NativeFileSystemClusterSizes(disk, FS_FAT16, false, BT_NON_BOOTABLE, rufus::PartitionScheme::Sfd).Allowed, quint32(0));
        QCOMPARE(rufus::NativeFileSystemClusterSizes(64 * MiB, FS_FAT16, false, BT_IMAGE, rufus::PartitionScheme::Sfd).Allowed, quint32(0));
        QCOMPARE(rufus::NativeFileSystemClusterSizes(64 * MiB, FS_FAT16, true, BT_NON_BOOTABLE, rufus::PartitionScheme::Sfd).Allowed, quint32(0));
    }

    void sfdNtfsClusters()
    {
        for (quint64 disk : {16 * MiB, 64 * MiB, 64 * GiB, 2048 * GiB, 2048 * GiB + 512, 256 * 1024 * GiB - 512})
        {
            const auto sizes = rufus::NativeFileSystemClusterSizes(disk, FS_NTFS, false, BT_NON_BOOTABLE, rufus::PartitionScheme::Sfd);
            QVERIFY(sizes.Allowed != 0 && (sizes.Allowed & sizes.Default) != 0);
            quint32 candidates = sizes.Allowed;
            while (candidates != 0)
            {
                const quint32 cluster = candidates & (0u - candidates);
                candidates &= ~cluster;
                const rufus::NtfsFormatOptions format = {disk, 0, cluster, {}};
                QCOMPARE(rufus::CheckNtfsFormatOptions(format), 0);
            }
        }
        // No partition prefix or GPT tail reduces this minimum native volume.
        QCOMPARE(rufus::NativeFileSystemClusterSizes(16 * MiB, FS_NTFS, false, BT_NON_BOOTABLE).Allowed, quint32(0));
        for (quint64 disk : {quint64(0), 16 * MiB - 512, 64 * MiB + 1, 256 * 1024 * GiB})
            QCOMPARE(rufus::NativeFileSystemClusterSizes(disk, FS_NTFS, false, BT_NON_BOOTABLE, rufus::PartitionScheme::Sfd).Allowed, quint32(0));
        QCOMPARE(rufus::NativeFileSystemClusterSizes(64 * MiB, FS_NTFS, false, BT_IMAGE, rufus::PartitionScheme::Sfd).Allowed, quint32(0));
        QCOMPARE(rufus::NativeFileSystemClusterSizes(64 * MiB, FS_NTFS, true, BT_NON_BOOTABLE, rufus::PartitionScheme::Sfd).Allowed, quint32(0));
    }

    void sfdExfatClusters()
    {
        for (quint64 disk : {16 * MiB, 64 * MiB, 256 * MiB - 512, 256 * MiB,
            32 * GiB - 512, 32 * GiB, 2048 * GiB, 256 * 1024 * GiB - 512})
        {
            const auto sizes = rufus::NativeFileSystemClusterSizes(disk, FS_EXFAT, false, BT_NON_BOOTABLE, rufus::PartitionScheme::Sfd);
            QVERIFY(sizes.Allowed != 0 && (sizes.Allowed & sizes.Default) != 0);
            const quint32 expectedDefault = disk < 256 * MiB ? 4096 : disk < 32 * GiB ? 32768 : 131072;
            QCOMPARE(sizes.Default, expectedDefault);
            quint32 candidates = sizes.Allowed;
            while (candidates != 0)
            {
                const quint32 cluster = candidates & (0u - candidates);
                candidates &= ~cluster;
                const rufus::ExfatFormatOptions format = {disk, cluster, {}, 0};
                QCOMPARE(rufus::CheckExfatFormatOptions(format), 0);
            }
        }
        // Minimum whole volume fits even though the GPT partition would be too small.
        QCOMPARE(rufus::NativeFileSystemClusterSizes(16 * MiB, FS_EXFAT, false, BT_NON_BOOTABLE).Allowed, quint32(0));
        const auto small = rufus::NativeFileSystemClusterSizes(64 * MiB, FS_EXFAT, false, BT_NON_BOOTABLE, rufus::PartitionScheme::Sfd);
        QVERIFY((small.Allowed & (32 * MiB)) == 0); // Native metadata cannot fit.
        for (quint64 disk : {quint64(0), 16 * MiB - 512, 64 * MiB + 1, 256 * 1024 * GiB})
            QCOMPARE(rufus::NativeFileSystemClusterSizes(disk, FS_EXFAT, false, BT_NON_BOOTABLE, rufus::PartitionScheme::Sfd).Allowed, quint32(0));
        QCOMPARE(rufus::NativeFileSystemClusterSizes(64 * MiB, FS_EXFAT, false, BT_IMAGE, rufus::PartitionScheme::Sfd).Allowed, quint32(0));
        QCOMPARE(rufus::NativeFileSystemClusterSizes(64 * MiB, FS_EXFAT, true, BT_NON_BOOTABLE, rufus::PartitionScheme::Sfd).Allowed, quint32(0));
    }

    void exfatGptClusters()
    {
        QCOMPARE(rufus::NativeFileSystemClusterSizes(16 * MiB, FS_EXFAT).Allowed, quint32(0));
        QCOMPARE(rufus::NativeFileSystemClusterSizes(64 * MiB, FS_EXFAT, true).Allowed, quint32(0));
        for (quint64 disk : {64 * MiB, 256 * MiB - 512, 256 * MiB, 32 * GiB - 512, 32 * GiB})
        {
            const RUFUS_CLUSTER_SIZES sizes = rufus::NativeFileSystemClusterSizes(disk, FS_EXFAT);
            const quint32 expectedDefault = disk < 256 * MiB ? 4096 : disk < 32 * GiB ? 32768 : 131072;
            QCOMPARE(sizes.Default, expectedDefault);
            quint32 candidates = sizes.Allowed;
            while (candidates != 0)
            {
                const quint32 cluster = candidates & (0u - candidates);
                candidates &= ~cluster;
                const quint64 trackBytes = 63 * 512;
                const quint64 size = (disk - MiB - 33 * 512) / trackBytes * trackBytes / cluster * cluster;
                const rufus::ExfatFormatOptions format = {size, cluster, {}, MiB};
                QCOMPARE(rufus::CheckExfatFormatOptions(format), 0);
            }
        }
        QVERIFY((rufus::NativeFileSystemClusterSizes(64 * MiB, FS_EXFAT).Allowed & (32 * MiB)) == 0);
        QVERIFY((rufus::NativeFileSystemClusterSizes(256 * MiB, FS_EXFAT).Allowed & (32 * MiB)) != 0);
    }

    void syslinuxClustersUseMbrLayout()
    {
        QCOMPARE(rufus::NativeClusterSizes(64 * MiB, true, true).Allowed, quint32(0x1f000));
        for (quint64 disk : {64 * MiB, 128 * MiB, 256 * MiB, 512 * MiB, 16 * GiB, 32 * GiB})
        {
            const RUFUS_CLUSTER_SIZES sizes = rufus::NativeClusterSizes(disk, false, BT_NON_BOOTABLE);
            QVERIFY(sizes.Default != 0 && (sizes.Default & sizes.Allowed) != 0);
            quint32 candidates = sizes.Allowed;
            while (candidates != 0)
            {
                const quint32 cluster = candidates & (0u - candidates);
                candidates &= ~cluster;
                // Independent validation of every offered cluster against the
                // actual MBR extent, with no GPT tail reservation.
                const quint64 trackBytes = 63 * 512;
                const quint64 size = (disk - MiB) / trackBytes * trackBytes / cluster * cluster;
                const FAT32_FORMAT_OPTIONS format = {MiB, size, cluster, 0, {0}};
                QCOMPARE(CheckFat32FormatOptions(&format), 0);
            }
        }
        QCOMPARE(rufus::NativeClusterSizes(512 * MiB, false, BT_NON_BOOTABLE).Default, quint32(4096));
        QCOMPARE(rufus::NativeClusterSizes(2048 * GiB, false, BT_NON_BOOTABLE).Allowed, quint32(0));
    }

    void syslinuxNtfsImageClusters()
    {
        for (bool efi : {false, true})
        {
            const auto sizes = rufus::NativeFileSystemClusterSizes(64 * MiB, FS_NTFS,
                true, BT_IMAGE, rufus::PartitionScheme::Gpt, efi);
            QCOMPARE(sizes.Allowed, quint32(0x1f000));
            QCOMPARE(sizes.Default, quint32(4096));
            for (quint32 cluster = 4096; cluster <= 65536; cluster *= 2)
            {
                rufus::PrepareImageRequest request;
                request.size = 64 * MiB;
                request.fileSystem = FS_NTFS;
                request.syslinux = true;
                request.uefiNtfs = efi;
                request.clusterSize = cluster;
                GPT_IMAGE_RESULT partition = {};
                QCOMPARE(rufus::PlanFormatPartition(request, partition), 0);
                // Independent 64-MiB layouts: 126880 sectors with the trailing
                // loader, 129024 without it, then main-volume cluster alignment.
                const quint64 mainBytes = efi ? 64962560 : 66060288;
                const quint64 expectedSize = mainBytes - mainBytes % cluster;
                QCOMPARE(partition.offset, quint64(MiB));
                QCOMPARE(partition.size, expectedSize);
                QVERIFY((partition.uefi_ntfs_size != 0) == efi);
                QCOMPARE(rufus::CheckFormatSettings(request, partition), 0);
            }
        }
        // An image's extra loader cannot turn another boot/filesystem mode into NTFS Syslinux.
        QCOMPARE(rufus::NativeFileSystemClusterSizes(64 * MiB, FS_FAT32,
            true, BT_IMAGE, rufus::PartitionScheme::Gpt, true).Allowed, quint32(0));
        QCOMPARE(rufus::NativeFileSystemClusterSizes(64 * MiB, FS_NTFS,
            true, BT_SYSLINUX_V6, rufus::PartitionScheme::Mbr, true).Allowed, quint32(0));
    }

    void windowsFat32ImageClusters()
    {
        for (auto boot : {MBR_BOOT_RUFUS, MBR_BOOT_WIN7})
        {
            const auto sizes = rufus::NativeFileSystemClusterSizes(64 * MiB, FS_FAT32,
                false, BT_IMAGE, rufus::PartitionScheme::Mbr, false, boot);
            QCOMPARE(sizes.Allowed, quint32(512));
            QCOMPARE(sizes.Default, quint32(512));
            rufus::PrepareImageRequest request;
            request.size = 64 * MiB;
            request.fileSystem = FS_FAT32;
            request.windowsBios = request.mbr = request.enhancedInstall = true;
            request.windowsRufusMbr = boot == MBR_BOOT_RUFUS;
            request.clusterSize = 512;
            request.volumeId = 0x1234abcd;
            GPT_IMAGE_RESULT partition = {};
            QCOMPARE(rufus::PlanFormatPartition(request, partition), 0);
            QCOMPARE(partition.offset, MiB);
            QCOMPARE(partition.size, quint64(66060288));
            QCOMPARE(partition.uefi_ntfs_offset, quint64(0));
            QCOMPARE(partition.uefi_ntfs_size, quint64(0));
            QCOMPARE(rufus::CheckFormatSettings(request, partition), 0);
            QCOMPARE(rufus::NativeFileSystemClusterSizes(64 * MiB, FS_FAT32,
                false, BT_IMAGE, rufus::PartitionScheme::Mbr, true, boot).Allowed, quint32(0));
            QCOMPARE(rufus::NativeFileSystemClusterSizes(64 * MiB, FS_FAT32,
                false, BT_IMAGE, rufus::PartitionScheme::Gpt, false, boot).Allowed, quint32(0));
        }
    }

    void windowsNtfsImageClusters()
    {
        for (auto boot : {MBR_BOOT_RUFUS, MBR_BOOT_WIN7})
        for (bool dual : {false, true})
        {
            const auto sizes = rufus::NativeFileSystemClusterSizes(64 * MiB, FS_NTFS,
                false, BT_IMAGE, rufus::PartitionScheme::Mbr, dual, boot);
            QCOMPARE(sizes.Allowed, quint32(0x1f000));
            QCOMPARE(sizes.Default, quint32(4096));
            for (quint32 cluster = 4096; cluster <= 65536; cluster *= 2)
            {
                rufus::PrepareImageRequest request;
                request.size = 64 * MiB;
                request.fileSystem = FS_NTFS;
                request.windowsBios = request.mbr = true;
                request.windowsRufusMbr = boot == MBR_BOOT_RUFUS;
                request.enhancedInstall = request.uefiNtfs = dual;
                request.clusterSize = cluster;
                GPT_IMAGE_RESULT partition = {};
                QCOMPARE(rufus::FormatMbrBootCode(request), boot);
                QCOMPARE(rufus::PlanFormatPartition(request, partition), 0);
                const quint64 mainBytes = dual ? 64962560 : 66060288;
                QCOMPARE(partition.offset, MiB);
                QCOMPARE(partition.size, mainBytes - mainBytes % cluster);
                QCOMPARE(partition.uefi_ntfs_size, dual ? quint64(1064448) : quint64(0));
                QCOMPARE(rufus::CheckFormatSettings(request, partition), 0);
            }
            QCOMPARE(rufus::NativeFileSystemClusterSizes(18 * MiB, FS_NTFS,
                false, BT_IMAGE, rufus::PartitionScheme::Mbr, dual, boot).Allowed,
                dual ? quint32(0) : quint32(0x1f000));
            for (int fs : {FS_FAT16, FS_EXFAT})
                QCOMPARE(rufus::NativeFileSystemClusterSizes(64 * MiB, fs,
                    false, BT_IMAGE, rufus::PartitionScheme::Mbr, dual, boot).Allowed,
                    fs == FS_FAT16 && !dual ? quint32(0x3800) : quint32(0));
            QCOMPARE(rufus::NativeFileSystemClusterSizes(64 * MiB, FS_NTFS,
                true, BT_IMAGE, rufus::PartitionScheme::Mbr, dual, boot).Allowed, quint32(0));
            QCOMPARE(rufus::NativeFileSystemClusterSizes(64 * MiB, FS_NTFS,
                false, BT_IMAGE, rufus::PartitionScheme::Gpt, dual, boot).Allowed, quint32(0));
            QCOMPARE(rufus::NativeFileSystemClusterSizes(2048 * GiB + 512, FS_NTFS,
                false, BT_IMAGE, rufus::PartitionScheme::Mbr, dual, boot).Allowed, quint32(0));
        }
    }

    void grubNtfsImageClusters()
    {
        for (bool efi : {false, true})
        {
            const auto sizes = rufus::NativeFileSystemClusterSizes(64 * MiB, FS_NTFS,
                false, BT_IMAGE, rufus::PartitionScheme::Mbr, efi);
            QCOMPARE(sizes.Allowed, quint32(0x1f000));
            QCOMPARE(sizes.Default, quint32(4096));
            for (quint32 cluster = 4096; cluster <= 65536; cluster *= 2)
            {
                rufus::PrepareImageRequest request;
                request.size = 64 * MiB;
                request.fileSystem = FS_NTFS;
                request.grub2 = request.mbr = true;
                request.uefiNtfs = efi;
                request.clusterSize = cluster;
                GPT_IMAGE_RESULT partition = {};
                QCOMPARE(rufus::PlanFormatPartition(request, partition), 0);
                // Independent track-rounded MBR extents before cluster alignment.
                const quint64 mainBytes = efi ? 64962560 : 66060288;
                QCOMPARE(partition.offset, MiB);
                QCOMPARE(partition.size, mainBytes - mainBytes % cluster);
                QCOMPARE(partition.uefi_ntfs_size, efi ? quint64(1064448) : quint64(0));
                QCOMPARE(rufus::CheckFormatSettings(request, partition), 0);
            }
            // At 18 MiB, only the plain BIOS volume reaches NTFS's 16-MiB minimum.
            QCOMPARE(rufus::NativeFileSystemClusterSizes(18 * MiB, FS_NTFS,
                false, BT_IMAGE, rufus::PartitionScheme::Mbr, efi).Allowed,
                efi ? quint32(0) : quint32(0x1f000));
            QCOMPARE(rufus::NativeFileSystemClusterSizes(19 * MiB, FS_NTFS,
                false, BT_IMAGE, rufus::PartitionScheme::Mbr, efi).Allowed, quint32(0x1f000));
        }
        for (int fs : {FS_FAT16, FS_FAT32, FS_EXFAT, FS_EXT2})
            QCOMPARE(rufus::NativeFileSystemClusterSizes(64 * MiB, fs,
                false, BT_IMAGE, rufus::PartitionScheme::Mbr, true).Allowed, quint32(0));
        for (auto scheme : {rufus::PartitionScheme::Gpt, rufus::PartitionScheme::Sfd})
            QCOMPARE(rufus::NativeFileSystemClusterSizes(64 * MiB, FS_NTFS,
                false, BT_IMAGE, scheme, true).Allowed, quint32(0));
        QCOMPARE(rufus::NativeFileSystemClusterSizes(64 * MiB, FS_NTFS,
            false, BT_GRUB2, rufus::PartitionScheme::Mbr, true).Allowed, quint32(0));
    }

    void clusterChoices()
    {
        QFETCH(quint64, disk);
        QFETCH(quint32, allowed);
        QFETCH(quint32, standard);
        const RUFUS_CLUSTER_SIZES sizes = rufus::NativeClusterSizes(disk, false);
        QCOMPARE(sizes.Allowed, allowed);
        QCOMPARE(sizes.Default, standard);
        // Planning is independent of libfat's temporary embedded-image sector
        // setting. It neither reads nor changes that worker-owned state.
        const quint32 saved = LIBFAT_SECTOR_SIZE;
        LIBFAT_SECTOR_SIZE = 4096;
        const RUFUS_CLUSTER_SIZES duringScan = rufus::NativeClusterSizes(disk, false);
        LIBFAT_SECTOR_SIZE = saved;
        QCOMPARE(duringScan.Allowed, allowed);
        QCOMPARE(duringScan.Default, standard);
    }
};

QTEST_GUILESS_MAIN(TestFormatOptions)
#include "test_format_options.moc"
