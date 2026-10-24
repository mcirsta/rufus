// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "platform/save_source.h"
#include <QProcess>
#include <QTemporaryDir>
namespace rufus {
class SaveSourceProcess {
public:
    SaveSourceProcess();
    ~SaveSourceProcess();
    int acquire(const PrepareImageRequest& source, bool iso, const QString& helper, int& descriptor, QJsonObject& metadata);
    int finish();
private:
    void drain();
    QProcess process_;
    QTemporaryDir directory_;
    int server_ = -1, peer_ = -1;
    QByteArray result_;
    bool started_ = false, finished_ = false, stopSent_ = false;
};
}
