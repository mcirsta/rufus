// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "compressed_ffu.h"
#include <QByteArray>
#include <QString>
#include <QVector>

namespace rufus
{
class PrepareDeviceTarget;
int UpdateFfuRecoveryXml(const QByteArray& input, const uint8_t diskId[16], quint64 recoveryOffset,
    QByteArray& output);
// The descriptor is a checked restored partition, or a disposable regular file
// when device is zero. Never move the start or shrink the captured filesystem.
int GrowFfuNtfs(int fd, quint64 device, quint64 capturedBytes, quint64 partitionBytes,
    unsigned sector, const QString& program);
int CheckFfuFinalization(PrepareDeviceTarget& target, const FFU_DEPLOYMENT_INFO& info,
    const QVector<FFU_DEPLOYMENT_PARTITION>& partitions);
int FinishFfuDeployment(PrepareDeviceTarget& target, const FFU_DEPLOYMENT_INFO& info,
    const QVector<FFU_DEPLOYMENT_PARTITION>& partitions, unsigned sector);
}
