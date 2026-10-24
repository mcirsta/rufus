// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/syslinux_native.h"
#include "syslinux_scan.h"
#include "syslinux_version.h"
#include "image_file.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <cstdio>
#include <cstring>

extern "C" void uprintf(const char*, ...) {}

static QByteArray readFixture(const char* path)
{
    constexpr qint64 maxFixtureBytes = 1024 * 1024;
    QFile file(QString::fromLocal8Bit(path));
    if (!file.open(QIODevice::ReadOnly) || file.size() > maxFixtureBytes)
        qFatal("Cannot read bounded fixture: %s", path);
    const QByteArray bytes = file.readAll();
    if (bytes.size() != file.size())
        qFatal("Incomplete fixture: %s", path);
    return bytes;
}

int main(int argc, char** argv)
{
    // loader, module, image config path, root config exists, filesystem, sector, offset
    if (argc != 8)
        return 64;
    const bool rootExists = strcmp(argv[4], "1") == 0;
    char rootConfig[] = "/syslinux.cfg";
    char* paths[] = {rootConfig, argv[3]};
    const uint32_t count = (argv[3][0] != 0 ? 1u : 0u) + (rootExists ? 1u : 0u);
    StrArray configs = {rootExists ? paths : paths + 1, count, 2};
    RUFUS_IMG_REPORT report = {};
    SelectSyslinuxConfig(&report, &configs);
    QByteArray loader = readFixture(argv[1]);
    const QByteArray module = readFixture(argv[2]);
    loader.append('\0');
    char* suffix = nullptr;
    const quint16 version = GetSyslinuxVersion(loader.data(), size_t(loader.size()), &suffix);
    const char* source = SelectSyslinuxCandidate(&report, nullptr, argv[1], version, suffix);
    const uint16_t embedded[] = {0x0407, 0x0604};
    SelectSyslinuxVersion(&report, source, !module.isEmpty(), embedded);
    unsigned char before[sizeof(report)];
    memcpy(before, &report, sizeof(report));
    bool fsOk, sectorOk, offsetOk;
    const int filesystem = QByteArray(argv[5]).toInt(&fsOk);
    const quint32 sector = QByteArray(argv[6]).toUInt(&sectorOk);
    const quint64 offset = QByteArray(argv[7]).toULongLong(&offsetOk);
    if (!fsOk || !sectorOk || !offsetOk)
        return 64;
    QString problem = QStringLiteral("stale diagnostic");
    const int error = rufus::CheckNativeSyslinuxPair(report, filesystem, sector, offset, module, problem);
    if (memcmp(&report, before, sizeof(report)) != 0)
        return 1;
    ISO_SYSLINUX_CONFIG config = {};
    GetIsoSyslinuxConfig(&report, rootExists, &config);
    const QJsonObject output = {
        {"error", error}, {"problem", problem}, {"detected", int(report.sl_detected_version)},
        {"detected_suffix", report.sl_detected_version_ext}, {"selected", int(report.sl_version)},
        {"selected_suffix", report.sl_version_ext}, {"inferred", bool(report.sl_version_inferred)},
        {"config", report.cfg_path}, {"create", config.create}, {"backup", config.backup},
        {"text", config.text},
    };
    const QByteArray json = QJsonDocument(output).toJson(QJsonDocument::Compact);
    puts(json.constData());
    return 0;
}
