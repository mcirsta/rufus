// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QElapsedTimer>
#include <QString>

namespace rufus
{
void ApplyApplicationAppearance(int mode);
bool ApplicationIsDark();

class ProgressDisplay
{
public:
    void observe(const QString& phase, quint64 processed, quint64 total);
    void clear();
    void cycle() { mode_ = (mode_ + 1) % 3; }
    QString format(const QString& percentage) const;
    int mode() const { return mode_; }
private:
    QString phase_;
    QElapsedTimer timer_;
    quint64 first_ = 0, processed_ = 0, total_ = 0;
    int mode_ = 0;
};
}
