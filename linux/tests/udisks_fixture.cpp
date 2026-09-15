// SPDX-License-Identifier: GPL-3.0-or-later
// A separate process exercises real D-Bus marshalling and delayed replies.
#include "platform/udisks_client.h"

#include <QCoreApplication>
#include <QDBusArgument>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusVirtualObject>
#include <QDBusUnixFileDescriptor>
#include <QFile>
#include <QTextStream>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace
{

const QString service = QStringLiteral("org.freedesktop.UDisks2");
const QString control = QStringLiteral("org.rufus.TestUDisks");
const QString root = QStringLiteral("/org/freedesktop/UDisks2");
const QString manager = QStringLiteral("org.freedesktop.DBus.ObjectManager");

class UDisksFixture : public QDBusVirtualObject
{
public:
    QString introspect(const QString&) const override
    {
        return QStringLiteral("<interface name=\"org.freedesktop.DBus.ObjectManager\">"
            "<method name=\"GetManagedObjects\"><arg type=\"a{oa{sa{sv}}}\" direction=\"out\"/>"
            "</method></interface>");
    }

    bool handleMessage(const QDBusMessage& message, const QDBusConnection& connection) override
    {
        if (message.interface() == manager && message.member() == QStringLiteral("GetManagedObjects"))
        {
            ++calls_;
            QDBusMessage reply = message.createReply(QVariant::fromValue(objects_));
            if (mode_ == QStringLiteral("error"))
                reply = message.createErrorReply(QStringLiteral("org.rufus.TestError"),
                    QStringLiteral("Simulated discovery failure"));
            else if (mode_ == QStringLiteral("malformed"))
                reply = message.createReply(QStringLiteral("wrong signature"));
            message.setDelayedReply(true);
            if (hold_)
                replies_.append(reply);
            else
                connection.send(reply);
            return true;
        }
        if (message.interface() == control)
            return handleControl(message, connection);
        if (message.interface() == QStringLiteral("org.freedesktop.UDisks2.Block")
            && message.member() == QStringLiteral("OpenDevice") && !openFile_.isEmpty())
            return handleOpen(message, connection);
        ++unexpected_;
        connection.send(message.createErrorReply(QDBusError::UnknownMethod,
            QStringLiteral("Only read-only discovery is supported by this fixture")));
        return true;
    }

private:
    bool handleOpen(const QDBusMessage& message, const QDBusConnection& connection)
    {
        ++opens_;
        const QList<QVariant> args = message.arguments();
        const QVariantMap options = args.size() == 2 ? qdbus_cast<QVariantMap>(args.at(1)) : QVariantMap();
        if (args.size() != 2 || args.at(0).toString() != QStringLiteral("rw")
            || options.value(QStringLiteral("flags")).toInt() != (O_EXCL | O_SYNC | O_CLOEXEC)
            || options.value(QStringLiteral("auth.no_user_interaction"), true).toBool()
            || !message.isInteractiveAuthorizationAllowed())
        {
            ++unexpected_;
            connection.send(message.createErrorReply(QDBusError::InvalidArgs, QStringLiteral("Wrong raw open request")));
            return true;
        }
        QDBusMessage reply;
        if (openMode_ == QStringLiteral("deny"))
            reply = message.createErrorReply(QStringLiteral("org.freedesktop.UDisks2.Error.NotAuthorized"), QStringLiteral("Denied"));
        else if (openMode_ == QStringLiteral("dismiss"))
            reply = message.createErrorReply(QStringLiteral("org.freedesktop.UDisks2.Error.NotAuthorizedDismissed"), QStringLiteral("Dismissed"));
        else if (openMode_ == QStringLiteral("unsupported"))
            reply = message.createErrorReply(QDBusError::UnknownMethod, QStringLiteral("OpenDevice unavailable"));
        else if (openMode_ == QStringLiteral("malformed"))
            reply = message.createReply(QStringLiteral("not a descriptor"));
        else
        {
            const int flags = openMode_ == QStringLiteral("readonly") ? O_RDONLY : O_RDWR;
            const int fd = ::open(QFile::encodeName(openFile_).constData(), flags | O_SYNC | O_CLOEXEC | O_NOFOLLOW);
            struct stat info;
            if (fd < 0 || fstat(fd, &info) != 0 || !S_ISREG(info.st_mode))
                qFatal("Fixture only accepts its disposable regular file");
            const QDBusUnixFileDescriptor descriptor(fd);
            ::close(fd);
            reply = message.createReply(QVariant::fromValue(descriptor));
        }
        message.setDelayedReply(true);
        if (holdOpen_)
            openReplies_.append(reply);
        else
            connection.send(reply);
        return true;
    }

    bool handleControl(const QDBusMessage& message, QDBusConnection connection)
    {
        const QString method = message.member();
        const QList<QVariant> args = message.arguments();
        if (method == QStringLiteral("SetObjects") && args.size() == 1)
            objects_ = qdbus_cast<rufus::UDisksObjects>(args.at(0));
        else if (method == QStringLiteral("Hold") && args.size() == 1)
            hold_ = args.at(0).toBool();
        else if (method == QStringLiteral("Mode") && args.size() == 1)
            mode_ = args.at(0).toString();
        else if (method == QStringLiteral("OpenFile") && args.size() == 1)
            openFile_ = args.at(0).toString();
        else if (method == QStringLiteral("OpenMode") && args.size() == 1)
            openMode_ = args.at(0).toString();
        else if (method == QStringLiteral("HoldOpen") && args.size() == 1)
            holdOpen_ = args.at(0).toBool();
        else if (method == QStringLiteral("ReplyOpen") && !openReplies_.isEmpty())
            connection.send(openReplies_.takeFirst());
        else if (method == QStringLiteral("ReplyOldest") && !replies_.isEmpty())
            connection.send(replies_.takeFirst());
        else if (method == QStringLiteral("ReleaseName"))
            connection.unregisterService(service);
        else if (method == QStringLiteral("AcquireName"))
        {
            if (!connection.registerService(service))
                return false;
        }
        else if (method == QStringLiteral("Quit"))
            QTimer::singleShot(0, QCoreApplication::instance(), &QCoreApplication::quit);
        else if (method == QStringLiteral("Added") && args.size() == 2)
        {
            QDBusMessage signal = QDBusMessage::createSignal(root, manager, QStringLiteral("InterfacesAdded"));
            signal.setArguments(args);
            connection.send(signal);
        }
        else if (method == QStringLiteral("Removed") && args.size() == 2)
        {
            QDBusMessage signal = QDBusMessage::createSignal(root, manager, QStringLiteral("InterfacesRemoved"));
            signal.setArguments(args);
            connection.send(signal);
        }
        else if (method == QStringLiteral("Changed") && args.size() == 4)
        {
            QDBusMessage signal = QDBusMessage::createSignal(args.at(0).toString(),
                QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("PropertiesChanged"));
            signal.setArguments(args.mid(1));
            connection.send(signal);
        }
        else if (method == QStringLiteral("Stats"))
        {
            connection.send(message.createReply(QVariantMap{{QStringLiteral("calls"), calls_},
                {QStringLiteral("held"), replies_.size()}, {QStringLiteral("unexpected"), unexpected_},
                {QStringLiteral("opens"), opens_}, {QStringLiteral("openHeld"), openReplies_.size()}}));
            return true;
        }
        else
            return false;
        connection.send(message.createReply());
        return true;
    }

    rufus::UDisksObjects objects_;
    QList<QDBusMessage> replies_;
    QString mode_;
    QString openFile_, openMode_;
    QList<QDBusMessage> openReplies_;
    bool holdOpen_ = false;
    int opens_ = 0;
    bool hold_ = false;
    int calls_ = 0;
    int unexpected_ = 0;
};

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    if (app.arguments().size() != 2 || !app.arguments().at(1).startsWith(QStringLiteral("unix:")))
        return 2;
    qDBusRegisterMetaType<rufus::UDisksInterfaces>();
    qDBusRegisterMetaType<rufus::UDisksObjects>();
    QDBusConnection connection = QDBusConnection::connectToBus(app.arguments().at(1),
        QStringLiteral("fixture"));
    UDisksFixture fixture;
    if (!connection.isConnected() || !connection.registerVirtualObject(root, &fixture, QDBusConnection::SubPath)
        || !connection.registerService(control) || !connection.registerService(service))
        return 3;
    QTextStream(stdout) << "ready\n" << Qt::flush;
    return app.exec();
}
