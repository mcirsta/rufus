// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/save_source.h"
#include "qt/save_source_process.h"
#include <QFile>
#include <QTemporaryDir>
#include <QCoreApplication>
#include <QJsonDocument>
#include <cassert>
#include <cstdio>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    rufus::SaveSourceRequest request;
    request.source.path = QStringLiteral("/dev/test-usb"); request.source.device = 2048; request.source.size = 67108864;
    request.source.diskSequence = 1; request.source.imageDevice = 123;
    request.source.objectPath = QStringLiteral("/org/freedesktop/UDisks2/block_devices/test_usb");
    request.source.serviceOwner = QStringLiteral(":1.3"); request.source.identity = {QStringLiteral("serial"), QStringLiteral("USB")};
    request.socket = QStringLiteral("/tmp/rufus-save-test/socket"); request.token = QByteArray(64, 'a');
    const QJsonObject original = rufus::EncodeSaveSourceRequest(request);
    rufus::SaveSourceRequest checked;
    assert(rufus::DecodeSaveSourceRequest(original, checked) && checked.source.identity == request.source.identity);
    const char* required[] = {"protocol", "device", "size", "sequence", "path", "object", "owner", "identity", "outputDevice", "allowFixedUsb", "iso", "socket", "token"};
    unsigned cases = 1;
    for (const char* name : required) {
        QJsonObject object = original; object.remove(QLatin1String(name));
        assert(!rufus::DecodeSaveSourceRequest(object, checked)); cases++;
    }
    for (const char* value : {"-1", "0", "18446744073709551616", "0001", "1.5"}) {
        QJsonObject object = original; object[QStringLiteral("device")] = QLatin1String(value);
        assert(!rufus::DecodeSaveSourceRequest(object, checked)); cases++;
    }
    int sockets[2]; assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) == 0);
    const int file = open("/dev/null", O_RDONLY | O_CLOEXEC); assert(file >= 0);
    assert(rufus::SendSaveSourceDescriptor(sockets[0], file, QJsonObject{{"error", 0}}) == 0);
    int received = -1; QJsonObject object;
    assert(rufus::ReceiveSaveSourceDescriptor(sockets[1], received, object) == 0 && received >= 0 && (fcntl(received, F_GETFD) & FD_CLOEXEC));
    assert(close(received) == 0); cases++;
    assert(rufus::SendSaveSourceDescriptor(sockets[0], -1, QJsonObject{{"error", 13}}) == 0);
    assert(rufus::ReceiveSaveSourceDescriptor(sockets[1], received, object) == 0 && received == -1 && object.value(QStringLiteral("error")).toInt() == 13); cases++;
    assert(send(sockets[0], "invalid", 7, 0) == 7);
    assert(rufus::ReceiveSaveSourceDescriptor(sockets[1], received, object) == EPROTO && received == -1); cases++;
    assert(close(file) == 0 && close(sockets[0]) == 0 && close(sockets[1]) == 0);
    const QByteArray oldPath = qgetenv("PATH");
    QTemporaryDir tools;
    assert(tools.isValid());
    qputenv("PATH", tools.path().toUtf8() + ':' + oldPath);
    for (int exitCode : {126, 127}) {
        QFile launcher(tools.filePath(QStringLiteral("pkexec")));
        assert(launcher.open(QIODevice::WriteOnly | QIODevice::Truncate));
        assert(launcher.write(QByteArray("#!/bin/sh\nexit ") + QByteArray::number(exitCode) + '\n') > 0);
        assert(launcher.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        launcher.close();
        rufus::SaveSourceProcess process;
        int descriptor = -1; QJsonObject metadata;
        const int error = process.acquire(request.source, false, QStringLiteral("/usr/bin/true"), descriptor, metadata);
        assert(error == (exitCode == 126 ? ECANCELED : EACCES) && descriptor == -1);
        cases++;
    }
    qputenv("PATH", oldPath);
    printf("Save source protocol: %u request/descriptor boundary cases\n", cases);
}
