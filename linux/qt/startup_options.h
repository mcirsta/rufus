// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "format_policy.h"
#include <QStringList>

namespace rufus
{
enum class StartupAction { Launch, Help, Version };
struct StartupOptions
{
    StartupAction action = StartupAction::Launch;
    QString image, locale;
    bool extraDevices = false, portable = false;
    int fileSystem = FS_UNKNOWN;
    int waitTicks = 0; // Original mutex loop waits 100 milliseconds per tick.
};
bool ParseStartupOptions(const QStringList& arguments, StartupOptions& options, QString& problem);
bool StartupRequestsText(const QStringList& arguments);
QString StartupHelp();
}
