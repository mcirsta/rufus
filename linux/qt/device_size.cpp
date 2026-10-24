// SPDX-License-Identifier: GPL-3.0-or-later
#include "device_size.h"
#include "platform/device.h"
#include <QCoreApplication>
#include <QtMath>
#include <iterator>

namespace rufus
{
QString DeviceCaption(const Device& device, bool properUnits)
{
    QString title = device.volumeLabel.isEmpty() ? device.name : device.volumeLabel;
    if (device.mountedVolumes > 1)
        title = QCoreApplication::translate("rufus::MainWindow","Multiple Volumes");
    return QStringLiteral("%1 — %2 (%3)").arg(title,device.path,DeviceSizeLabel(device.size,properUnits));
}
QString DeviceSizeLabel(quint64 size, bool properUnits)
{
    // SizeToHumanReadable(..., FALSE, TRUE): decimal units and Rufus's
    // near-power-of-two rounding for a manufacturer's nominal capacity.
    const char* units[] = {QT_TRANSLATE_NOOP("rufus::SelectionController", "bytes"),
        QT_TRANSLATE_NOOP("rufus::SelectionController", "KB"),
        QT_TRANSLATE_NOOP("rufus::SelectionController", "MB"),
        QT_TRANSLATE_NOOP("rufus::SelectionController", "GB"),
        QT_TRANSLATE_NOOP("rufus::SelectionController", "TB"),
        QT_TRANSLATE_NOOP("rufus::SelectionController", "PB")};
    const double divider = properUnits ? 1024.0 : 1000.0;
    constexpr double integerThreshold = 8.0;
    constexpr float roundingTolerance = 0.05f;
    double amount = double(size);
    int suffix = 0;
    while (amount >= divider && suffix < int(std::size(units)) - 1)
    {
        amount /= divider;
        ++suffix;
    }
    const QString unit = QCoreApplication::translate("rufus::SelectionController", units[suffix]);
    if (suffix == 0)
        return QStringLiteral("%1 %2").arg(size).arg(unit);
    if (properUnits) {
        const bool nearInteger = amount * 10.0 - qFloor(amount) * 10.0 < 0.5;
        return QStringLiteral("%1 %2").arg(QString::number(amount, 'f', nearInteger ? 0 : 1), unit);
    }
    if (amount < integerThreshold)
    {
        const bool nearInteger = qAbs(amount * 10.0 - qFloor(amount + 0.5) * 10.0) < 0.5;
        return QString::number(amount, 'f', nearInteger ? 0 : 1) + unit;
    }
    const quint32 powerOfTwo = qNextPowerOfTwo(quint32(amount) - 1);
    const bool nearPowerOfTwo = qAbs(1.0 - amount / powerOfTwo) < roundingTolerance;
    const quint32 nominalSize = nearPowerOfTwo ? powerOfTwo : quint32(amount);
    return QStringLiteral("%1 %2").arg(nominalSize).arg(unit);
}
}
