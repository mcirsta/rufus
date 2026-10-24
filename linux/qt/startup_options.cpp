// SPDX-License-Identifier: GPL-3.0-or-later
#include "startup_options.h"
#include <QCommandLineParser>
#include <cerrno>
#include <climits>
#include <cstdlib>

namespace rufus
{
namespace
{
void configure(QCommandLineParser& parser)
{
    parser.setApplicationDescription(QStringLiteral("Create bootable USB drives from disk and ISO images."));
    parser.addHelpOption(); parser.addVersionOption();
    parser.addOption({{QStringLiteral("i"), QStringLiteral("iso")}, QStringLiteral("Select a disk or ISO image on startup."), QStringLiteral("path")});
    parser.addOption({{QStringLiteral("x"), QStringLiteral("extra-devs")}, QStringLiteral("List extra USB devices, including USB hard drives.")});
    parser.addOption({{QStringLiteral("g"), QStringLiteral("gui")}, QStringLiteral("Start the GUI (the Linux application already defaults to GUI mode).")});
    parser.addOption({{QStringLiteral("l"), QStringLiteral("locale")}, QStringLiteral("Choose a Rufus language tag or numeric Windows LCID."), QStringLiteral("locale")});
    parser.addOption({{QStringLiteral("f"), QStringLiteral("filesystem")}, QStringLiteral("Prefer this filesystem name or numeric index when available."), QStringLiteral("filesystem")});
    parser.addOption({{QStringLiteral("w"), QStringLiteral("wait")}, QStringLiteral("Wait this many 100-millisecond ticks for the running instance to exit."), QStringLiteral("timeout"), QStringLiteral("0")});
    parser.addOption({QStringLiteral("portable"), QStringLiteral("Use rufus.ini and local storage beside this executable.")});
    parser.addPositionalArgument(QStringLiteral("image"), QStringLiteral("Image to select on startup."), QStringLiteral("[image]"));
}

int filesystem(const QString& text)
{
    const QByteArray encoded = text.toLatin1();
    if (!encoded.isEmpty() && encoded[0] >= '0' && encoded[0] <= '9') {
        errno = 0;
        const long index = std::strtol(encoded.constData(), nullptr, 0);
        return errno == 0 && index >= FS_UNKNOWN && index < FS_MAX ? int(index) : FS_UNKNOWN;
    }
    const char* const names[] = {"FAT16", "FAT32", "NTFS", "UDF", "exFAT", "ReFS", "ext2", "ext3", "ext4"};
    static_assert(sizeof(names) / sizeof(names[0]) == FS_MAX);
    for (int index = 0; index < FS_MAX; ++index)
        if (text.compare(QLatin1String(names[index]), Qt::CaseInsensitive) == 0) return index;
    return FS_UNKNOWN;
}
}

bool ParseStartupOptions(const QStringList& arguments, StartupOptions& options, QString& problem)
{
    options = {}; problem.clear();
    QStringList normalized = arguments;
    for (int index = 1; index < normalized.size(); ++index) {
        const QString argument = normalized[index];
        if (argument == QLatin1String("--")) break;
        const bool valueOption = argument == QLatin1String("-i") || argument == QLatin1String("--iso")
            || argument == QLatin1String("-l") || argument == QLatin1String("--locale")
            || argument == QLatin1String("-f") || argument == QLatin1String("--filesystem")
            || argument == QLatin1String("-w") || argument == QLatin1String("--wait");
        if (valueOption) { ++index; continue; }
        if (argument == QLatin1String("/W")) normalized[index] = QStringLiteral("--wait=150");
    }
    QCommandLineParser parser; configure(parser);
    if (!parser.parse(normalized)) { problem = parser.errorText(); return false; }
    const QStringList images = parser.positionalArguments();
    if (images.size() > 1 || (parser.isSet(QStringLiteral("iso")) && !images.isEmpty())) {
        problem = QStringLiteral("Select one startup image, using either --iso or a positional path."); return false;
    }
    options.image = parser.isSet(QStringLiteral("iso")) ? parser.value(QStringLiteral("iso")) : images.value(0);
    options.locale = parser.value(QStringLiteral("locale"));
    options.extraDevices = parser.isSet(QStringLiteral("extra-devs"));
    options.portable = parser.isSet(QStringLiteral("portable"));
    options.fileSystem = filesystem(parser.value(QStringLiteral("filesystem")));
    bool valid = false;
    const qlonglong wait = parser.value(QStringLiteral("wait")).toLongLong(&valid);
    if (!valid || wait < 0 || wait > INT_MAX / 100) {
        problem = QStringLiteral("--wait must be a nonnegative number of 100-millisecond ticks."); return false;
    }
    options.waitTicks = int(wait);
    if (parser.isSet(QStringLiteral("help")) || parser.isSet(QStringLiteral("help-all"))) options.action = StartupAction::Help;
    else if (parser.isSet(QStringLiteral("version"))) options.action = StartupAction::Version;
    return true;
}

QString StartupHelp()
{
    QCommandLineParser parser; configure(parser); return parser.helpText();
}

bool StartupRequestsText(const QStringList& arguments)
{
    // Values such as --iso --help remain image paths, including Qt options.
    const QStringList takesValue = {QStringLiteral("-i"), QStringLiteral("--iso"),
        QStringLiteral("-l"), QStringLiteral("--locale"), QStringLiteral("-f"), QStringLiteral("--filesystem"), QStringLiteral("-w"), QStringLiteral("--wait"),
        QStringLiteral("-platform"), QStringLiteral("-platformpluginpath"), QStringLiteral("-platformtheme"),
        QStringLiteral("-plugin"), QStringLiteral("-style"), QStringLiteral("-stylesheet"),
        QStringLiteral("-display"), QStringLiteral("-geometry"), QStringLiteral("-session"),
        QStringLiteral("-name"), QStringLiteral("-title"), QStringLiteral("-qmljsdebugger")};
    for (int index = 1; index < arguments.size(); ++index) {
        const QString& argument = arguments[index];
        if (argument == QLatin1String("--")) break;
        if (takesValue.contains(argument)) { ++index; continue; }
        const bool help = argument == QLatin1String("-h") || argument == QLatin1String("--help")
            || argument == QLatin1String("--help-all");
        const bool version = argument == QLatin1String("-v") || argument == QLatin1String("--version");
        if (help || version) return true;
    }
    return false;
}
}
