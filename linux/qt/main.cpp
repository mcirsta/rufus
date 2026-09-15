// SPDX-License-Identifier: GPL-3.0-or-later
#include "main_window.h"
#include "selection_controller.h"
#include "platform/udisks_client.h"

#include <QApplication>

int main(int argc, char* argv[])
{
    QApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("Rufus Linux preview"));
    rufus::SelectionController controller;
    controller.writer().setConnection(QDBusConnection::systemBus());
    rufus::UDisksClient devices(QDBusConnection::systemBus());
    QObject::connect(&devices, &rufus::UDisksClient::devicesChanged,
        &controller, &rufus::SelectionController::setDiscovery);
    rufus::MainWindow window(controller);
    window.show();
    devices.start();
    return application.exec();
}
