// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QString>

// Synthetic Windows-shaped image for source, GUI and disposable Linux-guest
// qualification. Boot payloads are test bytes, never Windows programs. Caller
// initializes wimlib; output must not already exist and remains caller-owned.
bool CreateWindowsToGoFixture(const QString& output, bool solid = false, const QString& change = {}, const QString& bootFile = {});
