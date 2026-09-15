// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QList>
#include <QMetaType>
#include <QString>
#include <QStringList>

namespace rufus
{

struct Device
{
    // Selection identity, not a persistent ID or permission to write.
    QString id;
    QString path;
    QString name;
    quint64 size = 0;
    bool removable = false;
    bool readOnly = false;
    // Native discovery facts, required when preparing a destructive operation.
    QString objectPath;
    QString drivePath;
    QString serviceOwner;
    quint64 number = 0;
    QStringList identity;
};

} // namespace rufus

Q_DECLARE_METATYPE(rufus::Device)
