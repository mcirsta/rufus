// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QMap>
#include <QSet>
#include <QSslCertificate>
#include <QSslKey>
#include <QSslSocket>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <algorithm>
namespace rufus_test
{
inline QByteArray response(const QByteArray& body, const QByteArray& headers = {}, qint64 length = -1,
    int status = 200, bool includeLength = true)
{
    QByteArray wire = "HTTP/1.1 " + QByteArray::number(status) + " Fixture\r\nConnection: close\r\n";
    if (includeLength)
        wire += "Content-Length: " + QByteArray::number(length < 0 ? body.size() : length) + "\r\n";
    return wire + headers + "\r\n" + body;
}

// Real HTTP framing on isolated loopback sockets, without mocks of Qt replies.
class HttpFixture final : public QTcpServer
{
    Q_OBJECT
public:
    QMap<QByteArray, QByteArray> responses;
    QMap<QByteArray, QByteArray> prefixResponses;
    QSet<QByteArray> holdHeaders;
    QSet<QByteArray> holdBodies;
    QSet<QByteArray> fragmented;
    QList<QByteArray> requests;
    QSslCertificate certificate;
    QSslKey key;

    HttpFixture()
    {
        timer_.setInterval(5);
        connect(&timer_, &QTimer::timeout, this, &HttpFixture::sendFragments);
    }
    QUrl url(const QByteArray& path = QByteArray("/grub-2.12/core.img")) const
    {
        return QUrl(QStringLiteral("%1://127.0.0.1:%2%3")
            .arg(certificate.isNull() ? "http" : "https").arg(serverPort()).arg(QString::fromLatin1(path)));
    }
    void releaseBodies()
    {
        for (QTcpSocket* socket : held_.keys()) {
            socket->write(held_.take(socket)); socket->disconnectFromHost();
        }
    }
    QList<QByteArray> paths() const
    {
        QList<QByteArray> result;
        for (const QByteArray& request : requests)
            result.append(request.split(' ').value(1));
        return result;
    }

protected:
    void incomingConnection(qintptr descriptor) override
    {
        QTcpSocket* socket;
        if (certificate.isNull()) socket = new QTcpSocket(this);
        else
        {
            QSslSocket* secure = new QSslSocket(this);
            secure->setLocalCertificate(certificate);
            secure->setPrivateKey(key);
            secure->setPeerVerifyMode(QSslSocket::VerifyNone); // Server requests no client certificate.
            socket = secure;
        }
        socket->setSocketDescriptor(descriptor);
        socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
        connect(socket, &QTcpSocket::readyRead, this, &HttpFixture::requestReady);
        connect(socket, &QTcpSocket::disconnected, this, &HttpFixture::socketClosed);
        if (!certificate.isNull()) static_cast<QSslSocket*>(socket)->startServerEncryption();
    }

private:
    struct Pending { QByteArray wire; qsizetype offset = 0; };
    QMap<QTcpSocket*, QByteArray> input_;
    QMap<QTcpSocket*, Pending> pending_;
    QMap<QTcpSocket*,QByteArray> held_;
    QTimer timer_;

    void requestReady()
    {
        QTcpSocket* socket = qobject_cast<QTcpSocket*>(sender());
        QByteArray& input = input_[socket];
        input += socket->readAll();
        if (!input.contains("\r\n\r\n")) return;
        requests.append(input);
        const QByteArray path = input.split(' ').value(1);
        input_.remove(socket);
        disconnect(socket, &QTcpSocket::readyRead, this, &HttpFixture::requestReady);
        if (holdHeaders.contains(path)) return;
        QByteArray wire=responses.value(path);
        if (wire.isEmpty())
            for (auto entry=prefixResponses.cbegin();entry!=prefixResponses.cend();++entry)
                if (path.startsWith(entry.key())) { wire=entry.value(); break; }
        if (wire.isEmpty()) wire=response({}, {}, 0, 404);
        if (holdBodies.contains(path)) {
            const qsizetype header=wire.indexOf("\r\n\r\n")+4;
            held_.insert(socket,wire.mid(header)); socket->write(wire.left(header));
        }
        else if (fragmented.contains(path))
        {
            pending_.insert(socket, {wire, 0});
            timer_.start();
        }
        else
        {
            socket->write(wire);
            socket->disconnectFromHost();
        }
    }

    void sendFragments()
    {
        const qsizetype fragmentBytes = 1024;
        for (QTcpSocket* socket : pending_.keys())
        {
            Pending part = pending_.take(socket);
            if (socket->state() != QAbstractSocket::ConnectedState) continue;
            const qsizetype size = std::min(fragmentBytes, part.wire.size() - part.offset);
            socket->write(part.wire.constData() + part.offset, size);
            part.offset += size;
            if (part.offset == part.wire.size()) socket->disconnectFromHost();
            else if (socket->state() == QAbstractSocket::ConnectedState) pending_.insert(socket, part);
        }
        if (pending_.isEmpty()) timer_.stop();
    }

    void socketClosed()
    {
        QTcpSocket* socket = qobject_cast<QTcpSocket*>(sender());
        input_.remove(socket);
        pending_.remove(socket);
        held_.remove(socket);
        socket->deleteLater();
    }
};
}
