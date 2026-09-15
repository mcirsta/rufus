// SPDX-License-Identifier: GPL-3.0-or-later
#include "prepare_image_protocol.h"

#include <QDir>
#include <QJsonArray>
#include <limits>

namespace rufus
{
namespace
{
bool unsignedValue(const QJsonValue& value, quint64& result)
{
    bool valid = false;
    const QString text = value.toString();
    result = text.toULongLong(&valid);
    return valid && QString::number(result) == text;
}
}

QJsonObject EncodePrepareRequest(const PrepareImageRequest& request)
{
    QJsonObject object = {{"operation", request.isDevice ? "prepare-device-v1" : "prepare-image-v1"}, {"path", request.path},
        {"device", QString::number(request.device)},
        {"size", QString::number(request.size)}, {"cluster", QString::number(request.clusterSize)},
        {"volume", QString::number(request.volumeId)}, {"label", request.label}};
    if (request.isDevice)
    {
        object.insert("sequence", QString::number(request.diskSequence));
        object.insert("object", request.objectPath);
        object.insert("owner", request.serviceOwner);
        object.insert("identity", QJsonArray::fromStringList(request.identity));
    }
    else
        object.insert("inode", QString::number(request.inode));
    return object;
}

bool DecodePrepareRequest(const QJsonObject& object, PrepareImageRequest& request)
{
    PrepareImageRequest decoded;
    quint64 cluster = 0, volume = 0;
    const bool image = object.size() == 8 && object.value("operation") == "prepare-image-v1";
    const bool device = object.size() == 11 && object.value("operation") == "prepare-device-v1";
    if (!image && !device)
        return false;
    decoded.path = object.value("path").toString();
    decoded.label = object.value("label").toString();
    const bool usablePath = QDir::isAbsolutePath(decoded.path) && !decoded.path.contains(QChar::Null);
    const bool textLabel = object.value("label").isString() && !decoded.label.contains(QChar::Null);
    if (!usablePath || !textLabel)
        return false;
    if (!unsignedValue(object.value("device"), decoded.device))
        return false;
    if (image && !unsignedValue(object.value("inode"), decoded.inode))
        return false;
    if (!unsignedValue(object.value("size"), decoded.size))
        return false;
    if (!unsignedValue(object.value("cluster"), cluster) || cluster > std::numeric_limits<quint32>::max())
        return false;
    if (!unsignedValue(object.value("volume"), volume) || volume > std::numeric_limits<quint32>::max())
        return false;
    decoded.clusterSize = quint32(cluster);
    decoded.volumeId = quint32(volume);
    decoded.isDevice = device;
    if (device)
    {
        if (!unsignedValue(object.value("sequence"), decoded.diskSequence) || decoded.diskSequence == 0)
            return false;
        decoded.objectPath = object.value("object").toString();
        decoded.serviceOwner = object.value("owner").toString();
        const bool nativeObject = decoded.objectPath.startsWith(QStringLiteral("/org/freedesktop/UDisks2/block_devices/"));
        const bool nativeOwner = decoded.serviceOwner.startsWith(QLatin1Char(':'));
        if (!nativeObject || !nativeOwner || decoded.device == 0)
            return false;
        if (!object.value("identity").isArray())
            return false;
        for (const QJsonValue& value : object.value("identity").toArray())
        {
            if (!value.isString())
                return false;
            decoded.identity.append(value.toString());
        }
        if (decoded.identity.isEmpty())
            return false;
    }
    request = decoded;
    return true;
}

QJsonObject EncodePrepareOutcome(const PrepareImageOutcome& outcome)
{
    return {{"event", "result"}, {"error", outcome.error}, {"syncError", outcome.syncError},
        {"started", outcome.started}, {"offset", QString::number(outcome.offset)},
        {"size", QString::number(outcome.size)}, {"filesystemBytes", QString::number(outcome.filesystemBytes)}};
}

bool DecodePrepareOutcome(const QJsonObject& object, PrepareImageOutcome& outcome)
{
    PrepareImageOutcome decoded;
    if (object.size() != 7 || object.value("event") != "result")
        return false;
    decoded.error = object.value("error").toInt(-1);
    decoded.syncError = object.value("syncError").toInt(-1);
    if (decoded.error < 0 || decoded.syncError < 0 || !object.value("started").isBool())
        return false;
    decoded.started = object.value("started").toBool();
    if (!unsignedValue(object.value("offset"), decoded.offset))
        return false;
    if (!unsignedValue(object.value("size"), decoded.size))
        return false;
    if (!unsignedValue(object.value("filesystemBytes"), decoded.filesystemBytes))
        return false;
    outcome = decoded;
    return true;
}
}
