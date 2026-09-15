// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/udisks_client.h"

#include <QCoreApplication>
#include <QTextStream>

class DiscoveryProbe : public QObject
{
    Q_OBJECT

public slots:
    void completed(const QList<rufus::Device>& devices, const QString& problem)
    {
        if (!problem.isEmpty())
        {
            QTextStream(stderr) << problem << '\n';
            QCoreApplication::exit(1);
            return;
        }
        QTextStream(stdout) << "UDisks2 discovery succeeded: " << devices.size()
            << " eligible USB device(s). No device nodes opened.\n";
        QCoreApplication::exit(0);
    }

    void timedOut()
    {
        QTextStream(stderr) << "UDisks2 discovery timed out.\n";
        QCoreApplication::exit(2);
    }
};

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    if (app.arguments().size() != 2 || app.arguments().at(1) != QStringLiteral("--system"))
    {
        QTextStream(stderr) << "Usage: udisks_probe --system (read-only host discovery)\n";
        return 2;
    }
    rufus::UDisksClient client(QDBusConnection::systemBus());
    DiscoveryProbe probe;
    QObject::connect(&client, &rufus::UDisksClient::devicesChanged,
        &probe, &DiscoveryProbe::completed, Qt::QueuedConnection);
    QTimer::singleShot(7000, &probe, &DiscoveryProbe::timedOut);
    QTimer::singleShot(0, &client, &rufus::UDisksClient::start);
    return app.exec();
}

#include "udisks_probe.moc"
