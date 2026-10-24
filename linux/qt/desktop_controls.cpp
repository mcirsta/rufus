// SPDX-License-Identifier: GPL-3.0-or-later
#include "desktop_controls.h"
#include "device_size.h"
#include <QApplication>
#include <QPalette>
#include <QStyle>
#include <QStyleFactory>
#include <cmath>
#include <limits>

namespace rufus
{
bool ApplicationIsDark() { return qGray(qApp->palette().color(QPalette::Window).rgb()) < 128; }

void ApplyApplicationAppearance(int mode)
{
    if (mode != 1 && mode != 2) return;
    // A native theme can ignore application colors. Explicit user overrides
    // select Fusion so all existing and subsequently created dialogs agree.
    qApp->setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    QPalette palette = qApp->style()->standardPalette();
    if (mode == 1) {
        palette.setColor(QPalette::Window, QColor(45,45,45));
        palette.setColor(QPalette::Base, QColor(28,28,28));
        palette.setColor(QPalette::AlternateBase, QColor(48,48,48));
        palette.setColor(QPalette::Button, QColor(55,55,55));
        palette.setColor(QPalette::WindowText, Qt::white);
        palette.setColor(QPalette::Text, Qt::white);
        palette.setColor(QPalette::ButtonText, Qt::white);
        palette.setColor(QPalette::ToolTipBase, QColor(55,55,55));
        palette.setColor(QPalette::ToolTipText, Qt::white);
        palette.setColor(QPalette::Highlight, QColor(45,115,185));
        palette.setColor(QPalette::HighlightedText, Qt::white);
        palette.setColor(QPalette::Link, QColor(115,185,255));
        for (auto role : {QPalette::WindowText,QPalette::Text,QPalette::ButtonText})
            palette.setColor(QPalette::Disabled, role, QColor(150,150,150));
    }
    qApp->setPalette(palette);
}

void ProgressDisplay::clear()
{
    phase_.clear(); timer_.invalidate(); first_ = processed_ = total_ = 0;
}

void ProgressDisplay::observe(const QString& phase, quint64 processed, quint64 total)
{
    if (total == 0 || processed > total) { clear(); return; }
    if (phase != phase_ || total != total_ || processed < processed_ || !timer_.isValid()) {
        phase_ = phase; first_ = processed; timer_.start();
    }
    processed_ = processed; total_ = total;
}

QString ProgressDisplay::format(const QString& percentage) const
{
    if (mode_ == 0 || !timer_.isValid() || !percentage.contains(QLatin1String("%p%"))) return percentage;
    constexpr qint64 minimumSampleMs = 200;
    const qint64 elapsed = timer_.elapsed();
    if (elapsed < minimumSampleMs || processed_ <= first_) return percentage;
    const double rate = double(processed_ - first_) * 1000.0 / double(elapsed);
    QString measurement;
    if (mode_ == 1) measurement = QCoreApplication::translate("ProgressDisplay", "%1/s").arg(DeviceSizeLabel(quint64(qMin(rate,double(std::numeric_limits<quint64>::max()/2))), false));
    else {
        constexpr double maximumSeconds = 999.0 * 60.0 * 60.0;
        const quint64 seconds = quint64(qMin(maximumSeconds, std::ceil(double(total_ - processed_) / rate)));
        const QString time = QStringLiteral("%1:%2:%3").arg(seconds / 3600,2,10,QLatin1Char('0'))
            .arg((seconds / 60) % 60,2,10,QLatin1Char('0')).arg(seconds % 60,2,10,QLatin1Char('0'));
        measurement = QCoreApplication::translate("ProgressDisplay", "%1 remaining").arg(time);
    }
    return QString(percentage).replace(QStringLiteral("%p%"), measurement);
}
}
