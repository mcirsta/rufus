// SPDX-License-Identifier: GPL-3.0-or-later
#include "qt/application_updates.h"
#include "rufus_build_info.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
class Reporter : public QObject
{
    Q_OBJECT
public slots:
    void finished(rufus::ApplicationUpdateResult result)
    {
        const QJsonObject report{{"error", result.error}, {"available", result.available}, {"release_found", result.releaseFound},
            {"version", result.release.version}, {"page", result.release.page.toString()}, {"server_time", result.serverTime},
            {"binary_downloaded", false}, {"repository", "mcirsta/rufus"}};
        QTextStream(stdout) << QJsonDocument(report).toJson(QJsonDocument::Indented);
        QCoreApplication::exit(result.error == 0 ? 0 : 1);
    }
};
int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    Reporter reporter; rufus::ApplicationUpdates updates;
    QObject::connect(&updates, &rufus::ApplicationUpdates::completed, &reporter, &Reporter::finished);
    const int error = updates.start(QLatin1String(RUFUS_LINUX_VERSION), QLatin1String(RUFUS_NATIVE_ARCH), false);
    if (error != 0) return 1;
    return application.exec();
}
#include "application_update_probe.moc"
