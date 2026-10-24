// SPDX-License-Identifier: GPL-3.0-or-later
#include "media_appearance.h"
#include <QFile>
#include <QResource>
#include <QtEndian>
#include <cerrno>

static bool registerMediaResources()
{
    Q_INIT_RESOURCE(media_appearance);
    return true;
}

namespace rufus
{
QString ExtendedMediaLabel(const QString& text)
{
    QString label = text.left(MediaLabelUnits);
    if (!label.isEmpty() && label.back().isHighSurrogate()) label.chop(1);
    return label;
}

bool ValidMediaLabel(const QString& text)
{
    if (text.size() > MediaLabelUnits || !QStringView(text).isValidUtf16()) return false;
    for (QChar character : text)
        if (character.unicode() < 32 || character.unicode() == 127) return false;
    return true;
}

int ReadMediaIcon(QByteArray& icon)
{
    static const bool ready = registerMediaResources();
    Q_UNUSED(ready);
    QFile resource(QStringLiteral(":/rufus/appearance/autorun.ico"));
    if (!resource.open(QIODevice::ReadOnly)) return ENOENT;
    icon = resource.readAll();
    constexpr qsizetype maximumIconBytes = 1024 * 1024;
    if (icon.size() < 6 || icon.size() > maximumIconBytes) return EBADMSG;
    const auto* bytes = reinterpret_cast<const uchar*>(icon.constData());
    const unsigned count = qFromLittleEndian<quint16>(bytes + 4);
    const bool iconHeader = qFromLittleEndian<quint16>(bytes) == 0
        && qFromLittleEndian<quint16>(bytes + 2) == 1 && count != 0 && count <= 64;
    if (!iconHeader || 6 + count * 16 > unsigned(icon.size())) return EBADMSG;
    for (unsigned i = 0; i < count; ++i)
    {
        const quint32 length = qFromLittleEndian<quint32>(bytes + 6 + i * 16 + 8);
        const quint32 offset = qFromLittleEndian<quint32>(bytes + 6 + i * 16 + 12);
        if (length == 0 || offset < 6 + count * 16 || offset > unsigned(icon.size())
            || length > unsigned(icon.size()) - offset) return EBADMSG;
    }
    return 0;
}

}
