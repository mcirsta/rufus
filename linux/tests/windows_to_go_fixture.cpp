// SPDX-License-Identifier: GPL-3.0-or-later
#include "windows_to_go_fixture.h"
#include <hivex.h>
#include <wimlib.h>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtEndian>

namespace
{
bool put(const QString& path, const QByteArray& bytes)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.flush();
}

QByteArray pe(quint16 machine)
{
    QByteArray bytes(256, '\0');
    bytes.replace(0, 2, "MZ");
    qToLittleEndian<quint32>(64, bytes.data() + 60);
    bytes.replace(64, 4, QByteArray("PE\0\0", 4));
    qToLittleEndian<quint16>(machine, bytes.data() + 68);
    return bytes;
}

bool dword(hive_h* hive, hive_node_h node, const char* name, quint32 number)
{
    quint32 encoded = qToLittleEndian(number);
    hive_set_value value = {const_cast<char*>(name), hive_t_REG_DWORD, sizeof(encoded), reinterpret_cast<char*>(&encoded)};
    return node != 0 && hivex_node_set_value(hive, node, &value, 0) == 0;
}

bool makeHive(const QString& path, bool bcd)
{
    if (!QFile::copy(QStringLiteral(RUFUS_MINIMAL_HIVE), path)) return false;
    hive_h* hive = hivex_open(QFile::encodeName(path).constData(), HIVEX_OPEN_WRITE);
    if (hive == nullptr) return false;
    const hive_node_h root = hivex_root(hive);
    bool ok = root != 0;
    if (bcd)
    {
        const hive_node_h objects = ok ? hivex_node_add_child(hive, root, "Objects") : 0;
        ok = objects != 0;
        const char* const ids[] = {"{9dea862c-5cdd-4e70-acc1-f32b344d4795}",
            "{11111111-0000-0000-0000-000000000001}", "{22222222-0000-0000-0000-000000000002}"};
        for (int i = 0; ok && i < 3; ++i)
        {
            const hive_node_h object = hivex_node_add_child(hive, objects, ids[i]);
            if (object == 0) { ok = false; break; }
            const hive_node_h description = hivex_node_add_child(hive, object, "Description");
            const hive_node_h elements = hivex_node_add_child(hive, object, "Elements");
            ok = elements != 0 && dword(hive, description, "Type", i == 0 ? 0x10100002 : 0x10200003);
            if (!ok || i == 0) continue;
            const QString loader = i == 1 ? QStringLiteral("\\system32\\winload.efi") : QStringLiteral("\\system32\\winload.exe");
            QByteArray encoded((loader.size() + 1) * 2, '\0');
            for (qsizetype character = 0; character < loader.size(); ++character)
                qToLittleEndian<quint16>(loader[character].unicode(), encoded.data() + 2 * character);
            const hive_node_h element = hivex_node_add_child(hive, elements, "12000002");
            hive_set_value value = {const_cast<char*>("Element"), hive_t_REG_SZ, size_t(encoded.size()), encoded.data()};
            ok = element != 0 && hivex_node_set_value(hive, element, &value, 0) == 0;
        }
    }
    else
    {
        const hive_node_h select = ok ? hivex_node_add_child(hive, root, "Select") : 0;
        ok = dword(hive, select, "Default", 1);
        hive_node_h node = ok ? hivex_node_add_child(hive, root, "ControlSet001") : 0;
        const hive_node_h controlSet = node;
        for (const char* part : {"Services", "partmgr", "Parameters"})
        {
            if (node == 0) break;
            node = hivex_node_add_child(hive, node, part);
        }
        ok = ok && dword(hive, node, "SanPolicy", 1);
        if (ok) ok = hivex_node_add_child(hive, controlSet, "Control") != 0;
    }
    if (ok) ok = hivex_commit(hive, nullptr, 0) == 0;
    if (hivex_close(hive) != 0) ok = false;
    return ok;
}
}

bool CreateWindowsToGoFixture(const QString& output, bool solid, const QString& change, const QString& bootFile)
{
    if (QFile::exists(output)) return false;
    QTemporaryDir temporary;
    if (!temporary.isValid()) return false;
    WIMStruct* wim = nullptr;
    QByteArray efi = pe(0x8664);
    if (!bootFile.isEmpty())
    {
        QFile input(bootFile);
        if (!input.open(QIODevice::ReadOnly)) return false;
        efi = input.readAll();
        if (efi.isEmpty()) return false;
    }
    int error = wimlib_create_new_wim(solid ? WIMLIB_COMPRESSION_TYPE_LZMS : WIMLIB_COMPRESSION_TYPE_LZX, &wim);
    bool ok = error == 0;
    for (int image = 1; ok && image <= 2; ++image)
    {
        const QString tree = temporary.filePath(QStringLiteral("image-%1").arg(image));
        ok = QDir().mkpath(tree + "/Windows/System32/config")
            && makeHive(tree + "/Windows/System32/config/BCD-Template", true)
            && makeHive(tree + "/Windows/System32/config/SYSTEM", false)
            && put(tree + "/Windows/Boot/EFI/bootmgr.efi", efi)
            && put(tree + "/Windows/Boot/EFI/bootmgfw.efi", change == "wrong-arch" ? pe(0xaa64) : efi)
            && put(tree + "/Windows/Boot/PCAT/bootmgr", "BIOS manager")
            && put(tree + "/Windows/Boot/Fonts/wgl4_boot.ttf", "font")
            && put(tree + "/payload", QByteArray(128 * 1024, 'A'));
        if (ok && !bootFile.isEmpty()) ok = put(tree + "/edition.txt", QByteArray::number(image));
        if (ok && change == "existing-answer") ok = put(tree + "/Windows/Panther/unattend.xml", "original answer");
        const QByteArray name = "Windows edition " + QByteArray::number(image);
        if (ok) error = wimlib_add_image(wim, QFile::encodeName(tree).constData(), name.constData(), nullptr, 0);
        if (ok && error == 0) error = wimlib_set_image_property(wim, image, "DISPLAYNAME", name.constData());
        const char* const names[] = {"MAJOR", "MINOR", "BUILD", "SPBUILD"};
        const char* const values[] = {"10", "0", "26200", "0"};
        for (int field = 0; ok && error == 0 && field < 4; ++field)
        {
            const QByteArray property = QByteArray("WINDOWS/VERSION/") + names[field];
            error = wimlib_set_image_property(wim, image, property.constData(), values[field]);
        }
        ok = ok && error == 0;
    }
    if (ok) error = wimlib_write(wim, QFile::encodeName(output).constData(), WIMLIB_ALL_IMAGES, solid ? WIMLIB_WRITE_FLAG_SOLID : 0, 1);
    wimlib_free(wim);
    return ok && error == 0;
}
