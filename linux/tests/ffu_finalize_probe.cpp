// SPDX-License-Identifier: GPL-3.0-or-later
// No host block-device access. The driver owns every ordinary input/output file.
#include "platform/ffu_finalize.h"
#include "platform/compressed_write.h"
#include "platform/hash_native.h"
#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

static int collect(void* context, const FFU_DEPLOYMENT_PARTITION* part)
{
    auto* array = static_cast<QJsonArray*>(context);
    array->append(QJsonObject{{"number", int(part->number)}, {"offset", QString::number(part->offset)},
        {"bytes", QString::number(part->bytes)}});
    return 0;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (getuid() == 0 || argc < 5) return 64;
    struct stat sourceInfo;
    if (lstat(argv[2], &sourceInfo) != 0 || !S_ISREG(sourceInfo.st_mode) || sourceInfo.st_uid != getuid()) return 65;
    QJsonObject report;
    int error = EINVAL;
    if (strcmp(argv[1], "plan") == 0 && argc == 5)
    {
        IMAGE_FILE_SOURCE source = {};
        source.fd = -1;
        COMPRESSED_WRITE_PLAN write = {};
        FFU_DEPLOYMENT_INFO info = {};
        QJsonArray partitions;
        error = OpenImageFileSource(argv[2], &source);
        const unsigned sector = unsigned(strtoul(argv[4], nullptr, 10));
        if (error == 0) error = PrepareCompressedWrite(&source, argv[2], INT64_MAX, sector, true, &write);
        if (error == 0) error = PlanFfuDeployment(&source, strtoull(argv[3], nullptr, 10), sector,
            write.layout_sha256, &info, collect, &partitions);
        report = {{"optimized", info.optimized}, {"minimum", QString::number(info.minimum_bytes)},
            {"required", QString::number(write.target_bytes)}, {"captured", QString::number(write.image.decoded_bytes)},
            {"partition", int(info.partition)}, {"offset", QString::number(info.partition_offset)},
            {"bytes", QString::number(info.partition_bytes)}, {"filesystem", QString::number(info.filesystem_bytes)},
            {"partitions", partitions}};
        CloseImageFileSource(&source);
    }
    else if (strcmp(argv[1], "grow") == 0 && argc == 7)
    {
        const int fd = open(argv[2], O_RDWR | O_CLOEXEC | O_NOFOLLOW);
        error = fd < 0 ? errno : rufus::GrowFfuNtfs(fd, 0, strtoull(argv[3], nullptr, 10),
            strtoull(argv[4], nullptr, 10), unsigned(strtoul(argv[5], nullptr, 10)), QString::fromLocal8Bit(argv[6]));
        if (fd >= 0 && close(fd) != 0 && error == 0) error = errno;
    }
    else if (strcmp(argv[1], "xml") == 0 && argc == 6)
    {
        const QByteArray id = QByteArray::fromHex(argv[3]);
        QFile input(QString::fromLocal8Bit(argv[2]));
        QByteArray output;
        if (id.size() == 16 && input.open(QIODevice::ReadOnly))
            error = rufus::UpdateFfuRecoveryXml(input.read(1024 * 1024 + 1),
                reinterpret_cast<const uint8_t*>(id.constData()), strtoull(argv[4], nullptr, 10), output);
        if (error == 0)
        {
            QFile destination(QString::fromLocal8Bit(argv[5]));
            if (!destination.open(QIODevice::WriteOnly | QIODevice::NewOnly) || destination.write(output) != output.size()) error = EIO;
        }
    }
    report.insert("error", error);
    puts(QJsonDocument(report).toJson(QJsonDocument::Compact).constData());
    return 0;
}
