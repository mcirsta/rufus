// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/device_operations.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include <QFile>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
namespace
{
QString selected;
bool restoring = false;
int saveRecord(void* context, const QString& guid, bool restore)
{
    selected = guid; restoring = restore;
    return *static_cast<bool*>(context) ? 0 : EIO;
}
}
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 3) return 2;
    const int fd = open(argv[1], O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return 3;
    QStringList records;
    for (int i = 3; i < argc; ++i) records.append(QString::fromLocal8Bit(argv[i]));
    bool accepted = QByteArray(argv[2]) == "accept", started = false;
    const int error = rufus::ToggleDeviceEsp(fd, records, saveRecord, &accepted, started);
    const int sync = fsync(fd) == 0 ? 0 : errno;
    const int closed = close(fd) == 0 ? 0 : errno;
    QTextStream(stdout) << QJsonDocument(QJsonObject{{"error", error}, {"started", started}, {"guid", selected},
        {"restore", restoring}, {"sync", sync}, {"close", closed}}).toJson(QJsonDocument::Compact) << '\n';
    return 0;
}
