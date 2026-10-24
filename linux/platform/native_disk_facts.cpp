// SPDX-License-Identifier: GPL-3.0-or-later
#include "native_disk_facts.h"
#include "application_devices.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <cerrno>
#include <limits>
#include <sys/sysmacros.h>

namespace
{
constexpr qint64 attributeLimit = 4096;
constexpr quint64 sysfsSectorBytes = 512;

bool cardMedia(const QString& media)
{
    // UDisks Drive.MediaCompatibility identifiers. "thumb" is a USB stick,
    // whereas generic "flash" explicitly denotes a card. Do not guess from
    // an arbitrary "flash_*" prefix or from model/display-name substrings.
    const char* const kinds[] = {"flash", "flash_cf", "flash_ms", "flash_sm",
        "flash_sd", "flash_sdhc", "flash_sdxc", "flash_sdio", "flash_sd_combo", "flash_mmc"};
    for (const char* kind : kinds)
        if (media == QLatin1String(kind))
            return true;
    return false;
}

QByteArray readAttribute(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QByteArray bytes = file.read(attributeLimit + 1);
    const bool complete = file.error() == QFileDevice::NoError && bytes.size() <= attributeLimit;
    if (!complete || bytes.contains('\0'))
        return {};
    return bytes.trimmed();
}

bool readDecimal(const QString& path, quint64& value)
{
    const QByteArray bytes = readAttribute(path);
    if (bytes.isEmpty())
        return false;
    for (char c : bytes)
        if (c < '0' || c > '9')
            return false;
    bool valid = false;
    value = bytes.toULongLong(&valid, 10);
    return valid;
}

bool readUsbId(const QString& path, quint16& value)
{
    const QByteArray bytes = readAttribute(path);
    if (bytes.size() != 4)
        return false;
    for (char c : bytes)
    {
        const bool digit = c >= '0' && c <= '9';
        const bool lower = c >= 'a' && c <= 'f';
        const bool upper = c >= 'A' && c <= 'F';
        if (!digit && !lower && !upper)
            return false;
    }
    bool valid = false;
    value = bytes.toUShort(&valid, 16);
    return valid;
}

unsigned readSectorSize(const QString& path, const QString& attribute)
{
    quint64 bytes = 0;
    if (!readDecimal(path + QStringLiteral("/queue/") + attribute, bytes))
        return 0;
    const bool valid = bytes >= 512 && bytes <= 65536 && (bytes & (bytes - 1)) == 0;
    return valid ? unsigned(bytes) : 0;
}

bool hasSubsystem(const QString& path, const QString& subsystem, const QString& root)
{
    const QString actual = QFileInfo(path + QStringLiteral("/subsystem")).canonicalFilePath();
    const QString expected = QFileInfo(root + subsystem).canonicalFilePath();
    return !expected.isEmpty() && actual == expected;
}

QString usbParent(QString path, const QString& devices, const QString& root)
{
    while (path.startsWith(devices + u'/'))
    {
        const bool usb = hasSubsystem(path, QStringLiteral("/bus/usb"), root);
        if (usb)
        {
            const QList<QByteArray> properties = readAttribute(path + QStringLiteral("/uevent")).split('\n');
            if (properties.contains(QByteArrayLiteral("DEVTYPE=usb_device")))
                return path;
            // A malformed USB ancestor must not borrow IDs from the upstream hub.
            if (!properties.contains(QByteArrayLiteral("DEVTYPE=usb_interface")))
                return {};
        }
        path = QFileInfo(path).path();
    }
    return {};
}

bool checkIdentity(const QString& path, const QByteArray& numbers, quint64 size, quint64& sequence)
{
    if (QFileInfo::exists(path + QStringLiteral("/partition")))
        return false;
    if (readAttribute(path + QStringLiteral("/dev")) != numbers)
        return false;
    quint64 sectors = 0;
    if (!readDecimal(path + QStringLiteral("/size"), sectors))
        return false;
    const bool safeSize = sectors > 0 && sectors <= std::numeric_limits<quint64>::max() / sysfsSectorBytes;
    if (!safeSize || sectors * sysfsSectorBytes != size)
        return false;
    return readDecimal(path + QStringLiteral("/diskseq"), sequence) && sequence != 0;
}
}

namespace rufus
{
NativeDiskFilter EvaluateNativeDiskFilter(const NativeDiskFacts& facts,
    const QString& media, const QStringList& compatibility, HDD_SCORE& score)
{
    score = {};
    if (facts.size == 0)
        return NativeDiskFilter::Unknown;
    const bool virtualDisk = facts.deviceClass == DeviceClass::Virtual || facts.deviceClass == DeviceClass::Vmware;
    if (virtualDisk || facts.deviceClass == DeviceClass::Optical)
        return NativeDiskFilter::Include;
    bool card = facts.deviceClass == DeviceClass::Card || cardMedia(media);
    if (media.isEmpty())
        for (const QString& kind : compatibility)
            card = card || cardMedia(kind);
    if (card)
        return facts.size > MAX_DEFAULT_LIST_CARD_SIZE ? NativeDiskFilter::LargeCard : NativeDiskFilter::Include;
    if (facts.mediaRemoval == MediaRemoval::Unknown)
        return NativeDiskFilter::Unknown;

    // These strings were bounded/trimmed by the snapshot reader. Preserve
    // their native order and content; Windows transport suffixes are not facts
    // we have on Linux. In particular, never append "SCSI Disk Device" or strip
    // a real SCSI term just because this disk happens to use UAS.
    QByteArray model = facts.vendor;
    if (!model.isEmpty() && !facts.model.isEmpty())
        model += ' ';
    model += facts.model;
    constexpr quint16 unknownUsbId = UINT16_MAX; // Windows's failed lookup (-1).
    const quint16 vendor = facts.usbIdsKnown ? facts.usbVendor : unknownUsbId;
    const quint16 product = facts.usbIdsKnown ? facts.usbProduct : unknownUsbId;
    score = GetHddScore(facts.mediaRemoval == MediaRemoval::Fixed, facts.size,
        vendor, product, model.isEmpty() ? nullptr : model.constData());
    return score.total > 0 ? NativeDiskFilter::LikelyHdd : NativeDiskFilter::Include;
}

int ReadNativeDiskFacts(quint64 device, quint64 expectedSize, NativeDiskFacts& facts, const QString& sysfsRoot)
{
    facts = {};
    const bool validDevice = device != 0 && quint64(dev_t(device)) == device;
    if (!validDevice || expectedSize == 0)
        return EINVAL;
    const QString root = QFileInfo(sysfsRoot).canonicalFilePath();
    if (root.isEmpty())
        return ENODEV;
    const QString devices = QFileInfo(root + QStringLiteral("/devices")).canonicalFilePath();
    const QByteArray numbers = QByteArray::number(major(dev_t(device))) + ':' + QByteArray::number(minor(dev_t(device)));
    const QString link = root + QStringLiteral("/dev/block/") + QString::fromLatin1(numbers);
    const QString path = QFileInfo(link).canonicalFilePath();
    const bool insideDevices = !devices.isEmpty() && path.startsWith(devices + u'/');
    if (!insideDevices || !hasSubsystem(path, QStringLiteral("/class/block"), root))
        return ENODEV;

    NativeDiskFacts snapshot;
    if (!checkIdentity(path, numbers, expectedSize, snapshot.diskSequence))
        return ESTALE;
    snapshot.logicalSectorSize = readSectorSize(path, QStringLiteral("logical_block_size"));
    snapshot.physicalSectorSize = readSectorSize(path, QStringLiteral("physical_block_size"));
    const QString usb = usbParent(path, devices, root);
    snapshot.usbTransport = !usb.isEmpty();
    const QByteArray removable = readAttribute(path + QStringLiteral("/removable"));
    if (removable == "0")
        snapshot.mediaRemoval = MediaRemoval::Fixed;
    else if (removable == "1")
        snapshot.mediaRemoval = MediaRemoval::Removable;

    quint16 vendor = 0, product = 0;
    const bool vendorKnown = !usb.isEmpty() && readUsbId(usb + QStringLiteral("/idVendor"), vendor);
    const bool productKnown = !usb.isEmpty() && readUsbId(usb + QStringLiteral("/idProduct"), product);
    if (vendorKnown && productKnown)
    {
        snapshot.usbIdsKnown = true;
        snapshot.usbVendor = vendor;
        snapshot.usbProduct = product;
    }
    // Preserve separate native strings; score input and UI names have different
    // purposes. EvaluateNativeDiskFilter joins only these observed components.
    snapshot.vendor = readAttribute(path + QStringLiteral("/device/vendor"));
    snapshot.model = readAttribute(path + QStringLiteral("/device/model"));
    const bool optical = readAttribute(path + QStringLiteral("/device/type")) == "5";
    if (optical) snapshot.deviceClass = DeviceClass::Optical;
    else if (!snapshot.usbTransport)
    {
        // The Windows IsVHD table uses padded SCSI hardware IDs. Linux exposes
        // vendor/model separately; compare those native components, not UI names.
        const auto& vendor = snapshot.vendor;
        const auto& model = snapshot.model;
        const bool virtualDisk = (vendor == "Msft" && model == "Virtual Disk")
            || (vendor == "Arsenal" && model.startsWith("Virtual"))
            || (vendor == "KernSafe" && model.startsWith("Virtual"))
            || (vendor == "BHYVE" && model == "SATA DISK");
        const bool vmware = vendor == "VMware" && model.startsWith("VMware Virtual S");
        const QString nativeDevice = QFileInfo(path + QStringLiteral("/device")).canonicalFilePath();
        const bool card = !nativeDevice.isEmpty() && hasSubsystem(nativeDevice, QStringLiteral("/bus/mmc"), root);
        const bool fileDisk = path.startsWith(devices + QStringLiteral("/virtual/block/"))
            && (QFileInfo(path).fileName().startsWith(QStringLiteral("loop")) || QFileInfo(path).fileName().startsWith(QStringLiteral("nbd")));
        if (fileDisk) {
            QStringList paths;
            const int error = NativeFileDiskSources(path, paths, snapshot.backingIdentity, true);
            if (error != 0) return error;
            snapshot.deviceClass = DeviceClass::Virtual;
        }
        else if (virtualDisk) snapshot.deviceClass = DeviceClass::Virtual;
        else if (vmware) snapshot.deviceClass = DeviceClass::Vmware;
        else if (card) snapshot.deviceClass = DeviceClass::Card;
        else if (snapshot.mediaRemoval == MediaRemoval::Removable) snapshot.deviceClass = DeviceClass::NonUsbRemovable;
        else return ENOTSUP;
    }
    quint64 finalSequence = 0;
    const bool identityMatches = checkIdentity(path, numbers, expectedSize, finalSequence);
    const bool pathMatches = QFileInfo(link).canonicalFilePath() == path;
    const bool sameSector = readSectorSize(path, QStringLiteral("logical_block_size")) == snapshot.logicalSectorSize;
    const bool samePhysicalSector = readSectorSize(path, QStringLiteral("physical_block_size")) == snapshot.physicalSectorSize;
    if (!identityMatches || !pathMatches || !sameSector || !samePhysicalSector || finalSequence != snapshot.diskSequence)
        return ESTALE;
    if (!snapshot.backingIdentity.isEmpty()) {
        QStringList paths, identity;
        if (NativeFileDiskSources(path, paths, identity, true) != 0 || identity != snapshot.backingIdentity) return ESTALE;
    }
    snapshot.sysfsPath = path;
    snapshot.size = expectedSize;
    facts = snapshot;
    return 0;
}
}
