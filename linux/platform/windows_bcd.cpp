// SPDX-License-Identifier: GPL-3.0-or-later
#include "windows_bcd.h"
#include "rufus_reporting.h"

#include <QTemporaryDir>
#include <QStringDecoder>
#include <QtEndian>
#include <hivex.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace rufus
{
namespace
{
constexpr qsizetype maximumBcdBytes = 128 * 1024 * 1024;
constexpr quint32 bootManagerType = 0x10100002, osLoaderType = 0x10200003;
constexpr quint32 resumeType = 0x10200004, memoryDiagnosticType = 0x10200005;
constexpr quint32 templateClass = 0x40000000, elementClassMask = 0xf0000000;
constexpr int guidBytes = 16, deviceBytes = 88, deviceHeaderBytes = deviceBytes - guidBytes, partitionDeviceType = 6;
constexpr int deviceTypeOffset = 16, deviceSizeOffset = 24, partitionIdOffset = 32;
constexpr int partitionStyleOffset = 52, diskIdOffset = 56;
const char bootManagerId[] = "{9dea862c-5cdd-4e70-acc1-f32b344d4795}";

bool validPartition(const WindowsBcdPartition& part)
{
    if (part.gpt)
        return !part.diskGuid.isNull() && !part.partitionGuid.isNull() && part.mbrSignature == 0;
    const bool aligned = part.offset != 0 && part.offset % 512 == 0;
    return part.mbrSignature != 0 && aligned && part.diskGuid.isNull() && part.partitionGuid.isNull();
}

QByteArray windowsGuid(const QUuid& value)
{
    QByteArray bytes(guidBytes, Qt::Uninitialized);
    qToLittleEndian<quint32>(value.data1, bytes.data());
    qToLittleEndian<quint16>(value.data2, bytes.data() + 4);
    qToLittleEndian<quint16>(value.data3, bytes.data() + 6);
    memcpy(bytes.data() + 8, value.data4, sizeof(value.data4));
    return bytes;
}

QByteArray partitionDevice(const WindowsBcdPartition& part)
{
    // On-disk device element: optional-object GUID, then the boot library's
    // descriptor. This is not the WMI BCD_ELEMENT_DEVICE structure.
    QByteArray bytes(deviceBytes, '\0');
    qToLittleEndian<quint32>(partitionDeviceType, bytes.data() + deviceTypeOffset);
    qToLittleEndian<quint32>(deviceHeaderBytes, bytes.data() + deviceSizeOffset);
    if (part.gpt)
    {
        memcpy(bytes.data() + partitionIdOffset, windowsGuid(part.partitionGuid).constData(), guidBytes);
        memcpy(bytes.data() + diskIdOffset, windowsGuid(part.diskGuid).constData(), guidBytes);
    }
    else
    {
        qToLittleEndian<quint64>(part.offset, bytes.data() + partitionIdOffset);
        qToLittleEndian<quint32>(1, bytes.data() + partitionStyleOffset); // Boot library's MBR discriminator.
        qToLittleEndian<quint32>(part.mbrSignature, bytes.data() + diskIdOffset);
    }
    return bytes;
}

QByteArray stringBytes(const QString& text)
{
    QByteArray bytes((text.size() + 1) * 2, '\0');
    for (qsizetype i = 0; i < text.size(); ++i)
        qToLittleEndian<quint16>(text[i].unicode(), bytes.data() + i * 2);
    return bytes;
}

struct BcdEditor
{
    hive_h* hive;
    int error = 0;

    hive_node_h child(hive_node_h parent, const char* name, bool required = false, bool create = false)
    {
        if (error != 0) return 0;
        if (parent == 0) { error = EINVAL; return 0; }
        errno = 0;
        hive_node_h node = hivex_node_get_child(hive, parent, name);
        if (node == 0 && errno == 0 && create)
            node = hivex_node_add_child(hive, parent, name);
        if (node == 0 && (required || create || errno != 0))
            error = errno != 0 ? errno : EINVAL;
        return node;
    }

    QByteArray value(hive_node_h node, const char* name, hive_type expected, bool required)
    {
        if (error != 0 || node == 0) return {};
        errno = 0;
        const hive_value_h value = hivex_node_get_value(hive, node, name);
        if (value == 0)
        {
            if (required || errno != 0) error = errno != 0 ? errno : EINVAL;
            return {};
        }
        hive_type type;
        size_t size = 0;
        char* data = hivex_value_value(hive, value, &type, &size);
        if (data == nullptr) { error = errno != 0 ? errno : EIO; return {}; }
        const bool supported = type == expected && size <= size_t(maximumBcdBytes);
        QByteArray result;
        if (supported) result = QByteArray(data, qsizetype(size));
        else error = EINVAL;
        free(data);
        return result;
    }

    QByteArray element(hive_node_h object, const char* name, hive_type type)
    {
        const hive_node_h elements = child(object, "Elements");
        if (elements == 0) return {};
        const hive_node_h node = child(elements, name);
        return node != 0 ? value(node, "Element", type, true) : QByteArray();
    }

    QString text(hive_node_h object, const char* name)
    {
        const QByteArray bytes = element(object, name, hive_t_REG_SZ);
        if (bytes.isEmpty()) return {};
        if (bytes.size() % 2 != 0 || !bytes.endsWith(QByteArray(2, '\0')))
        { error = EINVAL; return {}; }
        QStringDecoder decoder(QStringDecoder::Utf16LE);
        const QString decoded = decoder(bytes);
        if (decoder.hasError()) { error = EINVAL; return {}; }
        QString result = decoded;
        result.chop(1);
        if (result.contains(QChar::Null)) { error = EINVAL; return {}; }
        return result;
    }

    quint32 type(hive_node_h object)
    {
        const hive_node_h description = child(object, "Description");
        const QByteArray bytes = value(description, "Type", hive_t_REG_DWORD, false);
        if (bytes.isEmpty()) return 0;
        if (bytes.size() != 4) { error = EINVAL; return 0; }
        return qFromLittleEndian<quint32>(bytes.constData());
    }

    void set(hive_node_h object, const char* name, hive_type type, const QByteArray& bytes)
    {
        if (error != 0) return;
        if (IsRufusJobCancelled()) { error = ECANCELED; return; }
        const hive_node_h elements = child(object, "Elements", false, true);
        const hive_node_h node = child(elements, name, false, true);
        if (error != 0) return;
        hive_set_value value = {const_cast<char*>("Element"), type, size_t(bytes.size()), const_cast<char*>(bytes.constData())};
        if (hivex_node_set_value(hive, node, &value, 0) != 0)
            error = errno != 0 ? errno : EIO;
    }

    void setText(hive_node_h object, const char* name, const QString& text)
    {
        set(object, name, hive_t_REG_SZ, stringBytes(text));
    }

    void removeTemplates(hive_node_h object)
    {
        if (error != 0) return;
        const hive_node_h elements = child(object, "Elements");
        if (elements == 0) return;
        hive_node_h* nodes = hivex_node_children(hive, elements);
        if (nodes == nullptr) { error = errno != 0 ? errno : EIO; return; }
        for (size_t i = 0; nodes[i] != 0 && error == 0; ++i)
        {
            char* name = hivex_node_name(hive, nodes[i]);
            if (name == nullptr) { error = errno != 0 ? errno : EIO; break; }
            bool valid = false;
            const quint32 element = QByteArray(name).toUInt(&valid, 16);
            free(name);
            if (valid && (element & elementClassMask) == templateClass
                && hivex_node_delete_child(hive, nodes[i]) != 0)
                error = errno != 0 ? errno : EIO;
        }
        free(nodes);
    }

    hive_node_h findLoader(hive_node_h objects, bool uefi, QUuid& id)
    {
        hive_node_h found = 0;
        hive_node_h* nodes = hivex_node_children(hive, objects);
        if (nodes == nullptr) { error = errno != 0 ? errno : EIO; return 0; }
        for (size_t i = 0; nodes[i] != 0 && error == 0; ++i)
        {
            if (IsRufusJobCancelled()) { error = ECANCELED; break; }
            if (type(nodes[i]) != osLoaderType) continue;
            // The setup template's default is often a PE loader. It cannot
            // boot the Windows tree we have just applied to NTFS.
            const QString ramdisk = text(nodes[i], "42000003");
            const QByteArray pe = element(nodes[i], "26000022", hive_t_REG_BINARY);
            if (!ramdisk.isEmpty() || (!pe.isEmpty() && pe[0] != '\0')) continue;
            QString path = text(nodes[i], "12000002");
            if (path.isEmpty()) path = text(nodes[i], "42000002");
            path.replace('/', '\\');
            const QString suffix = uefi ? QStringLiteral("\\winload.efi") : QStringLiteral("\\winload.exe");
            if (!path.endsWith(suffix, Qt::CaseInsensitive)) continue;
            if (found != 0) { error = EINVAL; break; }
            char* name = hivex_node_name(hive, nodes[i]);
            if (name == nullptr) { error = errno != 0 ? errno : EIO; break; }
            id = QUuid(QString::fromUtf8(name));
            free(name);
            if (id.isNull()) { error = EINVAL; break; }
            found = nodes[i];
        }
        free(nodes);
        if (found == 0 && error == 0) error = ENOTSUP;
        return found;
    }

    void configure(hive_node_h objects, const WindowsBcdOptions& options, QUuid& loaderId)
    {
        const hive_node_h manager = child(objects, bootManagerId, true);
        if (type(manager) != bootManagerType && error == 0) error = EINVAL;
        if (error != 0) return;
        const hive_node_h loader = findLoader(objects, options.uefi, loaderId);
        if (error != 0) return;
        const QByteArray windowsDevice = partitionDevice(options.windows);
        const QByteArray systemDevice = partitionDevice(options.system);
        const QString extension = options.uefi ? QStringLiteral(".efi") : QStringLiteral(".exe");
        set(loader, "11000001", hive_t_REG_BINARY, windowsDevice);
        set(loader, "21000001", hive_t_REG_BINARY, windowsDevice);
        setText(loader, "12000002", QStringLiteral("\\Windows\\system32\\winload") + extension);
        setText(loader, "12000004", options.description);
        setText(loader, "12000005", options.locale);
        setText(loader, "22000002", QStringLiteral("\\Windows"));
        set(loader, "16000009", hive_t_REG_BINARY, QByteArray(1, '\0')); // recoveryenabled no
        const QString resumeId = text(loader, "23000003");
        if (!resumeId.isEmpty())
        {
            const QUuid resumeUuid(resumeId);
            if (resumeUuid.isNull()) { error = EINVAL; return; }
            const QByteArray key = resumeUuid.toString().toUtf8();
            const hive_node_h resume = child(objects, key.constData(), true);
            if (type(resume) != resumeType && error == 0) error = EINVAL;
            set(resume, "11000001", hive_t_REG_BINARY, windowsDevice);
            set(resume, "21000001", hive_t_REG_BINARY, windowsDevice);
            setText(resume, "12000002", QStringLiteral("\\Windows\\system32\\winresume") + extension);
            setText(resume, "12000005", options.locale);
            removeTemplates(resume);
        }
        removeTemplates(loader);
        set(manager, "11000001", hive_t_REG_BINARY, systemDevice);
        setText(manager, "12000002", options.uefi ? QStringLiteral("\\EFI\\Microsoft\\Boot\\bootmgfw.efi") : QStringLiteral("\\bootmgr"));
        setText(manager, "12000005", options.locale);
        setText(manager, "23000003", loaderId.toString());
        set(manager, "24000001", hive_t_REG_MULTI_SZ, stringBytes(loaderId.toString()) + QByteArray(2, '\0'));
        removeTemplates(manager);
        const hive_node_h memory = child(objects, "{b2721d73-1db4-4c62-bf78-c548a880142d}");
        if (memory != 0)
        {
            if (type(memory) != memoryDiagnosticType && error == 0) error = EINVAL;
            set(memory, "11000001", hive_t_REG_BINARY, systemDevice);
            setText(memory, "12000002", options.uefi ? QStringLiteral("\\EFI\\Microsoft\\Boot\\memtest.efi") : QStringLiteral("\\Boot\\memtest.exe"));
            setText(memory, "12000005", options.locale);
            removeTemplates(memory);
            set(manager, "24000010", hive_t_REG_MULTI_SZ,
                stringBytes(QStringLiteral("{b2721d73-1db4-4c62-bf78-c548a880142d}")) + QByteArray(2, '\0'));
        }
    }
};

int readOutput(int descriptor, QByteArray& bytes)
{
    struct stat info;
    if (fstat(descriptor, &info) != 0) return errno;
    if (info.st_size <= 0 || info.st_size > maximumBcdBytes) return EFBIG;
    bytes.resize(info.st_size);
    for (qsizetype done = 0; done < bytes.size();)
    {
        if (IsRufusJobCancelled()) return ECANCELED;
        const ssize_t count = pread(descriptor, bytes.data() + done, size_t(bytes.size() - done), done);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return count == 0 ? EIO : errno;
        done += count;
    }
    return 0;
}
}

int PrepareWindowsToGoBcd(int templateFile, const WindowsBcdOptions& options, WindowsBcdResult& result)
{
    result = {};
    const bool partitions = validPartition(options.windows) && validPartition(options.system);
    const bool sameStyle = options.windows.gpt == options.system.gpt;
    const bool sameDisk = options.windows.gpt ? options.windows.diskGuid == options.system.diskGuid
        : options.windows.mbrSignature == options.system.mbrSignature;
    if (!partitions || !sameStyle || !sameDisk) return EINVAL;
    const bool labels = !options.description.isEmpty() && !options.locale.isEmpty()
        && !options.description.contains(QChar::Null) && !options.locale.contains(QChar::Null);
    if (!labels) return EINVAL;
    if (IsRufusJobCancelled()) return ECANCELED;
    struct stat before, after;
    if (fstat(templateFile, &before) != 0) return errno;
    if (!S_ISREG(before.st_mode)) return EINVAL;
    if (before.st_size <= 0 || before.st_size > maximumBcdBytes) return EFBIG;
    char retained[64];
    snprintf(retained, sizeof(retained), "/proc/self/fd/%d", templateFile);
    hive_h* hive = hivex_open(retained, HIVEX_OPEN_WRITE);
    if (hive == nullptr) return errno != 0 ? errno : EIO;
    BcdEditor editor = {hive};
    const hive_node_h root = hivex_root(hive);
    const hive_node_h objects = editor.child(root, "Objects", true);
    WindowsBcdResult output;
    if (editor.error == 0) editor.configure(objects, options, output.loader);
    int error = editor.error;
    QTemporaryDir temporary;
    int descriptor = -1;
    if (error == 0 && !temporary.isValid()) error = EIO;
    if (error == 0)
    {
        const QByteArray path = temporary.filePath(QStringLiteral("BCD")).toUtf8();
        descriptor = open(path.constData(), O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (descriptor < 0) error = errno;
    }
    if (error == 0)
    {
        snprintf(retained, sizeof(retained), "/proc/self/fd/%d", descriptor);
        if (IsRufusJobCancelled()) error = ECANCELED;
        else if (hivex_commit(hive, retained, 0) != 0) error = errno != 0 ? errno : EIO;
        if (error == 0 && fsync(descriptor) != 0) error = errno;
        if (error == 0) error = readOutput(descriptor, output.bytes);
    }
    if (hivex_close(hive) != 0 && error == 0) error = errno != 0 ? errno : EIO;
    if (descriptor >= 0 && close(descriptor) != 0 && error == 0) error = errno;
    if (!temporary.remove() && error == 0) error = EIO;
    if (fstat(templateFile, &after) != 0 && error == 0) error = errno;
    if (error == 0)
    {
        const bool identity = before.st_dev == after.st_dev && before.st_ino == after.st_ino && before.st_size == after.st_size;
        const bool modified = before.st_mtim.tv_sec == after.st_mtim.tv_sec && before.st_mtim.tv_nsec == after.st_mtim.tv_nsec;
        const bool changed = before.st_ctim.tv_sec == after.st_ctim.tv_sec && before.st_ctim.tv_nsec == after.st_ctim.tv_nsec;
        if (!identity || !modified || !changed) error = ESTALE;
    }
    if (error == 0 && IsRufusJobCancelled()) error = ECANCELED;
    if (error == 0) result = std::move(output);
    return error;
}
}
