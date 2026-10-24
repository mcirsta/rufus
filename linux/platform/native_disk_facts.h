// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "device_class.h"
#include <QByteArray>
#include <QString>
#include <QStringList>
#include "hdd_score.h"

namespace rufus
{
enum class MediaRemoval { Unknown, Fixed, Removable };

struct NativeDiskFacts
{
    QString sysfsPath;
    quint64 diskSequence = 0;
    quint64 size = 0;
    unsigned logicalSectorSize = 0; // Zero if absent/invalid; never assume 512 for compressed DD.
    unsigned physicalSectorSize = 0; // Native queue geometry; zero if absent/invalid.
    MediaRemoval mediaRemoval = MediaRemoval::Unknown;
    bool usbTransport = true;
    DeviceClass deviceClass = DeviceClass::Usb;
    bool usbIdsKnown = false;
    quint16 usbVendor = 0;
    quint16 usbProduct = 0;
    QByteArray vendor;
    QByteArray model;
    QStringList backingIdentity;
};

enum class NativeDiskFilter { Unknown, Include, LikelyHdd, LargeCard };

// Evaluate only Rufus's default HDD/card filter on a verified native snapshot.
// Include is not whole-device eligibility or permission to write. Unknown media
// removal stays unknown unless positive card metadata bypasses HDD scoring.
// Media and compatibility are UDisks Drive properties, not the UI device name.
NativeDiskFilter EvaluateNativeDiskFilter(const NativeDiskFacts& facts,
    const QString& media, const QStringList& compatibility, HDD_SCORE& score);

// Read-only supported whole-disk snapshot (USB, card, virtual or removable). Zero means identity/size/sequence verified;
// optional classification facts may still be unknown. No eligibility or write
// permission is implied. Caller must retain/revalidate identity before use.
// The alternate sysfs root is for filesystem fixtures, never a GUI/job option.
int ReadNativeDiskFacts(quint64 device, quint64 expectedSize, NativeDiskFacts& facts,
    const QString& sysfsRoot = QStringLiteral("/sys"));
}
