// SPDX-License-Identifier: GPL-3.0-or-later
#include "windows_to_go_fixture.h"
#include <wimlib.h>
#include <QCoreApplication>

extern "C" int IsIsoScanCancelled(void) { return 0; }

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    if (argc != 4) return 64;
    const QStringList arguments = application.arguments();
    const bool solid = arguments[2] == QStringLiteral("esd");
    if (!solid && arguments[2] != QStringLiteral("wim")) return 64;
    if (wimlib_global_init(WIMLIB_INIT_FLAG_ASSUME_UTF8) != 0) return 1;
    const bool created = CreateWindowsToGoFixture(arguments[1], solid, {}, arguments[3]);
    wimlib_global_cleanup();
    return created ? 0 : 1;
}
