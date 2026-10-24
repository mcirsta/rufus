// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "qt/prepare_image_protocol.h"
#include <QJsonObject>
namespace rufus {
struct SaveSourceRequest {
    PrepareImageRequest source;
    bool iso = false;
    QString socket;
    QByteArray token;
};
QJsonObject EncodeSaveSourceRequest(const SaveSourceRequest& request);
bool DecodeSaveSourceRequest(const QJsonObject& object, SaveSourceRequest& request);
int SendSaveSourceDescriptor(int socket, int descriptor, const QJsonObject& metadata);
int ReceiveSaveSourceDescriptor(int socket, int& descriptor, QJsonObject& metadata);
// Only callable by the installed --save-device helper after authentication.
// It accepts no destination pathname or write descriptor.
int RunSaveSourceHelper(int argc, char** argv);
}
