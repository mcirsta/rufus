// SPDX-License-Identifier: GPL-3.0-or-later
#include "main_window.h"
#include "selection_controller.h"
#include "application_instance.h"
#include "application_storage.h"
#include "device_policy.h"
#include "startup_options.h"
#include "application_language.h"
#include "platform/udisks_client.h"
#include "rufus_build_info.h"

#include <QApplication>
#include <QMessageBox>
#include <QTextStream>

static int startupText(const rufus::StartupOptions& options)
{
    if (options.action == rufus::StartupAction::Help) QTextStream(stdout) << rufus::StartupHelp();
    else QTextStream(stdout) << QCoreApplication::applicationName() << ' ' << QCoreApplication::applicationVersion() << '\n';
    return 0;
}

int main(int argc, char* argv[])
{
    QCoreApplication::setApplicationName(QStringLiteral("Rufus Linux preview"));
    QCoreApplication::setApplicationVersion(QStringLiteral(RUFUS_LINUX_VERSION));
    QStringList arguments;
    for (int index = 0; index < argc; ++index) arguments.append(QString::fromLocal8Bit(argv[index]));
    rufus::StartupOptions options;
    QString problem;
    // Preserve terminal-only help/version and let QApplication consume its
    // own platform/style switches before parsing ordinary GUI startup.
    if (rufus::StartupRequestsText(arguments)) {
        QCoreApplication application(argc, argv);
        if (rufus::ParseStartupOptions(application.arguments(), options, problem)) return startupText(options);
        QTextStream(stderr) << problem << '\n'; return 1;
    }
    QApplication application(argc, argv);
    if (!rufus::ParseStartupOptions(application.arguments(), options, problem)) {
        QTextStream(stderr) << problem << '\n'; return 1;
    }
    if (options.action != rufus::StartupAction::Launch) return startupText(options);
    rufus::ApplicationInstance instance;
    if (!instance.acquire(options.waitTicks, problem)) {
        QTextStream(stderr) << problem << '\n';
        QMessageBox::critical(nullptr, QStringLiteral("Rufus"), problem); return 1;
    }
    if (rufus::ConfigureApplicationStorage(application.applicationFilePath(), options.portable, problem) != 0) {
        QTextStream(stderr) << problem << '\n';
        QMessageBox::critical(nullptr, QStringLiteral("Rufus"), problem); return 1;
    }
    if (!rufus::InitializeApplicationLanguage(application.applicationFilePath(), options.locale, problem)) {
        QTextStream(stderr) << problem << '\n';
        QMessageBox::critical(nullptr, QStringLiteral("Rufus"), problem); return 1;
    }
    QGuiApplication::setDesktopFileName(QStringLiteral("org.rufus.Rufus"));
    rufus::SelectionController controller;
    controller.setStartupFileSystem(options.fileSystem);
    controller.writer().setConnection(QDBusConnection::systemBus());
    controller.configureDeviceOperations(QStringLiteral(RUFUS_PREPARE_HELPER_PATH));
    controller.writer().setHelper(QStringLiteral(RUFUS_PREPARE_HELPER_PATH));
    controller.isoWriter().configure(QDBusConnection::systemBus(), QStringLiteral(RUFUS_PREPARE_HELPER_PATH));
    rufus::UDisksClient devices(QDBusConnection::systemBus());
    QObject::connect(&devices, &rufus::UDisksClient::devicesChanged,
        &controller, &rufus::SelectionController::setDiscovery);
    QObject::connect(&controller, &rufus::SelectionController::fixedUsbChanged,
        &devices, &rufus::UDisksClient::setAllowFixedUsb);
    QObject::connect(&controller, &rufus::SelectionController::deviceClassesChanged,
        &devices, &rufus::UDisksClient::setDeviceClasses);
    QObject::connect(&controller,&rufus::SelectionController::usbDebugChanged,&devices,&rufus::UDisksClient::setUsbDebug);
    QObject::connect(&devices,&rufus::UDisksClient::logMessage,&controller,&rufus::SelectionController::logMessage);
    devices.setUsbDebug(controller.usbDebug());
    devices.setDeviceClasses(controller.deviceClasses());
    controller.setAllowFixedUsb(options.extraDevices);
    rufus::MainWindow window(controller);
    window.show();
    window.initializePolicyUpdates();
    if (!options.image.isEmpty()) controller.selectImage(options.image);
    devices.setExclusionsReader(rufus::ConfiguredDeviceExclusions);
    QObject::connect(&controller, &rufus::SelectionController::devicesRefreshRequested, &devices, &rufus::UDisksClient::rescan);
    devices.start();
    return application.exec();
}
