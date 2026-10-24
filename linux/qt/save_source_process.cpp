// SPDX-License-Identifier: GPL-3.0-or-later
#include "save_source_process.h"
#include "platform/rufus_reporting.h"
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QUuid>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace rufus {
SaveSourceProcess::SaveSourceProcess() : directory_(QDir::tempPath() + QStringLiteral("/rufus-save-socket-XXXXXX")) {}
SaveSourceProcess::~SaveSourceProcess() { finish(); if (peer_ >= 0) close(peer_); if (server_ >= 0) close(server_); }
void SaveSourceProcess::drain()
{
    const QByteArray log = process_.readAllStandardError();
    if (!log.isEmpty()) uprintfs(log.left(4096).constData());
    const QByteArray output = process_.readAllStandardOutput();
    result_.append(output.left(qMax(0, 8193 - int(result_.size()))));
    if (result_.size() > 8192 && !stopSent_) { process_.closeWriteChannel(); stopSent_ = true; }
}
int SaveSourceProcess::acquire(const PrepareImageRequest& source, bool iso, const QString& helper, int& descriptor, QJsonObject& metadata)
{
    descriptor = -1;
    if (started_ || !directory_.isValid() || !QDir::isAbsolutePath(helper) || !QFileInfo(helper).isExecutable()) return ENOENT;
    server_ = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (server_ < 0) return errno;
    const QString socketPath = directory_.filePath(QStringLiteral("source"));
    const QByteArray path = QFile::encodeName(socketPath);
    sockaddr_un address = {}; address.sun_family = AF_UNIX;
    if (path.size() >= int(sizeof(address.sun_path))) return ENAMETOOLONG;
    memcpy(address.sun_path, path.constData(), size_t(path.size() + 1));
    if (bind(server_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || listen(server_, 1) != 0) return errno;
    SaveSourceRequest request{source, iso, socketPath, QUuid::createUuid().toRfc4122().toHex() + QUuid::createUuid().toRfc4122().toHex()};
    const QByteArray packet = QJsonDocument(EncodeSaveSourceRequest(request)).toJson(QJsonDocument::Compact) + '\n';
    SaveSourceRequest checked;
    if (packet.size() > 8192 || !DecodeSaveSourceRequest(QJsonDocument::fromJson(packet).object(), checked)) return EINVAL;
    process_.start(QStringLiteral("pkexec"), {QStringLiteral("--disable-internal-agent"), helper, QStringLiteral("--save-device")});
    if (!process_.waitForStarted(5000)) return EIO;
    started_ = true;
    if (process_.write(packet) != packet.size()) return EIO;
    QElapsedTimer timer; timer.start();
    int error = 0;
    while (process_.state() != QProcess::NotRunning) {
        process_.waitForReadyRead(25); drain();
        if (IsRufusJobCancelled() || timer.elapsed() > 120000 || result_.size() > 8192) {
            error = IsRufusJobCancelled() ? ECANCELED : timer.elapsed() > 120000 ? ETIMEDOUT : EPROTO;
            break;
        }
        if (peer_ < 0) {
            peer_ = accept4(server_, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
            if (peer_ < 0 && errno != EAGAIN && errno != EINTR) { error = errno; break; }
            if (peer_ >= 0) {
                ucred credentials = {}; socklen_t size = sizeof(credentials);
                if (getsockopt(peer_, SOL_SOCKET, SO_PEERCRED, &credentials, &size) != 0) { error = errno; break; }
                if (size != sizeof(credentials) || credentials.uid != 0) { error = EPERM; break; }
            }
        }
        if (peer_ < 0) continue;
        error = ReceiveSaveSourceDescriptor(peer_, descriptor, metadata);
        if (error == EAGAIN || error == EINTR) { error = 0; continue; }
        if (error != 0) break;
        const bool bound = metadata.value(QStringLiteral("token")).toString().toLatin1() == request.token
            && metadata.value(QStringLiteral("iso")).isBool() && metadata.value(QStringLiteral("iso")).toBool() == iso;
        if (!bound || !metadata.value(QStringLiteral("error")).isDouble()) error = EPROTO;
        else error = metadata.value(QStringLiteral("error")).toInt(-1);
        if (error < 0 || error > 4095 || (error == 0 && descriptor < 0)) error = EPROTO;
        if (error != 0 && descriptor >= 0) { close(descriptor); descriptor = -1; }
        return error;
    }
    if (error == 0) error = EIO;
    const int finished = finish();
    return finished != 0 ? finished : error;
}
int SaveSourceProcess::finish()
{
    if (!started_ || finished_) return 0;
    finished_ = true;
    if (!stopSent_) {
        process_.write(IsRufusJobCancelled() ? "cancel\n" : "finish\n");
        process_.closeWriteChannel(); stopSent_ = true;
    }
    while (process_.state() != QProcess::NotRunning) { process_.waitForFinished(50); drain(); }
    drain();
    const QJsonObject result = QJsonDocument::fromJson(result_.trimmed()).object();
    const bool valid = result_.size() <= 8192 && result.value(QStringLiteral("event")).toString() == QLatin1String("result")
        && result.value(QStringLiteral("error")).isDouble() && process_.exitStatus() == QProcess::NormalExit;
    if (!valid) return process_.exitCode() == 126 ? ECANCELED : process_.exitCode() == 127 ? EACCES : EPROTO;
    const int error = result.value(QStringLiteral("error")).toInt(-1);
    if (error < 0 || error > 4095 || (process_.exitCode() == 0) != (error == 0)) return EPROTO;
    return error;
}
}
