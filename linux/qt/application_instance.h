// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QLockFile>
#include <QString>

namespace rufus
{
class ApplicationInstance
{
public:
    explicit ApplicationInstance(const QString& runtimeDirectory = {});
    bool acquire(int waitTicks, QString& problem);
private:
    QString directory_;
    QLockFile lock_;
};
}
