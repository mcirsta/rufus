// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QJsonObject>
#include <QMetaType>
#include <QString>

namespace rufus
{
constexpr qsizetype PrepareMessageBytes = 4096;

// A file uses filesystem device/inode; a native request uses block dev_t and
// the accepted UDisks identity plus the kernel disk sequence number.
struct PrepareImageRequest
{
    QString path;
    quint64 device = 0;
    quint64 inode = 0;
    quint64 size = 0;
    quint32 clusterSize = 512;
    quint32 volumeId = 0;
    QString label;
    bool isDevice = false;
    quint64 diskSequence = 0;
    QString objectPath;
    QString serviceOwner;
    QStringList identity;
};

struct PrepareImageOutcome
{
    int error = 0;
    int syncError = 0;
    bool started = false;
    quint64 offset = 0;
    quint64 size = 0;
    quint64 filesystemBytes = 0;
};

// Integers travel as decimal strings, preserving all 64 bits on Qt 6.2 too.
QJsonObject EncodePrepareRequest(const PrepareImageRequest& request);
bool DecodePrepareRequest(const QJsonObject& object, PrepareImageRequest& request);
QJsonObject EncodePrepareOutcome(const PrepareImageOutcome& outcome);
bool DecodePrepareOutcome(const QJsonObject& object, PrepareImageOutcome& outcome);
}

Q_DECLARE_METATYPE(rufus::PrepareImageOutcome)
